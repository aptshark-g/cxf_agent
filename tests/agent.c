#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cxf/world.h"

#include "agent.h"
#include "agent_action.h"
#include "agent_llm.h"      /* 只为 agent_llm_render_tools / 转义：语法只有一份实现 */
#include "agent_types.h"
#include "agent_types.gen.h"

/*
 * agent 运行时第一刀：单 rank 的能力派发（docs/agent.md §6）。
 *
 * 五个模块，各占一个内核 phase：
 *   agent_store    持有共享状态（无 tick，只参与装配期）
 *   agent_inject   INPUT    从入口 ring 取请求，把能力名解析成 DAG
 *   agent_match    UPDATE   可用性判定（存在 且 不越权），READY → RUNNING
 *   agent_execute  LATE     调 handler，失败沿降级链换实现
 *   agent_recycle  LAST     汇聚到父节点 / 产出最终结果，回收行与 blob
 *
 * 全程按 SoA index 遍历，没有任何"按 id 找对象"的扫描；
 * 能力解析是 cap_id → handle → row 的两次数组索引，不是查找。
 * 降级同样不进查找路径：cap_fb[] 在注册期就算好了。
 */

#define BLOB_COL_PTR 0u
#define BLOB_COL_LEN 1u
#define BLOB_COL_CAP 2u

#define IMPL_COL_API 0u
#define IMPL_COL_ID  1u

#define DEF_COL_DEF  0u
#define DEF_COL_ID   1u

#define IFACE_COL_DEF 0u
#define IFACE_COL_ID  1u

#define DESC_SAFE_MAX 96

/* ---- 名字与前缀 ---- */

static bool has_prefix(const char *s, const char *p, uint32_t n) {
    return strncmp(s, p, n) == 0;
}

/* 取第 idx 段（0 起）的点分片段。不存在返回 false。 */
static bool seg_span(const char *name, uint32_t idx, const char **p, uint32_t *n) {
    const char *start = name;

    for (uint32_t i = 0; i < idx; i++) {
        const char *dot = strchr(start, '.');
        if (dot == NULL) return false;
        start = dot + 1;
    }
    const char *dot = strchr(start, '.');
    *p = start;
    *n = dot != NULL ? (uint32_t)(dot - start) : (uint32_t)strlen(start);
    return *n > 0;
}

static void last_seg(const char *name, const char **p, uint32_t *n) {
    const char *dot = strrchr(name, '.');
    *p = dot != NULL ? dot + 1 : name;
    *n = (uint32_t)strlen(*p);
}

/* ---- 注册表：两层，按服务名前缀枚举 ---- */

static const CapImpl *cap_impl_of(AgentStore *s, uint32_t cap_id) {
    cxf_handle h;

    if (cap_id >= s->cap_count) return NULL;
    h = s->cap_of[cap_id];
    if (!cxf_table_live(&s->caps, h)) return NULL;

    return ((const CapImpl **)cxf_table_col(&s->caps, IMPL_COL_API))
           [cxf_table_row(&s->caps, h)];
}

static const CapDef *cat_def_of(AgentStore *s, uint32_t cat_id) {
    cxf_handle h;

    if (cat_id >= s->cat_count) return NULL;
    h = s->def_of[cat_id];
    if (!cxf_table_live(&s->defs, h)) return NULL;

    return ((const CapDef **)cxf_table_col(&s->defs, DEF_COL_DEF))
           [cxf_table_row(&s->defs, h)];
}

static const IfaceDef *iface_def_of(AgentStore *s, uint32_t iface_id) {
    cxf_handle h;

    if (iface_id >= s->iface_count) return NULL;
    h = s->iface_of[iface_id];
    if (!cxf_table_live(&s->ifaces, h)) return NULL;

    return ((const IfaceDef **)cxf_table_col(&s->ifaces, IFACE_COL_DEF))
           [cxf_table_row(&s->ifaces, h)];
}

const CapImpl *agent_cap_impl(AgentStore *s, uint32_t cap_id) {
    return cap_impl_of(s, cap_id);
}

const CapDef *agent_cat_def(AgentStore *s, uint32_t cat_id) {
    return cat_def_of(s, cat_id);
}

uint32_t agent_cat_id_n(AgentStore *s, const char *name, uint32_t len) {
    if (name == NULL) return CAP_NONE;

    for (uint32_t i = 0; i < s->cat_count; i++) {
        const CapDef *d = cat_def_of(s, i);

        /* 先比长度再比字节：长度不等就不可能是同一个名字，省一次 memcmp；
           更重要的是"必须相等"这件事写在了比较的第一位。 */
        if (d != NULL && strlen(d->name) == len && memcmp(d->name, name, len) == 0)
            return i;
    }
    return CAP_NONE;
}

uint32_t agent_cat_id(AgentStore *s, const char *capability) {
    return agent_cat_id_n(s, capability, (uint32_t)strlen(capability));
}

const IfaceDef *agent_iface_def(AgentStore *s, uint32_t iface_id) {
    return iface_def_of(s, iface_id);
}

uint32_t agent_iface_id(AgentStore *s, const char *name) {
    if (name == NULL) return IFACE_NONE;
    for (uint32_t i = 0; i < s->iface_count; i++) {
        const IfaceDef *d = iface_def_of(s, i);
        if (d != NULL && strcmp(d->name, name) == 0) return i;
    }
    return IFACE_NONE;
}

/*
 * 邻接链的节点编址（与 agent_caps.h 的 CAT_NODES 配套）。
 * 每个能力占 CAP_IN_MAX+1 个槽：0 号是产出端口，1..CAP_IN_MAX 是输入端口。
 * 于是"按端口"这件事被编进下标本身，链接槽不会互相覆盖。
 */
#define CAT_NODE_STRIDE (CAP_IN_MAX + 1u)
#define CAT_NODE_OUT(c) ((c) * CAT_NODE_STRIDE)
#define CAT_NODE_IN(c, k) ((c) * CAT_NODE_STRIDE + 1u + (k))
#define CAT_NODE_CAT(n) ((n) / CAT_NODE_STRIDE)

/*
 * 类型图上某个类型的入度/出度。
 *
 * 计数口径刻意不同：
 *   producers = 产出它的**能力**数（一个能力只有一个产出端口）；
 *   consumers = 消费它的**端口**数（同一能力可以有两个同类端口，plan.merge 就是）。
 * "多少个能力能给我这个类型"与"这条数据流要被喂几处"是两个不同的问题，
 * 后者才是 fan-in 要等的条数。
 */
uint32_t agent_iface_producers(AgentStore *s, uint32_t iface_id) {
    uint32_t n = 0;

    if (iface_id >= s->iface_count) return 0;
    for (uint32_t nd = s->prod_head[iface_id]; nd != CAP_NONE; nd = s->prod_next[nd]) n++;
    return n;
}

uint32_t agent_iface_consumers(AgentStore *s, uint32_t iface_id) {
    uint32_t n = 0;

    if (iface_id >= s->iface_count) return 0;
    for (uint32_t nd = s->cons_head[iface_id]; nd != CAP_NONE; nd = s->cons_next[nd]) n++;
    return n;
}

uint32_t agent_iface_producer_at(AgentStore *s, uint32_t iface_id, uint32_t i) {
    if (iface_id >= s->iface_count) return CAP_NONE;
    for (uint32_t nd = s->prod_head[iface_id]; nd != CAP_NONE; nd = s->prod_next[nd])
        if (i-- == 0) return CAT_NODE_CAT(nd);
    return CAP_NONE;
}

uint32_t agent_iface_consumer_at(AgentStore *s, uint32_t iface_id, uint32_t i) {
    if (iface_id >= s->iface_count) return CAP_NONE;
    for (uint32_t nd = s->cons_head[iface_id]; nd != CAP_NONE; nd = s->cons_next[nd])
        if (i-- == 0) return CAT_NODE_CAT(nd);
    return CAP_NONE;
}

/* 首选实现 = 该能力降级链的链头（拓扑性质，不是注册顺序）。 */
uint32_t agent_cap_id(AgentStore *s, const char *capability) {
    uint32_t cat = agent_cat_id(s, capability);
    return cat != CAP_NONE ? s->head_of[cat] : CAP_NONE;
}

static uint32_t cap_id_of_impl(AgentStore *s, const CapImpl *impl) {
    for (uint32_t id = 0; id < s->cap_count; id++)
        if (cap_impl_of(s, id) == impl) return id;
    return CAP_NONE;
}

static cxf_result cat_add(AgentStore *s, const CapDef *def) {
    cxf_handle h;
    uint32_t row;

    if (def->name == NULL || def->desc == NULL || def->name[0] == '\0')
        return CXF_ERR_RANGE;
    for (uint32_t i = 0; i < s->cat_count; i++) {
        const CapDef *o = cat_def_of(s, i);
        if (o != NULL && strcmp(o->name, def->name) == 0) return CXF_ERR_RANGE;
    }
    if (s->cat_count >= CAP_MAX) return CXF_ERR_FULL;

    h = cxf_table_add(&s->defs);
    if (cxf_handle_null(h)) return CXF_ERR_NOMEM;
    row = cxf_table_row(&s->defs, h);
    ((const CapDef **)cxf_table_col(&s->defs, DEF_COL_DEF))[row] = def;
    ((uint32_t *)cxf_table_col(&s->defs, DEF_COL_ID))[row] = s->cat_count;
    s->def_of[s->cat_count] = h;
    s->cat_count++;
    return CXF_OK;
}

static cxf_result iface_add(AgentStore *s, const IfaceDef *def) {
    cxf_handle h;
    uint32_t row;

    if (def->name == NULL || def->desc == NULL || def->name[0] == '\0')
        return CXF_ERR_RANGE;
    if (def->kind != IFACE_EXT && def->kind != IFACE_INT) return CXF_ERR_RANGE;
    for (uint32_t i = 0; i < s->iface_count; i++) {
        const IfaceDef *o = iface_def_of(s, i);
        if (o != NULL && strcmp(o->name, def->name) == 0) return CXF_ERR_RANGE;
    }
    if (s->iface_count >= IFACE_MAX) return CXF_ERR_FULL;

    h = cxf_table_add(&s->ifaces);
    if (cxf_handle_null(h)) return CXF_ERR_NOMEM;
    row = cxf_table_row(&s->ifaces, h);
    ((const IfaceDef **)cxf_table_col(&s->ifaces, IFACE_COL_DEF))[row] = def;
    ((uint32_t *)cxf_table_col(&s->ifaces, IFACE_COL_ID))[row] = s->iface_count;
    s->iface_of[s->iface_count] = h;
    s->iface_count++;
    return CXF_OK;
}

static cxf_result impl_add(AgentStore *s, const CapImpl *impl) {
    cxf_handle h;
    uint32_t row;

    if (impl->capability == NULL || impl->impl == NULL || impl->exec == NULL)
        return CXF_ERR_RANGE;
    if (s->cap_count >= CAP_MAX) return CXF_ERR_FULL;

    h = cxf_table_add(&s->caps);
    if (cxf_handle_null(h)) return CXF_ERR_NOMEM;
    row = cxf_table_row(&s->caps, h);
    ((const CapImpl **)cxf_table_col(&s->caps, IMPL_COL_API))[row] = impl;
    ((uint32_t *)cxf_table_col(&s->caps, IMPL_COL_ID))[row] = s->cap_count;
    s->cap_of[s->cap_count] = h;
    s->cap_count++;
    return CXF_OK;
}

static bool impl_ok(AgentStore *s, uint32_t cap_id, uint8_t trust_max) {
    const CapImpl *im = cap_impl_of(s, cap_id);
    return im != NULL && im->trust <= trust_max;
}

/*
 * 在某能力的降级链上挑第一个**够得着**的实现（从链头起）。
 * CAP_NONE 表示该能力在当前天花板下一个都用不了。
 * 这是分解期唯一需要知道的运行时事实 —— 也因此它是分解而不是派发的职责。
 */
static uint32_t chain_pick(AgentStore *s, uint32_t cat_id, uint8_t trust_max) {
    if (cat_id == CAP_NONE) return CAP_NONE;

    for (uint32_t id = s->head_of[cat_id]; id != CAP_NONE; id = s->cap_fb[id]) {
        if (impl_ok(s, id, trust_max)) return id;
        s->stat_denied++;
    }
    return CAP_NONE;
}

/*
 * 从当前位置往前走，找下一个够得着的实现。
 * 链在注册期已验证无环且单头，所以走一步少一步 —— 不需要重试计数兜底。
 */
static bool chain_advance(AgentStore *s, uint32_t *cap_id, uint8_t trust_max) {
    if (*cap_id == CAP_NONE) return false;

    for (uint32_t id = s->cap_fb[*cap_id]; id != CAP_NONE; id = s->cap_fb[id]) {
        if (impl_ok(s, id, trust_max)) { *cap_id = id; return true; }
        s->stat_denied++;
    }
    return false;
}

/*
 * 目录重建：把刚收进来的契约与实现接成一张可用的表。
 *
 * 全部是注册期不变式，违反就注册失败（响亮失败，不留半张表）：
 *   1. 每个实现必须能对上契约 —— 有实现没契约是注册 bug，不是"能力缺失"；
 *   2. 同一能力内实现名唯一；
 *   3. 降级链只能指向同能力的实现（跨能力"降级"是另一件事：通用兜底能力）；
 *   4. 每个能力的降级链必须**恰好一个链头** —— 0 个表示有环，>1 个表示链断成
 *      两支。这一条同时把"成环"从运行时的担心变成注册期的编译式错误，
 *      于是 WorkItem.tries 从"兜底"退化为"观测"；
 *   5. 一个能力名不得是另一个的点前缀（"travel" 与 "travel.train" 并存会让
 *      命名空间树的分叉语义含糊）；
 *   6. 能力的每个入/出端口引用的类型名必须**已注册** —— 写错立刻报错，
 *      不会静默长出一个"没人产出的新类型"；
 *   7. 每个 IFACE_INT 类型必须至少被一个能力产出（否则是**悬空入口**：
 *      有人要它，没人给得出）；
 *   8. 每个 IFACE_EXT 类型必须至少被一个能力消费（否则是**死的入口**）；
 *      并且能力不得产出 EXT 类型（EXT 的定义就是"无人产出"）。
 *
 * 6~8 合起来就是"类型图与能力图互为对偶"的机械保证。它们把三类最难查的错
 * （拼错类型名 / 缺能力 / 接不上）从"分解期搜不出路径"提前到**注册期报错** ——
 * 和"降级链成环"是同一招：把运行时的静默失败变成注册期的响亮失败。
 *
 * 复杂度 O(n²)，n ≤ CAP_MAX，且只在注册期跑。热路径的代价是零。
 */
