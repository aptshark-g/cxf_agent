#include <string.h>
#include <time.h>

#include "cxf/algo.h"
#include "cxf/vec.h"
#include "cxf/world.h"

/* 契约与不变式见 world.h / module.h，这里只写机制。 */

/* order 的元素访问：vec 只提供基址与长度，类型由调用点给定。 */
#define ORDER(w) cxf_vec_data(&(w)->order, uint32_t)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

cxf_result cxf_world_init(cxf_world *w, cxf_alloc *alloc, size_t arena_chunk) {
    memset(w, 0, sizeof(*w));
    w->alloc = alloc;
    w->order = cxf_vec_make(alloc, (uint32_t)sizeof(uint32_t));   /* 元素宽度必须显式给 */
    cxf_arena_init(&w->arena_state, alloc, arena_chunk);
    cxf_arena_init(&w->arena_frame, alloc, arena_chunk);
    return CXF_OK;
}

void cxf_world_release(cxf_world *w) {
    /* 逆序 deinit，与 stop 一致：后初始化的先销毁。 */
    for (uint32_t k = w->order.len; k-- > 0;) {
        cxf_module *m = &w->modules[ORDER(w)[k]];
        if (m->inited && m->def->deinit != NULL) m->def->deinit(w, m->state);
    }
    if (w->modules != NULL)
        cxf_alloc_free(w->alloc, w->modules, (size_t)w->module_cap * sizeof(cxf_module));
    cxf_vec_free(&w->order);      /* 容量随指针走，不可能用错 size */
    if (w->services != NULL)
        cxf_alloc_free(w->alloc, w->services, (size_t)w->service_cap * sizeof(cxf_service_def));
    /* types 只存指针，描述符是静态常量，不归 world 所有（K3） */
    if (w->types != NULL)
        cxf_alloc_free(w->alloc, w->types, (size_t)w->type_cap * sizeof(cxf_type_entry));
    cxf_arena_release(&w->arena_state);
    cxf_arena_release(&w->arena_frame);
    memset(w, 0, sizeof(*w));
}

const cxf_module *cxf_module_find(const cxf_world *w, const char *name) {
    for (uint32_t i = 0; i < w->module_count; i++) {
        if (w->modules[i].loaded && strcmp(w->modules[i].def->name, name) == 0)
            return &w->modules[i];
    }
    return NULL;
}

const cxf_module *cxf_module_get(const cxf_world *w, uint32_t id) {
    return id < w->module_count ? &w->modules[id] : NULL;
}

void *cxf_module_state(cxf_world *w, uint32_t id) {
    return id < w->module_count ? w->modules[id].state : NULL;
}

cxf_result cxf_module_load(cxf_world *w, const cxf_module_def *def, uint32_t *out_id) {
    cxf_module *m;

    if (cxf_module_find(w, def->name) != NULL) return CXF_ERR_RANGE;   /* M1 */

    if (w->module_count == w->module_cap) {
        /* 增长走唯一实现；指针与容量一起写回。 */
        void *p = w->modules;
        uint32_t cap = w->module_cap;
        cxf_result rc = cxf_vec_grow(w->alloc, &p, &cap, w->module_count,
                                     w->module_count + 1u,
                                     (uint32_t)sizeof(cxf_module));
        if (rc != CXF_OK) return rc;
        w->modules = (cxf_module *)p;
        w->module_cap = cap;
    }

    m = &w->modules[w->module_count];
    m->def = def;
    m->state = NULL;
    m->id = w->module_count;
    m->refcount = 0;
    m->loaded = true;
    m->inited = false;

    /* 状态从 arena_state 分配：只增不减，因此指针在 world 生命期内稳定（M4）。 */
    if (def->state_size != 0) {
        m->state = cxf_arena_alloc(&w->arena_state, def->state_size, CXF_MIN_ALIGN);
        if (m->state == NULL) return CXF_ERR_NOMEM;
        memset(m->state, 0, def->state_size);
    }

    w->module_count++;
    w->started = false;
    if (out_id != NULL) *out_id = m->id;
    return CXF_OK;
}

