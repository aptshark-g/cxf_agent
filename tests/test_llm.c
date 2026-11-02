#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_action.h"
#include "agent_llm.h"
#include "loopback.h"

/*
 * 协议层测试。分三段，对应三种不同性质的断言：
 *
 *   1. 请求体逐字节 —— 用**写死的期望串**比，不用"再算一遍比一遍"。
 *      后者只能证明函数是纯的，不能证明它写对了协议。
 *   2. 响应解析 —— 用真实形状的响应样本（含转义、代理对、null content、
 *      截断、空 choices）。这些正是成熟领域里"各家都这么发"的边角。
 *   3. 传输 —— 真起一个环回 socket 服务端，跑完整 HTTP 往返，
 *      并且**同时覆盖 Content-Length 与 chunked 两条路径**。
 *      chunked 必须测：只在 mock 上过、上线才发现响应是乱码，代价太高。
 */

static int g_fail = 0;

#define CHECK(c)                                                      \
    do {                                                              \
        if (!(c)) {                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);       \
            g_fail++;                                                 \
        }                                                             \
    } while (0)

/* ---- 1. 请求体 ---- */

/*
 * 断言 buf 里**恰好是一条**动作，且形状如期望。
 *
 * 为什么不比字节串：v1.13 起动作通道的产出是规范二进制块（头 9 + 名 + args），
 * 用字面量比对等于把布局抄进测试 —— 布局一改，一堆断言要跟着改，而那些断言
 * 想验的其实是语义（"这一轮就一件事，做的是 fs.grep，参数是 TOK_05"）。
 * 断言语义还额外白拿一条：**"恰好一条"**，即没有多余尾巴。
 */
static bool act_is(const char *buf, uint32_t len, uint8_t kind,
                   const char *cap, const char *args, uint32_t args_len) {
    AgentAct     a;
    uint32_t     cl = (cap != NULL) ? (uint32_t)strlen(cap) : 0u;

    if (!agent_act_decode(buf, len, &a)) return false;
    if (agent_act_bytes(&a) != len) return false;      /* 恰好一条，无尾巴 */
    if (a.kind != kind) return false;
    if (a.cap_len != cl || (cl > 0u && memcmp(a.cap, cap, cl) != 0)) return false;
    if (a.args_len != args_len) return false;
    return args_len == 0u || memcmp(a.args, args, args_len) == 0;
}

static void cfg_fixture(AgentLlmCfg *c) {
    memset(c, 0, sizeof(*c));
    strcpy(c->host, "127.0.0.1");
    c->port = 8080;
    strcpy(c->path, "/v1/chat/completions");
    strcpy(c->model, "m1");
    c->timeout_ms = 5000;
    c->scope = AGENT_SCOPE_LOCAL;
}

static void test_build_request(void) {
    AgentLlmCfg c;
    char a[1024], b[1024];
    uint32_t na, nb;

    cfg_fixture(&c);

    /* 形状写死比对：字段顺序、空格、引号都必须在期望串里出现一次。 */
    {
        static const char want[] =
            "{\"model\":\"m1\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
            "\"temperature\":0}";

        na = agent_llm_build_request(&c, "hi", 2u, a, sizeof(a));
        CHECK(na == strlen(want));
        CHECK(na == strlen(want) && memcmp(a, want, na) == 0);
    }

    /*
     * 带 system：形状固定，且 system 必须在 user **之前**。
     *
     * 这不是排版偏好 —— chat template 会把它渲染到对话最前，而"前缀稳定"
     * 正是要保的东西：固定的格式说明在前、变化的业务输入在后，前缀才不会被
     * 业务输入污染。反过来（把说明塞进 user）会让每次输入变化都改变前缀。
     */
    {
        AgentLlmCfg c2 = c;
        static const char want2[] =
            "{\"model\":\"m1\",\"messages\":["
            "{\"role\":\"system\",\"content\":\"只输出 DONE 或 CALL\"},"
            "{\"role\":\"user\",\"content\":\"hi\"}],\"temperature\":0}";

        strcpy(c2.system, "只输出 DONE 或 CALL");
        na = agent_llm_build_request(&c2, "hi", 2u, a, sizeof(a));
        CHECK(na == strlen(want2));
        CHECK(na == strlen(want2) && memcmp(a, want2, na) == 0);

        /* system 里的引号/换行照常转义，且两次构造逐字节相同 */
        strcpy(c2.system, "输出 \"CALL|x\"\n或 DONE");
        nb = agent_llm_build_request(&c2, "hi", 2u, b, sizeof(b));
        CHECK(nb > na);
        CHECK(strstr(b, "\"content\":\"输出 \\\"CALL|x\\\"\\n或 DONE\"") != NULL);
        na = agent_llm_build_request(&c2, "hi", 2u, a, sizeof(a));
        CHECK(na == nb && memcmp(a, b, na) == 0);
    }

    /*
     * ★ 确定性。这不是洁癖：这串字节就是 CDC 的输入、也是 prefix cache 的键。
     * 只要它每次不同（时间戳 / 随机 id / 浮点格式化），分块每次都会变，
     * 前缀缓存命中率归零。所以"两次逐字节相同"必须是一条断言，
     * 而不是一句注释里的愿望。
     */
    na = agent_llm_build_request(&c, "hello 世界", 12u, a, sizeof(a));
    nb = agent_llm_build_request(&c, "hello 世界", 12u, b, sizeof(b));
    CHECK(na == nb && na > 0);
    CHECK(memcmp(a, b, na) == 0);

    /* 转义：引号 / 反斜杠 / 换行 / 制表 / 控制字符 / UTF-8 透传 */
    {
        static const char in[] = "a\"b\\c\nd\te\x01" "f\xe4\xb8\xad";
        static const char want_tail[] = "a\\\"b\\\\c\\nd\\te\\u0001f\xe4\xb8\xad";

        na = agent_llm_build_request(&c, in, (uint32_t)sizeof(in) - 1u, a, sizeof(a));
        CHECK(na > 0);
        CHECK(strstr(a, want_tail) != NULL);
    }

    /* 容量不足只报告需要多少，不返回一个"看起来成功"的短串 */
    {
        uint32_t need = agent_llm_build_request(&c, "hi", 2u, NULL, 0);
        CHECK(need > 8u);
        memset(a, 0x7f, sizeof(a));
        CHECK(agent_llm_build_request(&c, "hi", 2u, a, 8u) == need);
    }

    /* 入参非法 → 0。0 与"放不下"是两种错，调用方必须分开处理。 */
    {
        AgentLlmCfg bad = c;
        bad.model[0] = '\0';
        CHECK(agent_llm_build_request(&bad, "hi", 2u, a, sizeof(a)) == 0u);
        CHECK(agent_llm_build_request(&c, NULL, 3u, a, sizeof(a)) == 0u);
    }

    /* 64 KiB 的提示词也不能崩 —— 只是会超 REQ_CAP，由 chat 如实返回 FULL */
    {
        static char big[70000];
        memset(big, 'x', sizeof(big));
        na = agent_llm_build_request(&c, big, (uint32_t)sizeof(big), a, sizeof(a));
        CHECK(na > sizeof(a));       /* 报告需要多少，而不是写爆 */
    }
}

