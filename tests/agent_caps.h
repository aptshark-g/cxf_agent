#ifndef AGENT_CAPS_H
#define AGENT_CAPS_H

#include <stdint.h>

#include "cxf/base.h"

/*
 * 能力（Capability）分两层注册。
 *
 *   契约 CapDef  —— 目录里的一项：名字 / 一句话描述 / 入出参契约。
 *   实现 CapImpl —— 真正跑的东西：tier / cost / trust / exec / 降级链。
 *
 * 为什么必须拆：L1 分解与 L0 渲染**只该看见能力，不该看见实现**。
 * 同一个"商品报价"能力可以有一个老解析器实现和一个模型实现；规划器不需要知道
 * 谁在跑，也不该知道 —— 那是路由（MATCH）的事。把两者塞进一个名字
 * （"quote.script" / "quote.llm"）会逼着分解器替路由做决定：
 * 它得先知道谁便宜、谁还没坏，而这两件事都是运行时事实。
 * 拆开之后，计划里出现的是 price.quote，选谁跑是 MATCH 的职责。
 *
 * 与 §3 的两平面对应：契约 = 目录平面（慢变，带世代），
 * 实现是否可用（`trust` 与将来的健康度）= 本 tick 的可用性判定。
 *
 * handler 拿不到 world，也就拿不到别人的数据：能力之间靠 WorkItem 的
 * args / result 字节通信，不靠互相调用（docs/agent.md §2）。
 */

typedef enum CapTier {
    CAP_TIER_SCRIPT = 0,   /* 纯 C 算法，确定性、近零成本 */
    CAP_TIER_LLM    = 1,   /* 模型调用，有延迟与费用 */
} CapTier;

/*
 * 信任级：这个实现**能做什么**，不是它花多少钱。
 * 与 cost 正交 —— 便宜的可能危险（删文件），贵的一般无害（大模型调用）。
 *
 * 它同时是两件事的判据（docs/agent.md §7 信任边界）：
 *   1. 注册放行：LOCAL 注册即可跑；IO 需附测试证据；WRITE 人审。
 *   2. 请求天花板：一个请求声明自己的 trust_max，分解期就只挑得动得着的实现。
 * 之所以卡在分解期而不是派发期，是因为分解期决定"做什么"；
 * 选不中的能力根本不会进 DAG，派发期就不用再判一次。
 * 但降级链会在派发期换实现，所以天花板必须随 WorkItem 走到底（见 agent_types.h）。
 */
typedef enum CapTrust {
    CAP_TRUST_LOCAL = 0,   /* 纯计算 / 只读本机数据 */
    CAP_TRUST_IO    = 1,   /* 出网 / 读盘 */
    CAP_TRUST_WRITE = 2,   /* 写盘 / 支付 / 对外副作用 */
} CapTrust;

/*
 * 容量与 id 空间。三个稠密 id 空间并列，各自独立：cat_id / cap_id / iface_id。
 * IFACE_MAX 与 CAP_MAX 数值相同只是巧合，分开写是为了将来能各自调。
 */
#define CAP_IN_MAX  4u          /* 一个能力最多几个输入端口 */
#define IFACE_MAX   16
/* 与 CAP_NONE 同值：都是"没有"。分名只为读起来不串味。 */
#define IFACE_NONE  0xFFFFFFFFu

/*
 * 接口类型（Interface）—— 第三个稠密 id 空间，与 cat_id / cap_id 并列而不混用。
 *
 * 为什么不能复用框架的布局类型 `cxf_type`：那是"值的内存形状"（AoS↔SoA 怎么切列），
 * 而这里是"数据流的端口"。三条理由，第三条是硬的：
 *   1. 粒度不同 —— 布局类型精确到字段偏移；接口类型是语义单位，可粗可细、可有抽象；
 *   2. 语义角色不同 —— 两个结构体字段恰好同名同型，不代表语义兼容
 *      （price 在火车票里是票价、在二手相机里是卖家标价），布局类型分辨不了；
 *   3. **生命周期冲突** —— 宪法说"类型永不卸载"，而能力必须热插拔。若两者是同一个
 *      东西，插件带来的新接口类型就永远卸不掉，注册表随插拔单调膨胀，
 *      热插拔承诺反而被类型系统吃掉。
 *
 * 它是**可分解性的前提**，不是装饰：没有接口，就判断不了"拆开之后能不能接上"，
 * 系统里就只剩模型路由。有了它，"任务 → 能力 DAG"退化成一张类型图上的可达性：
 * 类型是节点，能力是边（TTN，见 docs/l1-decomposer.md §3.1）。
 */
