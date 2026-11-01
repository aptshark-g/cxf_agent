#include <stdio.h>
#include <string.h>

#include "cxf/service.h"

#include "agent_caps.h"

/*
 * 能力实现 + 能力契约。
 *
 * 四个能力、五个实现：
 *   travel.train      api    脚本，但要出网（真身查 12306）
 *   market.secondhand llm    模型，出网
 *   price.quote       parse  本地解析器，只读本地缓存 → LOCAL
 *                     llm    模型实现 → IO，是 parse 的降级目标
 *   plan.merge        rules  纯规则聚合 + 偏好消解 → LOCAL
 *
 * 五者对编排层完全同形 —— 这正是要证明的事：LLM 在这里是管线里的一个
 * 计算原语，不是 agent 的大脑。
 *
 * 成本档与信任级刻意不同轴：plan.merge 最便宜也最安全，market.secondhand
 * 最贵且要出网，price.quote 两个实现在"钱"和"权"上都差一个档 —— 这样
 * 天花板一降，能看见的能力集就真的会缩，测得出东西。
 */

uint32_t cap_mock_price(const char *s, uint32_t len, uint32_t base, uint32_t span) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += (uint32_t)(unsigned char)s[i];
    return base + (sum % span);
}

/* 有界子串查找。args 来自 blob，不保证 NUL 结尾，所以不能用 strstr。 */
static bool has_substr(const char *s, uint32_t n, const char *needle) {
    uint32_t m = (uint32_t)strlen(needle);

    if (m == 0 || n < m) return false;
    for (uint32_t i = 0; i + m <= n; i++)
        if (memcmp(s + i, needle, m) == 0) return true;
    return false;
}

/* 输出 "<tag>|<price>"。聚合节点按这个格式解析，格式约定写在聚合 handler 里。 */
static cxf_result quote(const char *tag, uint32_t price,
                        char *out, uint32_t out_cap, uint32_t *out_len) {
    int n = snprintf(out, (size_t)out_cap, "%s|%u", tag, price);
    if (n < 0 || (uint32_t)n >= out_cap) return CXF_ERR_FULL;
    *out_len = (uint32_t)n;
    return CXF_OK;
}

/* travel.train / api：脚本化能力。固定性强、可枚举，因此不该交给模型。 */
static cxf_result cap_train_exec(const void *ctx, const char *args, uint32_t args_len,
                                 char *out, uint32_t out_cap, uint32_t *out_len) {
    (void)ctx;

    return quote("12306", cap_mock_price(args, args_len, 300, 500),
                 out, out_cap, out_len);
}

/*
 * market.secondhand / llm：mock LLM handler。
 * 真实版是远程 API（多轮调研）；这里用确定性输出，保证 tick 可复现 ——
 * 要断言状态机推进，任何随机性都会让断言失去意义。
 */
static cxf_result cap_secondhand_exec(const void *ctx, const char *args, uint32_t args_len,
                                      char *out, uint32_t out_cap, uint32_t *out_len) {
    (void)ctx;

    return quote("xianyu", cap_mock_price(args, args_len, 200, 300),
                 out, out_cap, out_len);
}

/*
 * price.quote / parse：本地解析器。
 * 命中本地缓存（入参里带 "cache"）才算成功 —— 于是同一个能力在不同信任
 * 天花板下会走出两条路：命中就走本地脚本，未命中升级到模型；而天花板不
 * 允许升级时如实失败。这就是"离线模式"的最小可测形态。
 */
static cxf_result cap_quote_parse_exec(const void *ctx, const char *args, uint32_t args_len,
                                       char *out, uint32_t out_cap, uint32_t *out_len) {
    (void)ctx;

    if (!has_substr(args, args_len, "cache")) return CXF_ERR_DEP;   /* 缓存未命中 */

    return quote("cached", cap_mock_price(args, args_len, 150, 250),
                 out, out_cap, out_len);
}

/*
 * price.quote / llm：模型实现（parse 的降级目标）。
 * 输出刻意很长（含填充），用来验证 result blob 的按需增长。
 */