static cxf_result catalog_rebuild(AgentStore *s) {
    for (uint32_t id = 0; id < s->cap_count; id++) {
        const CapImpl *im = cap_impl_of(s, id);
        uint32_t cat = agent_cat_id(s, im->capability);

        if (cat == CAP_NONE) return CXF_ERR_DEP;
        s->impl_cat[id] = cat;
        for (uint32_t j = 0; j < id; j++) {
            const CapImpl *oj = cap_impl_of(s, j);
            if (s->impl_cat[j] == cat && strcmp(oj->impl, im->impl) == 0)
                return CXF_ERR_RANGE;
        }
    }

    for (uint32_t id = 0; id < s->cap_count; id++) {
        const CapImpl *im = cap_impl_of(s, id);
        uint32_t fb;

        if (im->fallback == NULL) { s->cap_fb[id] = CAP_NONE; continue; }
        fb = cap_id_of_impl(s, im->fallback);
        if (fb == CAP_NONE) return CXF_ERR_DEP;
        if (s->impl_cat[fb] != s->impl_cat[id]) return CXF_ERR_RANGE;
        s->cap_fb[id] = fb;
    }

    for (uint32_t cat = 0; cat < s->cat_count; cat++) {
        uint32_t head = CAP_NONE, heads = 0;

        for (uint32_t id = 0; id < s->cap_count; id++) {
            bool pointed = false;

            if (s->impl_cat[id] != cat) continue;
            for (uint32_t j = 0; j < s->cap_count; j++)
                if (s->impl_cat[j] == cat && s->cap_fb[j] == id) { pointed = true; break; }
            if (!pointed) { head = id; heads++; }
        }
        if (heads != 1) return CXF_ERR_RANGE;
        s->head_of[cat] = head;
    }

    for (uint32_t i = 0; i < s->cat_count; i++)
        for (uint32_t j = 0; j < s->cat_count; j++) {
            const char *a = cat_def_of(s, i)->name;
            const char *b = cat_def_of(s, j)->name;
            uint32_t n = (uint32_t)strlen(a);

            if (i == j) continue;
            if (strncmp(a, b, n) == 0 && b[n] == '.') return CXF_ERR_RANGE;
        }

    /*
     * ---- 接口类型：解析端口、建正反邻接、验对偶不变式（6~8）----
     *
     * 建在 **cat 空间**上（契约），不建在 cap 空间（实现）：端口是契约说的，
     * 一个能力有几个实现与它"接得上谁"无关。建在实现上会让图随实现数膨胀，
     * 而这些边本该在能力集不变时一动不动。
     *
     * 每次都全量重建：新注册的能力可能正好补上了某个旧能力的输入来源，
     * 而"来源齐不齐"是整体性质，不能增量判定。
     */
    for (uint32_t t = 0; t < IFACE_MAX; t++) {
        s->prod_head[t] = CAP_NONE;
        s->cons_head[t] = CAP_NONE;
    }
    for (uint32_t nd = 0; nd < CAT_NODES; nd++) {
        s->prod_next[nd] = CAP_NONE;
        s->cons_next[nd] = CAP_NONE;
    }
    for (uint32_t cat = 0; cat < CAP_MAX; cat++) {
        s->cat_out[cat] = IFACE_NONE;
        for (uint32_t k = 0; k < CAP_IN_MAX; k++) s->cat_in[cat][k] = IFACE_NONE;
    }

    for (uint32_t cat = 0; cat < s->cat_count; cat++) {
        const CapDef *d = cat_def_of(s, cat);

        if (d == NULL) return CXF_ERR_DEP;

        if (d->out_iface != NULL) {
            uint32_t t = agent_iface_id(s, d->out_iface);

            if (t == IFACE_NONE) return CXF_ERR_DEP;
            if (iface_def_of(s, t)->kind != IFACE_INT)
                return CXF_ERR_RANGE;   /* EXT 的定义就是"无人产出" */
            s->cat_out[cat] = t;
        }

        for (uint32_t k = 0; k < CAP_IN_MAX && d->in_iface[k] != NULL; k++) {
            uint32_t t = agent_iface_id(s, d->in_iface[k]);

            if (t == IFACE_NONE) return CXF_ERR_DEP;
            s->cat_in[cat][k] = t;
        }
    }

    /*
     * 两类链都按 cat_id **升序**压栈 → 链头是最小的 cat_id，且同一能力的多个
     * 端口按 k 升序相邻。倒序插入才得到升序链，故两个循环都从高往低走。
     * 顺序确定 = 可复现：同样的能力集，分解期看到的候选序列永远一样。
     */
    for (uint32_t cat = s->cat_count; cat-- > 0;) {
        uint32_t t = s->cat_out[cat];

        if (t != IFACE_NONE) {
            uint32_t nd = CAT_NODE_OUT(cat);

            s->prod_next[nd] = s->prod_head[t];
            s->prod_head[t] = nd;
        }

        for (uint32_t k = CAP_IN_MAX; k-- > 0;) {
            uint32_t ti = s->cat_in[cat][k];
            uint32_t nd;

            if (ti == IFACE_NONE) continue;
            nd = CAT_NODE_IN(cat, k);
            s->cons_next[nd] = s->cons_head[ti];
            s->cons_head[ti] = nd;
        }
    }

    for (uint32_t t = 0; t < s->iface_count; t++) {
        const IfaceDef *d = iface_def_of(s, t);
        uint32_t np = agent_iface_producers(s, t);
        uint32_t nc = agent_iface_consumers(s, t);

        if (d->kind == IFACE_INT && np == 0) return CXF_ERR_DEP;    /* 悬空入口 */
        if (d->kind == IFACE_EXT && nc == 0) return CXF_ERR_RANGE;  /* 死的入口 */
    }

    /*
     * 控制流固化成布尔表（§4.8）。
     *
     * RECYCLE 每回收一个终态条目就要回答一次"这是终态产出，还是下一轮的输入"。
     * 这个问题在注册期就有确定答案（它只取决于 out_iface），所以它不该在热路径
     * 上被重新推导 —— 更不该从 result 的字节里猜（那正是方案 B 的代价）。
     *
     * 于是热路径只剩 cap_acts[cap_id] 一次数组索引，与 cap_fb / head_of 同一手法。
     */
    s->iface_action = agent_iface_id(s, IFACENAME_ACTION);
    for (uint32_t id = 0; id < s->cap_count; id++) {
        uint32_t t = s->cat_out[s->impl_cat[id]];

        s->cap_acts[id] = (t != IFACE_NONE && t == s->iface_action);
    }

    return CXF_OK;
}

/*
 * 增量同步：从 scanned 游标往后扫服务表，收进契约与实现。
 *
 * 这是"注册不硬编码"的全部实现 —— 没有名单数组，没有枚举 id，加一个能力
 * 就是往 world 里注册服务。id 按发现顺序稠密分配，因此既有 id 永不改变，
 * 新能力只追加（这正是热插拔能用世代号缓存的前提）。
 *
 * catalog_rebuild 每次都全量重跑：新注册的实现可能正是某个旧实现的降级目标，
 * 顺序无关才是对的。n ≤ CAP_MAX 且只在注册期，代价可忽略。
 */
static cxf_result registry_sync(AgentStore *s) {
    cxf_world *w = s->world;
    uint32_t added = 0;
    cxf_result rc;

    while (s->scanned < w->service_count) {
        const cxf_service_def *svc = &w->services[s->scanned++];

        if (has_prefix(svc->name, CAPIMPL_PREFIX, CAPIMPL_PREFIX_SZ)) {
            if (svc->api == NULL) return CXF_ERR_DEP;
            rc = impl_add(s, (const CapImpl *)svc->api);
        } else if (has_prefix(svc->name, CAPDEF_PREFIX, CAPDEF_PREFIX_SZ)) {
            if (svc->api == NULL) return CXF_ERR_DEP;
            rc = cat_add(s, (const CapDef *)svc->api);
        } else if (has_prefix(svc->name, IFACE_PREFIX, IFACE_PREFIX_SZ)) {
            if (svc->api == NULL) return CXF_ERR_DEP;
            rc = iface_add(s, (const IfaceDef *)svc->api);
        } else {
            continue;   /* 别人的服务 */
        }

        if (rc != CXF_OK) return rc;
        added++;
    }

    rc = catalog_rebuild(s);
    if (rc != CXF_OK) return rc;

    /* 世代只在真的变了的时候前进 —— 它是缓存键，不是 tick 计数。 */
    if (added > 0) s->cap_gen++;
    return CXF_OK;
}

/* ---- blob 池（§3.1）：跨 tick 存活的变长数据，显式分配、消费即释放 ---- */

static void blob_note(AgentStore *s) {
    if (s->blob_bytes > s->blob_peak) s->blob_peak = s->blob_bytes;
}

static cxf_handle blob_alloc(AgentStore *s, uint32_t cap) {
    void *p = cxf_alloc_alloc(s->alloc, cap, CXF_MIN_ALIGN);
    cxf_handle h;
    uint32_t row;

    if (p == NULL) return cxf_handle_make(0, 0);

    h = cxf_table_add(&s->blobs);
    if (cxf_handle_null(h)) {
        cxf_alloc_free(s->alloc, p, cap);
        return h;
    }
    row = cxf_table_row(&s->blobs, h);
    ((void **)cxf_table_col(&s->blobs, BLOB_COL_PTR))[row] = p;
    ((uint32_t *)cxf_table_col(&s->blobs, BLOB_COL_LEN))[row] = 0;
    ((uint32_t *)cxf_table_col(&s->blobs, BLOB_COL_CAP))[row] = cap;
    s->blob_bytes += cap;
    blob_note(s);
    return h;
}

static void blob_free(AgentStore *s, cxf_handle h) {
    void **pp;
    uint32_t *capp;

    if (cxf_handle_null(h) || !cxf_table_live(&s->blobs, h)) return;

    pp = (void **)cxf_table_get(&s->blobs, h, BLOB_COL_PTR);
    capp = (uint32_t *)cxf_table_get(&s->blobs, h, BLOB_COL_CAP);

    if (pp != NULL && capp != NULL && *pp != NULL) {
        cxf_alloc_free(s->alloc, *pp, *capp);
        s->blob_bytes -= *capp;
    }
    cxf_table_remove(&s->blobs, h);
}

static char *blob_data(AgentStore *s, cxf_handle h, uint32_t *out_len) {
    void **pp = (void **)cxf_table_get(&s->blobs, h, BLOB_COL_PTR);
    uint32_t *lenp = (uint32_t *)cxf_table_get(&s->blobs, h, BLOB_COL_LEN);

    if (pp == NULL || lenp == NULL) return NULL;
    if (out_len != NULL) *out_len = *lenp;
    return (char *)*pp;
}

/*
 * 扩到至少 need 字节。翻倍增长，内容保留。
 * 与 arena 的关系：arena 帧整帧回收、arena_state 只增不减，都不匹配 blob
 * 的生命周期（内核审核的结论）。所以 blob 自己管容量，且容量账目显式可见。
 */
static cxf_result blob_reserve(AgentStore *s, cxf_handle h, uint32_t need) {
    void **pp = (void **)cxf_table_get(&s->blobs, h, BLOB_COL_PTR);
    uint32_t *lenp = (uint32_t *)cxf_table_get(&s->blobs, h, BLOB_COL_LEN);
    uint32_t *capp = (uint32_t *)cxf_table_get(&s->blobs, h, BLOB_COL_CAP);

    uint32_t ncap;
    void *np;

    if (pp == NULL || lenp == NULL || capp == NULL) return CXF_ERR_BAD_HANDLE;
    if (need <= *capp) return CXF_OK;

    ncap = *capp != 0 ? *capp : 1u;
    while (ncap < need) ncap *= 2u;

    np = cxf_alloc_alloc(s->alloc, ncap, CXF_MIN_ALIGN);
    if (np == NULL) return CXF_ERR_NOMEM;
    if (*pp != NULL) {
        memcpy(np, *pp, *lenp);
        s->bytes_grow += *lenp;   /* 这次搬运纯粹因为容量估不准，与数据流无关 */
    }
    if (*pp != NULL) cxf_alloc_free(s->alloc, *pp, *capp);

    *pp = np;
    s->blob_bytes += (uint64_t)(ncap - *capp);
    *capp = ncap;
    s->stat_grow++;
    blob_note(s);
    return CXF_OK;
}

static cxf_result blob_append(AgentStore *s, cxf_handle h,
                              const char *src, uint32_t n) {
    void **pp = (void **)cxf_table_get(&s->blobs, h, BLOB_COL_PTR);
    uint32_t *lenp = (uint32_t *)cxf_table_get(&s->blobs, h, BLOB_COL_LEN);
    uint32_t *capp = (uint32_t *)cxf_table_get(&s->blobs, h, BLOB_COL_CAP);

    if (pp == NULL || lenp == NULL || capp == NULL) return CXF_ERR_BAD_HANDLE;
    if (*lenp + n > *capp) return CXF_ERR_FULL;

    if (n > 0) memcpy((char *)*pp + *lenp, src, n);
    *lenp += n;
    return CXF_OK;
}

/* 追加时按需扩容。返回值必须检查 —— 吞掉它等于吞掉一次数据丢失。 */
static cxf_result blob_append_grow(AgentStore *s, cxf_handle h,
                                   const char *src, uint32_t n) {
    uint32_t *lenp = (uint32_t *)cxf_table_get(&s->blobs, h, BLOB_COL_LEN);
    cxf_result rc;

    if (lenp == NULL) return CXF_ERR_BAD_HANDLE;
    rc = blob_reserve(s, h, *lenp + n);
    if (rc != CXF_OK) return rc;
    return blob_append(s, h, src, n);
}

static cxf_handle blob_from(AgentStore *s, const char *src, uint32_t n) {
    uint32_t cap = AGENT_ARGS_MIN;
    cxf_handle h;

    while (cap < n) cap *= 2u;

    h = blob_alloc(s, cap);
    if (cxf_handle_null(h)) return h;
    if (blob_append(s, h, src, n) != CXF_OK) {
        blob_free(s, h);
        return cxf_handle_make(0, 0);
    }
    return h;
}

/* ---- WorkItem 的列访问 ---- */

static uint32_t *item_u32(AgentStore *s, uint32_t leaf) {
    return (uint32_t *)cxf_soa_col(&s->items, leaf);
}

static uint8_t *item_u8(AgentStore *s, uint32_t leaf) {
    return (uint8_t *)cxf_soa_col(&s->items, leaf);
}

static cxf_handle item_handle(AgentStore *s, uint32_t row, uint32_t li, uint32_t lg) {
    uint32_t *ci = item_u32(s, li);
    uint32_t *cg = item_u32(s, lg);
    return cxf_handle_make(ci[row], cg[row]);
}

static void item_set_handle(AgentStore *s, uint32_t row,
                            uint32_t li, uint32_t lg, cxf_handle h) {
    item_u32(s, li)[row] = h.index;
    item_u32(s, lg)[row] = h.gen;
}