typedef enum IfaceKind {
    IFACE_EXT = 0,   /* 外部输入（起点）：无人产出，由 INJECT 提供 */
    IFACE_INT = 1,   /* 能力产出（内部）：必须至少被一个能力产出 */
} IfaceKind;

typedef struct IfaceDef {
    const char *name;   /* 点分："quote.item"。风格与能力名同族，但 id 空间独立 */
    const char *desc;   /* 一句话。渲染（§7.1）与检索（§1.4：字段名是最强判别词）都用它 */
    uint8_t     kind;   /* IfaceKind */
} IfaceDef;

/* 契约。目录里的一项 = 一个能力名 + 一句话 + 入出参契约。 */
typedef struct CapDef {
    const char *name;        /* 目录名，点分构成命名空间："travel.train" */
    /* 一句话描述。渲染进 L0；可能由 LLM 写 → 渲染时必须当**数据**处理，
       不能当可信指令（§7.1：防夹带污染规划）。 */
    const char *desc;
    /* 人看的散文契约。消费者是模型（渲染进工具卡），**机器不校验它**。
       机器契约是下面的 in_iface / out_iface —— 两份并存不是冗余：
       散文说"这个能力干什么"，词表说"它能接到哪"。 */
    const char *in_schema;
    const char *out_schema;

    /* 机器契约：入/出**接口类型名**（必须是已注册的 IfaceDef.name）。
       名字写错在注册期就报错，不会静默变成一个"没人产出的新类型" ——
       这与降级链成环、能力名点前缀冲突是同一类处理：注册期响亮失败。
     in_iface 是有序列表、可重复（两个同名 = 两个输入端口），NULL 结尾。
       第一版是定长语义（列几个就要几个）；变长（≥N 个）留到真有需求再表示。
     out_iface 为 NULL 表示这个能力没有产出（纯副作用能力）。 */
    const char *in_iface[CAP_IN_MAX];
    const char *out_iface;
} CapDef;

/* 实现。注册表按前缀枚举的就是它。 */
typedef struct CapImpl {
    const char *capability;  /* 实现的是哪个能力（CapDef.name 的同一字面量） */
    const char *impl;        /* 实现名，同一能力内唯一（"parse" / "llm" / "api"） */
    uint8_t     tier;        /* CapTier */
    uint8_t     cost;        /* 成本档 0..255：路由与投机政策用（§4.7） */
    uint8_t     trust;       /* CapTrust */

    /*
     * out_cap 不足返回 CXF_ERR_FULL —— 调用方据此扩大重试，不是静默截断。
     *
     * 第一个参数是 `ctx`：**装配期给定的实现参数**，执行层原样透传、不做解释。
     * 为什么必须有它：handler 拿不到 world（宪法第 5 条：一切挂显式 world），
     * 也不许读文件级全局（那会让同一个能力在同一个进程里只能有一份配置，
     * 且"这个 handler 连了哪里"变成需要翻源码才知道的事）。真实实现需要
     * 端点 / 密钥 / 超时这类参数，唯一的合法来源就是描述符本身 ——
     * 于是"哪个实现用什么配置"成为注册期事实，运行期既不查表也不猜。
     *
     * 边界：当前部署只认一个 OpenAI 兼容端点（指向谁由部署决定），所以
     * 进程内只有一份网关配置。这不是 ctx 的局限 —— 要多端点，构造两份
     * 描述符、ctx 各指一处即可，机制上已经允许。
     */
    cxf_result (*exec)(const void *ctx, const char *args, uint32_t args_len,
                       char *out, uint32_t out_cap, uint32_t *out_len);
    const void *ctx;

    /*
     * 输出起始容量提示。0 = 用 AGENT_RESULT_MIN。
     *
     * 这是 A1（容量契约）里**便宜的那一半**：真实模型输出长度不可预测，
     * 起始容量不足会触发"扩容重跑"，而重跑的返回字节**可能与第一次不同**
     * （采样随机性 / MoE 路由 / 批处理顺序）—— 于是同一任务的汇聚结果
     * 不再确定。那已经不是性能问题而是正确性问题，所以凡是输出长度
     * 不可预测的实现，都用它把起始容量一次给够。
     *
     * 贵的另一半（CapRerun 枚举 + 成本档超限直接失败）留到真有 COSTLY
     * 实现时再做：现在只有"重跑"一种行为，加枚举等于给一个取值。
     *
     * 之所以是**描述符字段**而不是运行时猜：这是注册期就知道的事实。
     * 凡是本可以提前知道的，别让它退化成运行时数据。
     */
    uint32_t out_hint;

    /*
     * 同能力的下一个实现（降级链，§8 的语义决策）。NULL 表示链到此为止。
     *
     * 三条注册期不变式（registry_sync 验证，违反即注册失败）：
     *   1. 只能指向**同能力**的实现 —— 跨能力的"降级"是另一件事（通用兜底能力），
     *      混进来会让"一个能力一张链"的前提垮掉；
     *   2. 链必须**无环**（环意味着某个实现被另一个指向，链头不唯一）；
     *   3. 链在同一能力内必须**单头**（恰好一个实现不被同能力的任何实现指向）
     *      —— 这个头就是该能力的首选实现。
     * 于是运行时不需要"重试次数"兜底：链有限且无环，走一步少一步。
     * WorkItem.tries 因此从"兜底"退化为"观测"（走了几个实现）。
     */
    const struct CapImpl *fallback;
} CapImpl;

