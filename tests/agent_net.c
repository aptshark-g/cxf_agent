/*
 * `http.post` 的实现：目录里第一个"副作用发生在我们的世界之外"的能力。
 *
 * 它只做三件事：判路径形状、把两段 payload 交给传输层、把状态码与正文
 * 装进收据。**不做**：重定向跟随、重试、JSON 解析、方法协商、请求头注入。
 *
 * 前四刀的失败都留在本机（多建一个文件、多写半段日志），它们的收据可以
 * 说"我这次没做"。这一刀不能：请求发出去之后，对方已经做完了那件事。
 * 所以"永不 FULL"不是保守，是这个能力的正确性前提 —— FULL 的语义是
 * "再给一倍，我重跑一遍"，而重跑在这里等于再发一次。
 */

#include <stdio.h>
#include <string.h>

#include "cxf/service.h"

#include "agent_args.h"
#include "agent_http.h"
#include "agent_net.h"

/*
 * 路径形状准入（L2 的第二份实现，与 `resolve_in_root` 同层级）。
 *
 * 它守的不是"文件在不在根里"，而是**"模型能不能自己挑对端"**：host / port
 * 来自 ctx，模型只有这一段字节。所以白名单要窄到"只可能是一条路径"。
 *
 * 为什么不收 `@` 与 `:`：这两个字符在 authority 段里是分隔符
 * （`user@host`、`host:port`）。在 path 里它们本来无害，但留着只会让
 * "POST http://evil/x" 这种 payload 看起来**像**合法输入 —— 拒掉它比解释它便宜。
 */
static bool http_path_ok(const char *p, uint32_t n) {
    if (n == 0u || n > HTTP_PATH_MAX) return false;
    if (p[0] != '/') return false;      /* 必须是绝对 path，相对路径没有"对端"可言 */

    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];

        /* 控制字符与空格：后者会截断请求行，前者能注入第二个头 */
        if (c <= 0x20u || c == 0x7fu) return false;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '.' || c == '_' || c == '~' ||
            c == '?' || c == '&' || c == '=' || c == '%' || c == '+')
            continue;
        return false;
    }

    /* `.` / `..` 段：`..` 能打到同主机的另一个端点，而"另一个端点"正是
     * 这条白名单想排掉的东西。'.' 只是噪音，一起拒。 */
    for (uint32_t i = 0; i + 1u < n; i++) {
        uint32_t j;

        if (p[i] != '/' || p[i + 1u] != '.') continue;
        j = i + 1u;
        while (j < n && p[j] == '.') j++;
        if (j == n || p[j] == '/') return false;
    }

    /* 空段（`//`）：URL 语义里它是 authority 的分隔符。真实的 API 路径不会
     * 有连续两个斜杠，而留着它只会让 `POST //evil.example/x` 这种 payload
     * **看起来**像换了对端 —— 拒掉比解释便宜。 */
    for (uint32_t i = 0u; i + 1u < n; i++)
        if (p[i] == '/' && p[i + 1u] == '/') return false;

    return true;
}

