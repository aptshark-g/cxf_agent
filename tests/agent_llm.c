#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_action.h"
#include "agent_http.h"
#include "agent_llm.h"

/* ---- 配置 ---- */

/* URL 解析：http://host[:port]/path。只认明文，只认一条路径。
 * 故意不接受 https:// —— 静默降级成明文比直接失败危险得多。
 * 独立公开是为了让环境变量与 JSON 共用同一个解析器，而不是养两份口径。 */
cxf_result agent_llm_cfg_set_url(AgentLlmCfg *c, const char *url) {
    const char *p, *hs, *he, *ps;
    uint32_t n;
    uint32_t port = 80u;

    if (c == NULL || url == NULL) return CXF_ERR_RANGE;
    if (strncmp(url, "http://", 7u) != 0) return CXF_ERR_FORMAT;
    p = url + 7;
    hs = p;
    while (*p != '\0' && *p != ':' && *p != '/') p++;
    he = p;
    if (*p == ':') {
        port = 0;
        p++;
        if (*p < '0' || *p > '9') return CXF_ERR_FORMAT;
        while (*p >= '0' && *p <= '9') {
            port = port * 10u + (uint32_t)(*p - '0');
            if (port > 65535u) return CXF_ERR_FORMAT;
            p++;
        }
    }
    ps = (*p == '/') ? p : "/";
    n = (uint32_t)(he - hs);
    if (n == 0 || n >= AGENT_LLM_HOST_CAP) return CXF_ERR_RANGE;
    memcpy(c->host, hs, n);
    c->host[n] = '\0';
    c->port = (uint16_t)port;
    n = (uint32_t)strlen(ps);
    if (n >= AGENT_LLM_PATH_CAP) return CXF_ERR_RANGE;
    memcpy(c->path, ps, n);
    c->path[n] = '\0';
    return CXF_OK;
}

cxf_result agent_llm_cfg_from_env(AgentLlmCfg *c) {
    const char *url = getenv("CXF_LLM_URL");
    const char *model = getenv("CXF_LLM_MODEL");
    const char *key = getenv("CXF_LLM_KEY");
    const char *sys = getenv("CXF_LLM_SYSTEM");
    uint32_t n;

    if (c == NULL) return CXF_ERR_RANGE;
    memset(c, 0, sizeof(*c));
    if (url == NULL || model == NULL) return CXF_ERR_DEP;
    {
        cxf_result rc = agent_llm_cfg_set_url(c, url);
        if (rc != CXF_OK) return rc;
    }

    n = (uint32_t)strlen(model);
    if (n == 0 || n >= AGENT_LLM_MODEL_CAP) return CXF_ERR_RANGE;
    memcpy(c->model, model, n);
    c->model[n] = '\0';

    if (key != NULL) {
        n = (uint32_t)strlen(key);
        if (n >= AGENT_LLM_KEY_CAP) return CXF_ERR_RANGE;
        memcpy(c->api_key, key, n);
        c->api_key[n] = '\0';
    }

    /* system 是**可选**的，但超长必须如实报错：截断一段格式说明的后果是
       "模型有时守格式有时不守"，而那是最难归因的一类失败。 */
    if (sys != NULL) {
        n = (uint32_t)strlen(sys);
        if (n >= AGENT_LLM_SYS_CAP) return CXF_ERR_RANGE;
        memcpy(c->system, sys, n);
        c->system[n] = '\0';
    }

    /*
     * 超时可配。写死 30 s 的代价是实测出来的：让模型输出 3000 行（≈12 KB）时，
     * 30 s 先到 —— 得到 `CXF_ERR_IO`，整条走失败（`failed 1 retried 1`，IO 不重试）。
     * 也就是说**长输出现在是一条死路**，而"多长算长"是部署事实（本地小模型与
     * 云端大模型差一个数量级），不该由协议层的常量决定。
     */
    c->timeout_ms = AGENT_HTTP_TIMEOUT_MS;
    {
        const char *to = getenv("CXF_LLM_TIMEOUT_MS");
        if (to != NULL && to[0] != '\0') {
            char *end = NULL;
            unsigned long v = strtoul(to, &end, 10);
            if (end == to || *end != '\0' || v < 1000ul || v > 600000ul)
                return CXF_ERR_RANGE;
            c->timeout_ms = (uint32_t)v;
        }
    }
    return CXF_OK;
}

/* ---- 请求体 ---- */

/*
 * 写游标：永远推进 w，只在放得下时落字节。
 * 于是"需要多少"与"放下了多少"共用一趟扫描，不需要先数一遍。
 */
typedef struct WBuf {
    char    *p;
    uint32_t cap;
    uint32_t w;
} WBuf;

static void wb_raw(WBuf *b, const char *s, uint32_t n) {
    if (n == 0) return;
    if (b->w <= b->cap && n <= b->cap - b->w && b->p != NULL)
        memcpy(b->p + b->w, s, n);
    b->w += n;
}

static void wb_str(WBuf *b, const char *s) {
    wb_raw(b, s, (uint32_t)strlen(s));
}

uint32_t agent_llm_json_esc(const char *s, uint32_t n, char *out, uint32_t cap) {
    static const char hex[] = "0123456789abcdef";
    uint32_t w = 0;

    for (uint32_t k = 0; k < n; k++) {
        unsigned char ch = (unsigned char)s[k];
        char     esc[6];
        uint32_t en = 0;

        switch (ch) {
        case '"':  esc[en++] = '\\'; esc[en++] = '"';  break;
        case '\\': esc[en++] = '\\'; esc[en++] = '\\'; break;
        case '\b': esc[en++] = '\\'; esc[en++] = 'b';  break;
        case '\f': esc[en++] = '\\'; esc[en++] = 'f';  break;
        case '\n': esc[en++] = '\\'; esc[en++] = 'n';  break;
        case '\r': esc[en++] = '\\'; esc[en++] = 'r';  break;
        case '\t': esc[en++] = '\\'; esc[en++] = 't';  break;
        default:
            if (ch < 0x20u) {
                esc[en++] = '\\'; esc[en++] = 'u'; esc[en++] = '0'; esc[en++] = '0';
                esc[en++] = hex[ch >> 4]; esc[en++] = hex[ch & 0x0fu];
            } else {
                /* >= 0x20 且不是引号/反斜杠：原样透传（含全部 UTF-8 字节） */
                if (out != NULL && w < cap) out[w] = (char)ch;
                w++;
                continue;
            }
        }
        for (uint32_t i = 0; i < en; i++) {
            if (out != NULL && w < cap) out[w] = esc[i];
            w++;
        }
    }
    return w;
}

/* 写进 WBuf。没放下也照样推进 w —— 与 wb_raw 同一语义（返回值是"需要多少"）。 */
static void wb_json(WBuf *b, const char *s, uint32_t n) {
    bool     room_ok = (b->p != NULL && b->w < b->cap);
    uint32_t room    = room_ok ? (b->cap - b->w) : 0u;

    b->w += agent_llm_json_esc(s, n, room_ok ? b->p + b->w : NULL, room);
}