/* ---- 2. 响应解析 ---- */

static uint32_t g_n;   /* 复用的输出游标，避免每个用例重复声明 */

static void test_parse_response(void) {
    static char out[512];

    /* 标准形状 */
    {
        static const char js[] =
            "{\"id\":\"x\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
            "\"content\":\"hello\"},\"finish_reason\":\"stop\"}]}";
        CHECK(agent_llm_parse_response(js, (uint32_t)strlen(js), out, sizeof(out), &g_n) == CXF_OK);
        CHECK(g_n == 5u && memcmp(out, "hello", 5u) == 0);
    }

    /* 前后空白、字段顺序变化、多出别的 choices —— 都不该影响结果 */
    {
        static const char js[] =
            " \n{\"object\":\"chat.completion\",\"choices\":["
            "{\"finish_reason\":\"stop\",\"message\":{\"content\":\"ok\",\"role\":\"assistant\"}},"
            "{\"message\":{\"content\":\"second\"}}]}";
        CHECK(agent_llm_parse_response(js, (uint32_t)strlen(js), out, sizeof(out), &g_n) == CXF_OK);
        CHECK(g_n == 2u && memcmp(out, "ok", 2u) == 0);
    }

    /* 转义解码 */
    {
        static const char js[] = "{\"choices\":[{\"message\":{\"content\":\"a\\\"b\\\\c\\nd\\te\\/f\"}}]}";
        CHECK(agent_llm_parse_response(js, (uint32_t)strlen(js), out, sizeof(out), &g_n) == CXF_OK);
        CHECK(g_n == 11u && memcmp(out, "a\"b\\c\nd\te/f", 11u) == 0);
    }

    /* \u：BMP 与代理对（emoji 在模型输出里很常见，不合成就是两个非法序列） */
    {
        static const char js[] =
            "{\"choices\":[{\"message\":{\"content\":\"\\u4e2d\\u6587 \\ud83d\\ude00\"}}]}";
        CHECK(agent_llm_parse_response(js, (uint32_t)strlen(js), out, sizeof(out), &g_n) == CXF_OK);
        CHECK(g_n == 11u);
        CHECK(memcmp(out, "\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80", 11u) == 0);
    }

    /* 嵌套对象/数组里的括号不能让扫描器错位 */
    {
        static const char js[] =
            "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":{\"name\":\"f\","
            "\"arguments\":\"{\\\"a\\\":[1,2]}\"}}],\"content\":\"after\"}}]}";
        CHECK(agent_llm_parse_response(js, (uint32_t)strlen(js), out, sizeof(out), &g_n) == CXF_OK);
        CHECK(g_n == 5u && memcmp(out, "after", 5u) == 0);
    }

    /* content 为 null：只有 tool_calls 的一轮。不是空答案，是"这轮没文本"。 */
    {
        static const char js[] = "{\"choices\":[{\"message\":{\"content\":null}}]}";
        CHECK(agent_llm_parse_response(js, (uint32_t)strlen(js), out, sizeof(out), &g_n)
              == CXF_ERR_FORMAT);
    }

    /* 各种"看起来像但不是"的输入 —— 全部必须拒绝，不许尽量解读 */
    {
        static const char *bad[] = {
            "",
            "{",
            "{}",
            "{\"choices\":[]}",                                   /* 空 choices */
            "{\"choices\":[{}]}",                                  /* 没有 message */
            "{\"choices\":[{\"message\":{}}]}",                     /* 没有 content */
            "{\"choices\":[{\"message\":{\"content\":5}}]}",         /* content 不是字符串 */
            "{\"error\":{\"message\":\"rate limited\"}}",           /* 错误信封 */
            "{\"choices\":[{\"message\":{\"content\":\"a\\qb\"}}]}", /* 非法转义 */
            "{\"choices\":[{\"message\":{\"content\":\"unterminated}}]",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            cxf_result r = agent_llm_parse_response(bad[i], (uint32_t)strlen(bad[i]),
                                                    out, sizeof(out), &g_n);
            if (r != CXF_ERR_FORMAT) {
                printf("  case %zu not rejected (r=%d)\n", i, (int)r);
                CHECK(!"坏输入被接受了");
            }
        }
    }

    /*
     * 容量不足 → FULL，不是 FORMAT。两者的处置相反：
     * FULL 要扩容重试，FORMAT 说明模型回了别的东西，重试也没用。
     */
    {
        static const char js[] = "{\"choices\":[{\"message\":{\"content\":\"0123456789\"}}]}";
        char tiny[4];
        CHECK(agent_llm_parse_response(js, (uint32_t)strlen(js), tiny, sizeof(tiny), &g_n)
              == CXF_ERR_FULL);
    }
}

/* ---- 3. 传输：环回服务端 ---- */

/*
 * 跑一次完整往返。resp 是服务端要回的原样字节（含状态行与头）——
 * 写死在这里而不是让服务端生成，因为"线上到底长什么样"正是要固定的东西。
 *
 * g_req / g_last_cfg 存下来供断言使用：**线上请求字节必须就是协议层纯函数
 * 算出的那串**。这是"确定性"这条硬契约在传输层的落地检查 ——
 * 少了它，就只剩"函数是纯的"这一半（§确定性是硬契约）。
 */
static char      g_req[AGENT_LLM_REQ_CAP];
static uint32_t  g_req_len;
static AgentLlmCfg g_last_cfg;

static cxf_result roundtrip(const char *resp, uint32_t resp_len,
                            char *out, uint32_t out_cap, uint32_t *out_len) {
    LbSrv srv;
    AgentLlmCfg c;
    static char scratch[AGENT_LLM_RAW_CAP];
    cxf_result rc;

    if (!lb_start(&srv, resp, resp_len, 1u)) return CXF_ERR_IO;

    cfg_fixture(&c);
    c.port = srv.port;
    g_last_cfg = c;

    rc = agent_llm_chat(&c, "hi", 2u, scratch, (uint32_t)sizeof(scratch),
                        out, out_cap, out_len);

    g_req_len = lb_body(&srv, g_req, (uint32_t)sizeof(g_req));
    CHECK(lb_stop(&srv));
    return rc;
}

/*
 * 与 roundtrip 同一件事，但走 `_action` 入口 —— 于是它验的是"线上真实字节 →
 * 动作字节"，中间隔着 Content-Length / chunked 解码与状态码判定，
 * 而这三处恰好是纯解析函数看不见的。
 */
static cxf_result roundtrip_action(const char *resp, uint32_t resp_len,
                                   char *out, uint32_t out_cap, uint32_t *out_len) {
    LbSrv srv;
    AgentLlmCfg c;
    static char scratch[AGENT_LLM_RAW_CAP];
    cxf_result rc;

    if (!lb_start(&srv, resp, resp_len, 1u)) return CXF_ERR_IO;

    cfg_fixture(&c);
    c.port = srv.port;

    rc = agent_llm_chat_action(&c, "hi", 2u, scratch, (uint32_t)sizeof(scratch),
                               out, out_cap, out_len);
    CHECK(lb_stop(&srv));
    return rc;
}

static void test_transport(void) {
    static char out[512];
    /* --- Content-Length 路径 --- */
    {
        static const char body[] = "{\"choices\":[{\"message\":{\"content\":\"pong\"}}]}";
        char resp[512];
        int n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %u\r\n"
                         "Connection: close\r\n\r\n%s",
                         (unsigned)(sizeof(body) - 1u), body);

        CHECK(roundtrip(resp, (uint32_t)n, out, sizeof(out), &g_n) == CXF_OK);
        CHECK(g_n == 4u && memcmp(out, "pong", 4u) == 0);

        /*
         * 线上字节 == 纯函数算出的那串。
         *
         * 这是"确定性"这条契约在**传输层**的落地：请求体会被前缀缓存当作键，
         * 任何一次不经意的差异（多一个头里的时间戳不算，但 body 里一旦有
         * 随机 / 时间 / 浮点格式化就完了）都会让分块每次都变。
         */
        {
            char want[512];
            uint32_t wn = agent_llm_build_request(&g_last_cfg, "hi", 2u, want, sizeof(want));

            CHECK(wn > 0u && wn == g_req_len);
            CHECK(wn > 0u && memcmp(want, g_req, wn) == 0);
        }
    }

    /* --- chunked 路径：Go 的 net/http 在没缓冲时会这样发 --- */
    {
        static const char resp[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n"
            "a\r\n{\"choices\"\r\n"
            "20\r\n:[{\"message\":{\"content\":\"ch\"}}]}\r\n"
            "0\r\n\r\n";

        CHECK(roundtrip(resp, (uint32_t)sizeof(resp) - 1u, out, sizeof(out), &g_n) == CXF_OK);
        CHECK(g_n == 2u && memcmp(out, "ch", 2u) == 0);
    }

    /*
     * --- 动作通道的完整往返：tool_calls → CALL|cap\nargs ---
     *
     * 这是"清单走协议、动作走文本"在**线上**那一段：模型回的是协议结构，
     * 出来的却必须是下游唯一认得的那串文本。两条路都测（有 tool_calls /
     * 只有 content），因为后者正是回落路径 —— 老模型不该因此变差。
     */
    {
        static const char tc_body[] =
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
            "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":"
            "{\"name\":\"travel.train\",\"arguments\":\"{\\\"args\\\":\\\"Q\\\"}\"}}]}}]}";
        static const char txt_body[] =
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"DONE\"}}]}";
        char resp[1024];
        int n;

        n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n%s",
                     (unsigned)(sizeof(tc_body) - 1u), tc_body);
        CHECK(n > 0);
        CHECK(roundtrip_action(resp, (uint32_t)n, out, sizeof(out), &g_n) == CXF_OK);
        CHECK(act_is(out, g_n, AGENT_ACT_CALL, "travel.train", "Q", 1u));

        n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n%s",
                     (unsigned)(sizeof(txt_body) - 1u), txt_body);
        CHECK(n > 0);
        CHECK(roundtrip_action(resp, (uint32_t)n, out, sizeof(out), &g_n) == CXF_OK);
        CHECK(act_is(out, g_n, AGENT_ACT_DONE, NULL, NULL, 0u));
    }

    /* --- 500：传输成功但应用失败。要如实报错，不能当空答案 --- */
    {
        static const char resp[] =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 30\r\n\r\n"
            "{\"error\":{\"message\":\"boom\"}}";

        CHECK(roundtrip(resp, (uint32_t)sizeof(resp) - 1u, out, sizeof(out), &g_n)
              == CXF_ERR_FORMAT);
    }

    /* --- 200 但信封是 error：解析端必须拒，不许退回"拿全文当答案" --- */
    {
        static const char body[] = "{\"error\":{\"message\":\"quota\"}}";
        char resp[256];
        int n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n%s",
                         (unsigned)(sizeof(body) - 1u), body);

        CHECK(roundtrip(resp, (uint32_t)n, out, sizeof(out), &g_n) == CXF_ERR_FORMAT);
    }

    /* --- 连不上：端口上没人监听 → IO 错，不是崩溃、不是空答案 --- */
    {
        AgentLlmCfg c;
        LbSrv srv;
        static char scratch[1024];

        /* 起一个 nreq=0 的服务端：端口被绑定过、随即释放 ⇒ 一定没人听。
           用夹具而不是另写一套 bind/close，是为了让"端口是怎么来的"只有一处。 */
        CHECK(lb_start(&srv, "", 0u, 0u));
        CHECK(lb_stop(&srv));

        cfg_fixture(&c);
        c.port = srv.port;
        CHECK(agent_llm_chat(&c, "hi", 2u, scratch, sizeof(scratch), out, sizeof(out), &g_n)
              == CXF_ERR_IO);
    }
}

