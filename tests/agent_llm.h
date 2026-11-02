#ifndef AGENT_LLM_H
#define AGENT_LLM_H
#include "agent_policy.h"

#include <stdbool.h>
#include <stdint.h>

#include "cxf/base.h"

/*
 * 协议层：OpenAI 兼容的 chat/completions，**非流式**。
 *
 * 这一层为什么必须存在、又必须这么小，两条判据一起看：
 *
 *   必须存在：它是唯一能产出**真实字节流**的入口。没有真实字节流，
 *   CDC（内容定义分块）没有输入，上下文管理没有对象，推理引擎的 prefix cache
 *   命中率无从谈起 —— 这条链条的第一格就是这里。
 *
 *   必须这么小：协议转换是成熟领域（`/v1/chat/completions` 已是事实标准，
 *   LiteLLM / OpenRouter / 各云网关都是对照实现）。成熟的操作含义是
 *   **规格稳定、可对照验证**，不是"要持续投入"。所以这一层是**件**不是**层**：
 *   一次性写完约 300 行，之后只在协议真的变时改。
 *
 * 划定边界（越界即预警：三个月后这个文件还在长，说明它变成了"路线"）：
 *   不做多厂商、不做 provider 注册表、不做插件、不做配置 DSL、
 *   不做重试策略、不做限流、不做 token 计费。
 *   前三条属于"再做一个 adam"，后三条属于网关（relay-gateway）。
 *
 * 不做流式，理由不是省事：streaming 下 tool_calls[].function.arguments 是
 * 按 index 分片到达的，必须重组 —— 那是这个成熟领域里最容易写错的子问题。
 * 而多轮闭环按 tick 推进，本身就是"一次调用一个完整响应"的形状，用不着流式。
 */

#define AGENT_LLM_HOST_CAP   64u
#define AGENT_LLM_PATH_CAP   96u
#define AGENT_LLM_MODEL_CAP  64u
#define AGENT_LLM_KEY_CAP    160u
/*
 * 请求体上限，超了如实失败（不截断）。
 *
 * 从 8192 抬到 16384 的理由只有一个：**tools 清单是最大单项**。抓包实测
 * （2026-10-20，Hermes / Codex / Claude Code 三份）tools 定义分别占 54 KB / 84 KB /
 * 65 KB —— 比 system 还大，是固定前缀的主体。我们自己 6 个能力约 1.5 KB，
 * 16 个（CAP_MAX）约 8 KB，所以 16 KB 够当前尺度。
 *
 * 边界写在这里：真接上几十个工具的那一天，这个栈上缓冲必须换成调用方给的
 * 缓冲（与 scratch 同一手法）。现在不做的理由是"没有调用方"，不是"不需要"。
 */
#define AGENT_LLM_REQ_CAP    16384u
#define AGENT_LLM_RAW_CAP    (64u * 1024u)            /* 建议的响应落点大小 */
#define AGENT_LLM_SYS_CAP    512u                     /* system 消息正文上限 */
#define AGENT_LLM_TOOL_NAME_CAP 64u                   /* 工具名长度上限（"fs.grep" 这类） */
#define AGENT_LLM_TOOL_ARGS_CAP 1024u                 /* 单个工具参数字节上限 */