static cxf_result cap_quote_llm_exec(const void *ctx, const char *args, uint32_t args_len,
                                     char *out, uint32_t out_cap, uint32_t *out_len) {
    (void)ctx;

    uint32_t price = cap_mock_price(args, args_len, 200, 300);
    uint32_t pad = 560u;
    int n = snprintf(out, (size_t)out_cap, "quote.llm|%u|", price);

    if (n < 0 || (uint32_t)n >= out_cap) return CXF_ERR_FULL;
    if ((uint32_t)n + pad > out_cap) return CXF_ERR_FULL;
    memset(out + n, 'x', pad);
    *out_len = (uint32_t)n + pad;
    return CXF_OK;
}

/*
 * plan.merge / rules：聚合 + 偏好消解。纯规则，因此 LOCAL。
 * 子项结果按行拼接，每行 "<tag>|<price>[|…]"；取每行**第一个** '|' 之后
 * 紧跟的数字，因此尾部填充不会干扰解析。
 */
static cxf_result cap_merge_exec(const void *ctx, const char *args, uint32_t args_len,
                                 char *out, uint32_t out_cap, uint32_t *out_len) {
    (void)ctx;

    uint32_t best = UINT32_MAX;
    char best_tag[16];
    uint32_t i = 0;
    int n;

    memset(best_tag, 0, sizeof(best_tag));

    while (i < args_len) {
        uint32_t e = i;
        uint32_t price = 0;
        const char *bar = NULL;

        while (e < args_len && args[e] != '\n') e++;

        for (uint32_t k = i; k < e; k++)
            if (args[k] == '|') { bar = &args[k]; break; }

        if (bar != NULL) {
            for (const char *p = bar + 1; p < args + e && *p >= '0' && *p <= '9'; p++)
                price = price * 10u + (uint32_t)(*p - '0');
            if (price < best) {
                uint32_t taglen = (uint32_t)(bar - (args + i));
                if (taglen >= sizeof(best_tag)) taglen = sizeof(best_tag) - 1;
                memcpy(best_tag, args + i, taglen);
                best_tag[taglen] = '\0';
                best = price;
            }
        }
        i = e + 1;
    }

    if (best == UINT32_MAX) return CXF_ERR_RANGE;   /* 没有一个子项给出报价 */

    n = snprintf(out, (size_t)out_cap, "plan|cheapest|%s|%u", best_tag, best);
    if (n < 0 || (uint32_t)n >= out_cap) return CXF_ERR_FULL;
    *out_len = (uint32_t)n;
    return CXF_OK;
}

/* ---- 接口类型（第三个 id 空间）---- */

/*
 * 两个 kind 的分工就是"两张图互为对偶"的全部内容：
 *   EXT 只能被消费，不能被产出 —— 它是外部喂进来的起点；
 *   INT 只能被产出，且必须至少被一个能力产出 —— 否则就是悬空入口。
 * 注册期把这两条卡住，等于保证"类型图上搜出的路径"一定能原样展开成可执行的 DAG。
 */
static const IfaceDef iface_task = {
    IFACENAME_TASK, "任务的自然语言描述", IFACE_EXT
};
static const IfaceDef iface_quote = {
    IFACENAME_QUOTE, "一项可比价的报价", IFACE_INT
};
static const IfaceDef iface_plan = {
    IFACENAME_PLAN, "消解后的最终方案", IFACE_INT
};

/* ---- 契约（目录项）---- */

/*
 * 散文（in_schema / out_schema）给人看，词表（in_iface / out_iface）给机器看。
 * plan.merge 的输入刻意写两次 quote.item —— 可重复即多端口，这就是 Petri 网
 * 语义里那条 hyper-edge（多个输入 state → 一个输出 state）。
 */
