#ifndef AGENT_H
#define AGENT_H

#include <stdbool.h>
#include <stdint.h>

#include "cxf/alloc.h"
#include "cxf/arena.h"
#include "cxf/handle.h"
#include "cxf/module.h"
#include "cxf/ring.h"
#include "cxf/service.h"
#include "cxf/soa.h"
#include "cxf/str.h"
#include "cxf/table.h"
#include "cxf/type.h"
#include "cxf/world.h"

#include "agent_caps.h"

/*
 * agent 运行时的模块切分（docs/agent.md §4.1 / §6.3）。
 *
 * 一个模块占一个内核 phase：INJECT→INPUT，MATCH→UPDATE，EXECUTE→LATE，
 * RECYCLE→LAST。phase 枚举保持中性，内核不认 agent 语义；红利是
 * world.phase_ns[] 直接给出四段耗时，可观测是白拿的。
 *
 * 共享状态（WorkItem SoA、blob 池、目录/注册表、入口 ring）由 agent_store 持有，
 * 经服务表固化到各模块的 state —— 装配期 find 一次，调用期零查找。
 *
 * 注册表分三层（§3 目录平面 / §7.1 渲染 / l1-decomposer §3.1）：
 *   接口 IfaceDef —— 数据流的端口类型，能力之间靠它接得上；
 *   契约 CapDef   —— 目录项，L1 与 L0 只看见它；
 *   实现 CapImpl  —— cap_id 指向它，MATCH 在它之间做可用性判定与降级。
 * 三层各有自己的稠密 id 空间（iface_id / cat_id / cap_id），互不混用。
 *
 * 两层能力（契约 / 实现）解决的是"同一件事谁来做"，第三层（接口）解决的是
 * "不同的事怎么接起来" —— 前者是选择，后者是结构。没有接口就没有 DAG，
 * 只剩模型路由（docs/l1-decomposer.md §0）。
 */

/* demo 的查询串。放在这里是为了让测试与 INJECT 用同一份字面量 ——
 * 价格由确定性算法算出，测试要能独立复算出同样的值。 */
#define AGENT_Q_12306  "北京 上海 2026-10-20"
#define AGENT_Q_XIANYU "二手相机 咸鱼"
#define AGENT_Q_QUOTE  "iPhone 17 报价"          /* 本地缓存未命中 → 需升级到模型 */
#define AGENT_Q_CACHED "iPhone 17 报价 cache"    /* 本地缓存命中 → 本地脚本即可 */
#define AGENT_Q_LATE   "运行时注册的能力"
#define AGENT_Q_LOOP   "回路起点"

/* fs.grep 的 pattern。与其它 query 不同，它不是自然语言而是**检索键** ——
 * 因为 fs.grep 的 args 就是 pattern（见 agent_fs.h：路径不在 payload 里）。
 * 语料是测试现场生成的，这个串在其中恰好出现一次，于是"命中 1 行"可断言。 */
#define AGENT_Q_FS     "TOK_05"

/* fs.write_new 的 args：`path\ncontent`（见 agent_fs.h）。与上面的检索键不同，
 * 它是**两段**：第一行是相对路径，其后全部字节是内容 —— 所以这个字面量里
 * 那个换行不是格式噪音，它就是分隔符本身。 */
#define AGENT_Q_WRITE  "notes/out.txt\nhello write_new\n"

/* fs.diff 的 args：**同一方言**。它指向 write_new 建出来的那个文件，
 * 而内容只差一个词 —— 于是断言能同时说清两件事：对齐是对的（只改一行），
 * 而且这个能力读到的正是上一个能力写下的东西。 */
#define AGENT_Q_DIFF   "notes/out.txt\nhello diff_new\n"

/*
 * 能力名（`CAPNAME_*`）已移到 `agent_caps.h`：它们是**契约命名空间**的一部分，
 * 与 `IFACENAME_*` 同源。移过去是为了让 `agent_caps.c` 能定义
 * `capdef_step` 而不必反过来依赖本文件（本文件依赖 `agent_caps.h`）。
 *
 *   CAPNAME_LATE  使用方运行时注册（契约 + 实现）—— 用来证明目录与注册表都是动态的。
 *   CAPNAME_STEP  §4.8 的回路步进能力；同样由使用方运行时注册 ——
 *                 它产出的 `agent.action` 是一个**新接口类型**，与它的产出者
 *                 在同一次 sync 里到达，正好验证"类型 + 能力一起长出来"。
 *   CAPNAME_FS    §4.15.1 对比台的胜者转正：目录里第一个**真实**实现。
 */

/*
 * 闭环的硬上限（§4.8）。判据在**派生点**：派生出的条目 rounds 超过它，
 * 就不再派生，而是 `stat_failed++`。
 *
 * 为什么必须是 HARD 且必须计数：一个不收敛的回路的默认表现是"任务卡住"——
 * 界面上什么都不动，没有任何一条日志说它错了。把上限做成"静默停住"等于
 * 把最贵的一类 bug 藏进配置项。所以它超限时走的是**失败路径**，与其它失败
 * 同一条、同一个计数器。
 */
#define AGENT_MAX_ROUNDS  3u

/* 单 tick 可挂起的动作数。§4.8 的回路一次推进一轮，所以 1 就够；
 * 留几个位是因为"一轮里多个控制流条目同时就绪"是合法形状（分叉回路），
 * 超限时响亮失败而不是静默丢一条动作。 */
#define AGENT_ACT_BATCH   4u

#define AGENT_STORE_CAP    0x1u   /* 能力位：共享状态提供者 */
#define AGENT_STORE_SVC    "agent/store"
#define AGENT_INGEST_SVC   "agent/ingest"
#define AGENT_REGISTRY_SVC "agent/registry"

#define AGENT_ARGS_MIN     256u   /* args 起始容量，按需增长 */
/*
 * args **累积上限**（§4.16 末）。args 不是"一轮的输入"，而是**跨轮累积的上下文**：
 * 工具结果 append 进来、下一轮这份更大的 args 再进模型。没有上限，它就是一条
 * 随轮数线性增长的隐式预算 —— 而"给多少上下文"正是 §4.15 要测的 `b*`。
 *
 * 所以这里钉一个**显式**默认值。注意它只是**默认值**：真正生效的是
 * `AgentStore.args_budget`（见下），运行期可下调 —— 因为 b* 是靠"把预算调小、
 * 看哪一档开始出问题"测出来的，写死成常量就没法测。
 *
 * 64 KiB 是天花板不是目标值 —— 它该由 `b*` 的实测来调小，而不是靠猜。
 */