typedef struct AgentLlmCfg {
    char     host[AGENT_LLM_HOST_CAP];
    uint16_t port;
    char     path[AGENT_LLM_PATH_CAP];
    char     model[AGENT_LLM_MODEL_CAP];
    char     api_key[AGENT_LLM_KEY_CAP];

    /*
     * system 消息的正文。空串 = 不带 system 消息（请求体因此与"没有这个字段"
     * 的版本逐字节相同 —— 这是一个**向后兼容**的默认值，不是省略）。
     *
     * 为什么必须有它（docs/agent.md §4.11）：本地小模型要稳定地吐出
     * `CALL|...` / `DONE` 这种线格式，几乎一定需要 system 里的一段格式说明
     * 或示例。没有 system，唯一能放说明的地方是 user 消息，而那就是把
     * "格式约定"与"业务输入"混进同一段字节 —— 于是每次业务输入变化都会
     * 改变格式说明所在的上下文位置，前缀缓存与行为稳定性一起变差。
     *
     * 它是**装配期固定**的：同一份 cfg 两次构造出的请求体逐字节相同，
     * 确定性契约不受影响。但改它等于改请求字节 ⇒ CDC 分块与前缀缓存键
     * 会重建一次，所以它属于"部署配置"，不是运行时旋钮。
     */
    char     system[AGENT_LLM_SYS_CAP];

    /*
     * tools 清单：**已经渲染好的 JSON 数组文本**（`[{...},{...}]`），不是结构体数组。
     *
     * 为什么是字节而不是结构化字段：装配方（目录）才知道 trust / tier / 排序，
     * 协议层只该做一件事——把它原样放进请求体。协议层若自己去遍历能力表，
     * 它就得知道 CapDef，于是"清单走协议"变成"协议层认识能力层"。
     *
     * 为什么是**指针**不是数组：清单是装配期渲染一次的产物，生命周期必然长于
     * 一次调用；塞进 cfg 会让 cfg 变成几十 KB 的值类型，每次拷贝都搬它，
     * 而每次调用只读它一遍。同一份清单也可以服务多个端点。
     *
     * tools_len == 0 或 tools == NULL ⇒ 整个 tools / tool_choice 字段消失，
     * 请求体与"没有这个功能"的版本**逐字节相同**（§4.10 硬契约）。
     */
    const char *tools;
    uint32_t    tools_len;

    /*
     * 额外的 HTTP 请求头（必须 CRLF 结尾，NULL = 不带）。**诊断用**。
     *
     * ★ 它为什么不违反"请求体逐字节确定"那条硬契约（§4.9）：那条契约的对象是
     *   **body**，因为 body 是 CDC 的输入、也是前缀缓存的键。请求头两者都不是，
     *   推理引擎不会因为多一个头而改变 token 序列。这条区分必须写下来 ——
     *   否则下一个人会按"确定性"去删掉它，于是"端点上几十条相同请求到底是
     *   哪次跑的"这个问题就永远无解。
     *
     * 典型用途：`X-CXF-Ctx: run=48211 call=3` —— 让我们这边的 trace 与服务端的
     * 访问日志可以按 (run, call) 对齐。没有它，日志里 N 条逐字节相同的请求
     * 无法区分"重复测量"与"框架重发"。
     */
    const char *hdr;

    uint32_t timeout_ms;

    /*
     * ★ 观测量，不是配置（2026-10-23 加）。每次 chat 调用结束后由协议层写回：
     *
     *     last_status   本次的 HTTP 状态码（0 = 根本没走到 HTTP，例如连接失败）
     *     last_rc       本次的 cxf_result（0 = CXF_OK）
     *
     * 为什么必须留下它 —— 本文件上面的原话是"上层需要的是「这次调用没成」，
     * 不是「供应商怎么措辞的」"。那句话在**只有一个端点**时是成立的，
     * 在云端开测时被实测推翻：非 2xx 一律归成 FORMAT，而 FORMAT 的既定语义是
     * **"重试没用"**。于是 401（key 错）/ 429（对端限速）/ 500（对端抖动）
     * 在这个语义下变成**同一件事**，而它们下一步动作完全相反：
     * 改凭据 / 等一会儿 / 直接再发。
     *
     * 归成一个**码**没错 —— 协议层不做重试策略，是文件头写死的边界。
     * 错的是把**事实**也丢了：码可以合成一个，读数不能不留。
     * 这与「低报的计数器比没有更坏」同族：一个抹平了差异的 failed
     * 会把人送去改提示词，而真正该改的是 CXF_LLM_KEY。
     *
     * 为什么放在这里而不是 store：它随**端点**来，和 host/model/key 同生命周期
     * （换端点就该重新观测）；配额 / max_rounds 那些随**部署**来，所以它们在
     * store（见 agent.h:419 那段）。判据是"换了谁会失效"，不是"哪个结构更顺"。
     *
     * 它不参与请求体构造 ⇒ 不影响"请求体逐字节确定"那条硬契约。
     */
    uint16_t last_status;
    int16_t  last_rc;
    const AgentPolicy *policy;
    uint32_t scope;
} AgentLlmCfg;