/*
 * 服务名前缀。三者互不为前缀，注册表按它枚举。
 *   接口  iface/<name>                例：iface/quote.item
 *   契约  cap/<capability>            例：cap/price.quote
 *   实现  impl/<capability>.<impl>    例：impl/price.quote.llm
 *
 * 键里的点与目录里的点同形，可读可 grep；但**分组不靠解析字符串**，
 * 靠描述符自己的字段。命名与机制分离：改名不会改行为。
 */
#define CAPDEF_PREFIX     "cap/"
#define CAPIMPL_PREFIX    "impl/"
#define IFACE_PREFIX      "iface/"
#define CAPDEF_PREFIX_SZ  (sizeof(CAPDEF_PREFIX) - 1u)
#define CAPIMPL_PREFIX_SZ (sizeof(CAPIMPL_PREFIX) - 1u)
#define IFACE_PREFIX_SZ   (sizeof(IFACE_PREFIX) - 1u)

/* id 由注册表按序稠密分配（先扫到的先得），因此 MATCH 是数组索引而非查找。
   运行时新注册的能力排在末尾，既有 id 不受影响。 */
#define CAP_MAX 16

/*
 * 类型图邻接链的节点空间：每个**能力**占 CAP_IN_MAX+1 个节点 ——
 * 第 0 个是它的产出端口，第 1..CAP_IN_MAX 个是它的输入端口。
 *
 * 两条都踩过坑，所以写下来：
 *   1. 节点按端口编址，不按能力编址 —— cap_in 允许同一类型填两次（plan.merge
 *      就是两个 quote.item 端口）。若链按能力索引，两个端口会写同一个 next 槽，
 *      第二个端口把它指回自己，遍历立刻自环挂死。
 *   2. 图建在**契约**（cat_id）上，不建在实现（cap_id）上 —— 端口是契约声明的，
 *      一个能力有几个实现与"它能接到哪"无关。若建在实现上，price.quote 有两个
 *      实现就会在图里出现两次，fan-in 要等的条数凭空翻倍。
 *      顺带这也让 L1 不必看见实现，与"分解只看契约"完全一致。
 */
#define CAT_NODES (CAP_MAX * (CAP_IN_MAX + 1u))

/* 无效 id。取 UINT32_MAX 而不是 0：0 是合法 id，用 0 表示"没有"
   会让第一个能力无法与"缺失"区分。 */
#define CAP_NONE 0xFFFFFFFFu

/* 能力名（目录名）。点分 = 命名空间；§7.2 的联邦目录按它分工（一空间一 curator）。 */
#define CAPNAME_TRAIN      "travel.train"
#define CAPNAME_SECONDHAND "market.secondhand"
#define CAPNAME_QUOTE      "price.quote"
#define CAPNAME_MERGE      "plan.merge"

/*
 * 接口类型名。刻意只有三个 —— 足以让类型图有分叉、有汇聚、有一个终点，
 * 又不至于让"哪个能力接哪个"变成需要查表才能读懂的事：
 *
 *      task.text ──┬── travel.train ──────┐
 *                 ├── market.secondhand ─┼── plan.merge ──> plan.result
 *                 └── price.quote ───────┘
 *
 * 三者都是"一项报价"而不是三个不同类型，这是刻意的：火车票报价与二手相机
 * 报价在**结构上同形**（都是 <tag>|<price>），差异在语义而不在形状 ——
 * 而语义差异正是 nominal 类型该管的（名字等价，不是结构等价）。若走结构等价，
 * 这三个能力会因为字段恰好一样而互相错误地可替换。
 */
#define IFACENAME_TASK  "task.text"     /* 外部输入：任务的自然语言描述 */
#define IFACENAME_QUOTE "quote.item"    /* 中间：一项可比价的报价 */
#define IFACENAME_PLAN  "plan.result"   /* 终点：消解后的最终方案 */