static const CapDef capdef_travel_train = {
    CAPNAME_TRAIN, "火车票直购报价", "出发地 目的地 日期", "<tag>|<price>",
    { IFACENAME_TASK, NULL }, IFACENAME_QUOTE
};
static const CapDef capdef_market_secondhand = {
    CAPNAME_SECONDHAND, "二手平台行情调研", "商品关键词", "<tag>|<price>",
    { IFACENAME_TASK, NULL }, IFACENAME_QUOTE
};
static const CapDef capdef_price_quote = {
    CAPNAME_QUOTE, "商品报价", "商品描述（本地缓存命中时用本地解析）", "<tag>|<price>",
    { IFACENAME_TASK, NULL }, IFACENAME_QUOTE
};
static const CapDef capdef_plan_merge = {
    CAPNAME_MERGE, "多路结果聚合 + 偏好消解（最便宜胜出）",
    "若干行 <tag>|<price>", "plan|cheapest|<tag>|<price>",
    { IFACENAME_QUOTE, IFACENAME_QUOTE, NULL }, IFACENAME_PLAN
};

/* ---- 实现 ---- */
/* 降级目标（llm）必须先声明：parse 的 fallback 指向它。 */

static const CapImpl cap_price_quote_llm = {
    CAPNAME_QUOTE, "llm", CAP_TIER_LLM, 80, CAP_TRUST_IO,
    cap_quote_llm_exec, NULL, 0u, NULL
};
static const CapImpl cap_price_quote_parse = {
    CAPNAME_QUOTE, "parse", CAP_TIER_SCRIPT, 5, CAP_TRUST_LOCAL,
    cap_quote_parse_exec, NULL, 0u,
    &cap_price_quote_llm      /* 链头是 parse：本地优先，不成才出网 */
};
static const CapImpl cap_travel_train_api = {
    CAPNAME_TRAIN, "api", CAP_TIER_SCRIPT, 10, CAP_TRUST_IO,
    cap_train_exec, NULL, 0u, NULL
};
static const CapImpl cap_market_secondhand_llm = {
    CAPNAME_SECONDHAND, "llm", CAP_TIER_LLM, 90, CAP_TRUST_IO,
    cap_secondhand_exec, NULL, 0u, NULL
};
static const CapImpl cap_plan_merge_rules = {
    CAPNAME_MERGE, "rules", CAP_TIER_SCRIPT, 1, CAP_TRUST_LOCAL,
    cap_merge_exec, NULL, 0u, NULL
};

/* 注册项只能写在实现文件里：CXF_SERVICE 展开成外部链接的常量。 */
CXF_SERVICE(iface_task, "iface/task.text", 1, &iface_task);
CXF_SERVICE(iface_quote, "iface/quote.item", 1, &iface_quote);
CXF_SERVICE(iface_plan, "iface/plan.result", 1, &iface_plan);

CXF_SERVICE(capdef_travel_train, "cap/travel.train", 1, &capdef_travel_train);
CXF_SERVICE(capdef_market_secondhand, "cap/market.secondhand", 1, &capdef_market_secondhand);
CXF_SERVICE(capdef_price_quote, "cap/price.quote", 1, &capdef_price_quote);
CXF_SERVICE(capdef_plan_merge, "cap/plan.merge", 1, &capdef_plan_merge);

CXF_SERVICE(capimpl_price_quote_llm, "impl/price.quote.llm", 1, &cap_price_quote_llm);
CXF_SERVICE(capimpl_price_quote_parse, "impl/price.quote.parse", 1, &cap_price_quote_parse);
CXF_SERVICE(capimpl_travel_train_api, "impl/travel.train.api", 1, &cap_travel_train_api);
CXF_SERVICE(capimpl_market_secondhand_llm, "impl/market.secondhand.llm", 1, &cap_market_secondhand_llm);
CXF_SERVICE(capimpl_plan_merge_rules, "impl/plan.merge.rules", 1, &cap_plan_merge_rules);

/*
 * `agent.action` 与 `agent.step` 的契约。**刻意没有 `CXF_SERVICE` 注解** ——
 * 它们不进 bundle，必须由使用方在运行时与实现一起注册（不变式 7：控制流类型
 * 的产出者要跟类型同一次 sync 到达）。声明在 agent_caps.h，理由见那里。
 */
const IfaceDef iface_action = {
    IFACENAME_ACTION, "下一步动作（控制流，不是数据）", IFACE_INT
};
const CapDef capdef_step = {
    CAPNAME_STEP, "回路的一步：产出下一步动作或终值",
    "回路状态", "kind│cap│args（见 agent_action.h）",
    { IFACENAME_TASK, NULL }, IFACENAME_ACTION
};