/*
 * 从环境变量读配置：
 *   CXF_LLM_URL    http://127.0.0.1:8080/v1/chat/completions   （只接受 http://）
 *   CXF_LLM_MODEL  claude-sonnet-4 / gpt-4o-mini / ...          （网关侧的模型名）
 *   CXF_LLM_KEY    可选。本地网关通常不需要
 *   CXF_LLM_SYSTEM 可选。system 消息正文（格式说明 / 示例）；超长如实报错不截断
 *
 * 未设置 URL 或 MODEL 时返回 CXF_ERR_DEP —— 调用方据此回落到 mock。
 * 这是"能力层决定用哪个实现"，而不是"协议层偷偷用默认值连出去"：
 * 一个会在没配置时自己找地方连的传输层，是测试不可复现的根源。
 */
cxf_result agent_llm_cfg_from_env(AgentLlmCfg *c);

/* JSON / 环境变量共用的 URL 解析入口；只修改 host、port、path。 */
cxf_result agent_llm_cfg_set_url(AgentLlmCfg *c, const char *url);

/* Allocation-free transcript views. Spans borrow log (or fixed literals),
 * valid only during the synchronous visit; do not mutate log or retain spans.
 * CALL/TOOL share a 1-based call_id. Callback failure stops traversal. */
typedef enum AgentConvoKind { AGENT_CONVO_USER, AGENT_CONVO_CALL, AGENT_CONVO_TOOL } AgentConvoKind;
typedef struct AgentConvoView {
    AgentConvoKind kind;
    uint32_t call_id;
    const char *text; uint32_t text_len;
    const char *name; uint32_t name_len;
    const char *args; uint32_t args_len;
} AgentConvoView;
typedef cxf_result (*AgentConvoVisit)(void *, const AgentConvoView *);
cxf_result agent_llm_visit_conversation(const char *log, uint32_t len, AgentConvoVisit, void *ctx);

/*
 * 请求体（纯函数，零分配）。
 *
 * 形状：`{"model":M,"messages":[...],?,"tools":[...],"tool_choice":"auto"?,"temperature":0}`
 *
 * messages 由 `convo_render` 从 prompt 渲染（v1.14.8）：prompt 是 agent.c
 * 追加出来的那条 transcript blob（问题段 + #call/#result/#fail 记录），
 * 渲染成**标准 tool-calling messages** —— 调用→assistant(tool_calls)、
 * 结果→tool、`#budget:`→user 注记。**没有标记的 plain prompt 渲染成一条
 * user 消息，与旧版本逐字节相同**（向后兼容由这条兜底）。
 *
 * system 那一条**只在 cfg.system 非空时出现**，且排在渲染结果之前 ——
 * 于是"不用 system"的调用与旧版本逐字节相同。
 *
 * cfg.tools 非空时在 messages 之后插入 `"tools":[...],"tool_choice":"auto"`。
 * 追加而不是前置，仍是为了同一条理由：没有 tools 的请求体与旧版本逐字节相同。
 * 顺带说明为什么 JSON 字段序对前缀缓存无关：真正决定前缀的是 chat template，
 * 而模板总把 tools 渲染在 messages 之前 —— 所以这里怎么排都不影响缓存键。
 *
 * ★ 确定性是硬契约：同一组入参，两次调用必须产出**逐字节相同**的结果。
 * 理由不是洁癖 —— 这个字节串就是 CDC 的输入，也是推理引擎 prefix cache 的键。
 * 只要它每次不同（时间戳、随机 id、map 遍历序、浮点格式化差异），
 * 分块每次都会变，前缀缓存命中率归零。所以这里不许出现时间、不许出现随机、
 * 不许用 %f 打印浮点（"temperature":0 是常量）。
 *
 * 返回值是**需要**的字节数（snprintf 语义）：> out_cap 表示没放下，缓冲区内容
 * 可能被截断，调用方必须比较而不是直接信任。返回 0 表示入参非法。
 */
uint32_t agent_llm_build_request(const AgentLlmCfg *c,
                                 const char *prompt, uint32_t prompt_len,
                                 char *out, uint32_t out_cap);

uint32_t agent_llm_data_class(const char *,uint32_t);

/* Stateless, tool-free auxiliary data channel; user bytes are not transcript.
 * Fixed max_tokens=256; rejects tool calls and non-stop completion reasons. */