/*
 * 判失败的唯一写入口。
 *
 * 为什么不是 `status[r] = WI_FAILED; s->stat_failed++;` 两行照抄六处 ——
 * 因为 `err` 加进来之后，"三条一起变"就成了不变式，而**纪律挡不住第六处**：
 * 本仓库已经栽过一次（`stat_cost` 加一列时有两处口径没跟上，见 A3）。
 * 少写 `err[r]` 的那一处不会崩，它会印出一个**看起来合理**的原因（0 = OK），
 * 也就是"低报"的第四种形态。
 *
 * 这个函数的形参只要 `rc`：`status` / `err` / `stat_failed` 三者同源
 * （同一件事的三个投影），分开传就又能漂移。
 */
static void item_fail(AgentStore *s, uint32_t r, cxf_result rc) {
    item_u8(s, s->leaf.status)[r] = WI_FAILED;
    item_u8(s, s->leaf.err)[r] = (uint8_t)rc;
    s->stat_failed++;
}

/* ---- L0 目录渲染（§7.1）---- */

typedef struct Out {
    char    *buf;
    uint32_t cap;
    uint32_t len;
} Out;

static void out_mem(Out *o, const char *src, uint32_t n) {
    if (o->buf != NULL && o->len < o->cap) {
        uint32_t room = o->cap - o->len;
        uint32_t k = n < room ? n : room;
        if (k > 0) memcpy(o->buf + o->len, src, k);
    }
    o->len += n;
}

static void out_str(Out *o, const char *src) {
    out_mem(o, src, (uint32_t)strlen(src));
}

static void out_ch(Out *o, char c) {
    out_mem(o, &c, 1u);
}

static void out_u32(Out *o, uint32_t v) {
    char t[12];
    int n = snprintf(t, sizeof(t), "%u", v);
    if (n > 0) out_mem(o, t, (uint32_t)n);
}

/*
 * 描述消毒：换行、制表、其它控制字符 → 空格，连续空白压成一个。
 *
 * 不是为了让输出好看。契约描述可能由 LLM 写，它是**数据不是指令**（§7.1）：
 * 一个能带进换行的描述就等于能凭空伪造出一整行假的目录项，进而污染规划。
 * "一行一条"是渲染的结构不变式；压缩空白再挡住"用空格伪造对齐与徽标"。
 */
static void desc_safe(char *dst, uint32_t cap, const char *src) {
    uint32_t n = 0;
    bool prev_blank = false;

    if (cap == 0) return;
    for (const char *p = src; *p != '\0' && n + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        bool blank = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                      c < 0x20 || c == 0x7f);

        if (blank) {
            if (!prev_blank) { dst[n++] = ' '; prev_blank = true; }
            continue;
        }
        dst[n++] = (char)c;
        prev_blank = false;
    }
    if (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}

/*
 * 徽标：够得着的实现按链序排开的档位，加上其中最高的信任级。
 * 只统计够得着的 —— 天花板上看不见的东西不该出现在徽标里，
 * 否则模型会以为它有一个其实用不到的实现。
 */
static bool cat_tag(AgentStore *s, uint32_t cat_id, uint8_t trust_max,
                    char *tag, uint32_t cap) {
    uint32_t n = 0;
    uint8_t top = CAP_TRUST_LOCAL;
    bool first = true, any = false;

    for (uint32_t id = s->head_of[cat_id]; id != CAP_NONE; id = s->cap_fb[id]) {
        const CapImpl *im = cap_impl_of(s, id);
        const char *t;
        uint32_t tn;

        if (im == NULL || im->trust > trust_max) continue;

        any = true;
        if (im->trust > top) top = im->trust;
        if (!first && n + 1 < cap) tag[n++] = '|';
        first = false;

        t = im->tier == CAP_TIER_LLM ? "llm" : "script";
        tn = (uint32_t)strlen(t);
        for (uint32_t k = 0; k < tn && n + 1 < cap; k++) tag[n++] = t[k];
    }

    if (any && top != CAP_TRUST_LOCAL) {
        const char *m = top == CAP_TRUST_WRITE ? ",write" : ",io";
        for (const char *p = m; *p != '\0' && n + 1 < cap; p++) tag[n++] = *p;
    }
    tag[n] = '\0';
    return any;
}

typedef struct CatView {
    const char *name;
    char        tag[24];
    char        desc[DESC_SAFE_MAX];
} CatView;

/*
 * 按名字排序后的视图递归打印。
 *
 * 排序的理由：目录是给人（和模型）看的，顺序必须稳定且与注册顺序无关 ——
 * 否则同一份目录在不同装配顺序下渲染出不同文本，前缀缓存全废（§7.1）。
 * 递归的理由：命名空间树的分叉是**名字的性质**，所以直接从名字切出来，
 * 不需要另建树结构 —— 树是渲染出来的，不是存起来的一份。
 */
static void render_level(const CatView *v, const uint32_t *idx,
                         uint32_t lo, uint32_t hi, uint32_t seg, uint32_t depth,
                         uint32_t leaf_w, uint32_t tag_w, Out *o) {
    uint32_t i = lo;

    while (i < hi) {
        const char *g, *nx;
        uint32_t gn, nxn, j;
        bool leaf;

        if (!seg_span(v[idx[i]].name, seg, &g, &gn)) { i++; continue; }

        j = i + 1;
        while (j < hi) {
            const char *h;
            uint32_t hn;
            if (!seg_span(v[idx[j]].name, seg, &h, &hn)) break;
            if (hn != gn || memcmp(g, h, gn) != 0) break;
            j++;
        }

        leaf = (j - i == 1) && !seg_span(v[idx[i]].name, seg + 1u, &nx, &nxn);

        for (uint32_t d = 0; d < depth; d++) out_str(o, "  ");
        out_mem(o, g, gn);

        if (leaf) {
            uint32_t tn = (uint32_t)strlen(v[idx[i]].tag);
            uint32_t pad = gn < leaf_w ? leaf_w - gn : 1u;

            for (uint32_t k = 0; k < pad; k++) out_ch(o, ' ');
            out_str(o, " [");
            out_str(o, v[idx[i]].tag);
            out_str(o, "]");
            for (uint32_t k = tn; k < tag_w; k++) out_ch(o, ' ');
            out_str(o, "  ");
            out_str(o, v[idx[i]].desc);
            out_ch(o, '\n');
        } else {
            out_ch(o, '\n');
            render_level(v, idx, i, j, seg + 1u, depth + 1u, leaf_w, tag_w, o);
        }
        i = j;
    }
}

/*
 * 从 registry 渲染 L0 目录。返回**需要**的字节数（snprintf 语义）。
 * 只读 registry，不缓存渲染结果：渲染是冷路径（秒级、每任务一次），
 * 而缓存就该是调用方的事（§7.1 说这个渲染天然是提示词前缀缓存键）。
 */
static uint32_t catalog_render(AgentStore *s, char *buf, uint32_t cap, uint8_t trust_max) {
    CatView  v[CAP_MAX];
    uint32_t idx[CAP_MAX];
    uint32_t kept = 0, leaf_w = 8, tag_w = 0;
    Out o;

    o.buf = buf;
    o.cap = cap;
    o.len = 0;

    for (uint32_t c = 0; c < s->cat_count && kept < CAP_MAX; c++) {
        const CapDef *d = cat_def_of(s, c);
        char tag[24];

        if (d == NULL) continue;
        if (!cat_tag(s, c, trust_max, tag, sizeof(tag))) continue;  /* 够不着：不列 */

        v[kept].name = d->name;
        memcpy(v[kept].tag, tag, sizeof(tag));
        desc_safe(v[kept].desc, sizeof(v[kept].desc), d->desc);
        kept++;
    }

    for (uint32_t i = 0; i < kept; i++) idx[i] = i;
    for (uint32_t i = 1; i < kept; i++) {
        uint32_t k = idx[i], j = i;
        while (j > 0 && strcmp(v[idx[j - 1u]].name, v[k].name) > 0) {
            idx[j] = idx[j - 1u];
            j--;
        }
        idx[j] = k;
    }

    for (uint32_t i = 0; i < kept; i++) {
        const char *p;
        uint32_t n, tn;
        last_seg(v[i].name, &p, &n);
        if (n > leaf_w) leaf_w = n;
        tn = (uint32_t)strlen(v[i].tag);
        if (tn > tag_w) tag_w = tn;
    }
    if (leaf_w > 24u) leaf_w = 24u;

    out_str(&o, "# catalog gen ");
    out_u32(&o, s->cap_gen);
    out_str(&o, "  caps ");
    out_u32(&o, kept);
    out_ch(&o, '/');
    out_u32(&o, s->cat_count);
    out_str(&o, "  impls ");
    out_u32(&o, s->cap_count);
    out_ch(&o, '\n');

    render_level(v, idx, 0u, kept, 0u, 0u, leaf_w, tag_w, &o);

    if (o.buf != NULL && o.cap > 0)
        o.buf[o.len < o.cap ? o.len : o.cap - 1u] = '\0';
    return o.len;
}

/*
 * 同一份目录的协议形状（§4.16）。与 catalog_render 共享**判据**、不共享**形状**：
 * 过滤（cat_tag）、天花板（trust_max）、排序（按名字，与注册顺序无关）三条必须一致，
 * 否则会出现"目录里看得见、清单里没有"这种最难查的错位；而输出形状必须分开，
 * 因为一个要按行读、一个要按 JSON 解析。
 *
 * 排序在这里做实，因为**顺序就是字节顺序**：同一份能力集在不同装配顺序下渲染出
 * 不同字节 ⇒ tools 前缀不稳定 ⇒ 前缀缓存与"重跑幂等"一起失效（§4.10）。
 * render_tools 自己不排序，那是它的边界，不是它的遗漏。
 */
static uint32_t catalog_tools(AgentStore *s, char *buf, uint32_t cap, uint8_t trust_max) {
    AgentToolSpec sp[CAP_MAX];
    AgentToolSpec sorted[CAP_MAX];
    uint32_t      idx[CAP_MAX];
    uint32_t      kept = 0;

    for (uint32_t c = 0; c < s->cat_count && kept < CAP_MAX; c++) {
        const CapDef *d = cat_def_of(s, c);
        char tag[24];

        if (d == NULL) continue;
        if (!cat_tag(s, c, trust_max, tag, sizeof(tag))) continue;   /* 够不着：不给模型看 */

        sp[kept].name       = d->name;
        sp[kept].desc       = d->desc;
        sp[kept].in_schema  = d->in_schema;
        sp[kept].out_schema = d->out_schema;
        kept++;
    }

    for (uint32_t i = 0; i < kept; i++) idx[i] = i;
    for (uint32_t i = 1; i < kept; i++) {
        uint32_t k = idx[i], j = i;
        while (j > 0 && strcmp(sp[idx[j - 1u]].name, sp[k].name) > 0) {
            idx[j] = idx[j - 1u];
            j--;
        }
        idx[j] = k;
    }
    for (uint32_t i = 0; i < kept; i++) sorted[i] = sp[idx[i]];

    return agent_llm_render_tools(sorted, kept, buf, cap);
}

/* ---- agent_store ---- */

static uint32_t leaf_of(const cxf_layout *l, const char *path) {
    const cxf_leaf *lf = cxf_layout_find(l, path);
    return lf != NULL ? lf->col : CXF_NO_INDEX;
}

static cxf_result store_init(cxf_world *w, void *st) {
    static const size_t blob_cols[] = {
        sizeof(void *), sizeof(uint32_t), sizeof(uint32_t)
    };
    static const size_t impl_cols[] = {
        sizeof(const CapImpl *), sizeof(uint32_t)
    };
    static const size_t def_cols[] = {
        sizeof(const CapDef *), sizeof(uint32_t)
    };
    static const size_t iface_cols[] = {
        sizeof(const IfaceDef *), sizeof(uint32_t)
    };

    AgentStore *s = (AgentStore *)st;
    cxf_alloc *a = w->alloc;
    cxf_result rc;

    s->alloc = a;
    s->world = w;

    if (cxf_layout_build(&s->layout, &cxf_type_WorkItem, a) != CXF_OK)
        return CXF_ERR_NOMEM;
    if (cxf_soa_init_class(&s->items, &s->layout, a, 16, AGENT_CLASS_ITEM) != CXF_OK)
        return CXF_ERR_NOMEM;
    if (cxf_table_init_class(&s->blobs, a, blob_cols, 3, 16, AGENT_CLASS_BLOB) != CXF_OK)
        return CXF_ERR_NOMEM;
    if (cxf_table_init_class(&s->caps, a, impl_cols, 2, 8, AGENT_CLASS_IMPL) != CXF_OK)
        return CXF_ERR_NOMEM;
    if (cxf_table_init_class(&s->defs, a, def_cols, 2, 8, AGENT_CLASS_DEF) != CXF_OK)
        return CXF_ERR_NOMEM;
    if (cxf_table_init_class(&s->ifaces, a, iface_cols, 2, 8, AGENT_CLASS_IFACE) != CXF_OK)
        return CXF_ERR_NOMEM;
    if (cxf_ring_init(&s->inbox, a, AGENT_INBOX_CAP) != CXF_OK)
        return CXF_ERR_NOMEM;

    /* 展平列号：装配期查一次，tick 里直接用 */
    s->leaf.cap_id    = leaf_of(&s->layout, "cap_id");
    s->leaf.status    = leaf_of(&s->layout, "status");
    s->leaf.pending   = leaf_of(&s->layout, "pending");
    s->leaf.trust_max = leaf_of(&s->layout, "trust_max");
    s->leaf.tries     = leaf_of(&s->layout, "tries");
    s->leaf.rounds    = leaf_of(&s->layout, "rounds");
    s->leaf.seq       = leaf_of(&s->layout, "seq");
    s->leaf.err       = leaf_of(&s->layout, "err");
    s->leaf.args_i    = leaf_of(&s->layout, "args.index");
    s->leaf.args_g    = leaf_of(&s->layout, "args.gen");
    s->leaf.result_i  = leaf_of(&s->layout, "result.index");
    s->leaf.result_g  = leaf_of(&s->layout, "result.gen");
    s->leaf.parent_i  = leaf_of(&s->layout, "parent.index");
    s->leaf.parent_g  = leaf_of(&s->layout, "parent.gen");
    if (s->leaf.cap_id == CXF_NO_INDEX || s->leaf.status == CXF_NO_INDEX ||
        s->leaf.pending == CXF_NO_INDEX || s->leaf.trust_max == CXF_NO_INDEX ||
        s->leaf.tries == CXF_NO_INDEX || s->leaf.rounds == CXF_NO_INDEX ||
        s->leaf.seq == CXF_NO_INDEX || s->leaf.err == CXF_NO_INDEX ||
        s->leaf.args_i == CXF_NO_INDEX ||
        s->leaf.result_i == CXF_NO_INDEX || s->leaf.parent_i == CXF_NO_INDEX)
        return CXF_ERR_DEP;

    s->act_n = 0;
    s->iface_action = IFACE_NONE;
    memset(s->cap_acts, 0, sizeof(s->cap_acts));

    /* 观测层（§10.7）：终值尚未产生、水位快照归零。
       final_h 显式置空而不是靠模块状态的零初始化 —— "0 号槽位恰好等于空"
       是一个实现细节，不该被两个地方的初始化共同依赖。 */
    s->final_h = cxf_handle_make(0, 0);
    s->stall_ticks = 0;
    s->stall_max = 0;
    s->stat_stalled = 0;
    s->stall_latched = false;
    s->obs_items = 0;
    s->obs_done = 0;
    s->obs_final = 0;
    s->stat_audit_bad = 0;
    s->stat_resumed = 0;
    s->stat_capped = 0;
    s->stat_quota_wait = 0;
    s->stat_fanout_kids = 0;
    /* 配额默认不限：不声明的部署（单机实验、无限额的端点）一字节代价都不付 */
    s->quota.conc_max = 0u;
    s->quota.rpm = 0u;
    s->quota.tok_milli = 0u;
    s->quota.last_ns = 0u;
    s->clock = NULL;
    s->args_budget = AGENT_ARGS_MAX;   /* 装配预算的默认值；§4.16 末 */
    s->max_rounds  = AGENT_MAX_ROUNDS;
    s->bytes_lost = 0;

    rc = registry_sync(s);
    if (rc != CXF_OK) return rc;

    s->ingest.inbox = &s->inbox;
    s->registry.sync = registry_sync;
    s->registry.render = catalog_render;
    s->registry.tools = catalog_tools;

    rc = cxf_service_register(w, AGENT_INGEST_SVC, 1, &s->ingest);
    if (rc != CXF_OK) return rc;
    rc = cxf_service_register(w, AGENT_REGISTRY_SVC, 1, &s->registry);
    if (rc != CXF_OK) return rc;

    /* store 服务最后注册：前面几个服务都成了，外部才可能拿到 store。 */
    return cxf_service_register(w, AGENT_STORE_SVC, 1, s);
}