#define AGENT_ARGS_MAX     (64u * 1024u)
/* 入口载荷上限（§4.16 之后补）。一帧 = `sizeof(InjectMsg)` + text_len，必须装得进
 * `AGENT_INBOX_CAP`；超限**整帧消费掉再拒**并计数 —— 只读头会把载荷永久留在 ring 里，
 * 下一次读把头当载荷，整条入口从此错位（比拒绝坏得多）。 */
#define AGENT_INPUT_MAX    2048u
/*
 * 预算丢弃标记的最大长度。见 recycle_tick 里那段：整条丢弃时**必须留一行标记**
 * 进 args，它是"这段上下文被砍掉了"对模型与日志唯一同时可见的证据。
 * 长短：`#budget: dropped ` (17) + 10 位十进制 + `B\n` —— 32 字节够。
 */
#define AGENT_ARGS_NOTE_MAX 32u
/*
 * 汇聚记录（§4.16.3 立，v1.14.8 改写）——装配方往 args 里写的第二类元信息
 * （第一类是 `#budget`）。**v1.14.8 起它不再是一个"头"，而是一对记录：**
 *
 *     #call  <cap> <args_len>\n<args>\n#result <cap> <res_len>B\n<body>\n
 *     #call  <cap> <args_len>\n<args>\n#fail <seq> <cap> <why>\n
 *
 * `seq` 是**本轮第几条动作**（1 基，0 = 不在批次里）。v1.14.8 把调用字节
 * 放进记录之后，配对已经可靠；但"这一条是我写的第几条"仍然丢失 ——
 * 而段化（§4.18.16）要靠它决定"从第几段重来"。见 `WorkItem.seq` 的理由。
 * `why`（v1.14.24）是**哪一侧出错**（`WorkItem.err` 的词化），
 * 它把"没成"分成四条下一步动作相反的路。见 `WorkItem.err` 的理由。
 *
 * 为什么从"24 B 参数摘要的头"改成"完整调用记录"：v1.14.8 查出调用根本不在
 * transcript 里（只有结果的头），配对靠顺序，而 recycle_tick 倒序回收让那个顺序
 * 今天就已经不成立。修法是把 blob 渲染成**标准 tool-calling messages**
 * （调用→assistant(tool_calls)、结果→tool）—— 那是模型后训练过的形状，
 * 而 `tool` 消息在协议上必须挂在带 `tool_calls` 的 assistant 之后，
 * 所以"回传完整调用"是约束不是偏好。旧头里 `AGENT_RESULT_IN_MAX = 24` 的
 * 参数截断随之取消：调用身份由完整 args 承担，不再由前缀猜。
 *
 * 记录里所有变长区（args / body）都长度前缀自定界 ⇒ 内容里出现 "#result"
 * 字样也不会被误解析。唯一未加前缀的是开头的问题段（部署侧输入，与 v1.14.8
 * 之前同信任级）。记录不再过固定缓冲区：头与载荷分段直接 append 进父 args，
 * 因为 args 可以很长（`AGENT_ARGS_MAX`），任何定长头缓冲都会造出一个新的
 * "装不下就悄悄截断"的洞。
 */

/*
 * 配额（§4.17.8）：**两个正交的闸门**，不是一个"并发数"。
 *
 *   conc_max  瞬时并发上限（同时在飞几个）  —— 约束**瞬时占用**
 *   rpm       每分钟请求数                  —— 约束**区间总量**
 *
 * 为什么必须分开：K 个并发在飞 1 分钟能打 60K 次。只给并发数，等于对速率
 * 一无所知；只给 rpm，等于允许一分钟内全挤在第一秒。真实云端**两条都写**，
 * 所以这里也两条都收。
 *
 * 为什么是**可设置**而不是算出来的：RPM 是**买来的契约**（对方的合同），
 * 不是我们能观测到的事实 —— 能声明的东西就别去试探。AIMD 只能在它**下面**
 * 往下调，不能往上探，否则就是拿 429 当探针（每次试探代价是一次真实调用）。
 *
 * 单位是**毫令牌**（token × 1000）：桶的算术要能表达"每 6 秒一个"，整数除法
 * 会把亚令牌的部分抹掉，而抹掉的部分恰好是长期速率偏差的来源。
 */
typedef struct AgentQuota {
    uint32_t conc_max;    /* 同时允许几个。0 = 不限（也是"还没有在飞集合"时的唯一合法值） */
    uint32_t rpm;         /* 每分钟请求数。0 = 不限 */
    uint32_t tok_milli;   /* 令牌桶当前值，单位 1/1000 个请求 */
    uint64_t last_ns;     /* 上次补充时刻（由注入的时钟给） */
} AgentQuota;
#define AGENT_RESULT_MIN   128u   /* result 起始容量，不足则翻倍重试 */
#define AGENT_RESULT_TRIES 5u     /* 单次执行内允许的扩容重试上限 */

/*
 * 一次动作指针最多派生几个子项（§4.17.9 轮内 fan-out）。
 *
 * 与 `agent_llm.h` 的 `AGENT_LLM_MAX_CALLS` **同值但不是同一个事实**：
 *   那边是"协议通道最多接受几条 tool_calls"（解码器的接受上限）；
 *   这里是"运行时最多认几个子项"（派生的容量上限）。
 * 两层各自设界，因为产出方可以不是 LLM —— 一个脚本能力同样可能吐出一串动作。
 * 两个数改一处就要改另一处，`test_fanout` 用 9 条块把这条钉住。
 */
#define AGENT_FANOUT_MAX   8u
#define AGENT_FINAL_MIN    256u   /* 终值 blob 起始容量 */
#define AGENT_FINAL_MAX    (64u * 1024u)  /* 终值上限：超了响亮计数，不静默丢 */
#define AGENT_INBOX_CAP    4096u
#define AGENT_RENDER_CAP   1024u  /* L0 目录渲染缓冲 */