uint32_t agent_llm_build_text_request(const AgentLlmCfg *, const char *, uint32_t, char *, uint32_t);
cxf_result agent_llm_chat_text(AgentLlmCfg *, const char *, uint32_t,
                              char *, uint32_t, char *, uint32_t, uint32_t *);

/* ---- tools 清单（清单走协议、动作走文本）---- */

/*
 * JSON 字符串转义：把 s[0..n) 写成字符串**体**（不含两侧引号）。
 * 返回写入/需要的字节数（snprintf 语义：> cap 表示没放下）；out 为 NULL 时只计数。
 * `>= 0x80` 的字节原样透传（入参按 UTF-8 处理）：转成 `\uXXXX` 会让同一个汉字
 * 有多种合法编码，前缀就不再稳定 —— 而前缀稳定是本项目一切缓存的前提。
 *
 * 为什么它必须是**公开的唯一一份实现**：目录树与 tools 清单都要转义描述文本，
 * 而"两处各写一份"必然漂移 —— 一边补了代理对、另一边没补，症状是某个工具名
 * 里出现 emoji 时清单悄悄变成非法 JSON。语法处理只该有一份。
 */
uint32_t agent_llm_json_esc(const char *s, uint32_t n, char *out, uint32_t cap);

/*
 * 工具清单的一项。这是**协议层视角**的形状，不是 CapDef：
 * 协议只认识 name / description / parameters，不认识 tier / trust / 降级链。
 * 谁把 CapDef 翻译成它，谁负责过滤与排序（目录侧，见 AgentRegistry.tools）。
 */
typedef struct AgentToolSpec {
    const char *name;        /* "fs.grep" */
    const char *desc;        /* 干什么 */
    const char *in_schema;   /* 怎么填 args（散文，人读的契约） */
    const char *out_schema;  /* 产出什么形状（帮模型读懂工具结果） */
} AgentToolSpec;

/*
 * 渲染成 OpenAI tools 数组。返回字节数（snprintf 语义）。
 *
 * 形状：
 *   [{"type":"function","function":{"name":N,"description":D,
 *     "parameters":{"type":"object","properties":{"args":{"type":"string",
 *     "description":IN}},"required":["args"],"additionalProperties":false}}},...]
 *
 * ★ 每个工具只有**一个字符串参数 args**，这是刻意的，不是偷懒：
 *   我们的能力 ABI 是字节进字节出（CapImpl.exec）。若给每个工具编一份结构化
 *   JSON Schema，模型就会产出 per-tool 结构化的 arguments，解码器数量随工具数
 *   线性增长，每个都要防幻觉字段、各自一套版本兼容 —— 那正是 §3 第 19 条
 *   （免转义 ⇒ 只有一个解码器）要避免的局面。
 *   钉成一个字符串，模型的自由度只剩"这串字节写什么"，而它进去的东西
 *   与文本通道**同一个解码器**。
 *   代价明确：模型得把结构化意图编码成我们的文本格式。这个代价可测（拿真实
 *   模型量），不该先假设它更差。
 *
 * ★ 不排序：specs 的顺序就是字节顺序，排序是调用方的责任（目录已经按名字排过）。
 *   在这里再排一次等于同一件事有两份实现，而"渲染"与"排序"是两件事。
 */
uint32_t agent_llm_render_tools(const AgentToolSpec *specs, uint32_t n,
                                char *out, uint32_t out_cap);

/*
 * 一次工具调用的解码结果。name 空串 = 没有工具调用（见 parse_tool_call 的 present）。
 */
typedef struct AgentToolCall {
    char     name[AGENT_LLM_TOOL_NAME_CAP];
    char     args[AGENT_LLM_TOOL_ARGS_CAP];
    uint32_t args_len;
} AgentToolCall;

/*
 * 从响应里取 `choices[0].message.tool_calls[0].function.{name,arguments}`。
 *
 *   CXF_OK + *present=true   取到了
 *   CXF_OK + *present=false  **响应里根本没有 tool_calls** —— 不是错误，
 *                            是"这一轮走的是文本通道"，调用方据此回落
 *   CXF_ERR_FORMAT           tool_calls 存在但结构坏 / arguments 不是
 *                            `{"args":"..."}` / 转义非法
 *   CXF_ERR_FULL             name 或 args 超过静态上限 —— 不截断，如实失败
 *
 * ★ present 这个出参不是可有可无：只返回 FORMAT 会让"没有这个通道"与
 *   "这个通道坏了"变成同一种结果，而它们的处理方式相反（前者回落，后者报警）。
 *   与 agent_act_decode 同一条原则：产出方可能是模型，不是我们那份代码。
 *
 * ★ arguments 是**字符串里套 JSON**（模型把 JSON 编码成字符串再塞进 JSON），
 *   所以要解两层。第二层我们只认一个固定形状 `{"args":"..."}` —— 不通用解析，
 *   因为通用解析会让"模型发明了新字段"变成静默忽略。
 */