cxf_result http_post_exec(const void *ctx, const char *args, uint32_t args_len,
                          char *out, uint32_t out_cap, uint32_t *out_len) {
    const HttpCfg *cfg = (const HttpCfg *)ctx;
    char        pbuf[HTTP_PATH_MAX + 1u];
    const char *path, *body;
    uint32_t    plen = 0u, blen = 0u, hlen;
    AgentHttpReq  req;
    AgentHttpResp resp;
    cxf_result   rc;
    int          tn;

    if (out_len != NULL) *out_len = 0u;
    if (out == NULL || out_cap == 0u) return CXF_ERR_RANGE;
    if (cfg == NULL || cfg->host == NULL) return CXF_ERR_RANGE;
    if (args == NULL || args_len == 0u) return CXF_ERR_FORMAT;

    split_args(args, args_len, &path, &plen, &body, &blen);
    if (plen == 0u) return CXF_ERR_FORMAT;
    if (!http_path_ok(path, plen)) return CXF_ERR_RANGE;
    if (blen > HTTP_BODY_MAX) return CXF_ERR_RANGE;

    /*
     * ★ 缓冲不足在这里是 **RANGE，不是 FULL**。
     *
     * 框架看到 `FULL` 会**把 exec 从头再调一次**（`tests/agent.c:1821-1834`）。
     * 对写盘族那是幂等重做；对出网能力那是**再发一次请求** —— 对方收到两次。
     * 所以"我的输出缓冲区不够"这个事实，在这个能力上必须报成"设计边界"
     * （重试无用），把重跑的许可收回去。`out_hint` 的存在就是为了让这一行
     * 在正常部署里永远不触发。
     */
    if (out_cap <= HTTP_OUT_MAX) return CXF_ERR_RANGE;

    /* 传输层要 NUL 结尾的 path（它用 %s 拼请求行），而 payload 是字节串 */
    memcpy(pbuf, path, plen);
    pbuf[plen] = '\0';

    memset(&req, 0, sizeof req);
    req.host       = cfg->host;        /* 部署事实：模型给不了 */
    req.port       = cfg->port;
    req.path       = pbuf;
    req.auth       = cfg->auth;        /* 只进这一个地方，见头文件第三条 */
    req.body       = body;
    req.body_len   = blen;
    req.timeout_ms = cfg->timeout_ms;

    /*
     * 响应落点紧接在收据头区之后：`out[0..HTTP_OUT_MAX)` 留给第一行，
     * 之后整段给传输层（它会把状态行 + 头 + 正文都写进去）。
     * 这样不需要额外的临时缓冲 —— 正文剥离后往前挪一次就够了。
     */
    rc = agent_http_post(&req, out + HTTP_OUT_MAX, out_cap - HTTP_OUT_MAX, &resp);
    if (rc != CXF_OK) return rc;       /* IO：连不上 / 超时 / 报文不认识 */

    /* `<status> <b>[ #truncated]\n` —— 第一行只说事实，不下判断。
     * 5xx **不**映射成 CXF_ERR_*：对端拒绝我与我没能问出口，下一步完全不同。 */
    tn = snprintf(out, HTTP_OUT_MAX, "%d %u%s\n",
                  resp.status, (unsigned)resp.body_len,
                  resp.truncated ? " #truncated" : "");
    if (tn < 0 || (uint32_t)tn >= HTTP_OUT_MAX) return CXF_ERR_RANGE;   /* 不可达 */
    hlen = (uint32_t)tn;

    /* 正文从 out+HTTP_OUT_MAX 之后前移到 out+hlen：目标在前、源在后，
     * 即使有重叠 memmove 也是对的；而 hlen < HTTP_OUT_MAX 保证了不越界。 */
    if (resp.body_len > 0u)
        memmove(out + hlen, resp.body, resp.body_len);

    if (out_len != NULL) *out_len = hlen + resp.body_len;
    return CXF_OK;
}

/* ---- 契约与实现描述符 ---- */

const IfaceDef http_iface_resp = {
    IFACENAME_HTTP_RESP,
    "一次出网调用的结果：第一行 <status> <bytes>[ #truncated]，其后是响应正文",
    IFACE_INT
};

const CapDef http_capdef_post = {
    CAPNAME_HTTP_POST,
    "向部署指定的对端 POST 一个请求体，返回状态码与响应正文",
    "path\\nbody（与 fs 族同形：第一行路径，其后是完整请求体）",
    "<status> <b>\\n<body>；只读到前 b 字节时第一行带 #truncated",
    { IFACENAME_TASK, NULL }, IFACENAME_HTTP_RESP
};

/*
 * ★ trust 是 **CAP_TRUST_WRITE**，理由与写盘族不同但同档：档位的含义是
 *   "能造成多大的不可逆后果"。写盘毁的是本地文件，出网毁的是**别人的账**——
 *   两者都收不回来，所以同档。`CapTrust` 的注释里本来就写着"写盘 / 支付 /
 *   对外副作用"，这一刀是那句话第一次有了实现。
 *   要表达"只给出网、不给写盘"的部署：注册 http.post、不注册 fs 写族，
 *   天花板仍是 WRITE —— 天花板管"最多能造成什么"，注册管"实际有哪个"。
 *
 * ★ cost = 50 比写盘族的 10~20 高：一次出网是一个 RTT，且可能计费。
 *   这个数只用于路由与投机政策（§4.7），不影响正确性。
 *
 * ★ 同样**不进 bundle**（无 `CXF_SERVICE`）：能往哪台机器发是部署事实，
 *   bundle 里放一个默认 host 等于给所有部署塞了一个没人同意的对端。
 */
const CapImpl http_capimpl_post = {
    CAPNAME_HTTP_POST, "net", CAP_TIER_SCRIPT, 50, CAP_TRUST_WRITE,
    http_post_exec, NULL, HTTP_OUT_HINT, NULL
};