/*
 * 停滞判定的阈值（§10.7 观测层）。判据是"连续多少个 tick 没有任何推进"，
 * 而**不是**"跑了多少 tick 还没完" —— 后者会把正常的慢任务判成卡住。
 *
 * 为什么跟着 AGENT_MAX_ROUNDS 走而不是写一个固定数：一条合法回路从入口到
 * 收敛最多 AGENT_MAX_ROUNDS+1 个 tick，每个 tick 都有推进（execute 一定 +1）。
 * 取它的两倍，保证**任何合法回路都不可能触发**，于是这个计数器上出现非零
 * 就只有一种解释：真的卡住了。写死 8 会在调大轮数上限之后变成误报源。
 */
#define AGENT_STALL_TICKS  ((AGENT_MAX_ROUNDS + 1u) * 2u)

/*
 * tick 末尾是否自动跑审计器。默认关。
 *
 * 关的理由不是"跑得慢"（虽然它确实是 O(n²)），而是**审计器的价值在两处，
 * 都不需要每 tick 跑**：测试里主动调（断言"任意时刻不脏"），以及排查时
 * 打开这个开关抓第一次变脏的 tick。生产默认开等于把一个调试断言变成
 * 一个可能自己失败的运行时组件 —— 那是把观测层混进了执行层。
 *
 * 打开方式：编译时 -DAGENT_AUDIT=1，或改这里的默认值。
 */
#ifndef AGENT_AUDIT
#define AGENT_AUDIT 0
#endif

/* 容器 class（见 handle.h H3）：装配期分配、运行期不变。
 * 有了它，把 blob 的 handle 喂进 items 表就是**确定性拒绝**，
 * 而不是"index 撞上就静默成功"。0 保留给未标注。 */
#define AGENT_CLASS_ITEM   1u
#define AGENT_CLASS_BLOB   2u
#define AGENT_CLASS_IMPL   3u
#define AGENT_CLASS_DEF    4u
#define AGENT_CLASS_IFACE  5u

/*
 * 计划（plan）= 一张能力 DAG 的模板。第一刀只有"若干叶子并行 → 一个汇聚节点"
 * 这一种形状，因此用表描述；L1 分解器（§4.6）成熟后这张表会被换成运行时构造。
 *
 * 表里放的是**能力名**而不是 cap_id，更不是实现名：
 * 能力名是稳定契约，cap_id / cat_id 是本次装配期的分配结果，实现名是路由细节。
 * 三者依次是"契约 → 装配产物 → 运行时事实"，只有第一个该写进计划。
 */
#define AGENT_PLAN_COUNT   8u
#define AGENT_PLAN_LEAVES  2u

#define AGENT_PLAN_TRIP    0u     /* 行程规划：两个都要出网的能力并行调研 */
#define AGENT_PLAN_QUOTE   1u     /* 报价（缓存未命中）：本地脚本失效 → 升级到模型 */
#define AGENT_PLAN_CACHED  2u     /* 报价（缓存命中）：本地脚本即可，不出网 */
#define AGENT_PLAN_LATE    3u     /* 依赖一个运行时才注册的能力 */
#define AGENT_PLAN_LOOP    4u     /* 多轮闭环（§4.8）：一个会自我派生的条目 */
#define AGENT_PLAN_FS      5u     /* 真实能力：在语料根下检索（§4.15.1 胜者转正）
                                   * fanin = false，理由同 LOOP：输出的接口是
                                   * fs.hits，不是 quote.item，plan.merge 读不懂它。 */

/*
 * 第一个**会改变外部状态**的计划（§4.18.6）。单节点、无 fanin，理由同 FS：
 * 它产出的是 `fs.wrote`（一条收据），不是 `quote.item` —— 汇聚点读不懂它。
 *
 * 它还有一个别的计划没有的性质：条目是否**选得中**取决于请求的 trust 天花板
 * （实现的 trust 是 WRITE）。于是这个 kind 同时是一张"权限闸门"的探针：
 * 天花板低于 WRITE 时，条目在 MATCH 里被判失败而不是"悄悄地不做"。
 */
#define AGENT_PLAN_WRITE   6u

/*
 * 第二个真实能力，也是**第一个 dry-run 能力**（§4.18.7）。
 *
 * 它与 WRITE 同为单节点、无 fanin，但有一格刻意不同：实现的 trust 是 **IO**
 * 而不是 WRITE。于是同一个 store、同一套天花板，WRITE 被挡住而它被选中 ——
 * 这条"一挡一放"才是"dry-run 是能力不是开关"的可测形状：
 * 若把它并进写能力当开关，这里就只剩"整组都挡住"，而看不到
 * "能看、但不能写"这一格。
 */
#define AGENT_PLAN_DIFF    7u

/*
 * 入口消息。ring 是字节流，帧格式由本模块定 —— 内核不管消息边界（ring.h）。
 *
 * ★ 边界由**谁定格式谁管**：本模块定格式，所以本模块负责把"头 + 载荷"切成帧。
 *   做法是显式长度前缀（`text_len`）+ 变长尾，而不是把载荷塞进定长结构体：
 *   定长会把上限（以及那个上限造成的丢弃）悄悄烤进一个 `char[512]` 里，
 *   而变长帧的上限是**一处可数的判据**。
 *
 * ★ 为什么这一格以前没有载荷：原来的入口只传"要做什么"（kind），具体 args 由
 *   INJECT 按 kind 查表组装（`AGENT_Q_*`）。那在剧本里成立，因为问题是我们自己
 *   写死的。`agentctl` 是第一个**外部**调用方 —— 用户从 stdin 打一句话进来，
 *   查表就再也糊不过去了。这是"没有真实调用方所以看不见"的又一例。
 */
typedef struct InjectMsg {
    uint32_t kind;         /* AGENT_PLAN_* */
    uint32_t origin_rank;  /* 发起方 rank；单机恒 0，跨节点时是路由回执的去处 */
    uint32_t seq;
    uint32_t text_len;     /* 紧跟在本结构之后的载荷字节数。0 = 用计划里的固定查询词 */
    uint8_t  trust_max;    /* 本次请求的信任天花板（CapTrust） */
} InjectMsg;

/* 展平后的列号。装配期按 path 查一次存下来，tick 里直接用（type.h 三阶段纪律）。 */
typedef struct ItemLeaf {
    uint32_t cap_id;
    uint32_t status;
    uint32_t pending;
    uint32_t trust_max;
    uint32_t tries;
    uint32_t rounds;
    uint32_t seq;
    uint32_t err;
    uint32_t args_i, args_g;
    uint32_t result_i, result_g;
    uint32_t parent_i, parent_g;
} ItemLeaf;