cxf_result cxf_module_unload(cxf_world *w, uint32_t id) {
    cxf_module *m;
    cxf_caps need;

    if (id >= w->module_count || !w->modules[id].loaded) return CXF_ERR_BAD_HANDLE;
    m = &w->modules[id];
    if (m->refcount != 0) return CXF_ERR_RANGE;                        /* M3 */

    /* 归还它对别人的引用 —— 否则被依赖者永远卸不掉。 */
    need = m->def->requires_hard;
    while (need != 0) {
        uint32_t bit = need & (~need + 1);
        for (uint32_t j = 0; j < w->module_count; j++) {
            if (w->modules[j].loaded && (w->modules[j].def->provides & bit) != 0) {
                if (w->modules[j].refcount > 0) w->modules[j].refcount--;
                break;
            }
        }
        need ^= bit;
    }

    if (m->inited && m->def->deinit != NULL) m->def->deinit(w, m->state);
    m->loaded = false;
    m->inited = false;
    w->started = false;
    return CXF_OK;
}

void *cxf_world_alloc(cxf_world *w, size_t size, size_t align) {
    return cxf_arena_alloc(&w->arena_frame, size, align);
}

/*
 * 解析 HARD 依赖，产出边表 (from=提供者, to=依赖者)。
 * 一个模块可能通过多个能力位依赖同一提供者，去重后再建图，
 * 否则 refcount 会重复计数、让模块永远卸不掉。
 *
 * 边表用 cxf_vec 承载而不是 (指针, count, cap) 三个散装值：调用方拿到的是
 * "指向哪 + 多长 + 申请了多大"绑在一起的对象，释放时不可能取错大小。
 * 这里曾经就是错的 —— 用 cap 申请、用 count 释放，`bytes_live` 每次 start 漂移。
 *
 * **本函数不排序**：拓扑排序属于算法库（`cxf_algo_toposort`），它的输入形状是
 * `cxf_csr`。这里只负责"把注册期的声明翻译成一张图"，排序是下一步的事。
 * 分开的理由与库边界同源：一张图可以被排序、判环、求可达，那是三种变换，
 * 不该埋在注册流程里各写一份。
 */
static cxf_result build_graph(cxf_world *w, cxf_vec *out_edges) {
    uint32_t n = w->module_count;
    cxf_csr_edge *edges = NULL;
    cxf_result rc = CXF_ERR_DEP;      /* 默认失败原因：HARD 依赖缺失 */

    for (uint32_t i = 0; i < n; i++) {
        cxf_caps need;
        if (!w->modules[i].loaded) continue;
        need = w->modules[i].def->requires_hard;

        while (need != 0) {
            uint32_t bit = need & (~need + 1);
            uint32_t prov = CXF_NO_INDEX;

            for (uint32_t j = 0; j < n; j++) {
                if (j != i && w->modules[j].loaded &&
                    (w->modules[j].def->provides & bit) != 0) { prov = j; break; }
            }
            if (prov == CXF_NO_INDEX) return CXF_ERR_DEP;   /* HARD 缺失 */

            /* 去重：同一对 (提供者, 依赖者) 只留一条边，
             * 否则 refcount 被重复计数，模块永远卸不掉。 */
            edges = cxf_vec_data(out_edges, cxf_csr_edge);
            bool dup = false;
            for (uint32_t k = 0; k < out_edges->len; k++)
                if (edges[k].from == prov && edges[k].to == i) { dup = true; break; }

            if (!dup) {
                cxf_csr_edge *e = (cxf_csr_edge *)cxf_vec_push_slot(out_edges);
                if (e == NULL) { rc = CXF_ERR_NOMEM; return rc; }
                e->from = prov;
                e->to = i;
            }
            need ^= bit;
        }
    }
    return CXF_OK;
}

/*
 * 把拓扑序按 phase 分段：段内保持拓扑序，段间按 phase 升序（W2）。
 * 同时跳过未加载的模块 —— 拓扑序是**整张图**（含未加载节点）的次序，
 * 而调度只跑已加载的。未加载节点在图里是孤立的（它既没有入边也没有出边，
 * 因为建图只连接两端都 loaded 的模块），所以过滤只是跳过，不影响次序。
 */
