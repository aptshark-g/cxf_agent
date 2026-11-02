#ifndef AGENT_HTTP_H
#define AGENT_HTTP_H
#include "agent_policy.h"

#include <stdbool.h>
#include <stdint.h>

#include "cxf/base.h"

/*
 * 极小 HTTP/1.1 客户端：明文 TCP，一次请求一次响应，Connection: close。
 *
 * 边界（这是本文件存在的全部理由，写清楚才不会长歪）：
 *   只做传输。不认识 JSON、不认识 OpenAI、不认识 tool_calls ——
 *   协议语义在 agent_llm.h，这一层只负责把一串字节送到并对端把一串字节取回来。
 *
 * 为什么不做 TLS：真实的部署形态是"本地 relay-gateway"，地址就是
 * http://127.0.0.1:<port>。要直连厂商才需要 TLS，那一天把 TLS 加在
 * **这一层的下面**，agent_llm 与能力层都不用改 —— 这正是分层的兑现。
 *
 * 为什么不复用现成库：协议转换是成熟领域，成熟的意思是**规格稳定、可对照验证**，
 * 不是"必须用库"。而 C 侧没有 LiteLLM 那种现成物；净结果是这一层约 180 行、
 * 零依赖、可断点。**接第二家厂商之前不许再扩它** —— 那时该扩的是网关。
 *
 * 已知不做（都是"现在没有调用方"）：
 *   重定向、HTTP/2、gzip、代理、IPv6 字面量、连接复用、请求签名。
 *   每一条都不难，但每一条都只在"直连公网"时才需要，而现在的部署是本地环回。
 */

#define AGENT_HTTP_HDR_CAP        2048u
#define AGENT_HTTP_TIMEOUT_MS     30000u

typedef struct AgentHttpReq {
    const char *host;      /* "127.0.0.1" 或域名（无 SNI，因此域名只对明文有意义） */
    uint16_t    port;
    const char *path;      /* "/v1/chat/completions" */
    const char *auth;      /* 原样塞进 Authorization 的值；NULL = 不带 */
    const char *extra_hdr; /* 额外请求头，必须以 CRLF 结尾；NULL = 无 */
    const char *body;
    uint32_t    body_len;
    uint32_t    timeout_ms;/* 0 = 用默认值 */
    const AgentPolicy *policy;
    uint32_t scope, data_class; /* UNKNOWN is denied unless cloud egress is enabled */
} AgentHttpReq;

typedef struct AgentHttpResp {
    int      status;    /* HTTP 状态码。4xx/5xx 不是传输失败，是应用层的事 */
    char    *body;      /* 指向调用方 buf 内部；chunked 已在原地解码 */
    uint32_t body_len;
    /*
     * **可能被截断**：buf 恰好装满，因此"响应已经结束"无法确认。
     *
     * 为什么要有这个字段：`read_until_eof` 在 cap 满时停止读，然后按正常路径
     * 解析 —— 于是调用方拿到一个**看起来完整**的短响应。上一版注释写
     * "响应超出 buf ⇒ IO"，实现里并没有这个检查（写了但没落）。
     * 后果不是崩溃，是**低报**：JSON / HTML 被截断后，"解析失败"会被归因成
     * "对端返回了坏内容"，而真实原因是"我们没读全"。
     *
     * 语义刻意保守：`raw == cap` 就可能为真，哪怕响应恰好装满。宁可误报一次
     * 截断，也不要静默地把半截内容当成全部 —— **有界可以，静默不行。**
     */
    bool     truncated;
} AgentHttpResp;

/*
 * 发一次 POST。buf 是调用方提供的落点，响应原始字节整个落进去。
 *
 * 返回值：
 *   CXF_OK        拿到了响应（status 可能是 200 也可能是 500 —— 那要调用方判）；
 *                 是否读全看 `resp.truncated`
 *   CXF_ERR_IO    连接 / 超时 / 报文格式不认识（含"找不到头体分界"）
 *
 * 校验与策略分开：这里不判断 200 与否，也不读 error 字段。重试策略是上层的事，
 * 传输层一旦开始"聪明地重试"，重试次数就会散落在两层里而没人数得清。
 *
 * ★ 重试在这里格外贵：一次 POST 可能已经让对方**做完了那件事**（下单 / 扣费 /
 *   发通知）。所以上层看到 `truncated` 时不该"再要一倍"，而该把"没读全"这件事
 *   报给调用者 —— 见 `agent_net.h` 里 `#truncated` 的处置。
 */
cxf_result agent_http_post(const AgentHttpReq *req, char *buf, uint32_t cap,
                           AgentHttpResp *resp);

#endif