/* ---- 3.5 tools 清单与 tool_calls（§4.16 清单走协议、动作走文本）---- */

static void test_render_tools(void) {
    static const AgentToolSpec sp[3] = {
        { "fs.grep", "在语料根下检索", "子串", "fs.hits" },
        { "a.b", "带\"引号\"和\n换行", NULL, NULL },
        { "", "无名工具", "x", "y" },        /* 必须被跳过：模型无从调用它 */
    };
    static const char want[] =
        "[{\"type\":\"function\",\"function\":{\"name\":\"fs.grep\","
        "\"description\":\"在语料根下检索 出：fs.hits\","
        "\"parameters\":{\"type\":\"object\",\"properties\":"
        "{\"args\":{\"type\":\"string\",\"description\":\"子串\"}},"
        "\"required\":[\"args\"],\"additionalProperties\":false}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"a.b\","
        "\"description\":\"带\\\"引号\\\"和\\n换行\","
        "\"parameters\":{\"type\":\"object\",\"properties\":"
        "{\"args\":{\"type\":\"string\",\"description\":\"\"}},"
        "\"required\":[\"args\"],\"additionalProperties\":false}}}]";
    char a[1024], b[1024];
    uint32_t na, nb;

    na = agent_llm_render_tools(sp, 3u, a, sizeof(a));
    CHECK(na == strlen(want));
    CHECK(na == strlen(want) && memcmp(a, want, na) == 0);

    /* 确定性：同一份 specs 两次渲染必须逐字节相同（tools 是固定前缀的主体） */
    nb = agent_llm_render_tools(sp, 3u, b, sizeof(b));
    CHECK(na == nb && memcmp(a, b, na) == 0);

    /* 描述里的引号/换行必须转义 —— 否则一个带引号的契约描述就能把清单
       变成非法 JSON，而症状是"模型忽然看不见任何工具" */
    CHECK(strstr(a, "带\\\"引号\\\"和\\n换行") != NULL);

    /* 空清单仍然是合法 JSON：调用方想不发 tools 应该留 cfg.tools = NULL，
       而不是渲染一个空数组再发出去（后者会改变请求字节） */
    na = agent_llm_render_tools(NULL, 0u, a, sizeof(a));
    CHECK(na == 2u && memcmp(a, "[]", 2u) == 0);

    /* 只报需要多少，不写爆 */
    {
        uint32_t need = agent_llm_render_tools(sp, 3u, NULL, 0u);

        CHECK(need == strlen(want));
        CHECK(agent_llm_render_tools(sp, 3u, a, 8u) == need);
    }
}