typedef struct AgentStore AgentStore;

/*
 * 注册表入口：让"能力集变了"这件事可以从外部触发（LLM 写完工具 → 注册 →
 * 本句 sync → 下一个 tick 边界即可路由到它，docs/agent.md §7 的飞轮）。
 * 传 AgentStore 而不是 cxf_world：同步需要的是 world 的服务表，而 store
 * 自己就握着那个 world —— 函数指针表里塞 world 只会多一次转手。
 */
typedef struct AgentRegistry {
    cxf_result (*sync)(AgentStore *store);

    /*
     * L0 目录渲染（§7.1）：把命名空间树从 registry 渲染成明文。
     * 返回值是**需要**的字节数（snprintf 语义），buf 为 NULL 或 cap 为 0 时只计数。
     *
     * trust_max 是渲染的第一性参数而不是附加过滤：模型只该看见自己有权用的能力，
     * 否则它会把规划花在够不着的能力上 —— 规划一次废一次。
     * 与分解期用的是同一个天花板，因此"看得见"与"选得中"永远一致。
     */
    uint32_t (*render)(AgentStore *store, char *buf, uint32_t cap,
                       uint8_t trust_max);

    /*
     * 同一份目录的**协议形状**：渲染成 OpenAI `tools` 数组（§4.16 清单走协议）。
     *
     * 与 render 并存不是重复 —— 两者的消费者不同，因而**失效方式**不同：
     *   render  → 人/模型读的树文本，按行组织，靠换行分条（所以描述要 desc_safe）；
     *   tools   → 模型按协议解析的结构化清单，靠 JSON 语法承载（所以描述要转义）。
     * 一份渲染喂不了两个消费者：把树文本塞进 tools 字段，模型收到的是一坨字符串；
     * 把 JSON 塞进目录，人就再也读不出命名空间的形状。
     *
     * 返回值同样是**需要**的字节数（snprintf 语义）。
     * 过滤规则与 render 完全一致（同一个 trust_max、同一份 cat_tag 判据）——
     * 否则会出现"目录里有、清单里没有"这一类最难查的错位。
     */
    uint32_t (*tools)(AgentStore *store, char *buf, uint32_t cap,
                      uint8_t trust_max);
} AgentRegistry;

/*
 * 入口句柄：外部只拿到一条 ring，不拿到 store。
 * 这是"派发接口统一、基座按部署可插拔"（§5）的第一格 ——
 * 换成 shm ring 或 socket 时，改的是这个指针指向谁，不是调用方。
 */
typedef struct AgentIngest {
    cxf_ring *inbox;
} AgentIngest;

struct AgentStore {
    cxf_alloc *alloc;
    cxf_world *world;   /* 注册表同步要读服务表；装配期写入，之后只读 */

    cxf_layout layout;
    cxf_soa    items;   /* WorkItem SoA */
    cxf_table  blobs;   /* blob 池：行 { ptr, len, cap } */

    cxf_table  defs;    /* 目录（契约）表：行 { def, cat_id } */
    cxf_table  caps;    /* 实现表：行 { impl, cap_id } */
    cxf_table  ifaces;  /* 接口类型表：行 { def, iface_id } */

    /* 固化：cat_id → 契约行 handle */
    cxf_handle def_of[CAP_MAX];
    /* 固化：cap_id → 实现行 handle（MATCH 的主索引） */
    cxf_handle cap_of[CAP_MAX];
    /* 固化：iface_id → 类型行 handle */
    cxf_handle iface_of[IFACE_MAX];
    /* 固化：cap_id → 同一降级链上的下一个实现（CAP_NONE 表示链到此为止） */
    uint32_t   cap_fb[CAP_MAX];
    /* 固化：cap_id → 它实现的是哪个能力。降级与分组都靠它，无需解析名字 */
    uint32_t   impl_cat[CAP_MAX];
    /* 固化：cat_id → 链头（首选实现）cap_id。优先是链的拓扑性质，不是注册顺序 */
    uint32_t   head_of[CAP_MAX];
    /*
     * 固化：cap_id → 这个实现产出的是不是**控制流**（out_iface == agent.action）。
     *
     * 这是 §4.8 方案 D 相对方案 B 多买到的东西：B 把"这一步会派生"放在调用
     * 约定里，运行时从 payload 猜；D 把它变成数据流的类型事实，于是注册期
     * 就能把它算成一个 bool，热路径只剩一次数组索引。
     *
     * RECYCLE 靠它决定"这是终态产出，还是下一轮的输入"。
     */
    bool       cap_acts[CAP_MAX];
    /* 固化：IFACENAME_ACTION 的 iface_id（未注册时为 IFACE_NONE） */
    uint32_t   iface_action;

    /*
     * 类型图（TTN）的正反邻接。注册期一次建好，于是分解期的"从目标倒推"
     * 与热路径的判定都是查表，不是搜索 —— 与 cap_of / cap_fb / head_of 同一手法。
     *
     *   cat_out[cat]        这个能力产出哪个类型（IFACE_NONE = 无产出）
     *   cat_in[cat][k]      这个能力的第 k 个输入端口（IFACE_NONE = 端口结束）
     *   prod_head[t]        产出类型 t 的链头 —— **反向边，倒推靠它**
     *   cons_head[t]        消费类型 t 的链头
     *
     * 全部按 **cat_id** 索引，不按 cap_id：端口是契约声明的，一个能力有几个实现
     * 与"它能接到哪"无关。建在实现上会让 price.quote 的出现次数变成实现数，
     * fan-in 要等的条数凭空翻倍（见 agent_caps.h CAT_NODES）。
     *
     * 链上的节点按**端口**编址（每能力 CAP_IN_MAX+1 个槽）：cat_in 允许同一类型
     * 填两次（plan.merge 就是），按能力编址会让两个端口写同一个链接槽而自环。
     * 因此 cons 的度是"端口数"而不是"消费者数" —— 那才是要等的数据流条数。
     */
    uint32_t   cat_out[CAP_MAX];
    uint32_t   cat_in[CAP_MAX][CAP_IN_MAX];
    uint32_t   prod_head[IFACE_MAX];
    uint32_t   prod_next[CAT_NODES];
    uint32_t   cons_head[IFACE_MAX];
    uint32_t   cons_next[CAT_NODES];

