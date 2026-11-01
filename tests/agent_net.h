#ifndef AGENT_NET_H
#define AGENT_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "cxf/base.h"

#include "agent_caps.h"

/*
 * `http.post` —— 出网族第一个能力，也是目录里**第一个"副作用发生在我们的世界之外"**
 * 的能力。前四刀（grep / write_new / diff / write_apply / append）的失败都留在本机，
 * 这一刀的失败**收不回来**：对方已经收到了。
 *
 * ---- 它为什么必须存在 ----
 *
 * 前面所有能力都在回答"本机文件里有什么"，而 agent 真正要做的事常常是"把这个
 * 交出去"。没有它，模型只能把要发的内容写进文件，然后等人去发 —— 那不是自动化。
 * 但它进来的同时带来了两个前四刀都没有的东西：**不可撤销的对外动作**，以及
 * **凭据**。这两条决定了下面每一个设计决定。
 *
 * ---- L2 参数准入的第二份实现：根来自部署，路径来自模型 ----
 *
 * SSRF 的形状是"模型给了 host"。所以 host / port 一律来自 `ctx`（部署事实，
 * 模型看不见也改不了），模型只能给 **path**。这与 `fs.*` 完全同构：
 *
 *     fs : 根目录来自部署，相对路径来自模型，谓词 = realpath(父) ⊆ 根
 *     net: host:port 来自部署，路径来自模型，谓词 = 形状白名单 + 无 '..' 段
 *
 * 于是"打到内网 metadata 服务"这件事在**类型上就表达不出来** —— 不是靠运行时
 * 黑名单挡下来的（黑名单永远漏）。这是本项目反复用的那条：**把自由度收窄，
 * 比把检查写严更重要**。
 *
 * ---- 凭据：只进一个地方，且那个地方不接受模型输入 ----
 *
 * `cfg->auth` 只被塞进 `AgentHttpReq.auth`，它：
 *   1. **不来自 payload** —— payload 里根本没有凭据字段，模型无从提供；
 *   2. **不回显在收据里** —— 任何返回值都不含它（连错误信息也不带）；
 *   3. **不进输出缓冲区** —— 它只活在请求头那一行，走了就没了。
 * 这条不需要检查，因为结构上不可能违反（payload 只有两段：path 和 body）。
 * 但写下来是有用的：下一个想"顺手支持个 header 参数"的人会看到这条为什么在。
 *
 * ---- 方言：`path\nbody`（与 fs 族同形，两段） ----
 *
 *   path  以 '/' 开头，不带 scheme / host / query 之外的任何结构
 *   body  原样作为请求体发出（Content-Type: application/json）
 *
 * 没有第三个字段（没有 method、没有 header、没有 expect）。加 method 的那天，
 * 就是 GET 进来那天 —— 那时它是一个**新能力**，不是这个能力的参数（与
 * `fs.diff` 拒绝 dry-run 开关、`fs.append` 拒绝 mode 参数同一条理由）。
 *
 * ---- ★ 永不 `FULL`：截断用 `#truncated` 报，不用 FULL ----
 *
 * 这是本能力最容易被写错的一处。框架的语义是：`FULL` ⇒ **把 exec 从头再调一次**
 * （`tests/agent.c:1821-1834`）。对 fs 族那是"白写一次"，对 `http.post` 那是
 * **再发一次请求** —— 对方收到两次。所以：
 *
 *   - `out_hint` 一次给够（`HTTP_RESP_MAX + 128`），让第一次就不该 FULL；
 *   - 响应真的超过上限 ⇒ **截断 + 标记 `#truncated` + rc = OK**，不是 FULL。
 *     因为"内容多"在这里**不等于**"再给一倍有用"：再给一倍意味着再发一次 POST。
 *
 * 这与 `fs.diff` 的 `#coarse` / `#omitted` 是同一条纪律的第二个实例：
 * **有界内的截断不能借 `FULL` 报**，那会白烧一次重跑，而这次重跑有钱的代价。
 *
 * ---- 收据 ----
 *
 *   <status> <b>\n<body>               ← 第一行元信息，其后是响应体原文
 *   <status> <b> #truncated\n<body>    ← 只读到了前 b 字节
 *
 * `status` 是 HTTP 状态码（4xx/5xx 不是错误码，是**事实**：对端说你错了）。
 * 把 5xx 映射成 `CXF_ERR_*` 会让"对方拒绝了我"与"我没能问出口"混成一件，
 * 而这两件事的下一步完全不同（改参数 vs 重试）。
 *
 * ---- 三种错分开报 ----
 *
 *   FORMAT  payload 没有 path（第一段为空）
 *   RANGE   路径形状不合白名单 / 请求体超过 HTTP_BODY_MAX / payload 超长
 *   IO      连不上 / 超时 / 报文不认识（含找不到头体分界）
 */
#define HTTP_PATH_MAX      256u
#define HTTP_BODY_MAX      8192u
#define HTTP_RESP_MAX      16384u

/* 起始容量：一次给够，于是"扩容重跑"这条路在正常部署里根本不会发生。 */
#define HTTP_OUT_HINT      (HTTP_RESP_MAX + 128u)

/* 收据头（第一行）的上限：`<status> <b> #truncated\n` 最长 22 字节，留够余量。 */
#define HTTP_OUT_MAX       96u

/* 响应类型的名字。`http.resp` 与 `fs.wrote` 是两个类型 —— 形状不同：
 * 那个是"一行动词 + 字节数"，这个是"状态码 + 字节数 + 正文"。 */
#define IFACENAME_HTTP_RESP "http.resp"

extern const IfaceDef http_iface_resp;

/*
 * 装配期给定的实现参数（`CapImpl.ctx`）。
 *
 * `host` / `port` 是**部署事实**：这个进程能往哪台机器发，是启动时决定的，
 * 不由模型逐次指定。`auth` 是凭据，`NULL` = 不带 Authorization 头。
 */
typedef struct HttpCfg {
    const char *host;        /* "127.0.0.1" 或域名（明文，无 SNI） */
    uint16_t    port;
    const char *auth;        /* 原样塞进 Authorization；不进 payload、不回显 */
    uint32_t    timeout_ms;  /* 0 = 用 agent_http 的默认值 */
} HttpCfg;

extern const CapDef  http_capdef_post;
extern const CapImpl http_capimpl_post;

/*
 * handler。args = `path\nbody`（与 fs 族同一方言，切法共用 `agent_args.h`）。
 *
 * 三种错分开报：
 *   FORMAT  没有 path
 *   RANGE   路径形状不合白名单 / 请求体超 `HTTP_BODY_MAX` / 输出缓冲区不足
 *           （最后这条**故意不是 FULL**，见上面"永不 FULL"那段）
 *   IO      连不上 / 超时 / 报文不认识
 */
cxf_result http_post_exec(const void *ctx, const char *args, uint32_t args_len,
                          char *out, uint32_t out_cap, uint32_t *out_len);

#endif