/*
 * 双层转义（v1.14.8）。`arguments` 是**字符串里套 JSON**：模型那边
 * （`parse_one_call`）要解两层，我们这边就要转义两层 ——
 *   第一层：args 编进 `{"args":"<args>"}`；
 *   第二层：这整段文本编进外层 JSON 字符串。
 *
 * 逐字节映射（第一层的产物全是 ASCII，所以第二层只对 `"` 与 `\` 有活）：
 *     "      → \"      → \\\"       （4 字节）
 *     \      → \\     → \\\\       （4 字节）
 *     \n     → \n     → \\n        （3 字节）
 *     <0x20  → \u00XX → \\u00XX    （7 字节）
 *     其余（含全部 UTF-8 字节）→ 原样        （1 字节）
 *
 * ★ 与 `parse_one_call` **严格互逆**，这条互逆性有测试钉着
 *   （test_llm.c 的 round-trip 用例：渲染出的请求体经真实解码路径
 *   必须还原出逐字节相同的 args）。互逆断了不会有人报错 —— 服务端
 *   只会看到模型对着一段乱码 arguments 发呆。
 */
static void wb_json2(WBuf *b, const char *s, uint32_t n) {
    static const char hex[] = "0123456789abcdef";

    for (uint32_t k = 0; k < n; k++) {
        unsigned char ch = (unsigned char)s[k];
        char          esc[7];

        switch (ch) {
        case '"':  wb_str(b, "\\\\\\\""); continue;
        case '\\': wb_str(b, "\\\\\\\\"); continue;
        case '\b': wb_str(b, "\\\\b");    continue;
        case '\f': wb_str(b, "\\\\f");    continue;
        case '\n': wb_str(b, "\\\\n");    continue;
        case '\r': wb_str(b, "\\\\r");    continue;
        case '\t': wb_str(b, "\\\\t");    continue;
        default:
            if (ch >= 0x20u) {
                wb_raw(b, (const char *)&ch, 1u);
                continue;
            }
            esc[0] = '\\'; esc[1] = '\\'; esc[2] = 'u';
            esc[3] = '0';  esc[4] = '0';
            esc[5] = hex[ch >> 4];
            esc[6] = hex[ch & 0x0fu];
            wb_raw(b, esc, sizeof(esc));
        }
    }
}

/* ---- transcript → 标准 tool-calling messages（v1.14.8）---- */

/*
 * 为什么不能把整条 blob 塞进一条 user 消息（v1.14.3~v1.14.7 的做法）：
 *
 *   ① **协议约束，不是偏好**：OpenAI 兼容形状里 `tool` 消息必须挂在带
 *      `tool_calls` 的 assistant 消息之后。我们协议层本来就在说标准话
 *      （发 `tools`、解 `tool_calls`），唯一不标准的就是**把模型说过的话
 *      扔掉、永不回传** —— 异常在我们这边。
 *   ② **后训练形状**：模型见过成千上万轮"assistant 调工具 → tool 回结果"，
 *      没见过"user 消息里塞一段 #call/#result 文本约定"。自造约定要模型
 *      现学，而它学到的只是"看起来像"，不是结构。
 *   ③ **配对不能靠顺序**：`recycle_tick` 倒序回收 ⇒ 同 tick 内 fan-out 的
 *      记录是派出序的倒序，配额闸门下是完成序。文本约定里"第 N 个 #result
 *      配第 N 个 #call"这条隐式契约**从没有成立过**；id 配对才与顺序无关。
 *
 * blob 的形状（agent.c 追加，v1.14.8）：
 *
 *     #call  <cap> <args_len>\n<args>\n#result <cap> <res_len>B\n<body>\n
 *     #call  <cap> <args_len>\n<args>\n#fail <seq> <cap> <why>\n
 *
 * 渲染规则：
 *     第一个标记之前的字节（问题段）       → user
 *     `#call` + 入参                     → assistant（content:"" + tool_calls[]）
 *     `#result` / `#fail`                → tool（tool_call_id + content）
 *     `#budget:`                         → user 注记（装配方的声明，不是工具产出）
 *     孤儿 `#call`（完整调用、没有结局）   → assistant + tool("#lost")
 *     其余一切（认不出的行、孤儿 result）  → 归入相邻的 user 段，**一段都不丢**
 *
 * `tool_call_id` 渲染期按出现次序编号（c1 / c2 / …），**不进 blob**：
 * blob 是 prefix-stable append log，编号是渲染期的局部事实；写进去等于让
 * "第几个调用"变成一个跨轮稳定其实没有意义的字节，还每次都改缓存键。
 *
 * ★ 一轮 fan-out 的 N 条调用**不合并**成一个 assistant + N 个 tool_calls：
 *   blob 里没有"哪些调用属于同一轮"这个信息（记录按回收序落盘，而回收序
 *   既不是派出序也不是完成序），合并就是编造。N 对 assistant/tool 交替
 *   在协议上合法，且与"模型确实发了 N 次调用"这件事不冲突。
 */

typedef struct Convo {
    WBuf    *b;
    uint32_t ncalls;
    bool     first;      /* messages 数组的逗号控制 */
} Convo;

static void convo_user(Convo *c, const char *s, uint32_t n) {
    if (!c->first) wb_str(c->b, ",");
    c->first = false;
    wb_str(c->b, "{\"role\":\"user\",\"content\":\"");
    wb_json(c->b, s, n);
    wb_str(c->b, "\"}");
}

static void convo_tool(Convo *c, const char *id, const char *s, uint32_t n) {
    if (!c->first) wb_str(c->b, ",");
    c->first = false;
    wb_str(c->b, "{\"role\":\"tool\",\"tool_call_id\":\"");
    wb_str(c->b, id);
    wb_str(c->b, "\",\"content\":\"");
    wb_json(c->b, s, n);
    wb_str(c->b, "\"}");
}

static void convo_emit_call(Convo *c, char *id,
                            const char *name, uint32_t nl,
                            const char *args, uint32_t al) {
    if (!c->first) wb_str(c->b, ",");
    c->first = false;
    c->ncalls++;
    /* 12 字节 = "c" + 10 位十进制 + NUL：装得下任何 uint32 ⇒ 不可能截断。
     * （实际更宽松：blob 再大也大不过内存，ncalls 溢出需要数十 GB 的 args。） */
    (void)snprintf(id, 12u, "c%u", c->ncalls);
    wb_str(c->b, "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":"
                 "[{\"id\":\"");
    wb_str(c->b, id);
    wb_str(c->b, "\",\"type\":\"function\",\"function\":{\"name\":\"");
    wb_json(c->b, name, nl);
    wb_str(c->b, "\",\"arguments\":\"{\\\"args\\\":\\\"");
    wb_json2(c->b, args, al);
    wb_str(c->b, "\\\"}\"}}]}");
}

/* 十进制区间 → 整数。非数字或超过 10 位（uint32 上限就是 10 位）返回 false。 */
static bool cv_u32(const char *p, const char *e, uint32_t *out) {
    uint32_t v = 0;

    if (p >= e || (uint32_t)(e - p) > 10u) return false;
    for (const char *r = p; r < e; r++) {
        if (*r < '0' || *r > '9') return false;
        v = v * 10u + (uint32_t)(*r - '0');
    }
    *out = v;
    return true;
}

/*
 * 一条调用记录的解析结果。`end` 之外的字节解析器一概不看 ——
 * 长度前缀自定界，args / body 里出现 "#result" 字样不会误解析。
 */