static void store_deinit(cxf_world *w, void *st) {
    AgentStore *s = (AgentStore *)st;

    (void)w;
    for (uint32_t r = s->blobs.count; r-- > 0;)
        blob_free(s, cxf_table_at(&s->blobs, r));

    cxf_ring_release(&s->inbox, s->alloc);
    cxf_table_release(&s->blobs);
    cxf_table_release(&s->caps);
    cxf_table_release(&s->defs);
    cxf_table_release(&s->ifaces);
    cxf_soa_release(&s->items);
    cxf_layout_release(&s->layout, s->alloc);
}

/* ---- INJECT ---- */

static cxf_handle add_item(AgentStore *s, const WorkItem *wi) {
    cxf_handle h = cxf_soa_add(&s->items);
    if (cxf_handle_null(h)) return h;
    cxf_soa_scatter(&s->items, h, wi);
    return h;
}

/*
 * 计划表：叶子**能力名** + 各自入参。汇聚节点恒为 plan.merge。
 * 计划里只出现能力名：cap_id / cat_id 是装配期分配结果，实现名是路由细节，
 * 三者依次是"契约 → 装配产物 → 运行时事实"，只有第一个该写进计划。
 *
 * fanin = false 是 §4.8 的第二种形状：**一轮一个条目，没有汇聚点**。
 * 回路里的叶子产出的是 agent.action，plan.merge 读不懂它 —— 硬塞一个汇聚
 * 节点会让 pending 永远等不到，整条回路挂在 BLOCKED 上（这是形状问题，
 * 不是参数问题，所以它必须是一张表里的一列，而不是运行时特判）。
 */
typedef struct PlanSpec {
    const char *leaves[AGENT_PLAN_LEAVES];
    const char *queries[AGENT_PLAN_LEAVES];
    uint32_t    count;
    bool        fanin;
} PlanSpec;

static const PlanSpec plan_specs[AGENT_PLAN_COUNT] = {
    { { CAPNAME_TRAIN, CAPNAME_SECONDHAND },
      { AGENT_Q_12306, AGENT_Q_XIANYU }, 2, true },
    { { CAPNAME_QUOTE, NULL },
      { AGENT_Q_QUOTE, NULL }, 1, true },
    { { CAPNAME_QUOTE, NULL },
      { AGENT_Q_CACHED, NULL }, 1, true },
    { { CAPNAME_LATE, NULL },
      { AGENT_Q_LATE, NULL }, 1, true },
    { { CAPNAME_STEP, NULL },
      { AGENT_Q_LOOP, NULL }, 1, false },
    { { CAPNAME_FS, NULL },
      { AGENT_Q_FS, NULL }, 1, false },
    { { CAPNAME_FS_WRITE, NULL },
      { AGENT_Q_WRITE, NULL }, 1, false },
    { { CAPNAME_FS_DIFF, NULL },
      { AGENT_Q_DIFF, NULL }, 1, false },
};

/*
 * 能力名 → cat_id。只在目录世代变化时做一次（tick 边界，一次性）。
 * 这就是 §4.5 说的"感知在 tick 边界注入"：能力集变化必须在下一个 tick
 * 开始前对决策可见，而不是在派发途中被发现。
 *
 * 把它做成"世代缓存"而不是"每 tick 重解"，是同一条纪律的两面：解析本身是
 * 装配期语义（按名字找东西），不该在稳态里每 tick 重演一遍。代价从
 * "每 tick O(能力数) 次字符串比较"降到"每 tick 一次整数比较"。
 * stat_resolve 让这件事可观测 —— 稳态下它必须是 1。
 */
static void inject_resolve(InjectState *m) {
    AgentStore *s = m->store;

    for (uint32_t p = 0; p < AGENT_PLAN_COUNT; p++)
        for (uint32_t k = 0; k < AGENT_PLAN_LEAVES; k++)
            m->cat_ids[p][k] = plan_specs[p].leaves[k] != NULL
                             ? agent_cat_id(s, plan_specs[p].leaves[k])
                             : CAP_NONE;

    m->cat_merge = agent_cat_id(s, CAPNAME_MERGE);
    m->cap_gen = s->cap_gen;
    m->resolved = true;
    s->stat_resolve++;
}

/*
 * 建一张计划图：N 个叶子并行 → 一个汇聚节点（fan-in 靠 pending）。
 * DAG 写在数据里（WorkItem.parent + pending），不在模块 state 里 ——
 * 白盒可查，且每个条目都可能是别的 rank 上执行的。
 *
 * 先备齐入参、再加行，任何一步失败就整体回滚：半张图比没有图更难查。
 * 能力名在天花板下解析不出实现时 cap_id = CAP_NONE，不在这里拦 ——
 * 交给 MATCH 判失败，失败路径只有一条，不搞两处特判。
 */
static void inject_plan(InjectState *m, uint32_t kind, uint8_t trust_max,
                        const char *text, uint32_t text_len) {
    AgentStore *s = m->store;
    const PlanSpec *p;
    cxf_handle args[AGENT_PLAN_LEAVES];
    cxf_handle leaves[AGENT_PLAN_LEAVES];
    cxf_handle agg_h, agg_args;
    uint32_t n;
    WorkItem wi;

    if (kind >= AGENT_PLAN_COUNT) return;   /* 未知计划：入口协议的边界，丢弃 */
    p = &plan_specs[kind];
    n = p->count;

    agg_h = cxf_handle_make(0u, 0u);
    agg_args = cxf_handle_make(0u, 0u);
    for (uint32_t k = 0; k < AGENT_PLAN_LEAVES; k++) {
        args[k] = cxf_handle_make(0u, 0u);
        leaves[k] = cxf_handle_make(0u, 0u);
    }

    for (uint32_t k = 0; k < n; k++) {
        /*
         * 载荷优先：入口给了文本就用它，没给才回落到计划里的固定查询词。
         *
         * 只有 k == 0 吃载荷，不是省事 —— 一个计划里多个叶子是**并行子任务**
         * （比如行程里两个都要出网的调研），把同一句话拷贝给两个叶子等于让它们
         * 做同一件事。外部调用方要并行子任务，就该发两条消息（`seq` 区分），
         * 那是它的表达力，不该由我们替它猜。
         */
        const char *q    = (k == 0u && text_len > 0u) ? text : p->queries[k];
        uint32_t    qlen = (k == 0u && text_len > 0u) ? text_len
                                                     : (uint32_t)strlen(p->queries[k]);

        args[k] = blob_from(s, q, qlen);
        if (cxf_handle_null(args[k])) goto rollback;
        s->bytes_in += qlen;
    }

    /* 分解是冷路径（每请求一次）：在这里把"够得着的实现"定下来，
       DAG 里因此不会出现越权的条目 —— 门控在分解期，不在派发期。 */
    memset(&wi, 0, sizeof(wi));

    /*
     * 单节点形状（§4.8）：回路的第一轮就是一个条目，没有汇聚点。
     * 它直接 READY —— 没有父节点要等，也就没有"被放行"这回事。
     */
    if (!p->fanin) {
        for (uint32_t k = 0; k < n; k++) {
            wi.cap_id    = chain_pick(s, m->cat_ids[kind][k], trust_max);
            wi.status    = WI_READY;
            wi.args      = args[k];
            wi.trust_max = trust_max;
            leaves[k] = add_item(s, &wi);
            if (cxf_handle_null(leaves[k])) goto rollback;
        }
        s->stat_injected += n;
        return;
    }

    for (uint32_t k = 0; k < n; k++) {
        wi.cap_id    = chain_pick(s, m->cat_ids[kind][k], trust_max);
        wi.status    = WI_BLOCKED;   /* 父节点就位前不参与匹配 */
        wi.args      = args[k];
        wi.trust_max = trust_max;
        leaves[k] = add_item(s, &wi);
        if (cxf_handle_null(leaves[k])) goto rollback;
    }

    agg_args = blob_alloc(s, AGENT_ARGS_MIN);
    if (cxf_handle_null(agg_args)) goto rollback;

    wi.cap_id    = chain_pick(s, m->cat_merge, trust_max);
    wi.status    = WI_BLOCKED;
    wi.args      = agg_args;
    wi.pending   = n;
    wi.trust_max = trust_max;
    agg_h = add_item(s, &wi);
    if (cxf_handle_null(agg_h)) goto rollback;

    /* 汇聚点就位：接线并放行叶子。BLOCKED → READY 只在这里发生 */
    for (uint32_t k = 0; k < n; k++) {
        uint32_t row = cxf_table_row(&s->items.table, leaves[k]);
        item_set_handle(s, row, s->leaf.parent_i, s->leaf.parent_g, agg_h);
        item_u8(s, s->leaf.status)[row] = WI_READY;
    }
    s->stat_injected += 1u + n;
    return;

rollback:
    /* 半张图比没有图难查：已建的行与 blob 全部退回，统计不动 */
    if (!cxf_handle_null(agg_h)) cxf_soa_remove(&s->items, agg_h);
    if (!cxf_handle_null(agg_args)) blob_free(s, agg_args);
    for (uint32_t k = 0; k < n; k++) {
        if (!cxf_handle_null(leaves[k])) cxf_soa_remove(&s->items, leaves[k]);
        if (!cxf_handle_null(args[k])) blob_free(s, args[k]);
    }
}

/*
 * 抽出来的存在性检查：**「有没有一个够得着的实现」与「取一个并记账」是两件事**。
 *
 * 为什么需要这么一条：一轮多路派生要做成**事务**（要么全派出去，要么一条都不派）。
 * 事务要求"先问全部、再动全部"，而 `chain_pick` 会改 `stat_denied` —— 先问一遍
 * 再取一遍就会把同一件事记两次账。低报的计数器坏，"记账口径对不上的计数器"更坏：
 * 它让 `denied` 与 `executed` 的对比失去意义。
 */
static bool chain_reachable(AgentStore *s, uint32_t cat_id, uint8_t trust_max) {
    if (cat_id == CAP_NONE) return false;

    for (uint32_t id = s->head_of[cat_id]; id != CAP_NONE; id = s->cap_fb[id])
        if (impl_ok(s, id, trust_max)) return true;
    return false;
}

/* ---- 多轮闭环：把上一 tick 的动作变成这一轮的条目（§4.8）---- */

static void append_final(AgentStore *s, const char *res, uint32_t n);

/*
 * 派生点了什么。**三态而不是 bool**，因为"派生"之后有两种截然不同的后续
 * （§4.16 末 / 缺口 ⑤）：
 *
 *   FAIL    没派成 —— 调用方按失败收口（能力名不存在 / 够不着 / 内存不够）。
 *   CHAIN   派生的工具**自己也会派生**（`cap_acts[cid]`）⇒ 它接管续跑，
 *           原控制流条目照旧回收。§4.8 第一刀就是这种形状：唯一的派生目标是
 *           `agent.step`，而它本身是控制流生产者，所以链一路向前、不需要汇合。
 *   RESUME  派生的工具是**终态**（不派生，比如 `fs.grep`）⇒ 原条目改成续跑
 *           节点 S'，等工具结果 append 进自己的 args 再问一次。
 *
 * 这一条判断读的是**注册期事实** `cap_acts[]`，不是从 payload 猜 ——
 * 与 RECYCLE 判"这一步会不会派生"用的是同一个 bool。同一个事实两处读，
 * 于是不可能出现"RECYCLE 认为它派生、INJECT 认为它不派生"这种错位。
 */
typedef enum DeriveOut {
    DERIVE_FAIL = 0,
    DERIVE_CHAIN,
    DERIVE_RESUME
} DeriveOut;

/*
 * 派生点。判定在这里、不在执行点 —— 这一条决定了 rounds 必须**沿 lineage
 * 继承**：只有产生者知道这条血脉已经走了几轮。放在执行点等于让能力知道
 * "自己在第几步"，而它本该不知道图（这正是方案 C 被否掉的理由）。
 *
 * `self_h` 是**正在解码的那个控制流条目**。它有两个用途，二者都不需要额外状态：
 *   - CHAIN：什么都不做（它会被回收）；
 *   - RESUME：成为派生条目的 parent ⇒ "谁派生的，结果就回到谁"。
 */