cxf_result agent_llm_parse_tool_call(const char *json, uint32_t len,
                                     AgentToolCall *out, bool *present);

/*
 * 一次响应里最多接受几路并行工具调用（§4.17.9）。
 *
 * 为什么要有上限而不是"有多少收多少"：模型偶发会返回几十个 `tool_calls`
 * （重试性输出、被截断的循环）。全收下来会在一轮里派生几十个条目，
 * 把 `pending` 撑成一个巨型 fan-in，而真正确认那件事只需要几条。
 * **超限走失败路径并计数，不截断** —— 截断等于替模型决定"后两条不重要"，
 * 而那两条的结果会从上下文里消失，模型没有任何办法知道。
 */
#define AGENT_LLM_MAX_CALLS 8u

/*
 * 一次响应 → **N 条** `agent.action` 的规范字节，**首尾相接**（§4.17.9）。
 *
 * 为什么用"拼接"而不是新造一个容器格式：每条动作自带 `cap_len`，长度自定界
 * （见 `agent_act_bytes`），于是 N 条相接与 1 条在字节层是同一种东西 ——
 * 下游不需要"多路"这个概念，只需要一个 `while` 循环。**零新格式、零新解码器。**
 *
 *   CXF_OK            取到 *out_count 条（>= 1），*out_len 是拼接后的总字节
 *   CXF_ERR_FORMAT    结构坏 / 其中**任何一条**不合法 —— 整批作废，**不做部分应用**
 *   CXF_ERR_FULL      out_cap 不够（含**总量**不够：N 条一起放不下就整批失败）
 *   CXF_ERR_RANGE     条数超过 `AGENT_LLM_MAX_CALLS`
 *
 * ★ 为什么"部分应用"是错的：模型要了 3 件事，我们只派了 2 件，第 3 件的输出
 *   永远不回上下文，而模型看到的摘要**没有任何痕迹**说明少了一件。它会基于
 *   2 件编一个结论。要么整批派出去，要么整批失败 —— 与解码那条"不半解读"同源。
 *
 * ★ 文本通道（content 里带 `CALL|`）**只能是 1 条**。这不是偷懒：文本格式的
 *   体是"第一个换行之后的全部字节"，第二条头会与第一条的体不可区分；要分开就
 *   得引入转义，而那正是 §4.8 拒绝文本线格式的理由。协议通道（`tool_calls`）
 *   本来就是结构化的，所以 fan-out 只在那里存在 —— 不对称，但**代价是零**。
 */
cxf_result agent_llm_actions_from_response(const char *json, uint32_t len,
                                           char *out, uint32_t out_cap,
                                           uint32_t *out_len, uint32_t *out_count);

/*
 * 把一次响应翻译成 `agent.action` 线格式（§3 第 19 条）。
 *
 * ★ v1.13 起这是**单条入口**，只有一处还用它：需要断言"这一轮只有一条"的地方。
 *   生产路径走 `agent_llm_actions_from_response`（N 条）。这个函数是它的包装：
 *   真拿到多条时返回 `CXF_ERR_RANGE` —— 它**表达不了**多路，不能装作表达得了。
 *
 *   tool_calls 存在        → `CALL|<name>\n<args>`
 *   否则 content 带判别符   → 原样（模型学会了我们的格式）
 *   否则 content 是纯文本   → `DONE\n<content>`（**这就是"说完了"**）
 *   content 为空           → 原样（0 字节，留给下游判 FORMAT）
 *
 * ★ 这是整个改动里最关键的一处：**下游零改动**。
 *   `inject_actions` 仍然只认 `CALL|cap\nargs` 这一种字节形状，于是
 *   "清单走协议、动作走文本"成立 —— 协议通道只换掉了上游的表达方式，
 *   没有在下游多长出一个解码器。
 *
 * ★ 第三条（纯文本 ⇒ 终值）是**接真实模型当天补的**：判别符是我们内部的约定，
 *   模型不知道；它不调工具时就是直接说一句话。不翻译，整条任务必然 FORMAT 失败。
 *   这不是宽松解析 —— 空内容刻意**不**包装成 `DONE\n`，那会把"token 用完"伪装成
 *   "答完了"（reasoning 模型的典型形状）。
 *
 * 代价：一次额外的形状判断。收益：解码器数量保持 1。
 */