    uint32_t   cap_count;   /* 已分配 cap_id 数（稠密） */
    uint32_t   cat_count;   /* 已分配 cat_id 数（稠密） */
    uint32_t   iface_count; /* 已分配 iface_id 数（稠密） */
    uint32_t   cap_gen;     /* 目录世代：每次同步有新增就 +1。世代缓存的判据，
                               §7.1 说它兼任提示词前缀缓存键 */
    uint32_t   scanned;     /* 已扫描到的 service 下标（增量枚举游标） */

    /*
     * 装配预算（§4.16 末 / §4.15 的 `b*` 落点）：一次汇聚允许把多少字节的
     * 工具产出**装进 args**。默认 `AGENT_ARGS_MAX`，经 `agent_args_budget_set` 下调。
     *
     * ★ 为什么它在 store 里，而不在 `AgentLlmCfg` 里（这是本轮修正的一条设计）：
     *   1. **层不同**。`AgentLlmCfg` 是传输配置（host/port/model/key/tools），
     *      传输层不决定"给多少"，它只把已决定的字节送出去。让只会搬运的件
     *      对内容有意见，与 §4.10「协议层只做协议」直接相反。
     *   2. **方向不同**。`FsCfg.max_hits` 是**产出方**的上限（部署事实）；
     *      `b*` 是**装配方**的最优值。让装配方去驱动产出方的上限，看似闭环，
     *      实则把两个正交的量绑成一个 —— 于是再也回答不了"是产得太多，还是
     *      装得太多"，而那正是 b* 唯一要区分的东西。
     *   3. **可变性不同**。`FsCfg` 是 `CapImpl.ctx`，注册期固定、活得比 world 长、
     *      且全进程共享一份；让运行期改它 = 把部署事实变成运行时数据，
     *      而且多个 WorkItem 并发读同一份 ctx 就成了数据竞争的形状。
     *   所以两个预算各归其位：产出预算在能力 ctx（注册期），
     *   装配预算在 store（运行期）。中间没有谁驱动谁。
     */
    uint32_t   args_budget;

    /*
     * 回路轮数上限（运行期）。默认 `AGENT_MAX_ROUNDS`，经 `agent_set_max_rounds` 上调。
     *
     * ★ 为什么它必须是运行期的：`AGENT_MAX_ROUNDS` 是编译期常量时，"轮数上限"
     *   同时是**保护**和**天花板**，两件事被绑死。剧本里 3 轮够（工具一次、作答一次）；
     *   真实任务里 3 轮意味着"探索两下就必须给答案"，而那会把**上限太小**误读成
     *   **模型不会规划** —— 归因直接指向错误的方向。
     *   与 `args_budget` 同一形状：0 = 用默认，不夹紧（0 那档是合法实验点）。
     */
    uint32_t   max_rounds;

    /*
     * 配额（§4.17.8）。放在 store 而不是 `AgentLlmCfg`，与 `args_budget` / `max_rounds`
     * 同一条理由：**它是装配方（部署）的事实，且运行期可调**；`AgentLlmCfg` 是传输配置，
     * 不该持有"我们允许自己发多快"。
     *
     * 但注意一处**不对称**：`args_budget` 决定"装多少"，`quota` 决定"发多快" ——
     * 后者的效果只有让 tick **不往前推进**才能兑现（本 tick 不发，留给下一 tick）。
     * 于是在忙循环的 tick 里它表现为"空转等令牌"，这是已知代价，见 `stat_quota_wait`。
     */
    AgentQuota quota;

    /*
     * 时钟注入点。**默认 NULL = 用 CLOCK_MONOTONIC**，测试可以塞一个假时钟。
     *
     * 为什么必须可注入：一旦引入速率约束，行为就依赖**墙钟**，而墙钟会让
     * "跑一遍看结果"变得不可复现（这是内核此前完全没有的东西 —— tick 是纯
     * 逻辑推进，没有 dt）。不注入时钟，速率策略就只能靠"真的等 30 秒"来测：
     * 慢、不稳、且 CI 上表现为超时而不是失败。
     *
     * 与 allocator 是同一个手法：**只有一个点，且可替换**。区别是 alloc 在热路径，
     * 这个每 tick 调一次 —— 所以它可以是函数指针，而分配路径上的多态是要避免的。
     */
    uint64_t (*clock)(void);

    ItemLeaf leaf;

    cxf_ring      inbox;     /* 入口 ring：唯一的请求来源 */
    AgentIngest   ingest;
    AgentRegistry registry;

    uint64_t blob_bytes;     /* 活着的 blob 容量字节。白盒：泄漏一屏可见 */
    uint64_t blob_peak;      /* 历史峰值，用来证明扩容路径真的被走过 */

    /*
     * 终值。**放在 blob 池里，不放定长数组**（§10.7 观测层第一件）。
     *
     * 原来的 `char final_text[512]` 在回路下必然溢出：每一轮的 DONE 都追加
     * 一行，而 512 只够三行正常长度的结果。溢出的表现是 `append_final` 里
     * 那句"满了整行丢" —— 即**静默丢答案**。那不是一个容量参数选小了，
     * 是"用固定上界装无上界的东西"，属于正确性而非性能。
     *
     * 换成 blob 之后有三件事同时成立，且都不需要新结构：
     *   1. 长度由 blob 的 len 列给出，不再需要 final_len 与 NUL 终止；
     *   2. final_h 为空 == 还没有终值，**"空结果"与"没有结果"不再靠布尔区分**；
     *   3. 终值与中间结果同一条生命周期（都归 RECYCLE 与 store_deinit 管），
     *      于是"活着的 blob 恰好一行"成为可断言的收尾条件。
     *
     * 上限仍然有（AGENT_FINAL_MAX），但超限走 stat_final_lost 计数 ——
     * **有界可以，静默不行**。
     */
    cxf_handle final_h;
    uint32_t   stat_final;
    uint32_t   stat_final_lost;   /* 终值超上限或分配失败被丢弃的条数。非零即是缺陷 */

    /*
     * 停滞观测（§10.7）。三个数回答三个不同的问题：
     *   stall_ticks  现在连续卡了几个 tick（0 = 在动）—— 瞬时
     *   stall_max    历史最长连续停滞         —— 事后
     *   stat_stalled 进入停滞的次数（边沿计数）—— 累计
     *
     * 为什么三个都要：只留 stall_ticks，归零之后"卡过"这件事就被抹掉了；
     * 只留 stat_stalled，"卡了 3 tick"和"卡了 3000 tick"都记成 1。
     * 边沿计数是它非做不可的理由 —— 每 tick 累加的计数在长期卡住时会变成
     * 一个巨大的数，反而看不出"这是一次停滞还是很多次"。
     */
    uint32_t   stall_ticks;
    uint32_t   stall_max;
    uint32_t   stat_stalled;
    bool       stall_latched;     /* 边沿去重：一次停滞只记一次 */