static DeriveOut inject_derive(InjectState *m, const AgentAct *a,
                              uint32_t rounds, uint8_t trust, cxf_handle self_h,
                              uint8_t seq) {
    AgentStore *s = m->store;
    uint32_t cat, cid;
    cxf_handle args;
    WorkItem wi;
    DeriveOut out;

    cat = agent_cat_id_n(s, a->cap, a->cap_len);
    if (cat == CAP_NONE) return DERIVE_FAIL;        /* 模型点名了一个不存在的能力 */

    cid = chain_pick(s, cat, trust);
    if (cid == CAP_NONE) return DERIVE_FAIL;        /* 存在，但当前天花板下够不着 */

    args = blob_from(s, a->args, a->args_len);
    if (cxf_handle_null(args)) return DERIVE_FAIL;
    s->bytes_copy += a->args_len;   /* 动作里的 args 也要落一次盘 */
    s->copy_ops++;

    out = (cid < s->cap_count && s->cap_acts[cid]) ? DERIVE_CHAIN : DERIVE_RESUME;

    memset(&wi, 0, sizeof(wi));
    wi.cap_id    = cid;
    wi.status    = WI_READY;   /* READY 与"有没有 parent"无关：MATCH 只看 status */
    wi.args      = args;
    wi.trust_max = trust;      /* 天花板随血脉走：派生不是提权的地方 */
    wi.rounds    = rounds;
    wi.seq       = seq;        /* 本轮第几条动作；由派生点在遍历里给 */

    /*
     * parent 与 rounds 是两条不同的关系，刻意用两个字段：
     *   parent = 同轮内的 fan-in 汇聚（数据流，决定父节点何时 READY）；
     *   rounds = 跨轮的 lineage（控制流，决定回路还能不能继续）。
     * 把 lineage 塞进 parent 会让回收逻辑去等一个**已经消失的父节点**，
     * 于是每一轮都走"父节点不在 ⇒ 直接进最终结果"那条路 —— 结果是回路
     * 每一步的半成品都被当成最终答案。
     *
     * 缺口 ⑤ 补的是同一句话的另一半：**终态工具**派出来的条目，其 parent
     * 就是发起调用的那个控制流条目 —— 这不是 lineage，是**同轮的数据流**
     * （工具输出是下一轮的输入），所以用 parent 是对的。
     */
    if (out == DERIVE_RESUME) wi.parent = self_h;

    if (cxf_handle_null(add_item(s, &wi))) {
        blob_free(s, args);
        return DERIVE_FAIL;
    }
    s->stat_injected++;
    return out;
}

/*
 * 把控制流条目**就地改成续跑节点 S'**（缺口 ⑤）。
 *
 * 为什么复用这一行而不是新加一行：`S'.args` 要装"原 args"（方案 b），
 * 而**原 args 就在这一行里** —— 复用之后"转移"这个动作根本不存在，
 * 于是也就不存在"谁 free、失败时怎么退回"这一整类所有权问题。
 * 新加一行反而要把 args 搬过去，那才是容易写错的地方。
 *
 * 重置五件事，各自防一个具体的错：
 *   status / pending  T 完成前不参与匹配；`pending` = 本轮真正挂上来的子项数
 *                     （fan-out 之后不是 1，见 §4.17.9）
 *   rounds            下一轮派生命中同一个计数器 —— 不增长则 AGENT_MAX_ROUNDS 失效
 *   result = null     动作载荷已 free，留着就是悬空引用（A2 会当场抓）
 *   parent = null     它仍是无父节点的一轮起点
 *   tries = 0         新一轮从零开始，上一轮的重试额度不继承
 */
static void act_resume(AgentStore *s, uint32_t row, uint32_t rounds_next,
                       uint32_t pending_next) {
    cxf_handle none = cxf_handle_make(0u, 0u);

    item_u8(s, s->leaf.status)[row]    = WI_BLOCKED;
    item_u32(s, s->leaf.pending)[row]  = pending_next;
    item_u32(s, s->leaf.rounds)[row]   = rounds_next;
    item_u8(s, s->leaf.tries)[row]     = 0u;
    item_set_handle(s, row, s->leaf.result_i, s->leaf.result_g, none);
    item_set_handle(s, row, s->leaf.parent_i, s->leaf.parent_g, none);
}

/*
 * 解上一 tick 挂下的动作。
 *
 * 发生在 INPUT phase，与"从 ring 取请求"同一处，于是**同一 tick 内**派生出的
 * 条目就能被 UPDATE 匹配、LATE 执行 —— 一轮的迭代单位就是 tick，不需要 while。
 * 这也正是"个体 agent 循环降格为 WorkItem 状态机"在代码上的样子：
 * 循环的推进就是 tick 的推进。
 */
/*
 * 一轮动作的**意图可满足性**：每条 CALL 点的能力都存在、且在当前天花板下够得着。
 *
 * 这是"一轮一次事务"的前置。不预检的后果不是"少派一条"这么轻：
 * `inject_derive` 会在中途失败，前面几条已经建出来了 —— 那就是**部分应用**，
 * 而模型看到的摘要里没有任何痕迹说明少了一条。
 *
 * 只查存在性，不记账（见 `chain_reachable` 的理由）：真正的取用与 `stat_denied`
 * 记账发生在 `inject_derive` 里，一次动作只记一次。
 */
static bool intent_ok(AgentStore *s, const char *p, uint32_t len, uint8_t trust) {
    uint32_t off = 0;

    while (off < len) {
        AgentAct a;

        if (!agent_act_decode(p + off, len - off, &a)) return false;
        if (a.kind == AGENT_ACT_CALL) {
            uint32_t cat = agent_cat_id_n(s, a.cap, a.cap_len);

            if (cat == CAP_NONE) return false;
            if (!chain_reachable(s, cat, trust)) return false;
        }
        off += agent_act_bytes(&a);
    }
    return true;
}

static void inject_actions(InjectState *m) {
    AgentStore *s = m->store;
    uint32_t n = s->act_n;

    s->act_n = 0;
    for (uint32_t i = 0; i < n; i++) {
        cxf_handle item_h = s->acts[i];
        cxf_handle res_h, args_h;
        uint32_t row, rounds, res_len = 0;
        uint8_t trust;
        bool resumed = false;
        char *p;

        if (!cxf_soa_live(&s->items, item_h)) continue;
        row = cxf_table_row(&s->items.table, item_h);

        /*
         * 每个条目都重取列指针。**不能**把它们缓存在循环外：inject_derive 会
         * add_item，SoA 扩容会让上一轮取到的列指针指向已释放的旧数组。
         * 交错"读一列 + 加一行"的地方，指针寿命就是一次调用。
         */
        rounds = item_u32(s, s->leaf.rounds)[row];
        trust  = item_u8(s, s->leaf.trust_max)[row];
        res_h  = item_handle(s, row, s->leaf.result_i, s->leaf.result_g);
        args_h = item_handle(s, row, s->leaf.args_i, s->leaf.args_g);
        p      = blob_data(s, res_h, &res_len);

        /*
         * 第一遍：**只读，不改**。把这一份产出解完再决定怎么做（§4.17.9）。
         *
         * 为什么要两遍而不是边解边派：一份动作现在可能是 N 条首尾相接，
         * 而"最后一条坏掉"要在**派出去任何一条之前**就知道 —— 否则就是部分应用。
         * 模型要了 3 件事、我们派了 2 件，第 3 件的输出永远不回上下文，而模型
         * 看到的摘要里没有任何痕迹说明少了一件。它会基于 2 件编一个结论。
         */
        {
            uint32_t    off = 0, ncall = 0;
            const char *done_args = NULL;
            uint32_t    done_len = 0;
            bool        bad = (res_len == 0u);

            while (!bad && off < res_len) {
                AgentAct a;

                if (!agent_act_decode(p + off, res_len - off, &a)) { bad = true; break; }

                if (a.kind == AGENT_ACT_DONE) {
                    /* DONE 必须**独占整份产出**：它后面还有字节，就是产出方
                       在自相矛盾（既说完了又派活）。不猜哪个优先。 */
                    if (off != 0u || agent_act_bytes(&a) != res_len) { bad = true; break; }
                    done_args = a.args;
                    done_len  = a.args_len;
                } else if (++ncall > AGENT_FANOUT_MAX) {
                    bad = true;        /* 有界：超限走失败路径，不截断 */
                }
                off += agent_act_bytes(&a);
            }
            if (!bad && off != res_len) bad = true;   /* 收尾不干净 */

            if (bad) {
                /* 产出不是合法动作：能力的错，但必须在这里收口，不能往下传 */
                s->stat_failed++;
                s->stat_batch_bad++;
            } else if (done_args != NULL) {
                /* 终值：回路的出口。payload 就是最终结果，不是动作 */
                append_final(s, done_args, done_len);
            } else if (rounds + 1u > s->max_rounds) {
                /* 硬上限。走失败路径而不是静默停住 —— 不收敛的回路若表现成
                   "任务卡住"，就没人知道它错了。
                   但**要单独计数**：轮数用尽与"动作非法 / 派不出去"处置相反
                   （前者调大上限或换实现，后者修协议），共用一个数就没法归因。 */
                s->stat_failed++;
                s->stat_capped++;
            } else if (!intent_ok(s, p, res_len, trust)) {
                /*
                 * 事务的第二步：**全部**子项都派得出去，才开始派。
                 *
                 * ★ 这两种失败必须分开记（§4.18.17 六）：`bad` 是**语法**坏了
                 * （模型连行格式都不守），`rejected` 是语法守住了但点名了
                 * 不存在 / 天花板下够不着的能力。前者的处置是"降级到文本单条
                 * 通道或减 N"，后者是"能力表或折叠的问题" —— 混在一个
                 * `stat_failed` 里，量出来的 p 就没法归因，也就没法选档位。
                 */
                s->stat_failed++;
                s->stat_batch_rejected++;
            } else {
                uint32_t off2 = 0, kids = 0, chain = 0, seq_i = 0;

                resumed = false;
                while (off2 < res_len) {
                    AgentAct a;
                    DeriveOut d;

                    if (!agent_act_decode(p + off2, res_len - off2, &a)) break;
                    /*
                     * seq 是**遍历序**（1 基），不是完成序 —— 它必须在派生点定下来：
                     * 执行是并发的（A5 之后更是），完成顺序不可预测，
                     * 而"这是模型写的第几条"只有这里知道。
                     */
                    d = inject_derive(m, &a, rounds + 1u, trust, item_h,
                                      (uint8_t)(seq_i + 1u));
                    seq_i++;
                    if (d == DERIVE_FAIL) {
                        s->stat_failed++;      /* 第二遍仍可能失败：blob 分配 */
                    } else if (d == DERIVE_RESUME) {
                        kids++;                /* 这一条会回到 S' */
                        resumed = true;
                    } else {
                        chain++;
                    }
                    off2 += agent_act_bytes(&a);
                }
                if (resumed) {
                    /*
                     * 缺口 ⑤ + fan-out：工具是**终态**（不派生）⇒ 这一行不回收，
                     * 改成它们的续跑节点 S'。`pending` 是**本轮真正挂上来的子项数**
                     * —— 只数 RESUME 的那些：CHAIN 的子项没有 parent（它们自己
                     * 接着往下派），把它们的数量算进来会让 S' 永远等不到。
                     *
                     * args 不 free：它还挂在 S' 上，正是续跑的上下文。
                     */
                    blob_free(s, res_h);
                    act_resume(s, row, rounds + 1u, kids);
                    s->stat_resumed++;
                }
                s->stat_fanout_kids += kids + chain;
                s->stat_batch_ok++;
                /*
                 * 分母：这一轮派了几条。只在**真正派出去**的分支记 ——
                 * 被 `intent_ok` / 轮数上限挡下的那几轮没有段，记进来会把
                 * "批大小分布"污染成"模型想说几条"的分布。
                 */
                s->stat_seg_hist[ncall > AGENT_FANOUT_MAX ? AGENT_FANOUT_MAX : ncall]++;
            }
        }

        if (resumed) continue;

        blob_free(s, res_h);
        blob_free(s, args_h);   /* EXECUTE 成功路径不归还入参，这里统一归还 */
        cxf_soa_remove(&s->items, item_h);
        s->stat_recycled++;
    }
}

static cxf_result inject_init(cxf_world *w, void *st) {
    InjectState *m = (InjectState *)st;
    m->store = (AgentStore *)cxf_service_find(w, AGENT_STORE_SVC, 1);
    return m->store != NULL ? CXF_OK : CXF_ERR_DEP;
}

/*
 * 取一帧入口消息：定长头 + 变长载荷。
 *
 * `text` 是调用方的落点（栈上或 store 里的静态缓冲），`cap` 是它的容量。
 * 返回 false = **还没有完整的一帧**（不是错误）；帧非法/超限时本函数自己消费掉整帧、
 * 计数、并返回 false 的"已处理"情形由 `*skip` 区分。
 *
 * 两个细节都不是风格问题：
 *   1. **两步 peek**。先 peek 头才知道载荷多长；只按头判"够不够"会把半帧当整帧读，
 *      于是这一帧的载荷变成下一帧的头 —— 入口从此错位，而且错得很安静。
 *   2. **超限也整帧消费**。只读头就拒，载荷会永久留在 ring 里；下一轮读到的"头"
 *      其实是那截载荷。宁可丢一帧并计数，也不能让字节流对不齐。
 */
static bool inject_read_frame(AgentStore *s, InjectMsg *msg,
                             char *text, uint32_t cap, bool *skip) {
    uint32_t avail, total, n;

    *skip = false;

    avail = cxf_ring_size(&s->inbox);
    if (avail < (uint32_t)sizeof(*msg)) return false;

    n = cxf_ring_peek(&s->inbox, msg, (uint32_t)sizeof(*msg));
    if (n < (uint32_t)sizeof(*msg)) return false;

    total = (uint32_t)sizeof(*msg) + msg->text_len;
    if (avail < total) return false;          /* 载荷未到齐：等下一个 tick */

    cxf_ring_read(&s->inbox, msg, (uint32_t)sizeof(*msg));

    if (msg->text_len > cap) {
        /* 超限：整帧吃掉（分块读，不假设 ring 一次给得完） */
        uint32_t left = msg->text_len;
        char     sink[64];

        while (left > 0u) {
            uint32_t k = left < (uint32_t)sizeof(sink) ? left : (uint32_t)sizeof(sink);
            uint32_t got = cxf_ring_read(&s->inbox, sink, k);

            if (got == 0u) break;              /* 不可能：上面已确认 total 到齐 */
            left -= got;
        }
        s->stat_input_lost++;
        *skip = true;
        return false;
    }

    if (msg->text_len > 0u)
        (void)cxf_ring_read(&s->inbox, text, msg->text_len);
    text[msg->text_len] = '\0';
    return true;
}

static void inject_tick(cxf_world *w, void *st, cxf_arena *frame) {
    InjectState *m = (InjectState *)st;
    AgentStore *s = m->store;
    InjectMsg msg;
    char text[AGENT_INPUT_MAX + 1u];
    bool skip;

    (void)w;
    (void)frame;

    /* tick 边界 = 感知平面与决策平面的同步点（§4.5） */
    if (!m->resolved || m->cap_gen != s->cap_gen) inject_resolve(m);

    /*
     * 先解动作、再从 ring 取新请求。
     *
     * 顺序不是随意定的：动作是**已经在跑的任务**的下一步，新请求是**还没开始**
     * 的任务。让在跑的先把这一轮推进完，新请求最多等一个 tick；反过来则会让
     * 一条回路在每次外部请求到达时被插队一次。
     */
    inject_actions(m);

    for (;;) {
        if (!inject_read_frame(s, &msg, text, AGENT_INPUT_MAX, &skip)) {
            if (!skip) break;                  /* 半帧：等下一个 tick */
            continue;                          /* 超限帧：已消费并计数，看下一帧 */
        }
        inject_plan(m, msg.kind, msg.trust_max, text, msg.text_len);
    }
}