static void split_by_phase(cxf_world *w, const uint32_t *topo, uint32_t topo_n) {
    uint32_t counts[CXF_PHASE_COUNT] = { 0 };

    for (uint32_t k = 0; k < topo_n; k++) {
        const cxf_module *m = &w->modules[topo[k]];
        uint8_t p;

        if (!m->loaded) continue;
        p = m->def->phase;
        if (p < CXF_PHASE_COUNT) counts[p]++;
    }

    w->phase_begin[0] = 0;
    for (uint32_t p = 0; p < CXF_PHASE_COUNT; p++)
        w->phase_begin[p + 1] = w->phase_begin[p] + counts[p];

    uint32_t cursor[CXF_PHASE_COUNT];
    for (uint32_t p = 0; p < CXF_PHASE_COUNT; p++) cursor[p] = w->phase_begin[p];

    for (uint32_t k = 0; k < topo_n; k++) {
        const cxf_module *m = &w->modules[topo[k]];
        uint8_t p;

        if (!m->loaded) continue;
        p = m->def->phase;
        if (p < CXF_PHASE_COUNT) ORDER(w)[cursor[p]++] = topo[k];
    }
    w->order.len = w->phase_begin[CXF_PHASE_COUNT];
}

cxf_result cxf_world_start(cxf_world *w) {
    uint32_t n = w->module_count;
    uint32_t n1 = n + 1u;                 /* +1：n == 0 时不申请 0 字节 */
    cxf_vec edges = cxf_vec_make(w->alloc, (uint32_t)sizeof(cxf_csr_edge));
    cxf_csr csr = { NULL, NULL, 0, 0 };
    void *csr_mem = NULL;
    uint32_t csr_bytes = 0;               /* 申请时存下，释放时用它 —— 见 done: */
    uint32_t *indeg = NULL, *topo = NULL;
    cxf_result r;

    /* order 的长度由 split_by_phase 写回，这里只保证容量足够。 */
    if (cxf_vec_reserve(&w->order, n1) != CXF_OK) return CXF_ERR_NOMEM;
    w->order.len = 0;

    r = build_graph(w, &edges);
    if (r != CXF_OK) { cxf_vec_free(&edges); return r; }

    /* 建图、入度表、拓扑序三块都在同一个函数里申请与释放，且**用同一个值** ——
     * 上一轮的记账漂移就是忘了这一条。csr 自己是一整块（off 在前、adj 在后），
     * 所以只申请一次；它的字节数**在申请时存进 csr_bytes**，而不是释放时重算：
     * 释放点旁边 `cxf_vec_free(&edges)` 会把 edges.len 清零，重算必然偏小。 */
    csr_bytes = cxf_csr_bytes(n, edges.len);
    csr_mem = cxf_alloc_alloc(w->alloc, csr_bytes, CXF_MIN_ALIGN);
    indeg   = cxf_alloc_alloc(w->alloc, (size_t)n1 * 4, CXF_MIN_ALIGN);
    topo    = cxf_alloc_alloc(w->alloc, (size_t)n1 * 4, CXF_MIN_ALIGN);
    if (csr_mem == NULL || indeg == NULL || topo == NULL) { r = CXF_ERR_NOMEM; goto done; }

    /* 越界端点在装配期响亮失败（C2），不静默丢弃。 */
    r = cxf_csr_from_edges(&csr, csr_mem, n,
                          cxf_vec_data(&edges, cxf_csr_edge), edges.len);
    if (r != CXF_OK) goto done;
    CXF_ASSERT(cxf_csr_check(&csr));

    /* 有环 ⇒ DEP（M2）。初始零入度按升序入队 ⇒ 同一张图永远同一条序列。 */
    r = cxf_algo_toposort(&csr, indeg, topo);
    if (r != CXF_OK) goto done;

    /* refcount = 出度：有多少模块依赖我，就有多少条边从我出发。
     * 原来是对边表逐条 ++，现在是 O(n) 直接读区间长度。 */
    for (uint32_t i = 0; i < n; i++)
        w->modules[i].refcount = w->modules[i].loaded ? cxf_csr_deg(&csr, i) : 0;

    split_by_phase(w, topo, n);

    /* 按执行顺序 init，只初始化尚未 init 的模块，因此 start 可重复调用。 */
    for (uint32_t k = 0; k < w->order.len; k++) {
        cxf_module *m = &w->modules[ORDER(w)[k]];
        if (m->inited || m->def->init == NULL) continue;
        r = m->def->init(w, m->state);
        if (r != CXF_OK) goto done;
        m->inited = true;
    }

    w->started = true;

done:
    cxf_vec_free(&edges);
    if (csr_mem != NULL) cxf_alloc_free(w->alloc, csr_mem, csr_bytes);
    if (indeg != NULL) cxf_alloc_free(w->alloc, indeg, (size_t)n1 * 4);
    if (topo != NULL) cxf_alloc_free(w->alloc, topo, (size_t)n1 * 4);
    return r;
}