    /* 上一 tick 边界的快照。与当前值比较得出"本 tick 有没有推进"。 */
    uint32_t   obs_items;         /* items.table.count */
    uint32_t   obs_done;          /* stat_executed */
    uint32_t   obs_final;         /* stat_final */

    /* 审计器在 tick 边界累计发现的问题次数。AGENT_AUDIT=0 时恒为 0，
       非零就是"有人在生产路径上见过不一致" —— 这一条比任何日志都硬。 */
    uint32_t   stat_audit_bad;

    uint32_t stat_injected;
    uint32_t stat_executed;    /* 执行到 DONE 的条目 */
    uint32_t stat_retried;     /* 失败一轮、交给降级链的条目 */
    uint32_t stat_failed;      /* 终态 FAILED 的条目 */
    uint32_t stat_recycled;
    /*
     * 控制流条目**就地转成续跑节点**的次数（§4.16 末 / 缺口 ⑤）。
     *
     * 与 stat_recycled 并列而不是混进去：一个是"这一行走了"，一个是"这一行留下了"，
     * 两件事混在一个计数器里就再也看不出回路是"向前传"还是"原地等结果"。
     * 它同时也是缺口 ⑤ 有没有真的被走到的唯一证据 —— 工具派了但没续跑，
     * 这个数就停在 0，而 stat_recycled 照常增长。
     */
    uint32_t stat_resumed;
    /*
     * 工具结果因**装配预算**被整条挡在上下文外的次数（§4.16 末）。
     *
     * 与 stat_failed 并列而不是被它吸收：一次"预算丢弃"与一次"子项失败"
     * 的处置完全相反（后者该重试/换实现，前者该调大预算或缩短累积）。
     * 混进一个数里，就再也分不出该动哪一边。
     *
     * ★ 它**不是**失败计数。丢弃之后父节点照常放行、模型照常被再问一次 ——
     * 让父节点卡住会把"上下文装不下"变成"回路死锁"，而 A4 照样通过
     * （pending 确实等一个活子项），属于结构账平、行为已死的那一类。
     */
    uint32_t stat_args_lost;
    /*
     * 入口载荷被拒的次数（超 `AGENT_INPUT_MAX`，或帧未到齐时被外部判定非法）。
     * 非零说明有调用方在往入口灌超长任务 —— 那是**协议边界**，不是运行时故障，
     * 所以它不并进 stat_failed：失败是"任务没做成"，这个是"任务根本没进来"。
     */
    uint32_t stat_input_lost;
    /*
     * 撞到轮数上限的次数（§4.16.3）。**曾经被 stat_failed 吞掉**。
     *
     * 真实回路的第一组测量把它逼出来的：同一个问题、同一个 endpoint、
     * 请求体温度 0，5 次跑出 4/5/3/5/2 次模型调用，其中三次把轮数用光。
     * 而那时日志里只有 `failed 1` —— **"模型在打转"与"工具坏了 / 动作非法"
     * 是同一个数**，于是"该调大 --rounds 还是该修协议"这件事没有数据能回答。
     *
     * 这与 `stat_llm_calls` 曾经的低报是同一类错：**两个原因共用一个计数器，
     * 得到的不是"不够精确"，是"看起来成立"的错误结论**。
     * 它也不该被 stat_failed 吸收：轮数用尽意味着**上下文里已经有证据**，
     * 只是没被收口成答案；而 failed 意味着这条路根本没走通。
     */
    uint32_t stat_capped;
    /*
     * 因配额不足而**本 tick 没发**的次数（§4.17.8）。它不是失败，也不是丢弃：
     * 条目仍是 RUNNING，下一 tick 再试 —— 所以它既不能并进 `stat_failed`，
     * 也不能并进 `stat_capped`（那个是"永远不发了"）。
     *
     * 它还是"tick 在空转等令牌"的唯一可见证据：这个数涨得比 `stat_executed` 快，
     * 说明瓶颈在速率而不是在图 —— 而那时正确的动作是**让 tick 会睡**（A5），
     * 不是调大 rpm。
     */
    uint32_t stat_quota_wait;

    /*
     * 一轮动作一共派生了几个子项（§4.17.9 fan-out）。
     *
     * 为什么不能从 `stat_injected` 推：那个还包含入口进来的条目、以及派生派生
     * 出来的条目，三件事混在一个数里。这个计数器回答的是一个**独立的问题**：
     * "模型一次给了几件事"。稳态下它 / 轮数 就是真实的 fan-out 宽度 ——
     * 也是"异步 ABI 到底值不值得做"的分母：宽度恒为 1 时，多路 I/O 无处可用。
     */
    uint32_t stat_fanout_kids;

    /*
     * 段化收益实验的两个直方图（§4.18.16 八）。
     *
     * 要回答的问题只有一个：**"第几段第一次失败"落在哪**（与批大小一起）。
     * 它决定"只重写后缀"到底省不省 —— 若失败恒落在第 1~2 段，
     * 那么堆叠式对这类模型就是净亏，应该关掉而不是推广。
     *
     * 为什么是直方图而不是一个"平均 k"：均值会把"总是第 1 段失败"和
     * "一半第 1 段一半最后一段"混成同一个数，而这两件事的处置完全相反。
     *
     * ★ 口径：`stat_fail_seq[k]` 数的是**每一个失败子项**的 seq。
     *   今天同批动作是并行派发的（不做短路），所以一个批次里可能有多个失败；
     *   短路落地之后每批最多一次失败，**同一个直方图自动就是"第一次失败"的分布** ——
     *   不需要换仪器，这正是选它的理由。
     *
     * ★ 分母：`stat_seg_hist[N]` 在派发处按 `ncall` 记。两者相除才是"这个模型
     *   在第 N 段的失败率"。单看 `stat_fail_seq` 会把"批次大小分布"混进去。
     */
    uint32_t stat_seg_hist[AGENT_FANOUT_MAX + 1u];
    uint32_t stat_fail_seq[AGENT_FANOUT_MAX + 1u];