static void test_build_request_tools(void) {
    AgentLlmCfg c;
    char a[1024], b[1024];
    uint32_t na, nb;

    cfg_fixture(&c);

    /*
     * tools 插在 messages 之后、temperature 之前。写死期望串比对 ——
     * "再算一遍比一遍"只能证明函数是纯的，证明不了它写对了协议。
     */
    {
        static const char want[] =
            "{\"model\":\"m1\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
            "\"tools\":[{\"x\":1}],\"tool_choice\":\"auto\",\"temperature\":0}";

        c.tools = "[{\"x\":1}]";
        c.tools_len = 9u;
        na = agent_llm_build_request(&c, "hi", 2u, a, sizeof(a));
        CHECK(na == strlen(want));
        CHECK(na == strlen(want) && memcmp(a, want, na) == 0);
    }

    /*
     * ★ 不带 tools 时必须与旧版本**逐字节相同**。这不是兼容性洁癖：请求字节
     * 是 CDC 的输入、也是前缀缓存的键，多一个空 tools 就等于悄悄改一次缓存键。
     */
    {
        static const char want[] =
            "{\"model\":\"m1\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
            "\"temperature\":0}";

        c.tools = NULL;
        c.tools_len = 0u;
        na = agent_llm_build_request(&c, "hi", 2u, a, sizeof(a));
        CHECK(na == strlen(want) && memcmp(a, want, na) == 0);

        c.tools = "[{\"x\":1}]";       /* 指针在但长度 0：一律当"没有" */
        c.tools_len = 0u;
        nb = agent_llm_build_request(&c, "hi", 2u, b, sizeof(b));
        CHECK(nb == na && memcmp(a, b, na) == 0);
    }

    /* tools 与 system 并存时的顺序：system → user → tools → temperature */
    {
        AgentLlmCfg c2 = c;
        static const char want[] =
            "{\"model\":\"m1\",\"messages\":["
            "{\"role\":\"system\",\"content\":\"S\"},"
            "{\"role\":\"user\",\"content\":\"hi\"}],"
            "\"tools\":[{\"x\":1}],\"tool_choice\":\"auto\",\"temperature\":0}";

        strcpy(c2.system, "S");
        c2.tools = "[{\"x\":1}]";
        c2.tools_len = 9u;
        na = agent_llm_build_request(&c2, "hi", 2u, a, sizeof(a));
        CHECK(na == strlen(want));
        CHECK(na == strlen(want) && memcmp(a, want, na) == 0);
    }

    /*
     * ★ 诊断头（`cfg.hdr`）**不许**改变请求体一个字节。
     *
     * 这条断言是那个字段的"使用许可"：§4.9 的确定性契约约束的是 body
     * （CDC 输入 + 前缀缓存键），header 两者都不是。一旦有人把 hdr 的内容
     * 顺手拼进 body（"这样调试方便"），前缀缓存键每次调用都变、CDC 分块
     * 全部失效，而症状只是"缓存命中率变低了" —— 没人会想到是加了个头。
     */
    {
        c.tools = "[{\"x\":1}]";
        c.tools_len = 9u;
        na = agent_llm_build_request(&c, "hi", 2u, a, sizeof(a));
        c.hdr = "X-CXF-Ctx: run=1 call=2\r\n";
        nb = agent_llm_build_request(&c, "hi", 2u, b, sizeof(b));
        CHECK(nb == na && memcmp(a, b, na) == 0);
        c.hdr = NULL;
    }
}

/*
 * 数一下 hay 里 needle 出现几次（不重叠）。hay 必须 NUL 结尾 ——
 * `agent_llm_build_request` 在放得下时会补 NUL，这里全部用例都放得下。
 */
static uint32_t count_sub(const char *hay, const char *needle) {
    uint32_t    cnt = 0;
    size_t      nl  = strlen(needle);
    const char *p   = hay;

    if (nl == 0) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        cnt++;
        p += nl;
    }
    return cnt;
}

/*
 * transcript → 标准 tool-calling messages（v1.14.8）。
 *
 * 这一节断的是**协议形状**，所以主要用写死的期望串逐字节比 ——
 * "再算一遍比一遍"只能证明函数是纯的，证明不了它写对了协议。
 *
 * 每条断言挡一种具体的错：
 *   ① plain prompt（无标记）→ 一条 user，与旧版逐字节相同（向后兼容的唯一兜底）
 *   ② 一对记录 → user + assistant(tool_calls) + tool：id、配对、双层转义的形状
 *   ③ fan-out **逆序**落盘（recycle_tick 的实际顺序）仍按 id 正确配对
 *   ④ args 里含 "#result" 字样不误解析（长度前缀自定界）
 *   ⑤ `#budget:` → user 注记；`#fail` → tool 内容是整行（失败不许伪装成别的形状）
 *   ⑥ 孤儿 call（没有结局）→ assistant + tool("#lost")：协议才不被打翻
 *   ⑦ 未识别残余 → user 内容，一段都不丢
 *   ⑧ 渲染 ↔ 解码**互逆**：请求体里的 arguments 原样喂回解析路径，必须还原出
 *     逐字节相同的 args（含引号 / 反斜杠 / 控制字符 / UTF-8 / 标记文本）
 */