void cxf_world_tick(cxf_world *w) {
    if (!w->started) return;

    cxf_arena_push(&w->arena_frame);

    for (uint32_t p = 0; p < CXF_PHASE_COUNT; p++) {
        uint64_t t0 = now_ns();
        for (uint32_t k = w->phase_begin[p]; k < w->phase_begin[p + 1]; k++) {
            cxf_module *m = &w->modules[ORDER(w)[k]];
            if (m->def->tick != NULL) m->def->tick(w, m->state, &w->arena_frame);
        }
        w->phase_ns[p] = now_ns() - t0;
    }

    cxf_arena_pop(&w->arena_frame);
    w->tick_count++;
}

void cxf_world_stop(cxf_world *w) {
    for (uint32_t k = w->order.len; k-- > 0;) {
        cxf_module *m = &w->modules[ORDER(w)[k]];
        if (!m->inited || m->def->deinit == NULL) continue;
        m->def->deinit(w, m->state);
        m->inited = false;
    }
    w->started = false;
}

uint64_t cxf_world_phase_ns(const cxf_world *w, cxf_phase p) {
    return p < CXF_PHASE_COUNT ? w->phase_ns[p] : 0;
}

#ifndef NDEBUG
bool cxf_world_check(const cxf_world *w) {
    uint32_t in_order = 0;

    if (w == NULL) return false;

    for (uint32_t i = 0; i < w->module_count; i++) {
        const cxf_module *m = &w->modules[i];
        if (!m->loaded) continue;
        if (m->def->phase != CXF_PHASE_NONE && m->def->phase >= CXF_PHASE_COUNT)
            return false;                                        /* M5 */
        if (m->def->state_size != 0 && m->state == NULL) return false;  /* M4 */

        for (uint32_t j = 0; j < i; j++) {
            if (!w->modules[j].loaded) continue;
            if (strcmp(w->modules[j].def->name, m->def->name) == 0)
                return false;                                    /* M1 */
        }
    }

    for (uint32_t p = 0; p < CXF_PHASE_COUNT; p++) {
        if (w->phase_begin[p] > w->phase_begin[p + 1]) return false;   /* W4 单调 */
        for (uint32_t k = w->phase_begin[p]; k < w->phase_begin[p + 1]; k++) {
            const cxf_module *m = &w->modules[ORDER(w)[k]];
            if (m->def->phase != p) return false;                /* W2 分段 */
            in_order++;
        }
    }
    if (w->phase_begin[0] != 0) return false;                    /* W4 */
    if (w->phase_begin[CXF_PHASE_COUNT] != w->order.len) return false;

    if (w->started) {
        uint32_t ticking = 0;
        for (uint32_t i = 0; i < w->module_count; i++)
            if (w->modules[i].loaded && w->modules[i].def->phase != CXF_PHASE_NONE)
                ticking++;
        if (in_order != ticking) return false;                   /* W1 */
    }

    for (uint32_t i = 0; i < w->type_count; i++) {
        if (w->types[i].name == NULL || w->types[i].type == NULL) return false;
        for (uint32_t j = 0; j < i; j++)                         /* W5 名字唯一 */
            if (strcmp(w->types[j].name, w->types[i].name) == 0) return false;
    }

    return cxf_arena_check(&w->arena_state) && cxf_arena_check(&w->arena_frame);
}
#endif