/* ---- MATCH ---- */

static cxf_result phase_init(cxf_world *w, void *st) {
    PhaseState *m = (PhaseState *)st;
    m->store = (AgentStore *)cxf_service_find(w, AGENT_STORE_SVC, 1);
    return m->store != NULL ? CXF_OK : CXF_ERR_DEP;
}

/*
 * 单 rank 下没有 rank 可选，MATCH 的职责是**可用性判定**：
 *   可用 = 实现存在（未被热拔）且不越权（trust ≤ 请求天花板）。
 * 两种不可用同一套处理：沿降级链往前走。合在一起的理由是它们语义相同 ——
 * "这个实现现在用不了"，至于为什么用不了，是记在 stat_denied / 未注册上的事。
 *
 * 跨 rank 时这里才长出"选哪个 rank"——形状不变，输入换成 placement 快照（§4.5）。
 */
static void match_tick(cxf_world *w, void *st, cxf_arena *frame) {
    PhaseState *m = (PhaseState *)st;
    AgentStore *s = m->store;
    uint32_t *cap_id = item_u32(s, s->leaf.cap_id);
    uint8_t *status = item_u8(s, s->leaf.status);
    uint8_t *trust_max = item_u8(s, s->leaf.trust_max);

    (void)w;
    (void)frame;

    for (uint32_t r = 0; r < s->items.table.count; r++) {
        const CapImpl *api;

        if (status[r] != WI_READY) continue;
        if (cap_id[r] == CAP_NONE) {          /* 分解期就没有够得着的实现 */
            item_fail(s, r, CXF_ERR_DEP);
            continue;
        }

        api = cap_impl_of(s, cap_id[r]);
        if (api == NULL || api->trust > trust_max[r]) {
            if (api != NULL) s->stat_denied++;
            if (chain_advance(s, &cap_id[r], trust_max[r])) continue;
            item_fail(s, r, CXF_ERR_DEP);
            continue;
        }
        status[r] = WI_RUNNING;
    }
}

/* ---- EXECUTE ---- */

/* ---- 配额：两个正交闸门（§4.17.8）---- */

/*
 * 时钟。**每 tick 只调一次**，所以可以是函数指针；这与"热路径不用函数指针多态"
 * 不冲突（那条针对的是每 token / 每条目）。默认走 CLOCK_MONOTONIC，
 * 测试注入假时钟 —— 否则速率策略只能靠真的等 30 秒来验证。
 */
static uint64_t quota_now(AgentStore *s) {
    if (s->clock != NULL) return s->clock();
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
}

/* 桶容量：burst = conc_max，至少 1（否则 rpm=1 的部署一次都发不出去） */
static uint32_t quota_cap_milli(const AgentQuota *q) {
    return (q->conc_max != 0u ? q->conc_max : 1u) * 1000u;
}

void agent_quota_set(AgentStore *s, uint32_t conc_max, uint32_t rpm) {
    if (s == NULL) return;
    s->quota.conc_max = conc_max;
    s->quota.rpm = rpm;
    s->quota.last_ns = quota_now(s);
    s->quota.tok_milli = (rpm != 0u) ? quota_cap_milli(&s->quota) : 0u;
}

bool agent_quota_take(AgentStore *s) {
    AgentQuota *q;
    uint64_t now, dt;
    uint32_t add;

    if (s == NULL) return true;
    q = &s->quota;
    if (q->rpm == 0u) return true;              /* 不声明就是不限：默认行为不付任何代价 */

    /*
     * `last_ns` **只向前推进**。时钟倒退（注入源被换、或者哪天换成可调时钟）
     * 时若照样写回，下一次校正就会按"倒退前的时刻"算 dt —— 于是白送出一段
     * 本来没过去的时间的券。倒退一律当作"这一拍没过去时间"。
     */
    now = quota_now(s);
    if (now > q->last_ns) {
        dt = now - q->last_ns;
        q->last_ns = now;
    } else {
        dt = 0u;
    }

    /*
     * 补充量 = dt × rpm / 60s，单位毫令牌。先夹住 dt：一次长停顿（比如进程被
     * 挂起）之后 dt 可以是几分钟，乘出来会靠近 uint64 上界 —— 而那时桶早就满了，
     * 夹住不改变结果，只是让溢出不可能发生。
     */
    if (dt > 60000000000ull) dt = 60000000000ull;
    add = (uint32_t)((dt * (uint64_t)q->rpm * 1000ull) / 60000000000ull);

    {
        uint32_t cap = quota_cap_milli(q);

        if (add >= cap || q->tok_milli >= cap - add) q->tok_milli = cap;
        else q->tok_milli += add;
    }

    if (q->tok_milli < 1000u) return false;     /* 券不够：本 tick 不发，下一 tick 再试 */
    q->tok_milli -= 1000u;
    return true;
}

static void execute_tick(cxf_world *w, void *st, cxf_arena *frame) {
    PhaseState *m = (PhaseState *)st;
    AgentStore *s = m->store;
    uint32_t *cap_id = item_u32(s, s->leaf.cap_id);
    uint8_t *status = item_u8(s, s->leaf.status);
    uint8_t *trust_max = item_u8(s, s->leaf.trust_max);
    uint8_t *tries = item_u8(s, s->leaf.tries);

    (void)w;
    (void)frame;

    for (uint32_t r = 0; r < s->items.table.count; r++) {
        const CapImpl *api;
        cxf_handle args_h, res_h;
        const char *args;
        uint32_t args_len = 0, res_cap, out_len = 0;
        cxf_result rc;

        if (status[r] != WI_RUNNING) continue;

        api = cap_impl_of(s, cap_id[r]);
        if (api == NULL || api->trust > trust_max[r]) {
            /* 与 MATCH 同一套规则。这里再判一次是纵深防御：cap_id 是分解期
               写下的，不该假设它到这一刻仍然可用（能力可能刚被拔掉）。 */
            if (api != NULL) s->stat_denied++;
            if (chain_advance(s, &cap_id[r], trust_max[r])) {
                status[r] = WI_READY;   /* 回 MATCH：换来的实现同样要判可用性 */
                continue;
            }
            item_fail(s, r, CXF_ERR_DEP);
            continue;
        }

        /*
         * 配额闸门（§4.17.8）。放在 `tries[r]++` **之前** —— 一条因为速率被推迟的
         * 条目没有"尝试过"，把 tries 记上等于用配额消耗掉它的重试额度。
         *
         * 用 break 而不是 continue：券用完之后，本 tick 剩下的条目发了也是撞墙
         * （云端是 429），不如把它们留在 RUNNING，让下一 tick 从头来。
         *
         * ★ 已知代价：tick 是忙循环，所以"等下一个令牌"这段时间是**空转**的。
         *   这不是坏事暴露，是"限速需要 tick 会睡"的直接证据（`stat_quota_wait`
         *   与 `stat_executed` 的比值一涨就说明该给 tick 一个"最早唤醒时刻"）。
         */
        if (!agent_quota_take(s)) {
            s->stat_quota_wait++;
            break;
        }

        tries[r]++;
        if (tries[r] > s->stat_max_tries) s->stat_max_tries = tries[r];
        /*
         * `stat_llm_calls` / `stat_exec_calls` / `stat_cost` 三个计数器全部
         * 数**实际调用**，不数条目。
         *
         * 原来 `stat_cost` 记在派发处（一次派发加一次），而扩容重跑会把同一个
         * 逻辑调用放大 4 倍（`out_hint 0` 时实测 4 次）⇒ 两个"钱"代理自相矛盾：
         * `stat_llm_calls` 说 4 次，`stat_cost` 说 1 次的钱。**低报的计数器比
         * 没有更坏**（同 `stat_llm_calls` 的老账）—— 它给出"看起来成立"的结论：
         * "这个回路很便宜"，而真实账单是 4 倍。
         * 判据与口径：`stat_cost == Σ 实际调用 cost`，于是
         * `stat_exec_calls - stat_executed` 的**差值**就是浪费量（同 §4.9）。
         *
         * 顺带：这一行从 blob_alloc **之前**挪到调用点之后，于是"分配失败、
         * 一次都没调"不再被记钱 —— 与"按实际调用"自洽。
         */

        args_h = item_handle(s, r, s->leaf.args_i, s->leaf.args_g);
        args = blob_data(s, args_h, &args_len);
        if (args == NULL) args = "";

        /*
         * 起始容量来自描述符（out_hint），0 才回落默认值。
         *
         * 这一行是 A1 的一半：真实模型输出长度不可预测，而扩容**重跑**会把
         * "执行一次"变成"执行若干次"，重跑的返回字节可能与第一次不同（采样
         * 随机性 / MoE 路由）⇒ 同一任务的汇聚结果不再确定。所以实现自己声明
         * "我大概会写多大"，比让执行层在 128 字节上试探要正确得多。
         *
         * 默认值仍然只有 128：这是**故意的**，mock 的 560 字节填充就是用来
         * 压扩容路径的。一个从不增长的执行层等于把那条路径测没了。
         */
        res_cap = api->out_hint != 0u ? api->out_hint : AGENT_RESULT_MIN;
        res_h = blob_alloc(s, res_cap);
        if (cxf_handle_null(res_h)) {
            item_fail(s, r, CXF_ERR_NOMEM);
            continue;
        }

        /*
         * 输出容量不足不是失败，是"再给一倍"——但上限要明确：
         * 一个永远写不够的 handler 会在 AGENT_RESULT_TRIES 轮后收手，
         * 而不是把 tick 卡死。这一层重试是执行细节，不计入 WorkItem.tries。
         */
        rc = CXF_ERR_FULL;
        for (uint32_t a = 0; a < AGENT_RESULT_TRIES; a++) {
            char *out = blob_data(s, res_h, NULL);

            rc = api->exec(api->ctx, args, args_len, out, res_cap, &out_len);
            s->stat_exec_calls++;
            s->stat_cost += api->cost;   /* 按实际调用：扩容重跑也要付钱（A3） */
            if (api->tier == CAP_TIER_LLM) s->stat_llm_calls++;
            if (rc != CXF_ERR_FULL) break;
            if (blob_reserve(s, res_h, res_cap * 2u) != CXF_OK) {
                rc = CXF_ERR_NOMEM;
                break;
            }
            res_cap *= 2u;
        }

        if (rc != CXF_OK) {
            blob_free(s, res_h);
            s->stat_retried++;

            /*
             * 「模型给的段数超过上限」必须计入 `stat_batch_bad`，否则 p̂ **低报**。
             *
             * 实测（2026-10-23，121 那台）一次真实运行：模型对"九个符号各在哪"回了
             * **9 条 tool_calls**，`agent_llm_actions_from_response` 在
             * `agent_llm.c` 的 `i > max` 处响亮返回 `CXF_ERR_RANGE`（不截断，这是设计）。
             * 那条路径**根本没走到 `intent_ok`**，于是三个 batch 计数器一个都没动 ——
             * `[batch]` 印出"本次没有任何产出尝试 ⇒ p̂ 无定义"。
             *
             * 这是"低报的计数器比没有更坏"的第三个实例：模型明确产出过、被我们拒了，
             * 读数却说它没尝试。丢掉的正好是 `p̂` 的**分子意义上的 bad**，
             * 而这些样本恰恰是判断"要不要抬 `AGENT_FANOUT_MAX`"的唯一证据。
             *
             * 判据是**代理**：对 `CAP_TIER_LLM` 的实现，`RANGE` 的唯一来源就是
             * "段数超过 AGENT_FANOUT_MAX"（见 agent_llm.c 的 `i > max` 与
             * "解析单条动作时有多余字节"两处）。所以按 tier + rc 判，不新增错误码 ——
             * 加一个 `CXF_ERR_TOO_MANY_SEGS` 会让内核的错误码集合变宽，
             * 而目前只有一个读者（这一行）。
             */
            if (api->tier == CAP_TIER_LLM && rc == CXF_ERR_RANGE)
                s->stat_batch_bad++;

            /*
             * 降级链：换一个实现接着干。cap_fb[] 是注册期算好的 cap_id → cap_id，
             * 因此这里不进查找路径。链无环、有限，所以不需要重试计数兜底 ——
             * 走一步少一步，走到底就是 FAILED。
             */
            if (chain_advance(s, &cap_id[r], trust_max[r])) {
                status[r] = WI_READY;
                continue;
            }
            item_fail(s, r, rc);
            continue;
        }

        ((uint32_t *)cxf_table_get(&s->blobs, res_h, BLOB_COL_LEN))[0] = out_len;
        item_set_handle(s, r, s->leaf.result_i, s->leaf.result_g, res_h);
        s->bytes_out += out_len;
        status[r] = WI_DONE;
        s->stat_executed++;
    }
}

/* ---- RECYCLE ---- */

/*
 * 终值追加（§10.7）。
 *
 * 原来的实现是一个 `char final_text[512]` 定长数组，满了就**整行丢掉**。
 * 在回路下这不是"很少发生的边界"，是**一到三轮就必然发生**：每一轮的 DONE
 * 都要追加一行，512 装不下三行正常长度的结果。于是"答案正确但少了一段"
 * 这种最难查的 bug 就成了默认行为。
 *
 * 现在改成 blob + 单次 reserve：
 *   1. reserve 一次，随后两次 append 不可能失败 —— 保住"满了整行丢、不截半行"
 *      这条语义（半行比丢整行更难看出错）；
 *   2. 上限仍然存在（AGENT_FINAL_MAX），超限记 stat_final_lost 而**不是静默返回**。
 *      有界可以，静默不行。
 *   3. 不再写 '\0'：长度由 blob 的 len 列给出，数据通路里不带零终止串。
 */
static void append_final(AgentStore *s, const char *res, uint32_t n) {
    uint32_t have = 0, need;

    if (cxf_handle_null(s->final_h)) {
        s->final_h = blob_alloc(s, AGENT_FINAL_MIN);
        if (cxf_handle_null(s->final_h)) { s->stat_final_lost++; return; }
    }

    (void)blob_data(s, s->final_h, &have);
    need = have + n + 1u;               /* +1 是行分隔符 */
    if (need < have || need > AGENT_FINAL_MAX) { s->stat_final_lost++; return; }
    if (blob_reserve(s, s->final_h, need) != CXF_OK) { s->stat_final_lost++; return; }

    (void)blob_append(s, s->final_h, res, n);   /* 容量已在上一行保证 */
    (void)blob_append(s, s->final_h, "\n", 1u);
    s->bytes_copy += n + 1u;
    s->copy_ops++;
    s->stat_final++;
}