typedef struct BorrowCheck { const char *log; uint32_t seen, stop; } BorrowCheck;
static cxf_result check_borrow(void *ctx, const AgentConvoView *v) {
    BorrowCheck *c=ctx;
    static const AgentConvoKind kinds[]={AGENT_CONVO_USER,AGENT_CONVO_CALL,AGENT_CONVO_TOOL,
                                        AGENT_CONVO_USER,AGENT_CONVO_CALL,AGENT_CONVO_TOOL};
    CHECK(c->seen<6);
    if(c->seen>=6)return CXF_ERR_RANGE;
    CHECK(v->kind==kinds[c->seen]);
    if(c->seen==0){CHECK(v->text==c->log);CHECK(v->text_len==9);}
    if(c->seen==1){CHECK(v->call_id==1);CHECK(v->args==strstr(c->log,"a.c"));CHECK(v->args_len==3);}
    if(c->seen==2){CHECK(v->call_id==1);CHECK(v->text==strstr(c->log,"ok\n"));CHECK(v->text_len==3);}
    if(c->seen==3){CHECK(v->text==strstr(c->log,"#budget:"));CHECK(v->text_len==sizeof("#budget: dropped 5B")-1);}
    if(c->seen==4){CHECK(v->call_id==2);CHECK(v->args_len==0);}
    if(c->seen==5){CHECK(v->call_id==2);CHECK(v->text_len==5 && !memcmp(v->text,"#lost",5));}
    c->seen++;
    return c->seen==c->stop?CXF_ERR_IO:CXF_OK;
}
static void test_convo_views(void) {
    const char log[]="question\n#call fs.read 3\na.c\n#result fs.read 3B\nok\n\n#budget: dropped 5B\n#call fs.read 0\n\n";
    BorrowCheck c={log,0,0};
    CHECK(agent_llm_visit_conversation(log,sizeof log-1,check_borrow,&c)==CXF_OK);CHECK(c.seen==6);
    c.seen=0;c.stop=3;
    CHECK(agent_llm_visit_conversation(log,sizeof log-1,check_borrow,&c)==CXF_ERR_IO);CHECK(c.seen==3);
    CHECK(agent_llm_visit_conversation(NULL,1,check_borrow,&c)==CXF_ERR_RANGE);
    CHECK(agent_llm_visit_conversation(log,sizeof log-1,NULL,&c)==CXF_ERR_RANGE);
}
static void test_convo_render(void) {
    AgentLlmCfg c;
    char        a[2048];
    uint32_t    na;

    cfg_fixture(&c);

    /* ① 多行但无标记的 prompt：仍然是一条 user 消息，换行照常转义。
          这条是向后兼容的兜底 —— v1.14.8 之前的全部输入都是这个形状。 */
    {
        na = agent_llm_build_request(&c, "line1\nline2", 11u, a, sizeof(a));
        CHECK(na > 0);
        CHECK(strstr(a, "\"messages\":[{\"role\":\"user\","
                        "\"content\":\"line1\\nline2\"}]") != NULL);
    }
    {
        static const char want[] =
            "{\"model\":\"m1\",\"messages\":[{\"role\":\"user\",\"content\":\"\"}],"
            "\"temperature\":0}";

        na = agent_llm_build_request(&c, "", 0u, a, sizeof(a));
        CHECK(na == strlen(want) && memcmp(a, want, na) == 0);
    }

    /* ② 一对记录：问题段 → user，#call → assistant(tool_calls)，
          #result → tool。`arguments` 是字符串里套 JSON ⇒ 双层转义。
          问题段取"第一个标记之前的全部字节"，**原样**（含它自己的换行）——
          渲染器不改写字节，这与旧行为（整条 prompt 进一条 user）同规则。 */
    {
        static const char blob[] =
            "在仓库里找 TOK_05\n"
            "#call fs.grep 6\nTOK_05\n"
            "#result fs.grep 30B\nalpha/two.txt:2:second TOK_05\n\n";
        static const char want[] =
            "{\"model\":\"m1\",\"messages\":["
            "{\"role\":\"user\",\"content\":\"在仓库里找 TOK_05\\n\"},"
            "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"c1\","
            "\"type\":\"function\",\"function\":{\"name\":\"fs.grep\","
            "\"arguments\":\"{\\\"args\\\":\\\"TOK_05\\\"}\"}}]},"
            "{\"role\":\"tool\",\"tool_call_id\":\"c1\","
            "\"content\":\"alpha/two.txt:2:second TOK_05\\n\"}],"
            "\"temperature\":0}";

        na = agent_llm_build_request(&c, blob, (uint32_t)sizeof(blob) - 1u,
                                     a, sizeof(a));
        CHECK(na == strlen(want));
        CHECK(na == strlen(want) && memcmp(a, want, na) == 0);
    }

    /*
     * ③ 逆序配对。blob 里先落盘的是**后派出**的那条（recycle_tick 倒序回收的
     * 实际结果）—— 顺序配对在这里必错，id 配对才对。尾部 `]` 钉住消息次序。
     */
    {
        static const char blob[] =
            "问\n"
            "#call fs.grep 6\nTOK_05\n#result fs.grep 5B\nbody2\n"
            "#call fs.grep 5\nfirst\n#result fs.grep 5B\nbody1\n";

        na = agent_llm_build_request(&c, blob, (uint32_t)sizeof(blob) - 1u,
                                     a, sizeof(a));
        CHECK(na > 0);
        CHECK(strstr(a, "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":"
                        "[{\"id\":\"c1\",\"type\":\"function\",\"function\":"
                        "{\"name\":\"fs.grep\",\"arguments\":"
                        "\"{\\\"args\\\":\\\"TOK_05\\\"}\"}}]},") != NULL);
        CHECK(strstr(a, "{\"role\":\"tool\",\"tool_call_id\":\"c1\","
                        "\"content\":\"body2\"},") != NULL);
        CHECK(strstr(a, "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":"
                        "[{\"id\":\"c2\",\"type\":\"function\",\"function\":"
                        "{\"name\":\"fs.grep\",\"arguments\":"
                        "\"{\\\"args\\\":\\\"first\\\"}\"}}]},") != NULL);
        CHECK(strstr(a, "{\"role\":\"tool\",\"tool_call_id\":\"c2\","
                        "\"content\":\"body1\"}]") != NULL);
        CHECK(strstr(a, "\"id\":\"c3\"") == NULL);
    }

    /*
     * ④ 长度前缀自定界：args 里那串 "#result fs.grep 0B" 是**数据**，
     * 不是标记。按行扫的渲染器会把它当成一条孤儿结果，进而把后面的真结果
     * 配错甚至丢掉。断言：恰好一对 assistant/tool，args 原样进 arguments。
     */
    {
        static const char blob[] =
            "问\n"
            "#call fs.grep 18\n#result fs.grep 0B\n#result fs.grep 5B\nHELLO\n";

        na = agent_llm_build_request(&c, blob, (uint32_t)sizeof(blob) - 1u,
                                     a, sizeof(a));
        CHECK(na > 0);
        CHECK(count_sub(a, "\"role\":\"assistant\"") == 1u);
        CHECK(count_sub(a, "\"role\":\"tool\"") == 1u);
        CHECK(strstr(a, "\"arguments\":\"{\\\"args\\\":\\\""
                        "#result fs.grep 0B\\\"}\"") != NULL);
        CHECK(strstr(a, "\"content\":\"HELLO\"") != NULL);
    }

    /* ⑤ `#budget:` → user 注记；`#fail` → tool 内容是整行（不许只给能力名：
          那会让失败看起来像一次"内容为 fs.grep 的成功结果"）。 */
    {
        static const char blob[] =
            "问\n"
            "#budget: dropped 75B\n"
            "#call fs.grep 2\nhi\n"
            "#fail fs.grep\n";
        static const char want[] =
            "{\"model\":\"m1\",\"messages\":["
            "{\"role\":\"user\",\"content\":\"问\\n\"},"
            "{\"role\":\"user\",\"content\":\"#budget: dropped 75B\"},"
            "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"c1\","
            "\"type\":\"function\",\"function\":{\"name\":\"fs.grep\","
            "\"arguments\":\"{\\\"args\\\":\\\"hi\\\"}\"}}]},"
            "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"#fail fs.grep\"}],"
            "\"temperature\":0}";

        na = agent_llm_build_request(&c, blob, (uint32_t)sizeof(blob) - 1u,
                                     a, sizeof(a));
        CHECK(na == strlen(want));
        CHECK(na == strlen(want) && memcmp(a, want, na) == 0);
    }

    /* ⑥ 孤儿 call：协议要求 tool 必须跟在 assistant 之后 ⇒ 补一条 #lost，
          否则服务端会整包拒绝这个请求（而那看起来像网络问题）。 */
    {
        static const char blob[] = "问\n#call fs.grep 2\nhi\n";

        na = agent_llm_build_request(&c, blob, (uint32_t)sizeof(blob) - 1u,
                                     a, sizeof(a));
        CHECK(na > 0);
        CHECK(count_sub(a, "\"role\":\"assistant\"") == 1u);
        CHECK(count_sub(a, "\"role\":\"tool\"") == 1u);
        CHECK(strstr(a, "\"tool_call_id\":\"c1\",\"content\":\"#lost\"") != NULL);
    }

    /* ⑦ 零命中：tool 消息在，内容是空的（"跑成功了但什么都没有"）。 */
    {
        static const char blob[] = "问\n#call fs.grep 2\nhi\n#result fs.grep 0B\n\n";

        na = agent_llm_build_request(&c, blob, (uint32_t)sizeof(blob) - 1u,
                                     a, sizeof(a));
        CHECK(na > 0);
        CHECK(count_sub(a, "\"role\":\"tool\"") == 1u);
        CHECK(strstr(a, "\"role\":\"tool\",\"tool_call_id\":\"c1\","
                        "\"content\":\"\"}]") != NULL);
    }

    /* ⑧ 未识别残余：记录之后多出来的行成为最后一条 user 消息，不丢。 */
    {
        static const char blob[] =
            "问\n#call fs.grep 2\nhi\n#fail fs.grep\nGARBAGE LINE\n";

        na = agent_llm_build_request(&c, blob, (uint32_t)sizeof(blob) - 1u,
                                     a, sizeof(a));
        CHECK(na > 0);
        CHECK(strstr(a, "{\"role\":\"user\",\"content\":\"GARBAGE LINE\\n\"}]")
              != NULL);
    }

    /*
     * ⑨ 渲染 ↔ 解码互逆。把请求体里那个 arguments 字符串**原样抠出来**，
     * 拼成一份模型回发，喂 `agent_llm_actions_from_response` ——
     * 解出来的 args 必须与原始字节逐字节相同。
     *
     * 为什么这条是关键断言：双层转义写错时，请求体**看着仍是合法 JSON**，
     * 服务端不会报错，只有模型会对着一段乱码 arguments 发呆 ——
     * 那种失败没有任何一方会报告。
     */
    {
        static const char nasty[] =
            "a\"b\\c\nd\te\x01" "f\xe4\xb8\xad#result fs.grep 0B";
        char     blob[512], resp[1024], out[512];
        uint32_t blen = 0, n = 0, cnt = 0;
        AgentAct act;
        int      w, rw;

        w = snprintf(blob, sizeof(blob), "问\n#call fs.grep %u\n",
                     (unsigned)(sizeof(nasty) - 1u));
        CHECK(w > 0);
        blen = (uint32_t)w;
        memcpy(blob + blen, nasty, sizeof(nasty) - 1u);
        blen += (uint32_t)sizeof(nasty) - 1u;
        memcpy(blob + blen, "\n#result fs.grep 5B\nHELLO\n",
               sizeof("\n#result fs.grep 5B\nHELLO\n") - 1u);
        blen += (uint32_t)sizeof("\n#result fs.grep 5B\nHELLO\n") - 1u;

        na = agent_llm_build_request(&c, blob, blen, a, sizeof(a));
        CHECK(na > 0 && na < sizeof(a));
        if (na == 0 || na >= sizeof(a)) return;

        {
            const char *ap = strstr(a, "\"arguments\":\"");
            const char *tp;

            CHECK(ap != NULL);
            if (ap == NULL) return;
            ap += strlen("\"arguments\":\"");   /* 13，别手数 */
            tp = strstr(ap, "\"}}]}");
            CHECK(tp != NULL && tp > ap);
            if (tp == NULL || tp <= ap) return;

            rw = snprintf(resp, sizeof(resp),
                          "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":"
                          "{\"name\":\"fs.grep\",\"arguments\":\"%.*s\"}}]}}]}",
                          (int)(tp - ap), ap);
            CHECK(rw > 0 && (uint32_t)rw < sizeof(resp));
            if (rw <= 0 || (uint32_t)rw >= sizeof(resp)) return;

            CHECK(agent_llm_actions_from_response(resp, (uint32_t)rw,
                                                  out, sizeof(out), &n, &cnt)
                  == CXF_OK);
            CHECK(cnt == 1u);
            CHECK(agent_act_decode(out, n, &act));
            CHECK(act.kind == AGENT_ACT_CALL);
            CHECK(act.cap_len == 7u && memcmp(act.cap, "fs.grep", 7u) == 0);
            CHECK(act.args_len == sizeof(nasty) - 1u);
            CHECK(act.args_len == sizeof(nasty) - 1u &&
                  memcmp(act.args, nasty, act.args_len) == 0);
        }
    }
}