    /*
     * "这一次产出到底有没有变成一个批次"（§4.18.17 六）—— 三个**互不替代**的计数：
     *
     *   batch_bad        语法坏了：一块都解不出来 / 收尾不干净 / 超 `AGENT_FANOUT_MAX`
     *   batch_rejected   语法守住了，但点名了不存在或天花板下够不着的能力
     *   batch_ok         真的派出去了（N ≥ 1）
     *
     * 为什么不能合成一个"失败率"：两者的下一步动作相反 ——
     * `bad` 说明这个模型的**格式能力**不够（处置 = 降 N 或退到文本单条通道），
     * `rejected` 说明**能力表 / 折叠 / 天花板**有问题（处置 = 改目录，不是换模型）。
     * 混成一个数就会拿"模型不行"去解释一个其实是目录问题的现象。
     *
     * `p̂ = ok / (bad + rejected + ok)` 才是 §4.18.17 里那个守格式率；
     * 它和 `stat_seg_hist` 一起，才能算出 `N_max`。
     */
    uint32_t stat_batch_bad;
    uint32_t stat_batch_rejected;
    uint32_t stat_batch_ok;

    uint32_t stat_grow;        /* blob 扩容次数 */
    uint32_t stat_max_tries;   /* 任一 WorkItem 的最大执行轮数（= 链上走过的实现数） */
    uint32_t stat_resolve;     /* 能力名 → cat_id 的解析次数。稳态下应只有一次 */
    uint32_t stat_denied;      /* 因信任天花板被跳过的实现数（可观测的门控） */
    uint32_t stat_llm_calls;   /* LLM 档实现的**实际调用次数**（含扩容重跑），出网的白盒代理 */
    uint32_t stat_exec_calls;  /* handler 被调用的总次数。与 stat_executed 的差 = 扩容重跑的代价 */
    /*
     * 累计成本档，口径 = **按实际调用**（一次 handler 调用加一次 `CapImpl.cost`）。
     *
     * 曾经记在派发处（一个条目加一次），于是扩容重跑把一个逻辑调用放大 4 倍时，
     * `stat_cost` 只报 1 次而 `stat_llm_calls` 报 4 次 —— 两个"钱"代理互相矛盾，
     * 且都"看起来成立"（A3，2026-10-21 修）。判据：凡是钱/出网/重试一律按实际调用计。
     * 于是 `stat_cost` 与 `stat_llm_calls` 同口径，而
     * `stat_exec_calls - stat_executed` 的差值就是"付了钱但没买到结果"的浪费量。
     */
    uint32_t stat_cost;

    /*
     * 字节搬运账（§4.8 之后的第一个测量问题：agent 到底在搬多少字节）。
     *
     * 这四个数一起才构成一个可判定的问题 —— 单看任何一个都会得出相反结论：
     *   bytes_in   外部入参首次落地（INJECT 把 query 拷进 args blob）
     *   bytes_out  handler 写出的字节（能力的真实产出量）
     *   bytes_copy 数据流被**再次**搬运（聚合进父 args / 派生 args / 终值累加）
     *   bytes_grow 扩容时重新搬运的字节 —— 纯粹因为容量估计不足，与数据流无关
     *
     * 为什么必须分开记：bytes_copy 是宪法强制的（能力不知道父 ⇒ 输出先落自己的槽位
     * ⇒ 聚合必然再搬一次），改不掉；bytes_grow 是可改的（容量估准就不搬）。
     * 两者混成一个"拷贝字节数"会让"该改哪一个"变得无法判断 —— 这正是
     * 先测量再优化的全部意义。
     */
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t bytes_copy;
    uint64_t bytes_grow;
    /*
     * 因为装配预算而**没能进入上下文**的产出字节。与 bytes_copy 并列而不是
     * 从它里减去：bytes_copy 记"搬了几次"（数据流必需），bytes_lost 记
     * "搬了但白搬"（付了读盘与 handler 的钱，产出被挡在模型外面）。
     * 后者的唯一用途就是回答"预算调小要付多少代价"，而它是 b* 曲线的纵轴之一。
     */
    uint64_t bytes_lost;
    uint32_t copy_ops;         /* 搬运**次数**。字节数看不出"小步多次"和"一次大块" */

    /*
     * 待解的动作（§4.8）。RECYCLE 把 DONE 且产出控制流的条目挂到这里，
     * 行与 result blob 都**不回收** —— 它们就是本 tick 的交接物；
     * 下一个 tick 的 INJECT 解码它、派生新条目，然后才真正回收。
     *
     * 为什么交接而不直接把 rounds/trust/payload 抄进这几个字段：抄一遍等于
     * 把一条 WorkItem 拆成两半存，两份口径迟早不同步；而**行本来就在那里**，
     * 让 INJECT 直接读它，交接物只有"谁"这一个 handle。
     */
    cxf_handle acts[AGENT_ACT_BATCH];
    uint32_t   act_n;
};

/*
 * INJECT 的 state。cat_ids 是"计划里的能力名 → cat_id"的解析结果，按目录世代缓存：
 * 世代没变时每 tick 只花一次整数比较，不查表（§7.2 的按需拉取 + 世代缓存）。
 *
 * 缓存的是 cat_id 而不是 cap_id：cap_id 依赖**本次请求的天花板**（不同天花板能
 * 选中的实现不同），那是每个请求各算一次的事，不该烤进世代缓存。
 * 名字 → cat_id 才是不随请求变的部分，也正是该缓存的部分。
 */
typedef struct InjectState {
    AgentStore *store;
    bool        resolved;
    uint32_t    cap_gen;
    uint32_t    cat_merge;
    uint32_t    cat_ids[AGENT_PLAN_COUNT][AGENT_PLAN_LEAVES];
} InjectState;

typedef struct PhaseState {
    AgentStore *store;
} PhaseState;

/* 装配期与测试用的解析接口；tick 里不许用这些（那会退化成运行时查找）。 */
uint32_t       agent_cat_id(AgentStore *s, const char *capability);
/*
 * 切片版（§4.8）：动作里的能力名来自 blob，没有 NUL，也不该为它补一个 ——
 * 补一个就必须有界，而**截断的动作名会静默指向另一个能力**。
 * 按长度精确比较：多一个字符就是不相等。
 */