void agent_args_budget_set(AgentStore *s, uint32_t bytes) {
    if (s == NULL) return;
    s->args_budget = (bytes == 0u) ? AGENT_ARGS_MAX : bytes;
}

void agent_set_max_rounds(AgentStore *s, uint32_t rounds) {
    if (s == NULL) return;
    s->max_rounds = (rounds == 0u) ? AGENT_MAX_ROUNDS : rounds;
}

cxf_str agent_final(AgentStore *s) {
    cxf_str out = { NULL, 0 };
    uint32_t n = 0;
    char *p = blob_data(s, s->final_h, &n);

    if (p != NULL) {
        out.p = p;
        out.len = n;
    }
    return out;
}

/*
 * 水位观测（§10.7）。判定的是"这一 tick 有没有推进"，不是"任务跑完了没有"。
 *
 * 为什么判"推进"而不是判"完成"：一个正常的慢任务和一个卡住的任务都表现为
 * "还没完"，区别只在于前者每个 tick 都有东西在动。以完成为判据会把所有
 * 长任务判成卡住 —— 那种计数器上线第一天就会被关掉，然后连真卡住也看不见了。
 *
 * 三个"在动"的信号，缺一个就漏一类：
 *   items 数变了   —— 有注入或有回收（回路每轮都会变，所以回路不会误判停滞）
 *   executed 变了  —— 有条目走到了 DONE
 *   final 变了     —— 有终值落地
 * 只看 items 数会漏掉"一直在注入也一直在回收、但没一条跑完"的空转；
 * 只看 executed 会漏掉"父节点等齐了子项正要放行"这种纯结构推进。
 */
uint32_t agent_observe(AgentStore *s) {
    uint32_t items = s->items.table.count;
    bool moved = (items != s->obs_items)
              || (s->stat_executed != s->obs_done)
              || (s->stat_final != s->obs_final);

    s->obs_items = items;
    s->obs_done  = s->stat_executed;
    s->obs_final = s->stat_final;

    if (moved || items == 0) {
        /* 有推进；或者根本没有活的条目 —— **闲不是停**。
           少了 items == 0 这一条，每个空世界都会在 K tick 后报一次停滞。 */
        s->stall_ticks = 0;
        s->stall_latched = false;
        return 0;
    }

    if (s->stall_ticks < UINT32_MAX) s->stall_ticks++;
    if (s->stall_ticks > s->stall_max) s->stall_max = s->stall_ticks;

    /*
     * 阈值跟着**运行期**的轮数上限走，不跟编译期常量走：调大 `--rounds` 之后
     * 一条合法回路本来就能跑更多 tick，阈值若还钉在旧的 2×(3+1) 就会把正常
     * 长回路判成停滞 —— 那正是"阈值写死会变成误报源"（agent.h §停滞判定）。
     */
    if (s->stall_ticks >= (s->max_rounds + 1u) * 2u && !s->stall_latched) {
        s->stall_latched = true;
        s->stat_stalled++;
    }
    return s->stall_ticks;
}

uint32_t agent_audit(AgentStore *s) {
    uint32_t bad = 0;
    uint64_t sum_cap = 0;
    void    **ptr  = (void **)cxf_table_col(&s->blobs, BLOB_COL_PTR);
    uint32_t *blen = (uint32_t *)cxf_table_col(&s->blobs, BLOB_COL_LEN);
    uint32_t *bcap = (uint32_t *)cxf_table_col(&s->blobs, BLOB_COL_CAP);
    uint32_t n_items = s->items.table.count;

    /* A1 账目自洽。world.c 曾因为"用 A 算式申请、用 B 算式释放"让
       bytes_live 静默漂移 124 字节 —— 同一类错误在这里由一条等式钉住。 */
    for (uint32_t r = 0; r < s->blobs.count; r++) {
        if (ptr[r] == NULL || bcap[r] == 0) bad++;
        if (blen[r] > bcap[r]) bad++;
        sum_cap += bcap[r];
    }
    if (sum_cap != s->blob_bytes) bad++;

    /* A2 引用完整。args / result 要么为空，要么指向活着的 blob。
       跨表误用在 class 位加持下必然不等于，因此这条是**确定性**检查，
       不是概率拦截（handle.h H3 的残余项在此被兜住）。 */
    for (uint32_t r = 0; r < n_items; r++) {
        cxf_handle ah = item_handle(s, r, s->leaf.args_i, s->leaf.args_g);
        cxf_handle rh = item_handle(s, r, s->leaf.result_i, s->leaf.result_g);
        if (!cxf_handle_null(ah) && !cxf_table_live(&s->blobs, ah)) bad++;
        if (!cxf_handle_null(rh) && !cxf_table_live(&s->blobs, rh)) bad++;
    }

    /* A3 交接完整。挂起的动作必须仍是活条目 —— 否则 INJECT 会解一张
       已经不存在的行，那是 use-after-free 而不是"少派生一步"。 */
    for (uint32_t i = 0; i < s->act_n; i++)
        if (!cxf_soa_live(&s->items, s->acts[i])) bad++;

    /*
     * A4 汇聚账：BLOCKED 条目的 pending 必须等于仍指向它的活子项数。
     *
     * 这一条抓的是**静默停住**：pending 大于活子项数 ⇒ 父节点在等一个
     * 永远不会来的子项 ⇒ 它永远 BLOCKED ⇒ 界面什么都不动，没有崩溃、
     * 没有泄漏、没有任何一行日志。这是失败里最难查的一类。
     *
     * O(n²)：每个 BLOCKED 条目扫一遍全表。n 是"正在跑的条目数"，量级是
     * 几十，而这是调试件不是热路径 —— 用可读性换那点时间是对的。
     * 只在 tick 边界成立（见 agent.h 的说明）。
     */
    {
        uint32_t *pending = item_u32(s, s->leaf.pending);
        uint8_t  *status  = item_u8(s, s->leaf.status);

        for (uint32_t r = 0; r < n_items; r++) {
            cxf_handle self;
            uint32_t kids = 0;

            if (status[r] != WI_BLOCKED) continue;
            self = cxf_table_at(&s->items.table, r);
            for (uint32_t k = 0; k < n_items; k++) {
                cxf_handle par = item_handle(s, k, s->leaf.parent_i, s->leaf.parent_g);
                if (!cxf_handle_null(par) && cxf_handle_eq(par, self)) kids++;
            }
            if (pending[r] != kids) bad++;
        }
    }

    /* A5 子表自身的不变式（table T1~T8 / soa） */
    if (!cxf_table_check(&s->blobs)) bad++;
    if (!cxf_soa_check(&s->items)) bad++;

    return bad;
}

/*
 * 预算丢弃标记（§4.16 末）。它不是日志，是**要进 prompt 的一段字节** ——
 * 理由见 recycle_tick 里那段长注释。
 *
 * 名字与能力侧的 `#truncated` 刻意相近但不同：`#truncated` 是**产出方**
 * 声明"我就产出了这么多"（语义边界在能力的界内），`#budget` 是**装配方**
 * 声明"它产出了，我没装进去"。两者混成一个词，就再也分不清该调哪一边。
 *
 * ★ 标记本身**不计入预算**：预算小到装不下标记时，若连标记也丢，模型收到的
 * 就是一段悄悄变短的上下文 —— 那正是这一整套设计要消灭的东西。
 */
static bool args_budget_note(AgentStore *s, cxf_handle p_args, uint32_t dropped) {
    char buf[AGENT_ARGS_NOTE_MAX];
    int  w = snprintf(buf, sizeof(buf), "#budget: dropped %uB\n", dropped);

    if (w <= 0 || (uint32_t)w >= sizeof(buf)) return false;   /* 32 字节够 10 位十进制 */
    return blob_append_grow(s, p_args, buf, (uint32_t)w) == CXF_OK;
}

/*
 * 汇聚记录（§4.16.3 立，v1.14.8 从"一个头"改成"调用 + 结果"一对）。
 *
 * ★ 它存在的理由不是"给模型多一点提示"，而是**补一条缺失的记录**。
 *
 * 第一条真实轨迹的 args 长这样：
 *
 *     问题\n 结果 \n 结果 \n 结果 \n 结果
 *
 * 这里面**没有任何一条记录模型自己发过什么**。于是模型无法从上下文里看出
 * "这个工具我已经用同样的参数调过三次"，只能再调一次 —— 实测同一个问题连跑
 * 三次，grep 次数是 3 / 4 / 2，其中一次撞到轮数上限直接判失败。
 *
 * v1.14.3 的对策是写一个 `#result` 头（24 B 参数摘要）；v1.14.8 查出那不够：
 * 调用本身仍不在 transcript 里，配对靠顺序，而 `recycle_tick` 倒序回收让
 * **顺序今天就已经不成立**（同 tick 内是派出序的倒序，闸门下是完成序）。
 * 现在的形状是一对记录，`build_request` 再把整条 blob 渲染成**标准
 * tool-calling messages**（调用→assistant(tool_calls)、结果→tool）：
 *
 *     #call  <cap> <args_len>\n<args>\n#result <cap> <res_len>B\n<body>\n
 *     #call  <cap> <args_len>\n<args>\n#fail <seq> <cap> <why>\n
 *
 * 那是模型后训练过的形状，而且 `tool` 消息在协议上**必须**挂在带 `tool_calls`
 * 的 assistant 消息之后 —— 回传完整调用是约束，不是偏好。args 不再截断：
 * 调用身份由完整 args 承担，24 B 前缀猜身份正是上面那个洞。
 *
 * ★ **三种结局必须各留一行，因为它们对模型意味着三件不同的事：**
 *
 *     `#result fs.grep 31B`              成功且产出了 31 字节
 *     `#result fs.grep 0B`               成功，但什么都没有 —— 0 命中是**结论**，不是没跑
 *     `#fail 1 fs.write_apply io`        没成 —— 第 1 段，世界不配合（换路径 / 别原样重发）
 *
 * 第三种加了 `<why>` 之后，"没成"才真的能分成四个方向：`format`/`range`（我们这侧，
 * 改参数）、`io`（世界那侧，改路径或别重发）、`full`（该扩容量）、`dep`（没实现）。
 * 不加它的时候这四种在上下文里是同一句话，模型只能靠再调几次去试出来
 * （实测 3 次，见 §4.18.20）。
 *
 * 原来只写第一种：条件是 `res != NULL && res_len > 0`，于是后两种**合并成"什么也没写"**。
 * 模型看到 args 一字节没变，无法区分"工具跑了但空了"和"工具根本没跑成"。
 * 这和 `#budget` 是同一条原则的第二次应用：**上下文悄悄变短（或不长）必须留标记**。
 * 而这个洞在 fan-out 下被放大 —— N 条里失败一条，模型只看到 N-1 份结果，
 * 却没有任何一行说第 N 条去哪了。
 */
#define AGENT_NOTE_HEAD_MAX 96u

/* 能力名查不到时写 "?"：记录不能因为没有名字就消失。 */
static const char *note_cap_name(AgentStore *s, uint32_t cap_id) {
    if (cap_id < s->cap_count) {
        const CapDef *d = agent_cat_def(s, s->impl_cat[cap_id]);
        if (d != NULL && d->name != NULL) return d->name;
    }
    return "?";
}

/*
 * 三个头都只写**定长部分**。载荷（args / body）由 recycle_tick 分段直接
 * append，不过任何固定缓冲区 —— args 可以长到 `AGENT_ARGS_MAX`，定长缓冲
 * 会造出一个新的"装不下就悄悄截断"的洞（那正是 v1.14.8 修的那类）。
 * 返回写入长度；0 = 拼不出来，调用方必须让这条记录**响亮失败**。
 */
static uint32_t note_call_head(char *buf, uint32_t cap,
                               const char *name, uint32_t args_len) {
    int w = snprintf(buf, cap, "#call %s %u\n", name, args_len);
    return (w <= 0 || (uint32_t)w >= cap) ? 0u : (uint32_t)w;
}

static uint32_t note_result_head(char *buf, uint32_t cap,
                                 const char *name, uint32_t res_len) {
    int w = snprintf(buf, cap, "#result %s %uB\n", name, res_len);
    return (w <= 0 || (uint32_t)w >= cap) ? 0u : (uint32_t)w;
}

/*
 * 一个 `cxf_result` 对应一个词。**必须返回 NULL 而不是兜底成 "?"**：
 * 兜底会让"枚举加了新值、这张表没跟上"变成一句看起来正常的回执，
 * 于是下一个人只能在模型行为变差之后才怀疑到这里。NULL 逼调用方
 * 印出数字（`e<num>`），那是**可 grep 的、诚实的**未知。
 *
 * 为什么是词不是数字：模型手里没有错误码表。给它 `9` 等于没说，
 * 而要给它一张表得在提示词里多花几十字节，且那张表同样是"两处必须一起变"
 * 的东西 —— 还不如把词直接写进记录里（每个词 3~6 B，只出现在失败行）。
 *
 * `CXF_OK` 也映射：它表示"某条路径把条目判成 FAILED 却没给原因"。
 * 这不是一个合理的值，是**可见的故障**（v1.14.24 之前存在过的六处写法
 * 里任意一处漏设都会变成它）。
 */
static const char *err_word(uint8_t rc) {
    switch ((cxf_result)rc) {
    case CXF_OK:             return "?";      /* 漏设：可见的故障，不是一种原因 */
    case CXF_ERR_NOMEM:      return "mem";
    case CXF_ERR_FULL:       return "full";
    case CXF_ERR_EMPTY:      return "empty";
    case CXF_ERR_BAD_HANDLE: return "stale";
    case CXF_ERR_RANGE:      return "range";
    case CXF_ERR_DEP:        return "dep";
    case CXF_ERR_FORMAT:     return "format";
    case CXF_ERR_IO:         return "io";
    }
    return NULL;                              /* 未知枚举值 ⇒ 印 e<num> */
}

/*
 * `#fail` 带 seq：**记录的位置必须由数据说，不能由记录的先后说**。
 *
 * 原来只有能力名，于是"这一条是本轮第几条"在上下文里彻底丢失 ——
 * 而模型要靠它决定从第几段重来（§4.18.16）。顺序救不了：倒序回收使记录
 * 是派出序的倒序，配额闸门下又是完成序，三种顺序都真实存在。
 *
 * 代价 2 字节（一位数 + 空格）。成功路径的 `#result` 暂不带 seq：它的读者
 * 还没有（回退未落地），而它落在**前缀**上，每多一字节都要付缓存与计费的账。
 *
 * v1.14.24 加第三个字段（`err`）：`#fail <seq> <cap> <why>`。
 * 它回答的问题与 seq 不同 —— seq 说"哪一条"，why 说"哪一侧出错"。
 * 后者决定下一步动作（改参数 / 换请求 / 别重发 / 扩容量），而此前它
 * 只在失败那一刻存在过一瞬间。
 */