typedef struct Rec {
    const char *name;      uint32_t name_len;
    const char *args;      uint32_t args_len;
    uint8_t     kind;      /* 0 = #result，1 = #fail */
    const char *body;      uint32_t body_len;    /* kind 0 */
    const char *fail_line; uint32_t fail_len;    /* kind 1：整行内容（含 #fail 前缀，不含换行）*/
    const char *end;       /* 记录结束后的第一字节 */
    bool        orphan;    /* 调用完整，但没有可识别的结局 */
} Rec;

/*
 * 从 `#call` 行起点解**一整条**记录（调用 + 结局）。
 * 返回 false = 这一行认不出来（调用方把它当普通行，字节留在 user 段里）。
 *
 * 认不出与孤儿的区别：前者连调用头都拼不齐；后者调用是完整的，只是结局
 * 缺失或坏在壳上。孤儿仍然发 assistant（模型确实发过这个调用），只是
 * 结局用 `#lost` 顶替 —— 协议要求 tool 消息必须跟在 assistant 之后，
 * 少一个 tool 消息，整个请求会被服务端整包拒绝。
 */
static bool parse_record(const char *ls, const char *e, Rec *r) {
    const char *le = ls, *q, *ne, *args, *args_e, *p;

    while (le < e && *le != '\n') le++;
    if (le >= e) return false;                    /* 行没有收尾：入参起点不存在 */

    q = ls + 6u;                                  /* 跳过 "#call " */
    ne = q;
    while (ne < le && *ne != ' ') ne++;
    if (ne >= le || ne == q) return false;        /* 缺长度区 / 能力名为空 */

    args = le + 1;
    if (!cv_u32(ne + 1u, le, &r->args_len)) return false;
    if (r->args_len > (uint32_t)(e - args)) return false;   /* 含回绕：声明比剩余还长 */
    args_e = args + r->args_len;
    if (args_e >= e || *args_e != '\n') return false;       /* 入参后必须是换行 */

    r->name     = q;
    r->name_len = (uint32_t)(ne - q);
    r->args     = args;
    r->orphan   = true;
    r->end      = args_e + 1u;                    /* 孤儿时从结局行继续解析 */
    p          = r->end;
    if (p >= e) return true;                      /* 调用在 blob 末尾：没有结局 */

    {
        const char *rl = p, *re = p;

        while (re < e && *re != '\n') re++;

        if (re - rl > 8u && memcmp(rl, "#result ", 8u) == 0) {
            const char *rq = rl + 8u, *rne = rq, *body;
            uint32_t    rlen = 0;
            bool        ok = false;

            while (rne < re && *rne != ' ') rne++;
            /* 形状必须是 "name digitsB"：B 紧贴行尾，且 digits 非空 */
            if (rne > rq && rne + 2u < re && *(re - 1u) == 'B')
                ok = cv_u32(rne + 1u, re - 1u, &rlen);
            if (!ok) return true;                 /* 结局行认不出 ⇒ 孤儿 */

            body = (re < e) ? re + 1 : e;
            if (rlen > (uint32_t)(e - body)) return true;
            if (body + rlen >= e || body[rlen] != '\n') return true;
            r->kind     = 0;
            r->body     = body;
            r->body_len = rlen;
            r->orphan   = false;
            r->end      = body + rlen + 1u;
            return true;
        }
        if (re - rl > 6u && memcmp(rl, "#fail ", 6u) == 0) {
            /*
             * tool 内容是**整行**（含 `#fail ` 前缀），不是光秃秃一个能力名 ——
             * 只给名字的话，模型看到的是一次"内容为 fs.grep 的成功结果"，
             * 失败被伪装成了别的形状，正是这一整套设计要消灭的东西。
             */
            r->kind      = 1;
            r->fail_line = rl;
            r->fail_len  = (uint32_t)(re - rl);
            r->orphan    = false;
            r->end       = (re < e) ? re + 1 : e;
            return true;
        }
    }
    return true;                                  /* 下一行认不出 ⇒ 孤儿 */
}

cxf_result agent_llm_visit_conversation(const char *log, uint32_t len, AgentConvoVisit visit, void *ctx) {
    if (!visit || (len && !log)) return CXF_ERR_RANGE;
    if (!log) log = "";
    const char *p = log, *e = log + len;
    const char *pend = log;    /* 待定 user 段的起点（问题段 + 一切认不出的字节）*/
    uint32_t calls = 0, emitted = 0;
    cxf_result rc;
#define CONVO_VISIT(...) do { AgentConvoView v = { __VA_ARGS__ }; \
    rc = visit(ctx, &v); if (rc != CXF_OK) return rc; emitted++; } while (0)

    while (p < e) {
        const char *ls = p, *le = p;
        Rec         r;

        while (le < e && *le != '\n') le++;

        if (le - ls > 6u && memcmp(ls, "#call ", 6u) == 0 &&
            parse_record(ls, e, &r)) {
            if (ls > pend) { CONVO_VISIT(.kind=AGENT_CONVO_USER, .text=pend, .text_len=(uint32_t)(ls-pend)); }
            calls++;
            CONVO_VISIT(.kind=AGENT_CONVO_CALL, .call_id=calls, .name=r.name, .name_len=r.name_len, .args=r.args, .args_len=r.args_len);
            if (r.orphan) { CONVO_VISIT(.kind=AGENT_CONVO_TOOL, .call_id=calls, .text="#lost", .text_len=5); }
            else if (r.kind == 0) { CONVO_VISIT(.kind=AGENT_CONVO_TOOL, .call_id=calls, .text=r.body, .text_len=r.body_len); }
            else { CONVO_VISIT(.kind=AGENT_CONVO_TOOL, .call_id=calls, .text=r.fail_line, .text_len=r.fail_len); }
            p = r.end;
            pend = p;
            continue;
        }
        if (le - ls >= 8u && memcmp(ls, "#budget:", 8u) == 0) {
            if (ls > pend) { CONVO_VISIT(.kind=AGENT_CONVO_USER, .text=pend, .text_len=(uint32_t)(ls-pend)); }
            CONVO_VISIT(.kind=AGENT_CONVO_USER, .text=ls, .text_len=(uint32_t)(le-ls));
            p = (le < e) ? le + 1 : e;
            pend = p;
            continue;
        }
        /* 普通行 / 认不出的行 / 孤儿 result|fail：留在待定段里，一段都不丢 */
        p = (le < e) ? le + 1 : e;
    }

    if (e > pend) { CONVO_VISIT(.kind=AGENT_CONVO_USER, .text=pend, .text_len=(uint32_t)(e-pend)); }
    /*
     * 空 blob：与旧版逐字节相同（一条空 user 消息）。走到这里说明整条
     * prompt 里一个标记都没有 —— 那正是 v1.14.8 之前唯一的情形。
     */
    if (!emitted) { CONVO_VISIT(.kind=AGENT_CONVO_USER, .text="", .text_len=0); }
#undef CONVO_VISIT
    return CXF_OK;
}