uint32_t       agent_cat_id_n(AgentStore *s, const char *name, uint32_t len);
const CapDef  *agent_cat_def(AgentStore *s, uint32_t cat_id);
uint32_t       agent_cap_id(AgentStore *s, const char *capability);  /* 首选实现 */
const CapImpl *agent_cap_impl(AgentStore *s, uint32_t cap_id);

/* 接口类型解析。名字写错返回 IFACE_NONE —— 注册期据此响亮失败，不静默新建类型。 */
uint32_t        agent_iface_id(AgentStore *s, const char *name);
const IfaceDef *agent_iface_def(AgentStore *s, uint32_t iface_id);

/*
 * 类型图上某个类型的入度/出度。两个用途：
 *   1. 注册期不变式 —— INT 必须有人产出、EXT 必须有人消费；
 *   2. 分解期选路标 —— 产出者多的类型更可能是"中间汇聚点"。
 *
 * 计数口径刻意不同：
 *   producers = 产出它的**能力**数（一个能力只有一个产出端口）；
 *   consumers = 消费它的**端口**数（同一能力可以有两个同类端口）。
 * "多少个能力能给我这个类型"和"这条数据流要被喂几处"是两个不同的问题，
 * 后者才是 fan-in 要等的条数。_at 按编号取第 i 个能力，顺序由 cat_id 定，
 * 因此确定可复现。
 */
uint32_t        agent_iface_producers(AgentStore *s, uint32_t iface_id);
uint32_t        agent_iface_consumers(AgentStore *s, uint32_t iface_id);
uint32_t        agent_iface_producer_at(AgentStore *s, uint32_t iface_id, uint32_t i);
uint32_t        agent_iface_consumer_at(AgentStore *s, uint32_t iface_id, uint32_t i);

/*
 * 装配期设预算。`bytes == 0` ⇒ 回到默认 `AGENT_ARGS_MAX`（"不设"与"设成 0"
 * 不用两个值表达，少一种误读）。
 *
 * 为什么是**函数**而不是让调用方直接写字段：结构体是公开的（白盒），但
 * "0 = 默认"这条规则必须有唯一一处实现，否则每个调用点自己解释一遍 0。
 *
 * 允许设得比起始容量（`AGENT_ARGS_MIN`）还小：那是一个**合法的实验点**
 * ——预算小于首轮入参时，任何工具产出都进不了上下文，恰好是 b* 曲线的左端。
 * 不在这里夹紧，是因为夹紧会把"预算 0 那一档"变成一个悄悄被抬高的数。
 */
void agent_args_budget_set(AgentStore *s, uint32_t bytes);

/*
 * 装配期设轮数上限。`rounds == 0` ⇒ 回到默认 `AGENT_MAX_ROUNDS`。
 * 见 `AgentStore.max_rounds`：它是保护，不该同时当地板。
 */
void agent_set_max_rounds(AgentStore *s, uint32_t rounds);

/*
 * 装配期设配额（§4.17.8）。`rpm == 0 && conc_max == 0` ⇒ 不限（默认）。
 *
 * 这是**声明**，不是探测：RPM / 并发上限是买来的契约，写下来最省事。
 * 设 rpm > 0 时桶会被填满（burst = conc_max，至少 1），所以"刚启动就能发几个"
 * 而不是"要等到第一秒过去"。
 */
void agent_quota_set(AgentStore *s, uint32_t conc_max, uint32_t rpm);

/*
 * 取一个发送令牌：true = 现在可以发一次。
 *
 * 公开而不是 static，唯一理由是**时间相关的算术必须能用假时钟直接测**：
 * 桶的补充量是 `dt × rpm / 60`，而这条式子错了不会崩、只会系统性慢或快一点点 ——
 * 靠跑真实回路根本发现不了（要么等 30 秒，要么误差被噪声吞掉）。
 */
bool agent_quota_take(AgentStore *s);

/*
 * 终值的只读视图（§10.7）。
 *
 * 返回 `cxf_str` 而不是 `char *`：**长度显式，不保证 NUL 终止**。
 * 这不是为了省一个字节，而是"零终止串不进数据通路"这条纪律的延伸 ——
 * 一旦允许返回 `char *`，下游就会写出 `strstr(final, "...")`，
 * 而这在结果里含 '\0' 时会静默只看到前半截。要搜索用 `cxf_str_find`。
 *
 * 指针指向 blob 内部，**下一次产生终值（append_final）时失效**。
 * 要跨 tick 持有，自己拷走。
 */
cxf_str agent_final(AgentStore *s);

/*
 * 审计器（§10.7）。返回**发现的问题条数**，0 = 干净。只在 tick 边界调用。
 *
 * 它把四类"平时靠运气不炸"的东西变成确定性断言：
 *   A1 账目自洽   blob_bytes == Σ(存活行 cap)，且 len <= cap、ptr 非空
 *   A2 引用完整   每个条目的 args/result handle 要么空、要么指向活着的 blob
 *   A3 交接完整   挂起的动作（acts[]）必须仍是活条目
 *   A4 汇聚账     BLOCKED 条目的 pending 必须等于仍指向它的活子项数
 *   A5 子表不变式 blob 表与 items 表的 T1~T8
 *
 * A4 是这里最有价值的一条：它把"父节点永远等一个不存在的子项"这种
 * **静默停住**变成可判定 —— 而那正是失败里最难查的一类（不崩、不漏、
 * 只是界面再也不动）。
 *
 * A4 只在 tick 边界成立：一个子项在 EXECUTE 后已 DONE 但尚未回收时，
 * 它仍然被 pending 数着。所以本函数不能用在 phase 中间。
 *
 * 代价是 O(n²)：A4 每个 BLOCKED 条目都要扫一遍表。这是调试件的代价，
 * 不进热路径；RECYCLE 末尾的自动调用由 AGENT_AUDIT 控制。
 */
uint32_t agent_audit(AgentStore *s);

/*
 * tick 边界的水位观测，返回当前连续停滞 tick 数。
 *
 * 生产路径由 RECYCLE 自动调用（它是 LAST phase，一个 tick 的终点）。
 * 公开出来是为了让测试能单独验证判定规则本身，而不必构造一整条业务链。
 */
uint32_t agent_observe(AgentStore *s);

extern const cxf_module_def agent_store_def;
extern const cxf_module_def agent_inject_def;
extern const cxf_module_def agent_match_def;
extern const cxf_module_def agent_execute_def;
extern const cxf_module_def agent_recycle_def;

#endif