/*
 * 第四个接口类型：**控制流**（docs/agent.md §4.8）。
 *
 * 前面三个携带的都是"数据"（任务是数据、报价是数据、方案是数据），
 * 这一个携带的是"下一步做什么"—— 它的消费者是**运行时**（INJECT），
 * 不是某个能力。所以它有一个不对称，值得记下：
 *   不变式 7/8 只要求 INT 有人产出、没要求有人消费；
 *   `agent.action` 恰好落在"只有能力侧一个端点"这一类上。
 * 这不是漏洞，它指出类别：**控制流类型天然只有半个端点**。
 *
 * 判别字段（CALL / DONE）在 payload 内部，不在类型上 —— 见 agent_action.h。
 */
#define IFACENAME_ACTION "agent.action" /* 控制流：下一步动作或终值 */

/*
 * 能力名。从 `agent.h` 移来：它们是**契约命名空间**的一部分，与 `IFACENAME_*`
 * 同源。移过来的实际理由是 `agent_caps.c` 要定义 `capdef_step`（两个调用方共用
 * 一份契约），而 `agent.h` 依赖本文件 —— 名字留在这边，依赖方向才不倒。
 *
 *   CAPNAME_LATE  使用方运行时注册（契约 + 实现）—— 用来证明目录与注册表都是动态的。
 *   CAPNAME_STEP  §4.8 的回路步进能力；同样由使用方运行时注册。
 *   CAPNAME_FS    §4.15.1 对比台的胜者转正：目录里第一个**真实**实现。
 */
#define CAPNAME_LATE "demo.late"
#define CAPNAME_STEP "agent.step"
#define CAPNAME_FS   "fs.grep"

/*
 * 第二个 fs 能力，也是目录里第一个**会改变外部状态**的能力（§4.18.6）。
 *
 * 名字放在这里而不是 `agent_fs.h`，因为计划表（`agent.c` 的 `plan_specs`）
 * 要按名字引用它，而运行时**不该 include 某个能力的实现头** —— 那会让
 * "运行时 → 具体能力"成为一条编译期依赖，于是加一个能力就要改运行时。
 * 能力名属于契约命名空间，与 `IFACENAME_*` 同源：它们是**字符串**，
 * 运行时只按名字查目录，从不认识实现。
 */
#define CAPNAME_FS_WRITE "fs.write_new"

/* 「如果换成这些字节会怎样」—— 零副作用。dry-run 不是开关，是能力（§4.18.7）。 */
#define CAPNAME_FS_DIFF  "fs.diff"

/* 「使它恰好等于这些字节」—— 目录里第一个**不可逆**的能力（§4.18.8）。
 * 与 write_new 分开：那个只在路径不存在时动世界（失败可 unlink、重跑即无事），
 * 这一个会把旧内容毁掉 —— 两者该不该被同一个部署放行，是两个问题。 */
#define CAPNAME_FS_APPLY "fs.write_apply"

/* 「往末尾再接一段」—— 与 apply 的分水岭是**新内容依赖旧内容**（§4.18.9）。
 * 单独成为一个能力而不是给 apply 加参数：append 的语义是"在既有内容之后"，
 * 而 apply 的语义是"恰好等于这些字节"，两者对"旧内容是什么"的依赖正好相反
 * （apply 不关心、append 必须关心）。 */
#define CAPNAME_FS_APPEND "fs.append"

/* 「把这个交出去」—— 目录里第一个**副作用发生在我们的世界之外**的能力（§4.18.10）。
 * 它单独成为一个能力而不是给 fs 族加参数：失败不可撤销、且带凭据，
 * 这两件事都不属于"文件"这个域。 */
#define CAPNAME_HTTP_POST "http.post"

/*
 * `agent.action` 的契约 + `agent.step` 的契约，**不在静态 bundle 里**。
 *
 * 它们必须由使用方在运行时与实现一起注册（§10.6 不变式 7：INT 必须有人产出，
 * 所以类型与它的产出者要在同一次 sync 里到达）。放这里而不是各自抄一份，
 * 是因为 `agent.step` 现在有**两个**调用方（`test_agent` 与 `agentctl`）——
 * 一份契约抄两遍，迟早两边对不上，而那种漂移的症状是"一边能路由、另一边选不中"。
 *
 * 定义在 `agent_caps.c`，但**没有 `CXF_SERVICE` 注解**，所以不进 bundle。
 */
extern const IfaceDef iface_action;
extern const CapDef   capdef_step;

/*
 * 确定性定价：把 args 映射成一个价格。
 * 让 mock handler 是"算法"而不是常量 —— 改了入参结果就变，测得出东西。
 */
uint32_t cap_mock_price(const char *s, uint32_t len, uint32_t base, uint32_t span);

#endif