static cxf_result convo_json_visit(void *ctx, const AgentConvoView *v) {
    Convo *c = ctx; char id[12];
    if (v->kind == AGENT_CONVO_USER) convo_user(c, v->text, v->text_len);
    else if (v->kind == AGENT_CONVO_CALL) convo_emit_call(c, id, v->name, v->name_len, v->args, v->args_len);
    else {
        (void)snprintf(id, sizeof id, "c%u", v->call_id);
        convo_tool(c, id, v->text, v->text_len);
    }
    return CXF_OK;
}
static void convo_render(WBuf *b, const char *log, uint32_t len) {
    Convo c = { b, 0u, true };
    (void)agent_llm_visit_conversation(log, len, convo_json_visit, &c);
}

uint32_t agent_llm_build_request(const AgentLlmCfg *c,
                                 const char *prompt, uint32_t prompt_len,
                                 char *out, uint32_t out_cap) {
    WBuf b;

    if (c == NULL || c->model[0] == '\0') return 0;
    if (prompt_len > 0 && prompt == NULL) return 0;

    b.p = out;
    b.cap = (out != NULL) ? out_cap : 0u;
    b.w = 0;

    /*
     * 形状固定、字段顺序固定、无时间戳、无请求 id、无随机数。
     * "temperature":0 一并写死：不是调参偏好，是为了**重跑安全** —— §4.9 的
     * 扩容重试会重跑 handler，采样随机性会让两次调用返回不同字节，
     * 于是同一个任务的汇聚结果不确定。那是正确性问题，不是成本问题。
     */
    wb_str(&b, "{\"model\":\"");
    wb_json(&b, c->model, (uint32_t)strlen(c->model));
    wb_str(&b, "\",\"messages\":[");

    /*
     * system 那一条只在非空时出现 —— 于是"不用 system"的调用与旧版本
     * 逐字节相同。这不是兼容性洁癖：请求字节是 CDC 的输入、也是前缀缓存
     * 的键，多一个空 system 就等于悄悄改一次缓存键。
     *
     * system 放在 user 之前不是排版偏好：chat template 会把它渲染到
     * 对话最前，而"前缀稳定"正是我们要的那件事 —— 变化的业务输入在后，
     * 固定的格式说明在前，前缀才不会被业务输入污染。
     */
    if (c->system[0] != '\0') {
        wb_str(&b, "{\"role\":\"system\",\"content\":\"");
        wb_json(&b, c->system, (uint32_t)strlen(c->system));
        wb_str(&b, "\"},");
    }

    /*
     * v1.14.8：prompt 不再整条塞进一条 user 消息，而是渲染成**标准
     * tool-calling messages**（调用→assistant、结果→tool）。理由见
     * `convo_render` 上面那段。没有标记的 plain prompt —— 也就是旧行为的
     * 全部输入 —— 渲染结果与旧版逐字节相同：还是一条 user 消息。
     */
    convo_render(&b, prompt, prompt_len);
    wb_str(&b, "]");

    /*
     * tools 清单。**追加**而不是前置，这样没有 tools 的请求体与旧版本逐字节
     * 相同 —— 与 system 同一条纪律。
     *
     * 顺带一个容易搞反的点：这里的 JSON 字段序对前缀缓存**无关**。真正决定
     * 前缀的是 chat template 渲染出来的 token 序列，而模板总把 tools 渲染在
     * messages 之前。所以"先写 tools 还是后写"只影响可读性，不影响命中率；
     * 真正影响命中率的是 tools 内容本身逐字节稳定（见 agent_llm_render_tools）。
     */
    if (c->tools != NULL && c->tools_len > 0) {
        wb_str(&b, ",\"tools\":");
        wb_raw(&b, c->tools, c->tools_len);
        wb_str(&b, ",\"tool_choice\":\"auto\"");
    }

    wb_str(&b, ",\"temperature\":0}");

    /* 放得下就补一个 NUL：报文本身不用它（长度显式传），
     * 但调试时能在 printf 里直接看，代价是一个字节。 */
    if (b.w < b.cap) out[b.w] = '\0';
    return b.w;
}

/* ---- tools 清单 ---- */

uint32_t agent_llm_render_tools(const AgentToolSpec *specs, uint32_t n,
                                char *out, uint32_t out_cap) {
    WBuf b;
    bool first = true;

    b.p = out;
    b.cap = (out != NULL) ? out_cap : 0u;
    b.w = 0;

    wb_str(&b, "[");
    for (uint32_t i = 0; i < n; i++) {
        const AgentToolSpec *t = &specs[i];

        /* 无名工具进不了清单：模型无从调用，占位只会挤掉真工具 */
        if (t->name == NULL || t->name[0] == '\0') continue;

        if (!first) wb_str(&b, ",");
        first = false;

        wb_str(&b, "{\"type\":\"function\",\"function\":{\"name\":\"");
        wb_json(&b, t->name, (uint32_t)strlen(t->name));
        wb_str(&b, "\",\"description\":\"");

        /*
         * description 的职责是"这个工具干什么、产出什么形状"。产出形状放在这里
         * 而不是参数里，因为模型读工具结果的时机在**调用之后** —— 它必须先知道
         * 该期待什么，才谈得上判断结果对不对。
         */
        if (t->desc != NULL && t->desc[0] != '\0')
            wb_json(&b, t->desc, (uint32_t)strlen(t->desc));
        if (t->out_schema != NULL && t->out_schema[0] != '\0') {
            wb_str(&b, " 出：");
            wb_json(&b, t->out_schema, (uint32_t)strlen(t->out_schema));
        }

        wb_str(&b, "\",\"parameters\":{\"type\":\"object\",\"properties\":{\"args\":"
                   "{\"type\":\"string\",\"description\":\"");
        if (t->in_schema != NULL && t->in_schema[0] != '\0')
            wb_json(&b, t->in_schema, (uint32_t)strlen(t->in_schema));
        wb_str(&b, "\"}},\"required\":[\"args\"],\"additionalProperties\":false}}}");
    }
    wb_str(&b, "]");

    if (b.p != NULL && b.cap > 0) b.p[b.w < b.cap ? b.w : b.cap - 1u] = '\0';
    return b.w;
}

/* Stateless data request: never interpret user bytes as transcript records. */
uint32_t agent_llm_build_text_request(const AgentLlmCfg *c, const char *text,
                                     uint32_t len, char *out, uint32_t cap) {
    if (!c || !c->model[0] || (!text && len)) return 0;
    WBuf b = {out, out ? cap : 0, 0};
    wb_str(&b, "{\"model\":\""); wb_json(&b, c->model, (uint32_t)strlen(c->model));
    wb_str(&b, "\",\"messages\":[{\"role\":\"system\",\"content\":\"");
    wb_json(&b, c->system, (uint32_t)strlen(c->system));
    wb_str(&b, "\"},{\"role\":\"user\",\"content\":\""); wb_json(&b, text, len);
    wb_str(&b, "\"}],\"temperature\":0,\"max_tokens\":256}");
    if (b.w < b.cap) out[b.w] = 0;
    return b.w;
}

/* ---- 响应解析 ---- */

typedef struct JCur {
    const char *p;
    const char *e;
} JCur;

static void j_ws(JCur *c) {
    while (c->p < c->e) {
        char ch = *c->p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') c->p++;
        else break;
    }
}