static uint32_t note_fail_head(char *buf, uint32_t cap, const char *name,
                               uint8_t seq, uint8_t err) {
    /*
     * 一律写 seq，`0` = 这一条不在批次里（单条派发）。
     *
     * 不用"批次时省略 seq、单条时省掉字段"的两种写法：那是**同一记录的两种方言**，
     * 读的一方要猜"第二个词是 seq 还是能力名"。字段默认 0 表示"没有"是本项目
     * 的既定写法（状态值不行 —— 状态漏一个分支就是静默错，标量可以）。
     */
    const char *why = err_word(err);
    int w;

    if (why != NULL)
        w = snprintf(buf, cap, "#fail %u %s %s\n", (unsigned)seq, name, why);
    else
        w = snprintf(buf, cap, "#fail %u %s e%u\n", (unsigned)seq, name,
                     (unsigned)err);
    return (w <= 0 || (uint32_t)w >= cap) ? 0u : (uint32_t)w;
}

/*
 * 收尾两条路都要走，各写**一对记录**（格式见上一节）：
 *   DONE   → `#call <cap> <args_len>` + args + `#result <cap> <res_len>B` + body
 *            ＋结果追加到父节点的 args（按需扩容），无父节点则进最终结果
 *   FAILED → `#call <cap> <args_len>` + args + `#fail <seq> <cap>`
 *            （**一对记录，不是什么都不写**），并且**必须仍然递减父节点的 pending**
 * 漏掉递减会让父节点永远 BLOCKED —— 失败不是泄漏内存，是泄漏条目，
 * 而且表现为整个流程静默停住，比崩溃难查得多。
 * 漏掉那对记录则是另一种静默：父节点看到的结果数少于它派出去的数量，
 * 而 args 里没有任何证据说明少的那个去哪了（调用为什么发、结果为什么没到）。
 *
 * 入参 blob 也在这里归还，不在 EXECUTE 里：失败可能在三个不同位置发生
 * （判缺能力、执行报错、扩容失败），逐处 free 必然漏掉某一处。
 * 终态条目一定经过本函数，所以只在这里 free 一次就够 ——
 * 成功路径上已归还的 handle 会因为 gen 失效而成为无操作。
 */
static void recycle_tick(cxf_world *w, void *st, cxf_arena *frame) {
    PhaseState *m = (PhaseState *)st;
    AgentStore *s = m->store;
    uint32_t *cap_id = item_u32(s, s->leaf.cap_id);
    uint8_t *status = item_u8(s, s->leaf.status);
    uint32_t *pending = item_u32(s, s->leaf.pending);
    uint8_t *seq = item_u8(s, s->leaf.seq);
    uint8_t *err = item_u8(s, s->leaf.err);

    (void)w;
    (void)frame;

    /* 倒序遍历：swap-remove 只搬动末尾行，倒序时末尾行已处理完毕（table.h） */
    for (uint32_t r = s->items.table.count; r-- > 0;) {
        cxf_handle item_h, parent_h, res_h, args_h;
        uint32_t res_len = 0;
        char *res = NULL;
        bool done;

        if (status[r] != WI_DONE && status[r] != WI_FAILED) continue;
        done = status[r] == WI_DONE;

        item_h = cxf_table_at(&s->items.table, r);

        /*
         * 控制流条目**不是终态，是下一轮的输入**：不回收，挂给 INJECT。
         * 行与 result blob 一起留着 —— 它们就是交接物，抄一份等于把一条
         * WorkItem 拆成两半存，两份口径迟早不同步。
         *
         * 判定读的是注册期算好的 cap_acts[]，不是从 payload 里猜 ——
         * "这一步会派生"是**契约的类型事实**（out_iface == agent.action），
         * 这是方案 D 相对方案 B 多买到的东西。
         */
        if (done && cap_id[r] < s->cap_count && s->cap_acts[cap_id[r]]) {
            if (s->act_n < AGENT_ACT_BATCH) {
                s->acts[s->act_n++] = item_h;
                continue;
            }
            /* 单 tick 可挂起的动作数超限：响亮失败，不静默丢一条控制流 */
            s->stat_failed++;
            blob_free(s, item_handle(s, r, s->leaf.result_i, s->leaf.result_g));
            blob_free(s, item_handle(s, r, s->leaf.args_i, s->leaf.args_g));
            s->stat_recycled++;
            cxf_soa_remove(&s->items, item_h);
            continue;
        }

        args_h = item_handle(s, r, s->leaf.args_i, s->leaf.args_g);
        res_h = item_handle(s, r, s->leaf.result_i, s->leaf.result_g);
        parent_h = item_handle(s, r, s->leaf.parent_i, s->leaf.parent_g);
        if (done) res = blob_data(s, res_h, &res_len);

        if (!cxf_handle_null(parent_h) && cxf_soa_live(&s->items, parent_h)) {
            uint32_t prow = cxf_table_row(&s->items.table, parent_h);
            bool ok = true;

            /*
             * ● 成功（哪怕 0 字节）与失败**都要在 args 里留一对记录**。
             *
             * 老条件是 `res != NULL && res_len > 0`，于是"成功但空"与"失败"
             * 合并成"什么也没写" —— 模型看到 args 一字节没变，无法区分这两件事。
             * 走到这里说明子项已经终态，所以**没有"什么都没发生"这种情形**，
             * 两种结局各留一对才是完整的记录（格式见上一节）。
             */
            if (res != NULL || !done) {
                cxf_handle p_args =
                    item_handle(s, prow, s->leaf.args_i, s->leaf.args_g);
                uint32_t plen = 0;
                char *pa = blob_data(s, p_args, &plen);
                uint32_t sep = 0, in_len = 0;
                char chbuf[AGENT_NOTE_HEAD_MAX], rhbuf[AGENT_NOTE_HEAD_MAX];
                uint32_t chlen = 0, rhlen = 0, rec_len;
                const char *cap_name;
                char *in;

                /*
                 * 拼接前的分隔符。★ 这一格是**读第一条真实轨迹读出来的**：
                 * 首轮 args 是外部发来的问题，末尾没有换行，于是第一份工具结果
                 * 直接粘在问句后面 —— prompt 里出现"用一句话回答agent.md:332:"。
                 * 模型仍答对了（它足够鲁棒），但那是运气：**装配方不能靠模型
                 * 容错来保证两个语义块可分**。`plen == 0` 时不加，否则以换行起头。
                 *
                 * v1.14.8 起这条规则对**每一次**追加生效，`#budget:` 标记也
                 * 包括在内 —— 它此前是唯一不补分隔符的落点，于是首轮就丢条目时
                 * 标记粘在问句上（"回路起点#budget: dropped 74B"）。
                 * `convo_render` 按行判别标记，粘进行里的标记不会被识别成标记，
                 * 只会整行变成问句的一部分。
                 */
                if (plen > 0u && pa != NULL && pa[plen - 1u] != '\n') sep = 1u;

                cap_name = note_cap_name(s, cap_id[r]);
                in = blob_data(s, args_h, &in_len);   /* 子项的 args = 工具入参 */
                chlen = note_call_head(chbuf, (uint32_t)sizeof(chbuf),
                                       cap_name, in_len);

                /*
                 * 装配预算（§4.16 末）。args 是**跨轮累积的上下文**，不是"一轮的
                 * 输入"：工具结果 append 进来，下一轮这份更大的 args 再进模型。
                 * 没有预算，它就是一条随轮数线性增长的隐式预算 —— 而"给多少
                 * 上下文"正是 §4.15 要测的 `b*`。
                 *
                 * ★ 超预算的做法是**尾部整条丢弃**（v1.14.8 起"一条" =
                 *   一对记录：调用 + 结局，不再只是结果体），三件事一起否掉：
                 *
                 *   不截半条：半截工具输出比没有更难判断对错 —— 模型会基于它
                 *   编一个看起来合理的结论，而没人知道它少了一半。所以要么整条
                 *   进上下文，要么整条不进。`bytes_lost == 丢掉的记录全长`
                 *   就是这条不变式的可断言形式（与 `#budget` 标记记的是同一个数）。
                 *
                 *   不判失败：判失败会把"上下文装不下"变成"任务失败"，于是预算
                 *   永远降不到产出量以下 ⇒ b* 曲线**测不出来**。预算只是装配方
                 *   的选择，不是任务的属性；把它记成失败等于让实验手段本身
                 *   被当成故障。
                 *
                 *   不丢头部：args 是一条 prefix-stable append log —— 已发出的
                 *   前缀字节永不改变，这是推理引擎 KV cache 命中的前提（§4.9）。
                 *   丢尾部 ⇒ 下一轮的 prompt 仍是"如果没丢"那串的前缀；
                 *   丢头部 ⇒ 从第一个字节起全部失效，每轮都全量 prefill。
                 *
                 * ★ 丢完必须留标记（`args_budget_note`）：不给标记，模型看到的
                 *   是一段**悄悄变短**的上下文，它没有任何办法知道"我上一轮要的
                 *   东西没到"。标记是这件事对模型与日志唯一同时可见的证据。
                 *
                 * ★ `#fail` 记录**不走预算**：它的长度有界（args 是模型自己
                 *   产出的入参，实践中几十字节；上界由 `AGENT_LLM_TOOL_ARGS_CAP`
                 *   卡在 1 KiB），而"因为预算紧张所以不告诉模型哪条失败了"
                 *   正好退回到本节要修的静默。代价是一条失败记录的字节数可能
                 *   超出预算，这是有意的 —— 与 `#budget` 标记不计入预算同族：
                 *   **让模型知道"有事没成"比守住预算更值**。
                 */
                if (chlen == 0u) {
                    ok = false;             /* 头都拼不出来：这条记录不能悄悄消失 */
                } else if (!done) {
                    rhlen = note_fail_head(rhbuf, (uint32_t)sizeof(rhbuf), cap_name,
                                           seq[r], err[r]);
                    /*
                     * 分子：失败落在第几段。seq = 0（不在批次里）不计数 ——
                     * 它不是"第 0 段失败"，是"没有段这个概念"，混进直方图
                     * 会让第 0 格变成一个假的众数。
                     */
                    if (seq[r] > 0u) {
                        s->stat_fail_seq[seq[r] > AGENT_FANOUT_MAX
                                         ? AGENT_FANOUT_MAX : seq[r]]++;
                    }
                    if (rhlen == 0u) {
                        ok = false;
                    } else {
                        rec_len = sep + chlen + in_len + 1u + rhlen;
                        ok = (sep == 0u || blob_append_grow(s, p_args, "\n", 1u) == CXF_OK) &&
                             blob_append_grow(s, p_args, chbuf, chlen) == CXF_OK &&
                             (in_len == 0u ||
                              blob_append_grow(s, p_args, in, in_len) == CXF_OK) &&
                             blob_append_grow(s, p_args, "\n", 1u) == CXF_OK &&
                             blob_append_grow(s, p_args, rhbuf, rhlen) == CXF_OK;
                        s->bytes_copy += rec_len;
                        s->copy_ops++;
                    }
                } else {
                    rhlen = note_result_head(rhbuf, (uint32_t)sizeof(rhbuf),
                                             cap_name, res_len);
                    if (rhlen == 0u) {
                        ok = false;
                    } else {
                        rec_len = sep + chlen + in_len + 1u + rhlen + res_len + 1u;
                        if (plen + rec_len > s->args_budget) {
                            ok = (sep == 0u ||
                                  blob_append_grow(s, p_args, "\n", 1u) == CXF_OK) &&
                                 args_budget_note(s, p_args, rec_len);
                            s->stat_args_lost++;
                            s->bytes_lost += rec_len;
                        } else {
                            ok = (sep == 0u || blob_append_grow(s, p_args, "\n", 1u) == CXF_OK) &&
                                 blob_append_grow(s, p_args, chbuf, chlen) == CXF_OK &&
                                 (in_len == 0u ||
                                  blob_append_grow(s, p_args, in, in_len) == CXF_OK) &&
                                 blob_append_grow(s, p_args, "\n", 1u) == CXF_OK &&
                                 blob_append_grow(s, p_args, rhbuf, rhlen) == CXF_OK &&
                                 (res_len == 0u ||
                                  blob_append_grow(s, p_args, res, res_len) == CXF_OK) &&
                                 blob_append_grow(s, p_args, "\n", 1u) == CXF_OK;
                            s->bytes_copy += rec_len;   /* 数据流的第 N 次落地 */
                            s->copy_ops++;
                        }
                    }
                }
                if (!ok && status[prow] != WI_FAILED) {   /* 汇聚写不下：父节点判失败 */
                    item_fail(s, prow, CXF_ERR_FULL);
                }
            }
            /* 只有 BLOCKED 的父节点能被最后一个子项放行。
               这个前置条件同时挡住"已判失败的父节点被兄弟子项救活"。 */
            if (ok && status[prow] == WI_BLOCKED && pending[prow] > 0 &&
                --pending[prow] == 0)
                status[prow] = WI_READY;
        } else if (res != NULL) {
            append_final(s, res, res_len);
        }

        blob_free(s, res_h);
        blob_free(s, args_h);   /* 终态条目的入参一律归还（成功路径已归还的会是无操作） */
        s->stat_recycled++;
        cxf_soa_remove(&s->items, item_h);
    }

    /*
     * tick 边界观测（§10.7）。放在这里是因为 RECYCLE 是 LAST phase ——
     * 走到这行时本 tick 的四个 phase 都已产出完毕，"这一 tick 有没有推进"
     * 才是一个有定义的问题。放在任何其它 phase 都会读到半成品水位。
     */
    (void)agent_observe(s);

#if AGENT_AUDIT
    {
        uint32_t bad = agent_audit(s);
        if (bad != 0) {
            s->stat_audit_bad += bad;
            (void)fprintf(stderr, "agent: audit %u bad (items %u, blobs %u)\n",
                          bad, s->items.table.count, s->blobs.count);
        }
    }
#endif
}

/* ---- 模块定义 ---- */

const cxf_module_def agent_store_def = {
    "agent.store", CXF_PHASE_INPUT, sizeof(AgentStore),
    AGENT_STORE_CAP, 0, 0,
    store_init, NULL, store_deinit
};

const cxf_module_def agent_inject_def = {
    "agent.inject", CXF_PHASE_INPUT, sizeof(InjectState),
    0, 0, AGENT_STORE_CAP,
    inject_init, inject_tick, NULL
};

const cxf_module_def agent_match_def = {
    "agent.match", CXF_PHASE_UPDATE, sizeof(PhaseState),
    0, 0, AGENT_STORE_CAP,
    phase_init, match_tick, NULL
};

const cxf_module_def agent_execute_def = {
    "agent.execute", CXF_PHASE_LATE, sizeof(PhaseState),
    0, 0, AGENT_STORE_CAP,
    phase_init, execute_tick, NULL
};

const cxf_module_def agent_recycle_def = {
    "agent.recycle", CXF_PHASE_LAST, sizeof(PhaseState),
    0, 0, AGENT_STORE_CAP,
    phase_init, recycle_tick, NULL
};