static void test_tool_call(void) {
    AgentToolCall call;
    bool          present = false;
    char          out[256];
    uint32_t      n;

    /*
     * 真实形状：content 为 null（这一轮没有文本），tool_calls 里 arguments 是
     * **字符串里套 JSON**。所以这里是两层转义。
     */
    {
        static const char resp[] =
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
            "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":"
            "{\"name\":\"fs.grep\",\"arguments\":\"{\\\"args\\\":\\\"TOK_05\\\"}\"}}]}}]}";

        CHECK(agent_llm_parse_tool_call(resp, (uint32_t)sizeof(resp) - 1u,
                                       &call, &present) == CXF_OK);
        CHECK(present);
        CHECK(strcmp(call.name, "fs.grep") == 0);
        CHECK(call.args_len == 6u && memcmp(call.args, "TOK_05", 6u) == 0);

        /* 译回线格式：下游只认 CALL|cap\nargs 这一种形状 ⇒ 解码器仍只有一份 */
        n = 0;
        CHECK(agent_llm_action_from_response(resp, (uint32_t)sizeof(resp) - 1u,
                                             out, sizeof(out), &n) == CXF_OK);
        CHECK(act_is(out, n, AGENT_ACT_CALL, "fs.grep", "TOK_05", 6u));

        /* 缓冲不够：如实 FULL，不截断一个"看起来能跑"的半个动作 */
        CHECK(agent_llm_action_from_response(resp, (uint32_t)sizeof(resp) - 1u,
                                             out, 8u, &n) == CXF_ERR_FULL);
    }

    /* 第二层里的转义：args 自身含引号与换行 */
    {
        static const char resp[] =
            "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":"
            "{\"name\":\"n\",\"arguments\":\"{\\\"args\\\":\\\"a\\\\\\\"b\\\\nc\\\"}\"}}]}}]}";

        CHECK(agent_llm_parse_tool_call(resp, (uint32_t)sizeof(resp) - 1u,
                                       &call, &present) == CXF_OK);
        CHECK(present);
        CHECK(call.args_len == 5u && memcmp(call.args, "a\"b\nc", 5u) == 0);
    }

    /*
     * ★ "没有这个通道"与"这个通道坏了"必须分得开：前者回落文本，后者报警。
     * 只返回 FORMAT 会让两件事变成同一种结果，而它们的处理方式相反。
     */
    {
        static const char plain[] =
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"DONE\"}}]}";
        char out2[64];
        uint32_t m = 0;

        CHECK(agent_llm_parse_tool_call(plain, (uint32_t)sizeof(plain) - 1u,
                                       &call, &present) == CXF_OK);
        CHECK(!present);

        /* 回落：字节就是 content 本身，与"没有 tools 功能"的旧行为一致 */
        CHECK(agent_llm_action_from_response(plain, (uint32_t)sizeof(plain) - 1u,
                                             out2, sizeof(out2), &m) == CXF_OK);
        CHECK(act_is(out2, m, AGENT_ACT_DONE, NULL, NULL, 0u));
    }

    /*
     * 纯文本（没有判别符）⇒ 终值。这条是接真实模型当天补的：模型的"我说完了"
     * 就是一句人话，不带 `DONE` 前缀。不翻译则整条任务必然 FORMAT 失败。
     *
     * 两条负例同样重要：空内容**不许**被包装成 `DONE\n`（那是"token 用完"伪装成
     * "答完了"），已有判别符**不许**被二次包装。
     */
    {
        static const char prose[] =
            "{\"choices\":[{\"message\":{\"content\":\"TOK_05 在 alpha/two.txt 第 2 行\"}}]}";
        static const char empty[] =
            "{\"choices\":[{\"message\":{\"content\":\"\"}}]}";
        static const char short_cap_ok[] = "{\"choices\":[{\"message\":{\"content\":\"DONE\\nx\"}}]}";
        char out3[64];
        uint32_t m = 0;
        static const char want_prose[] = "DONE\nTOK_05 在 alpha/two.txt 第 2 行";

        CHECK(agent_llm_action_from_response(prose, (uint32_t)sizeof(prose) - 1u,
                                             out3, sizeof(out3), &m) == CXF_OK);
        CHECK(act_is(out3, m, AGENT_ACT_DONE, NULL, want_prose + 5u,
                     (uint32_t)strlen(want_prose) - 5u));

        /* 空内容：原样 0 字节，交给下游判 FORMAT */
        CHECK(agent_llm_action_from_response(empty, (uint32_t)sizeof(empty) - 1u,
                                             out3, sizeof(out3), &m) == CXF_OK);
        CHECK(m == 0u);

        /* 已有判别符：不二次包装 */
        CHECK(agent_llm_action_from_response(short_cap_ok, (uint32_t)sizeof(short_cap_ok) - 1u,
                                             out3, sizeof(out3), &m) == CXF_OK);
        CHECK(act_is(out3, m, AGENT_ACT_DONE, NULL, "x", 1u));
    }

    /*
     * 通道存在但结构坏 → 硬失败，不猜、不半解读
     */
    {
        static const char bad_shape[] =
            "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":"
            "{\"name\":\"n\",\"arguments\":\"{\\\"q\\\":1}\"}}]}}]}";
        static const char no_args[] =
            "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":"
            "{\"name\":\"n\"}}]}}]}";
        static const char empty_arr[] =
            "{\"choices\":[{\"message\":{\"tool_calls\":[]}}]}";
        static const char empty_name[] =
            "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":"
            "{\"name\":\"\",\"arguments\":\"{\\\"args\\\":\\\"x\\\"}\"}}]}}]}";

        CHECK(agent_llm_parse_tool_call(bad_shape, (uint32_t)sizeof(bad_shape) - 1u,
                                       &call, &present) == CXF_ERR_FORMAT);
        CHECK(agent_llm_parse_tool_call(no_args, (uint32_t)sizeof(no_args) - 1u,
                                       &call, &present) == CXF_ERR_FORMAT);
        CHECK(agent_llm_parse_tool_call(empty_arr, (uint32_t)sizeof(empty_arr) - 1u,
                                       &call, &present) == CXF_ERR_FORMAT);
        CHECK(agent_llm_parse_tool_call(empty_name, (uint32_t)sizeof(empty_name) - 1u,
                                       &call, &present) == CXF_ERR_FORMAT);
    }

    /* 超过静态上限 → FULL（有界可以，静默截断不行） */
    {
        static char resp[4096], args[AGENT_LLM_TOOL_ARGS_CAP + 64u];
        int m;

        memset(args, 'x', sizeof(args) - 1u);
        args[sizeof(args) - 1u] = '\0';
        m = snprintf(resp, sizeof(resp),
                     "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":"
                     "{\"name\":\"n\",\"arguments\":\"{\\\"args\\\":\\\"%s\\\"}\"}}]}}]}",
                     args);
        CHECK(m > 0);
        CHECK(agent_llm_parse_tool_call(resp, (uint32_t)m, &call, &present)
              == CXF_ERR_FULL);
    }
}