/* 停在 '"'，跳过整串。转义只跳两字节 —— 具体是什么不重要，反正不解释。 */
static bool j_str_skip(JCur *c) {
    if (c->p >= c->e || *c->p != '"') return false;
    c->p++;
    while (c->p < c->e) {
        unsigned char ch = (unsigned char)*c->p;
        if (ch == '\\') {
            if (c->p + 1 >= c->e) return false;
            c->p += 2;
            continue;
        }
        c->p++;
        if (ch == '"') return true;
    }
    return false;
}

/* 跳过一个值。嵌套的 { } [ ] 各自配平；字符串里的括号不算。 */
static bool j_val_skip(JCur *c) {
    char open, close;
    int depth;

    j_ws(c);
    if (c->p >= c->e) return false;
    if (*c->p == '"') return j_str_skip(c);

    if (*c->p != '{' && *c->p != '[') {
        while (c->p < c->e) {
            char ch = *c->p;
            if (ch == ',' || ch == '}' || ch == ']' ||
                ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            c->p++;
        }
        return true;
    }

    open  = *c->p;
    close = (open == '{') ? '}' : ']';
    depth = 0;
    while (c->p < c->e) {
        char ch = *c->p;
        if (ch == '"') {
            if (!j_str_skip(c)) return false;
            continue;
        }
        if (ch == open) depth++;
        else if (ch == close) {
            depth--;
            c->p++;
            if (depth == 0) return true;
            continue;
        }
        c->p++;
    }
    return false;
}

/*
 * 在对象里找 key。停在 value 起点，返回 true。
 *
 * 键按**原始字节**比较（不解转义）：我们的四个键 choices / message / content
 * 与 tool_calls 都不会含转义，为了它们写一条转义解码路径是纯粹的负担。
 * 边界写在这里：键名里出现转义时匹配不上，会走"找不到"的失败路径，不会错配。
 */
static bool j_obj_key(JCur *c, const char *key, uint32_t klen) {
    j_ws(c);
    if (c->p >= c->e || *c->p != '{') return false;
    c->p++;

    for (;;) {
        const char *ks;
        uint32_t kl;

        j_ws(c);
        if (c->p < c->e && *c->p == '}') return false;
        if (c->p >= c->e || *c->p != '"') return false;

        ks = c->p + 1;
        if (!j_str_skip(c)) return false;
        kl = (uint32_t)((c->p - 1) - ks);

        j_ws(c);
        if (c->p >= c->e || *c->p != ':') return false;
        c->p++;
        j_ws(c);

        if (kl == klen && memcmp(ks, key, klen) == 0) return true;
        if (!j_val_skip(c)) return false;

        j_ws(c);
        if (c->p < c->e && *c->p == ',') { c->p++; continue; }
        return false;
    }
}

/* 在数组里走到第 idx 个元素。停在该元素起点。 */
static bool j_arr_at(JCur *c, uint32_t idx) {
    j_ws(c);
    if (c->p >= c->e || *c->p != '[') return false;
    c->p++;

    for (uint32_t i = 0;; i++) {
        j_ws(c);
        if (c->p < c->e && *c->p == ']') return false;
        if (i == idx) return true;
        if (!j_val_skip(c)) return false;
        j_ws(c);
        if (c->p < c->e && *c->p == ',') { c->p++; continue; }
        return false;
    }
}

static bool hex4(const char **pp, const char *e, uint32_t *out) {
    const char *p = *pp;
    uint32_t v = 0;

    if (e - p < 4) return false;
    for (uint32_t i = 0; i < 4; i++) {
        char d = p[i];
        uint32_t h;
        if (d >= '0' && d <= '9')      h = (uint32_t)(d - '0');
        else if (d >= 'a' && d <= 'f') h = (uint32_t)(d - 'a' + 10);
        else if (d >= 'A' && d <= 'F') h = (uint32_t)(d - 'A' + 10);
        else return false;
        v = (v << 4) | h;
    }
    *pp = p + 4;
    *out = v;
    return true;
}

static uint32_t utf8_put(uint32_t cp, char *o) {
    if (cp < 0x80u) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        o[0] = (char)(0xC0u | (cp >> 6));
        o[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        o[0] = (char)(0xE0u | (cp >> 12));
        o[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        o[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    o[0] = (char)(0xF0u | (cp >> 18));
    o[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    o[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    o[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* 解码一段 JSON 字符串。返回 1 = 成功，0 = 格式错，-1 = 放不下。 */
static int j_str_copy(JCur *c, char *out, uint32_t cap, uint32_t *olen) {
    uint32_t w = 0;

    if (c->p >= c->e || *c->p != '"') return 0;
    c->p++;

    while (c->p < c->e) {
        unsigned char ch = (unsigned char)*c->p;

        if (ch == '"') {
            c->p++;
            *olen = w;
            return 1;
        }
        if (ch != '\\') {
            if (w + 1u > cap) return -1;
            out[w++] = (char)ch;
            c->p++;
            continue;
        }

        if (c->p + 1 >= c->e) return 0;
        ch = (unsigned char)c->p[1];
        c->p += 2;

        switch (ch) {
        case '"':  if (w + 1u > cap) return -1; out[w++] = '"';  break;
        case '\\': if (w + 1u > cap) return -1; out[w++] = '\\'; break;
        case '/':  if (w + 1u > cap) return -1; out[w++] = '/';  break;
        case 'b':  if (w + 1u > cap) return -1; out[w++] = '\b'; break;
        case 'f':  if (w + 1u > cap) return -1; out[w++] = '\f'; break;
        case 'n':  if (w + 1u > cap) return -1; out[w++] = '\n'; break;
        case 'r':  if (w + 1u > cap) return -1; out[w++] = '\r'; break;
        case 't':  if (w + 1u > cap) return -1; out[w++] = '\t'; break;
        case 'u': {
            uint32_t cp;
            char tmp[4];
            uint32_t n;

            if (!hex4(&c->p, c->e, &cp)) return 0;

            /* 代理对：高半 + 低半合成一个码点。不合成的话
               emoji 会变成两个非法 UTF-8 序列，模型输出里这很常见。 */
            if (cp >= 0xD800u && cp <= 0xDBFFu &&
                c->p + 1 < c->e && c->p[0] == '\\' && c->p[1] == 'u') {
                const char *q = c->p + 2;
                uint32_t lo;
                if (hex4(&q, c->e, &lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    c->p = q;
                }
            }

            n = utf8_put(cp, tmp);
            if (w + n > cap) return -1;
            memcpy(out + w, tmp, n);
            w += n;
            break;
        }
        default:
            return 0;   /* 非法转义：不猜，直接拒 */
        }
    }
    return 0;
}

cxf_result agent_llm_parse_response(const char *json, uint32_t len,
                                    char *out, uint32_t out_cap, uint32_t *out_len) {
    JCur root, choices, msg;
    int rc;

    if (json == NULL || out == NULL || out_len == NULL) return CXF_ERR_RANGE;

    root.p = json;
    root.e = json + len;

    j_ws(&root);
    if (!j_obj_key(&root, "choices", 7)) return CXF_ERR_FORMAT;

    choices = root;
    if (!j_arr_at(&choices, 0)) return CXF_ERR_FORMAT;   /* 空 choices：模型什么都没说 */

    msg = choices;
    if (!j_obj_key(&msg, "message", 7)) return CXF_ERR_FORMAT;
    if (!j_obj_key(&msg, "content", 7)) return CXF_ERR_FORMAT;

    j_ws(&msg);
    /* content 可以是 null（只有 tool_calls 时）。那不是"空答案"，
       是"这一轮没有文本"—— 对设计成读文本的能力来说就是失败。 */
    if (msg.p < msg.e && *msg.p == 'n') return CXF_ERR_FORMAT;

    rc = j_str_copy(&msg, out, out_cap, out_len);
    if (rc == 1) return CXF_OK;
    return (rc == -1) ? CXF_ERR_FULL : CXF_ERR_FORMAT;
}

/* ---- tool_calls ---- */

/*
 * 解**第 idx 个** tool_call。`present` 在调用前已由上层判过（"有没有这个通道"）。
 *
 * 拆出来是为了 fan-out（§4.17.9）：一个响应里的第 0 个与第 3 个在结构上是
 * 同一种东西，把它们当成"特例 + 剩下的"会写出两条迟早不同步的路径。
 */
static cxf_result parse_one_call(JCur arr, uint32_t idx, AgentToolCall *out) {
    JCur tc = arr, fn;

    memset(out, 0, sizeof(*out));

    if (!j_arr_at(&tc, idx)) return CXF_ERR_FORMAT;   /* 元素结构坏 / 不存在 */
    if (!j_obj_key(&tc, "function", 8)) return CXF_ERR_FORMAT;
    fn = tc;

    {
        JCur     n = fn;
        uint32_t ncap = (uint32_t)sizeof(out->name) - 1u;
        uint32_t nl = 0;
        int      rc;

        if (!j_obj_key(&n, "name", 4)) return CXF_ERR_FORMAT;
        rc = j_str_copy(&n, out->name, ncap, &nl);
        if (rc != 1) return (rc == -1) ? CXF_ERR_FULL : CXF_ERR_FORMAT;
        out->name[nl] = '\0';
        if (nl == 0) return CXF_ERR_FORMAT;            /* 空名字调不动任何东西 */
    }

    /*
     * arguments 是**字符串里套 JSON**：模型把 `{"args":"..."}` 编码成字符串，
     * 再嵌进外层 JSON。所以要解两层，第一层解出来的字节才是第二层的输入。
     *
     * 第二层只认一个固定形状：不通用解析。通用解析会让"模型发明了一个新字段"
     * 变成静默忽略 —— 那正是幻觉字段最爱的落点。
     */
    {
        JCur     a = fn;
        char     raw[AGENT_LLM_TOOL_ARGS_CAP];
        uint32_t raw_len = 0;
        uint32_t alen = 0;
        int      rc;

        if (!j_obj_key(&a, "arguments", 9)) return CXF_ERR_FORMAT;
        rc = j_str_copy(&a, raw, (uint32_t)sizeof(raw), &raw_len);
        if (rc != 1) return (rc == -1) ? CXF_ERR_FULL : CXF_ERR_FORMAT;

        a.p = raw;
        a.e = raw + raw_len;
        j_ws(&a);
        if (!j_obj_key(&a, "args", 4)) return CXF_ERR_FORMAT;

        rc = j_str_copy(&a, out->args, (uint32_t)sizeof(out->args) - 1u, &alen);
        if (rc != 1) return (rc == -1) ? CXF_ERR_FULL : CXF_ERR_FORMAT;
        out->args[alen] = '\0';
        out->args_len = alen;
    }
    return CXF_OK;
}

/*
 * 数 tool_calls 有几个元素。**一遍扫完，并且必须看到收尾的 `]`。**
 *
 * 为什么要看到 `]`：`j_arr_at` 在"到头了"和"结构坏了"两种情况下都返回 false。
 * 不查收尾，一个被截断的响应会被当成"只有 N 条"—— 而它实际是被截断的，
 * 少的那几条永远不会回来，模型也无从得知。
 */
static cxf_result count_calls(JCur c, uint32_t max, uint32_t *n) {
    uint32_t i = 0;

    j_ws(&c);
    if (c.p >= c.e || *c.p != '[') return CXF_ERR_FORMAT;
    c.p++;

    j_ws(&c);
    if (c.p < c.e && *c.p == ']') return CXF_ERR_FORMAT;   /* 有通道却一条都没给 */

    for (;;) {
        if (!j_val_skip(&c)) return CXF_ERR_FORMAT;
        i++;
        if (i > max) return CXF_ERR_RANGE;    /* 有界：超限响亮失败，不截断 */

        j_ws(&c);
        if (c.p < c.e && *c.p == ',') { c.p++; j_ws(&c); continue; }
        if (c.p < c.e && *c.p == ']') break;
        return CXF_ERR_FORMAT;                /* 既不是 ',' 也不是 ']' ⇒ 坏 / 被截断 */
    }

    if (i == 0u) return CXF_ERR_FORMAT;
    *n = i;
    return CXF_OK;
}

cxf_result agent_llm_parse_tool_call(const char *json, uint32_t len,
                                     AgentToolCall *out, bool *present) {
    JCur root, choices, msg, tc;
    uint32_t n = 0;
    cxf_result rc;

    if (json == NULL || out == NULL || present == NULL) return CXF_ERR_RANGE;

    memset(out, 0, sizeof(*out));
    *present = false;

    root.p = json;
    root.e = json + len;

    j_ws(&root);
    if (!j_obj_key(&root, "choices", 7)) return CXF_ERR_FORMAT;

    choices = root;
    if (!j_arr_at(&choices, 0)) return CXF_ERR_FORMAT;

    msg = choices;
    if (!j_obj_key(&msg, "message", 7)) return CXF_ERR_FORMAT;

    /*
     * 到这里为止与 content 解析共用同一条路径。再往下**分叉**，而分叉点正是
     * "响应里有没有这个通道"：没有 ⇒ 不是错误，是"这一轮走的文本通道"；
     * 有 ⇒ 结构必须对，坏一点都算错（不猜、不半解读）。
     */
    if (!j_obj_key(&msg, "tool_calls", 10)) return CXF_OK;

    *present = true;

    tc = msg;
    rc = count_calls(tc, AGENT_LLM_MAX_CALLS, &n);
    if (rc != CXF_OK) return rc;

    return parse_one_call(tc, 0u, out);
}

/*
 * 纯文本 ⇒ 终值。
 *
 * 为什么需要它（真实模型第一次接上来的当天就会撞到）：`CALL|` / `DONE` 这个判别符
 * 是**我们内部**的线格式约定，模型并不知道。模型被给了 `tools` 通道，它想调工具时
 * 会走 `tool_calls`；它不想调工具时，就是**直接说一句话**。那句话在语义上就是终值，
 * 但语法上没有 `DONE` 前缀 —— 于是 `agent_act_from_text` 判 FORMAT，整条任务失败。
 *
 * 所以这不是"宽松解析"，是一次**命名空间的翻译**：协议层知道"没有 tool_calls 的
 * 内容 = 说完了"，于是把它翻译成我们自己的判别符。下游仍然只有一份解码器、
 * 仍然只有一个 FORMAT 出处 —— 与 `out_iface` 那条纪律一致："我的模型只会说人话"
 * 是实现细节，不该泄漏到执行层。
 *
 * 三条边界刻意保留：
 *   空内容 → 原样返回（让它去撞 FORMAT）。把 "DONE\n" + "" 当成功是**假装答完了**，
 *            而 reasoning 模型在 token 用尽时恰好就是这个形状 —— 那必须响亮地失败。
 *   已有判别符 → 原样返回（模型学会了我们的格式，不该被二次包装）。
 *   装不下 → FULL，由调用方扩容；不截断一句话。
 */
static cxf_result hoist_prose(char *out, uint32_t cap, uint32_t *len) {
    uint32_t n = *len;

    if (n == 0u) return CXF_OK;
    if (n >= 5u && memcmp(out, "CALL|", 5u) == 0) return CXF_OK;
    if (n >= 4u && memcmp(out, "DONE", 4u) == 0) return CXF_OK;
    if (n + 5u > cap) return CXF_ERR_FULL;

    memmove(out + 5, out, n);
    memcpy(out, "DONE\n", 5u);
    *len = n + 5u;
    return CXF_OK;
}

cxf_result agent_llm_actions_from_response(const char *json, uint32_t len,
                                           char *out, uint32_t out_cap,
                                           uint32_t *out_len, uint32_t *out_count) {
    JCur       root, choices, msg, tc;
    AgentToolCall call;
    cxf_result rc;
    uint32_t   ncall = 0, w = 0;

    if (json == NULL || out == NULL || out_len == NULL || out_count == NULL)
        return CXF_ERR_RANGE;
    *out_len = 0;
    *out_count = 0;

    root.p = json;
    root.e = json + len;
    j_ws(&root);
    if (!j_obj_key(&root, "choices", 7)) return CXF_ERR_FORMAT;
    choices = root;
    if (!j_arr_at(&choices, 0)) return CXF_ERR_FORMAT;
    msg = choices;
    if (!j_obj_key(&msg, "message", 7)) return CXF_ERR_FORMAT;

    /*
     * 没有 tool_calls ⇒ 这一轮走文本通道。文本通道**只能是一条**（见头文件），
     * 于是这里退回单条翻译，不假装能 fan-out。
     *
     * ★ 就地转换需要一个**前置余量**：二进制块比文本形态长 4 字节（头 5→9），
     *   所以文本必须先落在 `out + AGENT_ACT_HEADROOM`，才有地方原地长大。
     *   这不是省一次分配 —— 省的是"再开一个和 out_cap 一样大的缓冲"，
     *   而那个缓冲最终只能在栈上（内容长度上界就是 out_cap）。
     */
    if (!j_obj_key(&msg, "tool_calls", 10)) {
        char    *base = out + AGENT_ACT_HEADROOM;
        uint32_t bcap = out_cap - AGENT_ACT_HEADROOM;
        uint32_t tlen = 0, need;

        if (out_cap <= AGENT_ACT_HEADROOM + 8u) return CXF_ERR_FULL;

        rc = agent_llm_parse_response(json, len, base, bcap, &tlen);
        if (rc != CXF_OK) return rc;
        rc = hoist_prose(base, bcap, &tlen);
        if (rc != CXF_OK) return rc;
        if (tlen == 0u) return CXF_OK;        /* 0 字节：让下游判 FORMAT */

        need = agent_act_from_text(base, tlen, out, out_cap);
        if (need == 0u) return CXF_ERR_FORMAT;   /* 不合语法：重试没用 */
        if (need > out_cap) return CXF_ERR_FULL; /* 放不下：该扩容 */

        *out_len   = need;
        *out_count = 1u;
        return CXF_OK;
    }

    tc = msg;
    rc = count_calls(tc, AGENT_LLM_MAX_CALLS, &ncall);
    if (rc != CXF_OK) return rc;

    /*
     * 先全解完再全写出去：**不做部分应用**。半批次已经写出去的字节不能留，
     * 所以"会失败"必须在写第一个字节之前就知道。这里走两遍 —— 第一遍只算
     * 总量，第二遍才写。解析是纯函数，第二遍的代价只是再走一次字符串。
     */
    for (uint32_t i = 0; i < ncall; i++) {
        uint32_t need;

        rc = parse_one_call(tc, i, &call);
        if (rc != CXF_OK) return rc;

        need = agent_act_encode(AGENT_ACT_CALL, call.name,
                                (uint32_t)strlen(call.name),
                                call.args, call.args_len, NULL, 0u);
        if (need == 0u) return CXF_ERR_FORMAT;
        if (need > out_cap - w) return CXF_ERR_FULL;
        w += need;
    }

    /*
     * 第二遍真正写。走 `agent_act_encode` 而不是自己拼头 —— 布局只有一个正本，
     * 于是"解码器产出一个编码器不会产出的状态"在结构上不可能（与
     * `agent_act_from_text` 同一条纪律）。
     */
    w = 0;
    for (uint32_t i = 0; i < ncall; i++) {
        uint32_t need, cl;

        rc = parse_one_call(tc, i, &call);
        if (rc != CXF_OK) return rc;

        cl = (uint32_t)strlen(call.name);
        need = agent_act_encode(AGENT_ACT_CALL, call.name, cl,
                               call.args, call.args_len, NULL, 0u);
        (void)agent_act_encode(AGENT_ACT_CALL, call.name, cl,
                               call.args, call.args_len, out + w, need);
        w += need;
    }

    *out_len   = w;
    *out_count = ncall;
    return CXF_OK;
}

cxf_result agent_llm_action_from_response(const char *json, uint32_t len,
                                          char *out, uint32_t out_cap,
                                          uint32_t *out_len) {
    uint32_t   n = 0;
    cxf_result rc;

    rc = agent_llm_actions_from_response(json, len, out, out_cap, out_len, &n);
    if (rc != CXF_OK) return rc;
    /* 这个入口只能表达一条。多路时如实拒绝，而不是把第一条当全部。 */
    if (n > 1u) return CXF_ERR_RANGE;
    return CXF_OK;
}

/* The auxiliary data channel rejects tool/control responses. */
static cxf_result parse_text_only(const char *json, uint32_t len,
                                  char *out, uint32_t cap, uint32_t *written) {
    JCur root = {json, json + len}, choice, message, value;
    JCur complete = root;
    j_ws(&complete);
    if (complete.p == complete.e || *complete.p != '{' || !j_val_skip(&complete)) return CXF_ERR_FORMAT;
    j_ws(&complete);
    if (complete.p != complete.e) return CXF_ERR_FORMAT;
    if (!j_obj_key(&root, "choices", 7)) return CXF_ERR_FORMAT;
    choice = root;
    if (!j_arr_at(&choice, 0)) return CXF_ERR_FORMAT;
    value = choice;
    if (j_obj_key(&value, "finish_reason", 13)) {
        char reason[16]; uint32_t n = 0;
        if (j_str_copy(&value, reason, sizeof reason, &n) != 1 || n != 4 || memcmp(reason, "stop", 4))
            return CXF_ERR_FORMAT;
    }
    message = choice;
    if (!j_obj_key(&message, "message", 7)) return CXF_ERR_FORMAT;
    value = message;
    char role[16]; uint32_t role_size = 0;
    if (!j_obj_key(&value, "role", 4) || j_str_copy(&value, role, sizeof role, &role_size) != 1 ||
        role_size != 9 || memcmp(role, "assistant", 9)) return CXF_ERR_FORMAT;
    value = message;
    if (j_obj_key(&value, "tool_calls", 10) || j_obj_key(&message, "function_call", 13)) return CXF_ERR_FORMAT;
    cxf_result rc = agent_llm_parse_response(json, len, out, cap, written);
    if (rc == CXF_OK && (!*written || memchr(out, 0, *written))) return CXF_ERR_FORMAT;
    return rc;
}

static cxf_result classify_visit(void *ctx,const AgentConvoView *v){
    if(v->kind!=AGENT_CONVO_USER)*(uint32_t *)ctx=AGENT_DATA_WORKSPACE;
    return CXF_OK;
}
uint32_t agent_llm_data_class(const char *text,uint32_t n){
    uint32_t kind=AGENT_DATA_TASK;
    if(agent_llm_visit_conversation(text,n,classify_visit,&kind)!=CXF_OK)return AGENT_DATA_WORKSPACE;
    return kind;
}

/* ---- 组装 ---- */

/*
 * 一次往返的公共部分。as_action 决定**只解 content** 还是**先看 tool_calls**：
 *
 *   走 content     —— 能力的产出是数据（catalog 探针那类：quote.item）
 *   走 action      —— 能力的产出是控制流（agent.action），于是要先看协议通道
 *
 * 两件事共用一条传输路径而不是各写一份：重复的会是超时、状态码、chunked 解码、
 * 错误码映射 —— 那些恰好是最不该有两份实现的地方。
 */
/*
 * 内层：只做请求与解析，**不写回观测**。
 * 拆出来的理由不是分层审美：`chat_do` 有 6 条 return，把"写 last_status"
 * 散到这 6 处，下一个人加第 7 条时一定漏（本仓库里"加一列忘一处"已经发生过）。
 * 让唯一的外层去写，就把"每条路径都留读数"变成结构性质，而不是纪律。
 */
static cxf_result chat_do(AgentLlmCfg *c,
                          const char *prompt, uint32_t prompt_len,
                          char *scratch, uint32_t scratch_cap,
                          char *out, uint32_t out_cap, uint32_t *out_len,
                          unsigned mode, uint16_t *status_out) {
    AgentHttpReq req;
    AgentHttpResp resp;
    char body[AGENT_LLM_REQ_CAP];
    uint32_t need, ncall = 0;
    cxf_result rc;

    *status_out = 0u;
    if (c == NULL || scratch == NULL || out == NULL || out_len == NULL)
        return CXF_ERR_RANGE;

    uint32_t kind = mode==2 ? AGENT_DATA_TASK : agent_llm_data_class(prompt,prompt_len);
    if (!agent_policy_egress(c->policy,c->scope,kind,prompt,prompt_len) ||
        !agent_policy_text(c->policy,c->system,(uint32_t)strlen(c->system))) return CXF_ERR_RANGE;
    if(c->scope!=AGENT_SCOPE_LOCAL && c->policy && c->policy->cloud_egress==AGENT_EGRESS_TASK && c->tools_len)
        return CXF_ERR_RANGE;
    need = mode == 2 ? agent_llm_build_text_request(c, prompt, prompt_len, body, sizeof body)
                     : agent_llm_build_request(c, prompt, prompt_len, body, sizeof body);
    if (need == 0) return CXF_ERR_RANGE;
    /* 超上限不静默截断：截断的请求会让模型答非所问，而那看起来像模型的问题。 */
    if (need >= (uint32_t)sizeof(body)) return CXF_ERR_FULL;

    memset(&req, 0, sizeof(req));
    req.policy = c->policy; req.scope = c->scope; req.data_class = kind;
    req.host     = c->host;
    req.port     = c->port;
    req.path     = c->path;
    req.auth     = (c->api_key[0] != '\0') ? c->api_key : NULL;
    req.extra_hdr = c->hdr;
    req.body     = body;
    req.body_len = need;
    req.timeout_ms = c->timeout_ms;

    rc = agent_http_post(&req, scratch, scratch_cap, &resp);
    if (rc != CXF_OK) return rc;

    /* 拿到状态码就记下来，**在判 2xx 之前** —— 401/429 的答案就在这个数里，
       而下面的映射会把它们全变成 FORMAT。先记事实，再做判断。 */
    *status_out = resp.status;

    /* 非 2xx 一律当格式错，不尝试从 error 字段里抽话术：
       上层需要的是"这次调用没成"，不是"供应商怎么措辞的"。
       ★ 这句话仍然成立（措辞不进控制流），但"没成的原因"必须留下 ——
       就是上面那行 `*status_out`。两者不矛盾：**话术归人看，状态码归机器判**。 */
    if (resp.status < 200 || resp.status >= 300) return CXF_ERR_FORMAT;

    if (mode == 2) {
        if (resp.truncated) return CXF_ERR_RANGE;
        return parse_text_only(resp.body, resp.body_len, out, out_cap, out_len);
    }
    if (mode == 1)
        return agent_llm_actions_from_response(resp.body, resp.body_len,
                                               out, out_cap, out_len, &ncall);
    return agent_llm_parse_response(resp.body, resp.body_len, out, out_cap, out_len);
}

/* 外层：唯一的写回点（见 chat_do 上面那段）。 */
static cxf_result chat_impl(AgentLlmCfg *c,
                            const char *prompt, uint32_t prompt_len,
                            char *scratch, uint32_t scratch_cap,
                            char *out, uint32_t out_cap, uint32_t *out_len,
                            unsigned mode) {
    uint16_t status = 0u;
    cxf_result rc = chat_do(c, prompt, prompt_len, scratch, scratch_cap,
                            out, out_cap, out_len, mode, &status);
    if (rc==CXF_OK && c && out_len && !agent_policy_text(c->policy,out,*out_len)) {
        memset(out,0,*out_len);*out_len=0;rc=CXF_ERR_RANGE;
    }
    if (c != NULL) {
        c->last_status = status;
        c->last_rc     = (int16_t)rc;
    }
    return rc;
}

cxf_result agent_llm_chat(AgentLlmCfg *c,
                          const char *prompt, uint32_t prompt_len,
                          char *scratch, uint32_t scratch_cap,
                          char *out, uint32_t out_cap, uint32_t *out_len) {
    return chat_impl(c, prompt, prompt_len, scratch, scratch_cap,
                     out, out_cap, out_len, false);
}

cxf_result agent_llm_chat_action(AgentLlmCfg *c,
                                 const char *prompt, uint32_t prompt_len,
                                 char *scratch, uint32_t scratch_cap,
                                 char *out, uint32_t out_cap, uint32_t *out_len) {
    return chat_impl(c, prompt, prompt_len, scratch, scratch_cap,
                     out, out_cap, out_len, true);
}

cxf_result agent_llm_chat_text(AgentLlmCfg *c, const char *text, uint32_t len,
                                char *scratch, uint32_t scratch_cap,
                                char *out, uint32_t cap, uint32_t *written) {
    if (written) *written = 0;
    return chat_impl(c, text, len, scratch, scratch_cap, out, cap, written, 2);
}