cxf_result agent_llm_action_from_response(const char *json, uint32_t len,
                                          char *out, uint32_t out_cap,
                                          uint32_t *out_len);

/*
 * 从 OpenAI 兼容响应里取 `choices[0].message.content`（纯函数，零分配）。
 *
 *   CXF_OK          取到了，*out_len 是写入 out 的字节数（已解转义）
 *   CXF_ERR_FULL    content 比 out_cap 大 —— 调用方扩容重试
 *   CXF_ERR_FORMAT  JSON 坏 / 少了 content / content 是 null / 转义非法
 *
 * 刻意**不做"尽量解读"**：一套宽松解析（找不到 content 就退回拿全文）会让
 * "模型返回了错误结构"和"模型说了这句话"变成同一种结果，而这两种的处理方式
 * 相反。这一点与 agent_act_decode 同一条原则：产出方可能是模型，不是我们。
 */
cxf_result agent_llm_parse_response(const char *json, uint32_t len,
                                    char *out, uint32_t out_cap, uint32_t *out_len);

/*
 * 组装：建请求 → POST → 解析。
 *
 * scratch 是响应原始字节的落点，由调用方给（大小见 AGENT_LLM_RAW_CAP 的建议值）。
 * 它是排查"模型到底回了什么"的唯一证据，所以不藏在函数内部的静态缓冲里。
 *
 * **请求字节为什么不用返回**：`agent_llm_build_request` 是确定性纯函数，
 * 调用方要那串字节时**重算一遍就等价于缓存它**。确定性在这里第一次变成
 * 结构性的红利 —— 少一个输出参数、少一处"缓存与真值不同步"的可能。
 * （CDC 要切的正是这串请求字节：前缀稳定 ⇒ 分块稳定 ⇒ KV cache 命中。）
 *
 * ★ `c` 不再是 `const`：这两个入口会写回 `last_status` / `last_rc`（见结构体里
 *   那段）。**签名里去掉 const 是有意的**——它不是"顺手改个限定符"，而是把
 *   "这次调用会留下观测"这件事交给编译器看守。留着 const 再去强转，
 *   下一个读这个头的人会以为它无副作用。
 */
cxf_result agent_llm_chat(AgentLlmCfg *c,
                          const char *prompt, uint32_t prompt_len,
                          char *scratch, uint32_t scratch_cap,
                          char *out, uint32_t out_cap, uint32_t *out_len);

/*
 * 同上，但解的是 `agent.action`：先看 tool_calls，没有才回落 content。
 *
 * 为什么必须是**两个入口**而不是一个带开关：产出类型是能力的注册期事实
 * （`CapDef.out_iface`），一个 handler 生产的是数据还是控制流，在装配时就定了。
 * 让它跑在运行期由参数决定，等于把一个静态事实退化成运行时数据 ——
 * 与"凡是本可以提前知道的，别让它退化成运行时数据"直接相反。
 *
 * ★ 产出的是**规范二进制动作字节**（可能 N 条首尾相接，见
 *   `agent_llm_actions_from_response`），**不是文本**。v1.13 之前是文本，
 *   于是每个调用方都得自己再来一遍"文本 → 编码"；那段转换忘了写就是
 *   "executed +1 但工具没跑过"（`agentctl` 第一版正是这么错的）。
 *   把转换收进这一层之后，那类错误**在结构上不可能**：调用方拿到的就是终态。
 */
cxf_result agent_llm_chat_action(AgentLlmCfg *c,
                                 const char *prompt, uint32_t prompt_len,
                                 char *scratch, uint32_t scratch_cap,
                                 char *out, uint32_t out_cap, uint32_t *out_len);

#endif