/*
 * 一次响应里的**多路**工具调用（§4.17.9）。
 *
 * 这一格存在的理由：原来解码器只解 `tool_calls[0]`，`[1..]` **静默丢弃** ——
 * 真 curl 实测模型一次会返回 2 个。丢弃的表现是"模型说要查 A 和 B，实际只查了 A"，
 * 而摘要里没有任何痕迹说少了 B，于是模型基于半份证据编结论。
 *
 * 五条断言，每条挡一种具体的错：
 *   ① 两条都解出来、顺序保持、各自 args 正确
 *   ② 尾部被截断（少了 `]`）⇒ FORMAT —— 不能把"被截断"当成"就这么多"
 *   ③ 超过 AGENT_LLM_MAX_CALLS ⇒ RANGE，且**一个字节都不写**
 *   ④ 第二条坏 ⇒ 整批 FORMAT，`*out_len == 0` —— **不做部分应用**
 *   ⑤ 单条入口（`..._action_from_response`）拿到多条时如实拒绝，不装作只有一条
 */
static void test_tool_calls_multi(void) {
    static const char two_ok[] =
        "{\"choices\":[{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"fs.grep\",\"arguments\":\"{\\\"args\\\":\\\"A\\\"}\"}},"
        "{\"function\":{\"name\":\"fs.ls\",\"arguments\":\"{\\\"args\\\":\\\"B\\\"}\"}}"
        "]}}]}";
    static const char truncated[] =
        "{\"choices\":[{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"fs.grep\",\"arguments\":\"{\\\"args\\\":\\\"A\\\"}\"}}";
    static const char second_bad[] =
        "{\"choices\":[{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"fs.grep\",\"arguments\":\"{\\\"args\\\":\\\"A\\\"}\"}},"
        "{\"function\":{\"name\":\"fs.ls\"}}"
        "]}}]}";
    char     out[512];
    uint32_t n = 0, cnt = 0;
    AgentAct a;

    /* ① 两条都在，顺序与内容都保持 */
    memset(out, 0x7F, sizeof(out));
    CHECK(agent_llm_actions_from_response(two_ok, (uint32_t)sizeof(two_ok) - 1u,
                                         out, sizeof(out), &n, &cnt) == CXF_OK);
    CHECK(cnt == 2u);
    CHECK(agent_act_decode(out, n, &a));
    CHECK(a.kind == AGENT_ACT_CALL);
    CHECK(a.cap_len == 7u && memcmp(a.cap, "fs.grep", 7u) == 0);
    CHECK(a.args_len == 1u && a.args[0] == 'A');
    {
        uint32_t off = agent_act_bytes(&a);

        CHECK(agent_act_decode(out + off, n - off, &a));
        CHECK(a.kind == AGENT_ACT_CALL);
        CHECK(a.cap_len == 5u && memcmp(a.cap, "fs.ls", 5u) == 0);
        CHECK(a.args_len == 1u && a.args[0] == 'B');
        CHECK(off + agent_act_bytes(&a) == n);      /* 收尾干净：没有多余尾巴 */
    }

    /* ② 截断（少了 `]`）⇒ FORMAT。当成"只有一条"是最坏的一种解读 */
    CHECK(agent_llm_actions_from_response(truncated, (uint32_t)sizeof(truncated) - 1u,
                                          out, sizeof(out), &n, &cnt) == CXF_ERR_FORMAT);
    CHECK(n == 0u && cnt == 0u);

    /* ③ 超限 ⇒ RANGE，一个字节都不写 */
    {
        char     many[4096];
        int      w = 0, k;
        uint32_t i;

        w += snprintf(many + w, sizeof(many) - (size_t)w, "{\"choices\":[{\"message\":{\"tool_calls\":[");
        for (i = 0; i < AGENT_LLM_MAX_CALLS + 1u; i++) {
            k = snprintf(many + w, sizeof(many) - (size_t)w,
                         "%s{\"function\":{\"name\":\"f\",\"arguments\":\"{\\\"args\\\":\\\"x\\\"}\"}}",
                         i == 0u ? "" : ",");
            CHECK(k > 0);
            w += k;
        }
        w += snprintf(many + w, sizeof(many) - (size_t)w, "]}}]}");

        memset(out, 0x5A, sizeof(out));
        n = 123u;
        cnt = 123u;
        CHECK(agent_llm_actions_from_response(many, (uint32_t)w,
                                              out, sizeof(out), &n, &cnt) == CXF_ERR_RANGE);
        CHECK(n == 0u && cnt == 0u);
        CHECK((unsigned char)out[0] == 0x5A);       /* 真的没动过这块缓冲 */
    }

    /* ④ 第二条坏 ⇒ 整批 FORMAT，且不留下半批次 */
    memset(out, 0x33, sizeof(out));
    n = 7u;
    CHECK(agent_llm_actions_from_response(second_bad, (uint32_t)sizeof(second_bad) - 1u,
                                          out, sizeof(out), &n, &cnt)
          == CXF_ERR_FORMAT);
    CHECK(n == 0u && cnt == 0u);
    CHECK((unsigned char)out[0] == 0x33);           /* 一条都没写出去 */

    /* ⑤ 单条入口对多路如实拒绝：它表达不了多路 */
    CHECK(agent_llm_action_from_response(two_ok, (uint32_t)sizeof(two_ok) - 1u,
                                         out, sizeof(out), &n) == CXF_ERR_RANGE);
}


/* ---- 4. 配置 ---- */

static void test_config(void) {
    AgentLlmCfg c;

    (void)unsetenv("CXF_LLM_URL");
    (void)unsetenv("CXF_LLM_MODEL");
    CHECK(agent_llm_cfg_from_env(&c) == CXF_ERR_DEP);   /* 没配置就是没配置，不猜 */

    (void)setenv("CXF_LLM_URL", "http://127.0.0.1:9000/v1/chat/completions", 1);
    (void)setenv("CXF_LLM_MODEL", "m1", 1);
    CHECK(agent_llm_cfg_from_env(&c) == CXF_OK);
    CHECK(strcmp(c.host, "127.0.0.1") == 0);
    CHECK(c.port == 9000);
    CHECK(strcmp(c.path, "/v1/chat/completions") == 0);
    CHECK(strcmp(c.model, "m1") == 0);
    CHECK(c.api_key[0] == '\0');                         /* 没给 key：不带 Authorization */

    (void)setenv("CXF_LLM_URL", "https://api.example.com/v1/chat", 1);
    CHECK(agent_llm_cfg_from_env(&c) == CXF_ERR_FORMAT); /* 不收 https：静默降级成明文更危险 */

    (void)setenv("CXF_LLM_URL", "http://127.0.0.1", 1);
    CHECK(agent_llm_cfg_from_env(&c) == CXF_OK);
    CHECK(c.port == 80);                                 /* 没写端口：默认 80 */
    CHECK(strcmp(c.path, "/") == 0);

    /* system 可选：不设就是空串（请求体里不出现那一条） */
    CHECK(c.system[0] == '\0');

    (void)setenv("CXF_LLM_SYSTEM", "只输出 DONE 或 CALL", 1);
    CHECK(agent_llm_cfg_from_env(&c) == CXF_OK);
    CHECK(strcmp(c.system, "只输出 DONE 或 CALL") == 0);

    /*
     * 超长必须如实报错，不能截断：一段被截断的格式说明的表现是
     * "模型有时守格式有时不守"，而那是归因成本最高的一类失败。
     */
    {
        static char big[AGENT_LLM_SYS_CAP + 64u];

        memset(big, 'x', sizeof(big) - 1u);
        big[sizeof(big) - 1u] = '\0';
        (void)setenv("CXF_LLM_SYSTEM", big, 1);
        CHECK(agent_llm_cfg_from_env(&c) == CXF_ERR_RANGE);
    }

    (void)unsetenv("CXF_LLM_URL");
    (void)unsetenv("CXF_LLM_MODEL");
    (void)unsetenv("CXF_LLM_SYSTEM");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    test_build_request();
    test_parse_response();
    test_transport();
    test_render_tools();
    test_build_request_tools();
    test_tool_call();
    test_tool_calls_multi();
    test_convo_render();
    test_convo_views();
    test_config();

    if (g_fail == 0) {
        printf("all llm tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_fail);
    return 1;
}
