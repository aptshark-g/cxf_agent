/*
 * agentctl —— 第一个**可执行**的 agent 回路（docs/agent.md §4.16.3）。
 *
 * 它与 tests/ 下那些二进制的区别只有一句话：**它接受外部输入，并且真的出网**。
 *
 *      agentctl [--repl] --root DIR [--diff DIR] [--create DIR] [--write DIR] [--append DIR]
 *               [--net HOST:PORT] [--budget N] [--rounds N] [--conc N] [--rpm N] [--trace FILE] < 问题
 *      CXF_LLM_URL=http://host:port/v1/chat/completions CXF_LLM_MODEL=M CXF_LLM_KEY=K
 *
 * 为什么它必须是独立的可执行体，而不是又一个测试用例：
 *   测试断言的是"我知道会发生什么"，它断言的是"我不知道会发生什么"。
 *   真实模型会不守格式、会一次调两个工具、会把答案分两段说 —— 这些都不是
 *   过/不过，是**观察对象**。所以它不进 `make test` 的云端部分；REPL 另有本地
 *   环回端点回归，不需要真实 key。
 *
 * 端点可由 agentctl.json 或环境变量给，不通过 argv 传密钥。JSON 在每次 REPL
 * 任务前重读；环境变量提供初始值，文件里的字段优先。两种来源共用 URL 解析器。
 *
 * `--repl` 是持续进程里的多任务控制面，不是持久对话记忆：每条普通输入仍创建
 * 一份独立任务上下文。这一版刻意不做流式、运行中取消、多 rank、plan 分解、
 * 上下文树、TLS（`agent_http` 只做明文，见 agent_http.h）。
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>     /* clock_gettime：分段耗时 */
#include <unistd.h>   /* getpid / isatty */

#include "cxf/world.h"

#include "agent.h"
#include "agent_basic.h"
#include "agent_action.h"
#include "agent_caps.h"
#include "agent_caps.gen.h"
#include "agent_fs.h"
#include "agent_fs.gen.h"
#include "agent_llm.h"
#ifdef CXF_NATIVE_LLAMA
#include "agent_native.h"
#include "agent_tool_prepare.h"
static AgentToolPrepare *g_tool_prepare;
static bool g_tool_prepare_enabled;
static bool g_tool_output_profile,g_tool_output_enabled;
static uint64_t g_tool_output_calls;
static AgentToolOutputReport g_tool_output_last;
static AgentToolPrepareOptions g_prepare_options = { TOOL_KV_RESIDENT, TOOL_PREPARE_TIMED, 5.0 };
static AgentNative *g_native;
static const char *g_native_model, *g_native_backend;
static uint32_t g_native_context = 4096, g_native_max = 512, g_native_batch = 512;
static int32_t g_native_gpu = 99, g_native_threads = 4;
static void ctl_native_close(void) {
    if (g_native || g_tool_prepare) {
        AgentNativeStats s; if(g_tool_prepare) agent_tool_prepare_stats(g_tool_prepare, &s); else agent_native_stats(g_native, &s);
        fprintf(stderr, "[native] tokenized %llu reused_prefix %llu rebuilds %llu generations %llu\n",
                (unsigned long long)s.tokenized_tokens, (unsigned long long)s.reused_prefix_tokens,
                (unsigned long long)s.rebuilds, (unsigned long long)s.generations);
        if(g_tool_prepare) { agent_tool_prepare_close(g_tool_prepare); g_tool_prepare=NULL; }
        else { agent_native_close(g_native); g_native = NULL; }
    }
}
#endif
#include "agent_net.h"
#include "agentctl_config.h"
#include "agent_types.h"
#include "agent_types.gen.h"

#define CTL_TICKS  512u

/*
 * ★ 这几个必须是 static。
 *
 * 注册表存的是**指针**（`CapImpl.ctx` 指向 `AgentLlmCfg` / `FsCfg`），它们必须
 * 活得比 world 长；放栈上会读到栈残值 —— 症状是"注册成功、条目却在 MATCH 里判
 * FAILED"。这个坑在 tests/test_agent.c 与 §4.16.1 里各踩过一次，这里是第三次
 * 遇到同一个形状，所以干脆把"配置是静态的"做成这个文件的显式约定。
 */
static cxf_world   g_world;
static uint32_t    g_ids[5];
static AgentStore *g_store;

static AgentPolicy g_policy;
static uint32_t g_model_scope = AGENT_SCOPE_UNKNOWN, g_net_scope = AGENT_SCOPE_UNKNOWN;
static AgentLlmCfg g_llm;
static AgentLlmCfg g_local;
static const char *g_local_path;
static bool g_local_enabled;
static uint32_t g_local_calls;
static char        g_config_path[CTL_CFG_PATH_CAP] = "agentctl.json";
static bool        g_config_explicit;
static bool        g_config_loaded;
/* Check tool results before the executor can append them to its transcript. */
static FILE *g_trace;
static uint64_t ctl_observe_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC,&t)==0 ? (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec : 0;
}
static void ctl_timing(const char *kind,const char *name,uint64_t a,uint64_t b,uint64_t c,uint64_t d,int rc) {
    if(!g_trace)return;
    bool valid=a && a<=b && b<=c && c<=d;
    fprintf(g_trace,"[timing] {\"kind\":\"%s\",\"name\":\"%s\",\"valid\":%s,\"before_ns\":%llu,\"exec_ns\":%llu,\"after_ns\":%llu,\"total_ns\":%llu,\"rc\":%d}\n",
        kind,name,valid?"true":"false",(unsigned long long)(valid?b-a:0),(unsigned long long)(valid?c-b:0),(unsigned long long)(valid?d-c:0),(unsigned long long)(valid?d-a:0),rc);
}
typedef struct CtlGuard { CapImpl original, exposed; const char *name; } CtlGuard;
static CtlGuard g_guards[12];
static uint32_t g_guards_count;
static cxf_result ctl_guard_exec(const void *ctx,const char *args,uint32_t n,char *out,uint32_t cap,uint32_t *written) {
    const CtlGuard *g=ctx;
    uint64_t a=g_trace?ctl_observe_ns():0,b,c;
    *written=0;
    if(!agent_policy_text(&g_policy,args,n)) {
        b=g_trace?ctl_observe_ns():0;ctl_timing("tool",g->name,a,b,b,b,CXF_ERR_RANGE);return CXF_ERR_RANGE;
    }
    b=g_trace?ctl_observe_ns():0;
    cxf_result rc=g->original.exec(g->original.ctx,args,n,out,cap,written);
    c=g_trace?ctl_observe_ns():0;
    uint32_t visible=*written<cap?*written:cap;
    if(!agent_policy_text(&g_policy,out,visible)) {
        memset(out,0,visible);*written=0;
        fputs("[policy] blocked tool result; content withheld\n",stderr);
        rc=CXF_ERR_RANGE;
    }
    ctl_timing("tool",g->name,a,b,c,g_trace?ctl_observe_ns():0,rc);
    return rc;
}
static bool ctl_task_only(void) { return g_llm.scope != AGENT_SCOPE_LOCAL && g_policy.cloud_egress != AGENT_EGRESS_WORKSPACE; }
/*
 * 上一次**失败**响应的正文头部。给 `[llm]` 那个块当证据用。
 *
 * 为什么要留它：`409` / `404` / `200+HTML` 的**正文**才是能区分原因的东西 ——
 * 实测同一台 new-api 上，URL 少写 `/v1` 会返回 **200 + 前端 HTML**，
 * 而模型名不在 token 分组里会返回 **404**。两者的状态码都不"异常"，
 * 只看 `rc` 完全分不开。不留正文，诊断就只能靠猜（本轮猜错过两次）。
 *
 * 只留 120 字节：够看见 `<!doctype html>` 或 `{"error":{"message":"model not found"}}`，
 * 又不会把整份响应灌进终端。
 */
static char        g_raw_head[160];

/*
 * 从 scratch 里的**完整 HTTP 响应**中取出正文头部。
 *
 * 为什么要剥：`agent_http` 把整个响应（状态行 + 头 + 空行 + body）落在 scratch 上，
 * 于是"正文以 `<` 开头"这个判据直接对 raw 做会永远失败 —— 第一个字节是 `H`
 * （`HTTP/1.1`）。我在同一处连错两次：先写成判 raw，再发现 404 那行印出来是
 * `HTTP/1.1 404 Not Found` 而不是正文。**观测量取错了位置，等于没取。**
 *
 * 还要跳 chunked 的长度行（`286e\r\n`），否则 HTML 前面会挂着一段十六进制。
 */
static void raw_body_head(const char *raw, char *out, size_t cap) {
    const char *p;
    size_t i = 0, w = 0;

    if (raw == NULL || out == NULL || cap == 0u) return;
    p = strstr(raw, "\r\n\r\n");
    p = (p != NULL) ? p + 4 : raw;

    /* 跳 chunk 长度行：十六进制字符 + CRLF */
    while (p[i] != '\0' && w < 8u &&
           ((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f') ||
            (p[i] >= 'A' && p[i] <= 'F'))) {
        i++; w++;
    }
    if (w > 0u && p[i] == '\r' && p[i + 1u] == '\n') p += i + 2u;

    (void)snprintf(out, cap, "%.120s", p);
}
static FsCfg       g_fs;
static FsRootCfg  g_wfs;
static CapImpl     g_step_impl;
static CapImpl     g_grep_impl;
static BasicFsCfg  g_basic_fs, g_edit_fs;
static TestRunCfg  g_test_cfg;
static TestSandbox g_test_sandbox;
static CapImpl     g_read_impl, g_list_impl, g_test_impl, g_edit_impl, g_patch_impl;
#ifdef CXF_NATIVE_LLAMA
static cxf_result ctl_test_output_exec(const void *ctx,const char *args,uint32_t len,char *out,uint32_t cap,uint32_t *written){
#ifdef CTL_TOOL_OUTPUT_BEGIN
    CTL_TOOL_OUTPUT_BEGIN();
#endif
    cxf_result rc=agent_tool_prepare_output(g_tool_prepare,ctx,g_tool_output_enabled,args,len,out,cap,written,&g_tool_output_last);
#ifdef CTL_TOOL_OUTPUT_END
    CTL_TOOL_OUTPUT_END();
#endif
    AgentToolOutputReport *r=&g_tool_output_last;
    fprintf(stderr,"[tool-output] {\"kind\":\"tool\",\"seq\":%llu,\"enabled\":%s,\"entered_ns\":%llu,\"tool_start_ns\":%llu,\"tool_end_ns\":%llu,\"joined_ns\":%llu,\"returned_ns\":%llu,\"body_bytes\":%llu,\"observed\":%llu,\"submitted\":%llu,\"overwritten\":%llu,\"updates\":%llu,\"evaluated_tokens\":%llu,\"tokenized_tokens\":%llu,\"replayed_tokens\":%llu,\"front_ms\":%.6f,\"prefill_ms\":%.6f,\"stream_rc\":%d,\"tool_rc\":%d,\"eof_ns\":%llu,\"reaped_ns\":%llu,\"spawn_end_ns\":%llu,\"first_output_ns\":%llu,\"eof_polls\":%llu,\"eof_poll_ms\":%.6f,\"callback_ms\":%.6f,\"owner_queue_ms\":%.6f,\"owner_work_ms\":%.6f,\"first_native_ns\":%llu,\"last_native_ns\":%llu,\"decode_ms\":%.6f,\"sync_ms\":%.6f,\"decode_cpu_ms\":%.6f,\"sync_cpu_ms\":%.6f,\"decode_calls\":%llu,\"sync_calls\":%llu,\"consumer_threads\":%llu,\"consumer_launch_ms\":%.6f,\"cancelled\":%s}\n",
        (unsigned long long)++g_tool_output_calls,r->enabled?"true":"false",(unsigned long long)r->entered_ns,(unsigned long long)r->tool_start_ns,(unsigned long long)r->tool_end_ns,(unsigned long long)r->joined_ns,(unsigned long long)r->returned_ns,(unsigned long long)r->body_bytes,(unsigned long long)r->observed,(unsigned long long)r->submitted,(unsigned long long)r->overwritten,(unsigned long long)r->updates,(unsigned long long)r->evaluated,(unsigned long long)r->tokenized,(unsigned long long)r->replayed,r->front_ns/1e6,r->prefill_ns/1e6,r->stream_rc,r->tool_rc,(unsigned long long)r->process.eof_ns,(unsigned long long)r->process.reaped_ns,(unsigned long long)r->process.spawn_end_ns,(unsigned long long)r->process.first_output_ns,(unsigned long long)r->process.eof_polls,r->process.eof_poll_ns/1e6,r->process.callback_ns/1e6,r->owner_queue_ns/1e6,r->owner_work_ns/1e6,(unsigned long long)r->first_native_ns,(unsigned long long)r->last_native_ns,r->decode_ns/1e6,r->sync_ns/1e6,r->decode_cpu_ns/1e6,r->sync_cpu_ns/1e6,(unsigned long long)r->decode_calls,(unsigned long long)r->sync_calls,(unsigned long long)r->consumer_threads,r->consumer_launch_ns/1e6,r->cancelled?"true":"false");return rc;
}
typedef struct CtlToolObserved {const void *cfg;AgentToolPrepareReport *report;} CtlToolObserved;
static void ctl_tool_progress(void *ctx,const TestRunTrace *trace){
    CtlToolObserved *tool=ctx;
    (void)agent_tool_prepare_progress(g_tool_prepare,tool->report->epoch,trace->spawn_end_ns);
}
static cxf_result ctl_test_observed_exec(const void *ctx,const char *args,uint32_t len,char *out,uint32_t cap,uint32_t *written){
    CtlToolObserved *tool=(CtlToolObserved *)ctx;
    return test_run_exec_observed(tool->cfg,args,len,out,cap,written,NULL,TEST_RUN_AUTO,ctl_tool_progress,tool);
}
static cxf_result ctl_test_prepare_exec(const void *ctx,const char *args,uint32_t len,char *out,uint32_t cap,uint32_t *written) {
    if(!g_tool_prepare)return test_run_exec(ctx,args,len,out,cap,written);
    AgentToolPrepareReport r;CtlToolObserved observed={ctx,&r};
    cxf_result rc=agent_tool_prepare_execute(g_tool_prepare,1,ctl_test_observed_exec,&observed,args,len,out,cap,written,&r);
    fprintf(stderr,"[tool-prepare] mode=%d policy=%d tokens=%u tool=%.3fms wait=%.3fms save=%.3fms prepare_rc=%d cancelled=%u\n",
        (int)g_prepare_options.kv_mode,(int)g_prepare_options.policy,r.prefix_tokens,
        (r.tool_end_ns-r.tool_start_ns)/1e6,(r.finished_ns-r.tool_end_ns)/1e6,r.save_ns/1e6,r.prepare_rc,r.cancelled);
    return rc;
}
#endif

static const IfaceDef g_task_iface = { IFACENAME_TASK, "任务输入", IFACE_EXT };
static CapImpl     g_wn_impl;
static CapImpl     g_df_impl;
static CapImpl     g_ap_impl;
static CapImpl     g_app_impl;
static HttpCfg     g_net_cfg;
static CapImpl     g_net_impl;
static char        g_net_host[64];

/*
 * 本次请求的信任天花板。**它是部署声明，不是常量** —— 声明了写根才给
 * CAP_TRUST_WRITE，于是"模型看得见哪些能力"由这个进程被允许做什么决定，
 * 而不是由"这份代码编了哪些能力"决定（§3 第 24 条）。
 */
static uint8_t     g_trust = CAP_TRUST_IO;

static char        g_tools[AGENT_LLM_REQ_CAP];
static uint32_t    g_tools_len;
static const CapDef *g_public_tools[CAP_MAX];
static uint32_t g_public_tool_count;

static char        g_trace_path[1024];
static uint32_t    g_calls;
static uint32_t    g_total_ticks;
/* 每次调用重写的一段诊断头，见 ctl_step_exec 里那段 */
static char        g_hdr[96];
/* 本次任务的 phase 累计耗时（ns）。world 只保留"上一 tick"的槽位 */
static uint64_t    g_ph_ns[CXF_PHASE_COUNT];
static uint64_t    g_t0_ns, g_wall_ns;

/* 默认 system 由 ctl_system() 按挂载的能力集**拼**出来，不再写成几个固定串：
 * 三种能力有 2³ 种组合，写成字面量就是八份口径，而只要有一份忘了同步，
 * 表现就是"模型手里有工具、prompt 说它没有"—— 那会让能力装了等于没装，
 * 且看起来一切正常（这句话上一次是靠两个常量维持的，现在是靠构造）。 */

/*
 * 默认轮数。**部署选择，不是框架默认** —— 框架的 `AGENT_MAX_ROUNDS = 3` 是
 * 剧本算出来的（工具一次 + 作答一次），而真实模型在实测里 2~5 轮都出现过，
 * 3 轮会把"模型还没收口"变成"任务失败"（§4.16.3 的实测表）。
 * 8 是留够余量的值，不是调出来的最优值 —— 真要定它，得先有真实任务的
 * `capped` 率曲线。
 */
#define CTL_ROUNDS_DEFAULT 8u

/*
 * /context 的唯一真源是 LLM 能力边界，不是最终答案或当前工具清单。
 * 只保留最近任务；固定容量，所有丢失都进摘要，绝不把窗口说成完整历史。
 */
#define CTL_CONTEXT_EVENTS     64u
#define CTL_CONTEXT_PROMPT_CAP (64u * 1024u)
#define CTL_CONTEXT_ACT_CAP    (16u * 1024u)

typedef struct CtlContextEvent {
    uint32_t call;
    int16_t  rc;
    uint32_t prompt_off, prompt_len, prompt_dropped;
    uint32_t act_off, act_len, act_dropped;
} CtlContextEvent;

typedef struct CtlContext {
    bool valid;
    uint32_t task;
    char host[AGENT_LLM_HOST_CAP];
    uint16_t port;
    char path[AGENT_LLM_PATH_CAP];
    char model[AGENT_LLM_MODEL_CAP];
    uint32_t budget, rounds, tools_len, final_len;
    uint32_t events, dropped_events;
    uint64_t dropped_prompt, dropped_act;
    uint32_t prompt_used, act_used;
    CtlContextEvent event[CTL_CONTEXT_EVENTS];
    char prompt[CTL_CONTEXT_PROMPT_CAP];
    char act[CTL_CONTEXT_ACT_CAP];
} CtlContext;

static CtlContext g_context;

static void ctl_context_begin(void) {
    uint32_t task = g_context.task + 1u;

    memset(&g_context, 0, sizeof(g_context));
    g_context.valid = true;
    g_context.task = task;
    (void)snprintf(g_context.host, sizeof(g_context.host), "%s", g_llm.host);
    (void)snprintf(g_context.path, sizeof(g_context.path), "%s", g_llm.path);
    (void)snprintf(g_context.model, sizeof(g_context.model), "%s", g_llm.model);
    g_context.port = g_llm.port;
    g_context.budget = g_store->args_budget;
    g_context.rounds = g_store->max_rounds;
    g_context.tools_len = g_tools_len;
}

static uint32_t ctl_context_copy(char *pool, uint32_t cap, uint32_t *used,
                                 const char *src, uint32_t len, uint32_t *off) {
    uint32_t n;

    *off = *used;
    n = len <= cap - *used ? len : cap - *used;
    if (n > 0u) memcpy(pool + *used, src, n);
    *used += n;
    return len - n;
}

static void ctl_context_record(uint32_t call, cxf_result rc,
                               const char *args, uint32_t args_len,
                               const char *act, uint32_t alen) {
    CtlContextEvent *e;
    uint32_t action_len = rc == CXF_OK ? alen : 0u;

    if (!g_context.valid) return;
    if (g_context.events == CTL_CONTEXT_EVENTS) {
        g_context.dropped_events++;
        g_context.dropped_prompt += args_len;
        g_context.dropped_act += action_len;
        return;
    }
    e = &g_context.event[g_context.events++];
    memset(e, 0, sizeof(*e));
    e->call = call;
    e->rc = (int16_t)rc;
    e->prompt_dropped = ctl_context_copy(g_context.prompt, CTL_CONTEXT_PROMPT_CAP,
                                         &g_context.prompt_used, args, args_len,
                                         &e->prompt_off);
    e->act_dropped = ctl_context_copy(g_context.act, CTL_CONTEXT_ACT_CAP,
                                      &g_context.act_used, act, action_len,
                                      &e->act_off);
    e->prompt_len = args_len - e->prompt_dropped;
    e->act_len = action_len - e->act_dropped;
    g_context.dropped_prompt += e->prompt_dropped;
    g_context.dropped_act += e->act_dropped;
}

static void ctl_print_actions(FILE *dst, const char *act, uint32_t alen) {
    uint32_t off = 0u;

    while (off < alen) {
        AgentAct a;

        if (!agent_act_decode(act + off, alen - off, &a)) {
            fprintf(dst, "(坏块 @%u)\n", off);
            break;
        }
        if (a.kind == AGENT_ACT_DONE) {
            fputs("DONE ", dst);
            fwrite(a.args, 1u, a.args_len, dst);
        } else {
            fprintf(dst, "CALL %.*s ", (int)a.cap_len, a.cap);
            fwrite(a.args, 1u, a.args_len, dst);
        }
        fputc('\n', dst);
        off += agent_act_bytes(&a);
    }
}

static cxf_result ctl_register_impl(const char *name,const CapImpl *impl) {
    if(g_guards_count>=sizeof g_guards/sizeof g_guards[0])return CXF_ERR_RANGE;
    CtlGuard *g=&g_guards[g_guards_count++];g->original=*impl;g->exposed=*impl;
    g->name=name;
    g->exposed.exec=ctl_guard_exec;g->exposed.ctx=g;
    return cxf_service_register(&g_world,name,1,&g->exposed);
}

static cxf_result ctl_step_exec(const void *ctx, const char *args, uint32_t args_len,
                                char *out, uint32_t out_cap, uint32_t *out_len) {
    static char raw[AGENT_LLM_RAW_CAP];   /* 响应原始字节；static 理由同配置 */
    static char act[AGENT_LLM_REQ_CAP];   /* 规范动作字节（可能 N 条相接） */
    uint32_t alen = 0;
    cxf_result rc;

    g_calls++;
    *out_len=0; raw[0]=0;
    if (!agent_policy_egress(&g_policy,g_llm.scope,agent_llm_data_class(args,args_len),args,args_len)) {
        g_llm.last_status=0;g_llm.last_rc=CXF_ERR_RANGE;g_raw_head[0]=0;
        ctl_context_record(g_calls,CXF_ERR_RANGE,"",0,"",0);
        if(g_trace) { fputs("[policy] blocked model input; content withheld\n",g_trace);fflush(g_trace); }
        fputs("[policy] blocked model input or cloud egress; content withheld\n",stderr);
        return CXF_ERR_RANGE;
    }

    /*
     * 每次调用换一段诊断头：`X-CXF-Ctx: run=<pid> call=<n>`。
     *
     * ★ 这个头不加，端点上就**无法**区分"重复测量"与"框架重发" —— 两者在我们
     * 这边都是"同一份 body 又发了一次"，而服务端只看得见 body。加了之后，
     * 访问日志里 N 条相同请求可以按 (run, call) 归组：同 run 连续同 call 不存在，
     * 同 run call 递增 ⇒ 回路在推进；跨 run 相同 ⇒ 是有人在重复跑。
     *
     * 它不改变 body 一个字节（§4.9 的确定性契约只约束 body），也不改变
     * 前缀缓存的键 —— 引擎只看 body。
     */
    {
        int w = snprintf(g_hdr, sizeof(g_hdr), "X-CXF-Ctx: run=%ld call=%u\r\n",
                         (long)getpid(), g_calls);
        if (w > 0 && (size_t)w < sizeof(g_hdr)) g_llm.hdr = g_hdr;
    }

    uint64_t provider_start=g_trace?ctl_observe_ns():0;
#ifdef CXF_NATIVE_LLAMA
    if (g_native || g_tool_prepare) {
        rc = g_tool_prepare ? agent_tool_prepare_action(g_tool_prepare, (AgentLlmCfg *)ctx, args, args_len,
                                      raw, (uint32_t)sizeof(raw), act, (uint32_t)sizeof(act), &alen)
                            : agent_native_chat_action(g_native, (AgentLlmCfg *)ctx, args, args_len,
                                      raw, (uint32_t)sizeof(raw), act, (uint32_t)sizeof(act), &alen);
        /* These are fixed internal buffers. FULL here is a native context,
         * generation or parser limit; growing the outer WorkItem output would
         * only run the same inference again. Keep the original diagnostic. */
        if (rc == CXF_ERR_FULL) rc = CXF_ERR_RANGE;
    } else
#endif
    rc = agent_llm_chat_action((AgentLlmCfg *)ctx, args, args_len,
                               raw, (uint32_t)sizeof(raw), act, (uint32_t)sizeof(act), &alen);
    uint64_t provider_end=g_trace?ctl_observe_ns():0;
    ctl_timing("provider","primary",provider_start,provider_start,provider_end,provider_end,rc);
    raw[sizeof raw-1]=0;
    bool withheld = !agent_policy_text(&g_policy,raw,(uint32_t)strlen(raw));
    if (rc==CXF_OK) {
        if(!agent_policy_text(&g_policy,act,alen)) withheld=true;
        if(ctl_task_only()) for(uint32_t off=0;off<alen;) {
            AgentAct action;
            if(!agent_act_decode(act+off,alen-off,&action)||action.kind==AGENT_ACT_CALL) { rc=CXF_ERR_RANGE;break; }
            off+=agent_act_bytes(&action);
        }
    }
    if(withheld){rc=CXF_ERR_RANGE;alen=0;raw[0]=0;}
    ctl_context_record(g_calls, rc, args, args_len, act, alen);

    /* 失败时把正文头部留下来（见 g_raw_head 的注释）。成功则清空 ——
       不留上一次的残值，否则下一次失败会读到上一轮的正文。 */
    if (withheld) snprintf(g_raw_head,sizeof g_raw_head,"response withheld by data policy");
    else if (rc != CXF_OK)
        raw_body_head(raw, g_raw_head, sizeof(g_raw_head));
    else
        g_raw_head[0] = '\0';

    /*
     * trace 与 /context 共用动作投影；协议变化只改 agent_act_decode，
     * 展示层不得按工具名增加分支。
     */
    if (g_trace != NULL) {
        fprintf(g_trace, "=== call %u === rc=%d\n-- in (%u B) --\n", g_calls, (int)rc, args_len);
        fwrite(args, 1u, args_len, g_trace);
        fprintf(g_trace, "\n-- act (%u B) --\n", rc == CXF_OK ? alen : 0u);
        if (rc == CXF_OK) ctl_print_actions(g_trace, act, alen);
        fflush(g_trace);
    }
    if (rc != CXF_OK) return rc;

    /*
     * 这里**不再**做"文本 → 编码"。那个转换已经收进 `agent_llm_chat_action`：
     * 它忘了写就是"executed +1 但工具没跑过"（本文件第一版正是这么错的）。
     * 现在调用方拿到的就是终态字节，那类错误在结构上不可能再出现 ——
     * 少一步的检查，比多一步的注释值钱。
     *
     * 三种错分开报（§3 第 20 条）：FORMAT 重试没用 / RANGE 设计边界 / FULL 该扩容
     */
    if (alen == 0u) return CXF_ERR_FORMAT;
    if (alen > out_cap) return CXF_ERR_FULL;

    memcpy(out, act, alen);
    *out_len = alen;
    return CXF_OK;
}

static void die(const char *msg) {
    fprintf(stderr, "agentctl: %s\n", msg);
    exit(2);
}

/* Record the same declarations that were actually registered. agent.step is
 * an internal entry point, not a model-callable tool that can discard a task. */
static cxf_result ctl_register_tool(const char *key, const CapDef *def) {
    if (g_public_tool_count >= CAP_MAX) return CXF_ERR_RANGE;
    cxf_result rc = cxf_service_register(&g_world, key, 1, def);
    if (rc == CXF_OK) g_public_tools[g_public_tool_count++] = def;
    return rc;
}

static uint32_t ctl_render_tools(void) {
    if (ctl_task_only()) { memcpy(g_tools,"[]",3); return 2; }
    AgentToolSpec specs[CAP_MAX];
    for (uint32_t i = 0; i < g_public_tool_count; i++) {
        const CapDef *d = g_public_tools[i];
        AgentToolSpec s = { d->name, d->desc, d->in_schema, d->out_schema };
        uint32_t j = i;
        while (j && strcmp(specs[j - 1].name, s.name) > 0) {
            specs[j] = specs[j - 1]; j--;
        }
        specs[j] = s;
    }
    return agent_llm_render_tools(specs, g_public_tool_count, g_tools, sizeof g_tools);
}

/*
 * 四档部署：**看 / 建 / 改 / 追**。
 *
 * 每一档恰好对应目录里多一个能力，这不是整齐划一的审美，是四条不同的
 * 违约后果 —— 而"这个进程能造成多大的不可逆后果"本来就该按档声明，
 * 不该压成一个"能不能写"的布尔（§4.18.7）。
 *
 *   --diff    +fs.diff        只读：能回答"改成这样会怎样"，零副作用
 *   --create  +fs.write_new   能建新文件，但不会覆盖任何已有内容
 *   --write   +fs.diff +fs.write_apply   能改已有文件（因此也能建，因为覆盖的
 *             语义是"使它等于这些字节"）。**顺带挂 diff 不是送的**：一个
 *             "能改但看不见改成什么"的档位不是更安全，是更瞎。
 *   --append  +fs.append      能往已有文件末尾接，但既建不了也覆盖不了。
 *             它与上面三档**都不互斥**（建/改/追是三个正交动作），单独给就是
 *             "审计日志追加器"那一格。
 *
 * `--write` **不挂** `fs.write_new`：apply 已经涵盖创建，而 write_new 剩下的
 * 唯一区别是"拒绝覆盖"—— 在那个档位里它是一把没有钥匙的锁，只会多占 488 B
 * 的工具卡并给模型多一个选错的自由度。
 *
 * `--write` 也**不顺手挂** `--append`：覆盖与追加是两种动作，不是程度差别。
 * 想把两者都给出来就显式写两个开关 —— 让"这个部署允许什么"始终是命令行上
 * 数得出来的事，而不是某个开关的隐含副作用。
 */
typedef struct CtlRoots {
    const char *read;    /* --root */
    const char *diff;    /* --diff   */
    const char *create;  /* --create */
    const char *write;   /* --write  */
    const char *append;  /* --append */
    const char *net;     /* --net    HOST:PORT */
    const char *test_command; /* --test-command: fixed deployment command */
    uint32_t test_timeout_ms;
    uint32_t test_env_mode;
} CtlRoots;

/*
 * system 提示词**按挂载集合拼**，不写字面量。
 *
 * 512 B 的预算是硬的（`AGENT_LLM_SYS_CAP`）；完整参数契约在工具清单里，
 * system 只解释工作顺序，所有开关同时开启也必须装得下。
 */
static void ctl_system(char *dst, size_t cap,
                       bool diff, bool create, bool write, bool append, bool net, bool test) {
    static const char BASE[] =
        "你是工作区代码助手。";
    static const char F_DIFF[] =
        "fs.diff 只读预览；";
    static const char F_CREATE[] =
        "fs.write_new 只新建；";
    static const char F_WRITE[] =
        "fs.edit 版本化多行编辑，冲突先重读；fs.write_apply 整文件，fs.replace_line 单行；";
    static const char F_APPEND[] =
        "fs.append 追加；";
    static const char F_NET[] =
        "http.post 向固定服务发送；";
    static const char TAIL[] =
        "fs.list 列目录，fs.read 读文件。每轮只调一个工具，等结果再继续。"
        "不得虚报成功。";
    const char *frag[5];
    size_t n = 0u;

    frag[0] = diff   ? F_DIFF   : NULL;
    frag[1] = create ? F_CREATE : NULL;
    frag[2] = write  ? F_WRITE  : NULL;
    frag[3] = append ? F_APPEND : NULL;
    frag[4] = net    ? F_NET    : NULL;

#define CTL_APPEND(s)                                                       \
    do {                                                                    \
        size_t m = strlen(s);                                               \
        if (n + m >= cap) { fprintf(stderr, "agentctl: system 超长\n"); exit(2); } \
        memcpy(dst + n, (s), m);                                            \
        n += m;                                                             \
    } while (0)

    CTL_APPEND(BASE);
    for (uint32_t i = 0u; i < 5u; i++)
        if (frag[i] != NULL) CTL_APPEND(frag[i]);
    if (test) CTL_APPEND("改后 test.run(args=空)；exit=0 且 timeout=0 才能说通过。");
    if (create || write) CTL_APPEND("整文件写入 args 必须是路径、换行、完整内容，不能仅给路径。");
    CTL_APPEND(TAIL);
    dst[n] = '\0';
#undef CTL_APPEND
}

/*
 * `--net HOST:PORT`。刻意**不接受 URL**：`http://` 前缀会让人以为能在这里
 * 带 path 或凭据 —— 而 path 归模型（在 payload 里），凭据归环境变量
 * （`CXF_NET_AUTH`）。部署只声明一件事：**这个进程能往哪台机器发**。
 */
static bool parse_host_port(const char *s, char *host, size_t hcap, uint16_t *port) {
    const char *colon = strrchr(s, ':');
    size_t n;
    uint32_t p = 0u;

    if (colon == NULL) return false;
    n = (size_t)(colon - s);
    if (n == 0u || n >= hcap) return false;
    memcpy(host, s, n);
    host[n] = '\0';

    if (colon[1] == '\0') return false;
    for (const char *q = colon + 1; *q != '\0'; q++) {
        if (*q < '0' || *q > '9') return false;
        p = p * 10u + (uint32_t)(*q - '0');
        if (p > 65535u) return false;
    }
    *port = (uint16_t)p;
    return true;
}

/* 先在临时副本里验证完整配置，全部通过才替换运行值；失败绝不留下半份新配置。 */
static bool ctl_config_apply(const CtlFileConfig *f, char *err, uint32_t err_cap) {
    AgentLlmCfg next = g_llm;

    if ((f->present & CTL_CFG_API_URL) != 0u) {
        cxf_result rc = agent_llm_cfg_set_url(&next, f->url);
        if (rc != CXF_OK) {
            (void)snprintf(err, err_cap, "api.url 非法：%s", cxf_result_str(rc));
            return false;
        }
    }
    if ((f->present & CTL_CFG_API_MODEL) != 0u) {
        if (f->model[0] == '\0') {
            (void)snprintf(err, err_cap, "api.model 不能为空");
            return false;
        }
        memcpy(next.model, f->model, sizeof(next.model));
    }
    if ((f->present & CTL_CFG_API_KEY) != 0u)
        memcpy(next.api_key, f->key, sizeof(next.api_key));
    if ((f->present & CTL_CFG_API_TIMEOUT) != 0u) {
        if (f->timeout_ms < 1000u || f->timeout_ms > 600000u) {
            (void)snprintf(err, err_cap, "api.timeout_ms 必须在 1000..600000 之间");
            return false;
        }
        next.timeout_ms = f->timeout_ms;
    }
    if (next.host[0] == '\0' || next.model[0] == '\0') {
        (void)snprintf(err, err_cap, "缺少 api.url 或 api.model（也可由环境变量提供）");
        return false;
    }
    if ((f->present & (CTL_CFG_API_URL | CTL_CFG_API_MODEL |
                       CTL_CFG_API_KEY | CTL_CFG_API_TIMEOUT)) != 0u) {
        next.last_status = 0u;
        next.last_rc = (int16_t)CXF_OK;
    }
    AgentPolicy updated=g_policy;
    if(!agent_policy_protect(&updated,g_config_path)||!agent_policy_secret(&updated,next.api_key)) {
        snprintf(err,err_cap,"data policy registration failed");return false;
    }
    if(!agent_policy_text(&updated,next.model,(uint32_t)strlen(next.model)) ||
       !agent_policy_text(&updated,next.path,(uint32_t)strlen(next.path)) ||
       !agent_policy_text(&updated,next.system,(uint32_t)strlen(next.system))) {
        snprintf(err,err_cap,"sensitive model metadata blocked");return false;
    }
    g_policy=updated;g_llm = next;

    if (g_store != NULL) {
        if ((f->present & CTL_CFG_BUDGET) != 0u) agent_args_budget_set(g_store, f->budget);
        if ((f->present & CTL_CFG_ROUNDS) != 0u) agent_set_max_rounds(g_store, f->rounds);
        if ((f->present & (CTL_CFG_CONC | CTL_CFG_RPM)) != 0u) {
            uint32_t conc = (f->present & CTL_CFG_CONC) != 0u ? f->conc : g_store->quota.conc_max;
            uint32_t rpm = (f->present & CTL_CFG_RPM) != 0u ? f->rpm : g_store->quota.rpm;
            agent_quota_set(g_store, conc, rpm);
        }
    }
    return true;
}

static bool ctl_config_reload(bool initial, bool verbose) {
#ifdef CXF_NATIVE_LLAMA
    if (g_native_model) return true;
#endif
    CtlFileConfig f;
    char err[256];
    CtlConfigReadResult rc = ctl_config_read(g_config_path, &f, err, sizeof(err));

    if (rc == CTL_CONFIG_NOT_FOUND) {
        if ((initial && !g_config_explicit) || (!initial && !g_config_loaded)) return true;
        (void)snprintf(err, sizeof(err), "配置文件不存在");
    } else if (rc == CTL_CONFIG_OK) {
        if (!ctl_config_apply(&f, err, sizeof(err))) rc = CTL_CONFIG_ERROR;
        else {
            g_config_loaded = true;
            if (verbose)
                printf("[config] 已加载 %s；model %s，key %s\n",
                       g_config_path, g_llm.model, g_llm.api_key[0] != '\0' ? "已设置" : "未设置");
            return true;
        }
    }
    fprintf(stderr, "agentctl: 配置重载失败：%s：%s；保留上一份有效配置\n",
            g_config_path, err);
    return false;
}

static void ctl_config_init(const char *path, bool explicit_path) {
    size_t n = strlen(path);
    cxf_result rc;

    if (n == 0u || n >= sizeof(g_config_path)) die("配置路径过长");
    memcpy(g_config_path, path, n + 1u);
    g_config_explicit = explicit_path;

#ifdef CXF_NATIVE_LLAMA
    if (g_native_model) {
        if (explicit_path) die("原生模式不加载 HTTP 配置；system 请使用 CXF_LLM_SYSTEM");
        memset(&g_llm, 0, sizeof g_llm);
        (void)snprintf(g_llm.model, sizeof g_llm.model, "native-qwen3");
        const char *sys = getenv("CXF_LLM_SYSTEM");
        if (sys) {
            if (strlen(sys) >= sizeof g_llm.system) die("CXF_LLM_SYSTEM 过长");
            memcpy(g_llm.system, sys, strlen(sys) + 1);
        }
        return;
    }
#endif

    rc = agent_llm_cfg_from_env(&g_llm);
    if (rc == CXF_ERR_DEP) {
        memset(&g_llm, 0, sizeof(g_llm));  /* JSON 可以独立提供 URL + model */
    } else if (rc != CXF_OK) {
        die("环境变量里的 LLM 配置非法");
    }
    if (!ctl_config_reload(true, false)) exit(2);
    if (g_llm.host[0] == '\0' || g_llm.model[0] == '\0')
        die("缺端点配置：在 agentctl.json 填 api.url + api.model，"
            "或使用 CXF_LLM_URL + CXF_LLM_MODEL");
}

static void ctl_local_init(void) {
    if (!g_local_path) return;
    CtlFileConfig f; char err[256];
    if (ctl_config_read(g_local_path, &f, err, sizeof err) != CTL_CONFIG_OK)
        die("local config could not be read or parsed");
    const uint32_t api = CTL_CFG_API_URL | CTL_CFG_API_MODEL | CTL_CFG_API_KEY | CTL_CFG_API_TIMEOUT;
    if ((f.present & (CTL_CFG_API_URL | CTL_CFG_API_MODEL)) != (CTL_CFG_API_URL | CTL_CFG_API_MODEL) ||
        (f.present & ~api)) die("local config requires api.url/model and accepts no agent settings");
    memset(&g_local, 0, sizeof g_local);
    if (agent_llm_cfg_set_url(&g_local, f.url) != CXF_OK ||
        strcmp(g_local.host, "127.0.0.1") || !g_local.port || !f.model[0])
        die("local helper requires an explicit 127.0.0.1 HTTP endpoint and model");
    for (const unsigned char *p = (const unsigned char *)g_local.path; *p; p++)
        if (*p <= 32 || *p == 127) die("local helper URL contains invalid bytes");
    for (const unsigned char *p = (const unsigned char *)f.key; *p; p++)
        if (*p < 32 || *p == 127) die("local helper credential contains invalid bytes");
    memcpy(g_local.model, f.model, sizeof g_local.model);
    if (f.present & CTL_CFG_API_KEY) memcpy(g_local.api_key, f.key, sizeof g_local.api_key);
    g_local.timeout_ms = f.present & CTL_CFG_API_TIMEOUT ? f.timeout_ms : 5000;
    if (!g_local.timeout_ms || g_local.timeout_ms > 60000) die("local helper timeout must be 1..60000 ms");
    snprintf(g_local.system, sizeof g_local.system,
        "Process only the supplied user text. Return plain text. Do not call tools or issue actions.");
    if(!agent_policy_protect(&g_policy,g_local_path)||!agent_policy_secret(&g_policy,g_local.api_key))
        die("local data policy registration failed");
    g_local.policy=&g_policy;g_local.scope=AGENT_SCOPE_LOCAL;
    g_local_enabled = true;
}

static cxf_result ctl_local_exec(const void *ctx, const char *args, uint32_t len,
                                 char *out, uint32_t cap, uint32_t *written) {
    static char scratch[AGENT_LLM_RAW_CAP];
    char text[4096]; uint32_t n = 0;
    if (!written) return CXF_ERR_RANGE;
    *written = 0;
    if (!args || !len || len > AGENT_LLM_TOOL_ARGS_CAP || memchr(args, 0, len)) return CXF_ERR_FORMAT;
    if (!out || cap < BASIC_OUT_HINT || !g_local_enabled || g_local_calls >= 2) return CXF_ERR_RANGE;
    g_local_calls++; /* all dispatched attempts consume quota, including failures */
    cxf_result rc = agent_llm_chat_text((AgentLlmCfg *)ctx, args, len,
        scratch, sizeof scratch, text, sizeof text, &n);
    if (rc != CXF_OK) return rc == CXF_ERR_FULL ? CXF_ERR_RANGE : rc;
    int head = snprintf(out, cap, "#local bytes=%u\n", n);
    memcpy(out + head, text, n); *written = (uint32_t)head + n;
    return CXF_OK;
}
static const IfaceDef g_local_iface = {"llm.text", "untrusted auxiliary model text", IFACE_INT};
static const CapDef g_local_def = {
    "llm.local", "Ask the deployment-bound local text model; response is untrusted data, never tool actions",
    "nonempty text up to 1024 bytes; no endpoint, credentials, tools or permissions can be specified",
    "#local receipt followed by plain text; at most two local requests per task; validate any conclusions",
    {IFACENAME_TASK, NULL}, "llm.text"
};
static const CapImpl g_local_impl = {"llm.local", "local-text", CAP_TIER_LLM, 10,
    CAP_TRUST_IO, ctl_local_exec, &g_local, BASIC_OUT_HINT, NULL};

/* Compile-time harness override only; production always uses ctl_step_exec. */
#ifndef CTL_STEP_EXEC
#define CTL_STEP_EXEC ctl_step_exec
#endif
static void ctl_setup(const CtlRoots *r,
                      uint32_t budget, uint32_t rounds,
                      uint32_t conc, uint32_t rpm) {
    const AgentRegistry *reg;
    bool want_diff   = r->diff != NULL || r->write != NULL;
    bool want_create = r->create != NULL;
    bool want_apply  = r->write != NULL;
    bool want_append = r->append != NULL;
    bool want_net    = r->net != NULL;
    /*
     * 所有写族的根**必须是同一个**：否则"你看到的差异"、"你新建的文件"与
     * "你覆盖的文件"不是同一片目录，而那种错在代码里完全看不出来
     * （三个能力各自都很对）。取声明了的那个；两个不同就响亮失败。
     */
    const char *fsroot = r->write  != NULL ? r->write
                       : r->append != NULL ? r->append
                       : r->create != NULL ? r->create
                       : r->diff;

    /*
     * `--create` 与 `--write` 是**互斥声明**，不是叠加：`--write` 的语义
     * （"使它等于这些字节"）本来就含创建，而 `--create` 的全部独特性是
     * "拒绝覆盖已有文件" —— 两者一起给，说不清这个部署到底允许什么。
     * 拒绝而不是静默取其一：静默取宽的那一侧等于偷偷提权。
     */
    if (r->create != NULL && r->write != NULL)
        die("--create 与 --write 重叠（--write 已含创建）；"
            "要表达\"只建不改\"就只给 --create");
    if (fsroot != NULL) {
        const char *all[4];
        uint32_t k = 0u;

        all[k++] = r->diff;
        all[k++] = r->create;
        all[k++] = r->write;
        all[k++] = r->append;
        for (uint32_t i = 0u; i < 4u; i++)
            if (all[i] != NULL && strcmp(all[i], fsroot) != 0)
                die("--diff / --create / --write / --append 必须是同一个根"
                    "（否则看到差异的与覆盖的不是同一个文件）");
    }

    if (g_llm.system[0] == '\0' && ctl_task_only())
        snprintf(g_llm.system,sizeof g_llm.system,"Answer the supplied task as text. Workspace tools are disabled by deployment policy.");
    if (g_llm.system[0] == '\0')
        ctl_system(g_llm.system, sizeof g_llm.system,
                   want_diff, want_create, want_apply, want_append, want_net, r->test_command != NULL);

    g_fs = (FsCfg){ r->read, 0u, 0u, &g_policy };
    g_wfs = (FsRootCfg){ fsroot, &g_policy };

    g_grep_impl = fs_capimpl_grep;      /* 模板：契约已定，ctx 由部署填 */
    g_grep_impl.ctx = &g_fs;
    g_grep_impl.out_hint = FS_GREP_OUT_HINT;
    g_basic_fs = (BasicFsCfg){ r->read, &g_policy };
    g_read_impl = basic_impl_read; g_read_impl.ctx = &g_basic_fs;
    g_list_impl = basic_impl_list; g_list_impl.ctx = &g_basic_fs;

    g_step_impl = (CapImpl){
        .capability = CAPNAME_STEP,
        .impl       = "llm",
        .tier       = CAP_TIER_LLM,
        .cost       = 90,
        .trust      = CAP_TRUST_IO,
        .exec       = CTL_STEP_EXEC,
        .ctx        = &g_llm,
        .out_hint   = 8192u,
        .fallback   = NULL
    };

    if (cxf_world_init(&g_world, cxf_alloc_heap(), 1u << 16) != CXF_OK) die("world_init");
    if (cxf_type_register_bundle(&g_world, &cxf_types_agent_types) != CXF_OK) die("types");
    /* Demo capabilities remain in test_agent's bundle, not in a real deployment. */
    if (cxf_service_register(&g_world, "iface/task.text", 1, &g_task_iface) != CXF_OK) die("task iface");
    if (cxf_service_register_bundle(&g_world, &cxf_services_agent_fs) != CXF_OK) die("fs svc");
    g_public_tool_count = 0;
    g_public_tools[g_public_tool_count++] = &fs_capdef_grep; /* registered by fs bundle */

    /*
     * 类型 + 契约 + 实现必须在**同一次** sync 之前到齐（§10.6 不变式 7）：
     * agent.action 是 INT，必须有人产出它，否则 store_init 内部的 registry_sync
     * 直接返回 RANGE（目录项悬空）。这条已经拦下过一次设计错误，不是理论。
     */
    if (ctl_register_impl("impl/fs.grep.fs", &g_grep_impl) != CXF_OK)
        die("impl/fs.grep");
    if (cxf_service_register(&g_world, "iface/fs.view", 1, &basic_iface_view) != CXF_OK ||
        ctl_register_tool("cap/fs.read", &basic_capdef_read) != CXF_OK ||
        ctl_register_impl("impl/fs.read.fs", &g_read_impl) != CXF_OK ||
        ctl_register_tool("cap/fs.list", &basic_capdef_list) != CXF_OK ||
        ctl_register_impl("impl/fs.list.fs", &g_list_impl) != CXF_OK) die("basic fs");
    if (want_apply) {
        g_edit_fs = (BasicFsCfg){ r->write, &g_policy };
        g_edit_impl = basic_impl_edit; g_edit_impl.ctx = &g_edit_fs;
        if (ctl_register_tool("cap/fs.replace_line", &basic_capdef_edit) != CXF_OK ||
            ctl_register_impl("impl/fs.replace_line.fs", &g_edit_impl) != CXF_OK)
            die("line edit tool");
        g_patch_impl=basic_impl_patch;g_patch_impl.ctx=&g_edit_fs;
        if(ctl_register_tool("cap/fs.edit",&basic_capdef_patch)!=CXF_OK ||
           ctl_register_impl("impl/fs.edit.fs",&g_patch_impl)!=CXF_OK)die("versioned edit tool");
    }
    if (g_local_enabled) {
        if (cxf_service_register(&g_world, "iface/llm.text", 1, &g_local_iface) != CXF_OK ||
            ctl_register_tool("cap/llm.local", &g_local_def) != CXF_OK ||
            ctl_register_impl("impl/llm.local.local", &g_local_impl) != CXF_OK)
            die("local helper tool");
    }
    if (r->test_command) {
        if (!r->test_command[0]) die("--test-command must not be empty");
        g_test_sandbox.policy=&g_policy;
        g_test_cfg = (TestRunCfg){ r->read, r->test_command, r->test_timeout_ms, r->test_env_mode, &g_test_sandbox };
        g_test_impl = basic_impl_test; g_test_impl.ctx = &g_test_cfg;
#ifdef CXF_NATIVE_LLAMA
        if(g_tool_prepare_enabled)g_test_impl.exec=g_tool_output_profile?ctl_test_output_exec:ctl_test_prepare_exec;
#endif
        if (cxf_service_register(&g_world, "iface/test.result", 1, &basic_iface_test) != CXF_OK ||
            ctl_register_tool("cap/test.run", &basic_capdef_test) != CXF_OK ||
            ctl_register_impl("impl/test.run.process", &g_test_impl) != CXF_OK) die("test tool");
        g_trust = CAP_TRUST_WRITE;
    }

    /*
     * 差异能力是**读**能力（trust = IO），所以它先于写能力注册，且只要求
     * `--diff` 或 `--write` 之一 —— 只读部署照样能问"我这样改会变成什么样"。
     *
     * ★ 每一组都是"三件一起到齐"（iface + capdef + impl）。少任何一件，
     *   sync 会当场拒绝 —— 而这条纪律的代价就是下面这几行样板。
     */
    if (want_diff) {
        g_df_impl = fs_capimpl_diff;        /* 模板：契约已定，ctx 由部署填 */
        g_df_impl.ctx = &g_wfs;

        if (cxf_service_register(&g_world, "iface/fs.diffed", 1, &fs_iface_diffed) != CXF_OK)
            die("iface/fs.diffed");
        if (ctl_register_tool("cap/fs.diff", &fs_capdef_diff) != CXF_OK)
            die("cap/fs.diff");
        if (ctl_register_impl("impl/fs.diff.fs", &g_df_impl) != CXF_OK)
            die("impl/fs.diff");
    }

    /*
     * 写族的整组：**不声明就一件都不注册** —— 于是"这个进程能写哪片盘"是一个
     * 必须被说出来的部署事实，没有默认值。顺带这是天花板从哪来的答案：
     * 声明了写根 ⇒ 请求天花板才是 WRITE。反过来"注册了写能力但天花板还是 IO"
     * 是一种自相矛盾的部署（能力存在却永远选不中），不值得允许。
     *
     * `fs.wrote` 是这一族共用的**收据类型**（new / exists / wrote / same）：
     * 形状相同就不该是两个类型，否则类型图上会有两条边指向同一个结构。
     */
    if (want_create || want_apply || want_append) {
        g_trust = CAP_TRUST_WRITE;
        if (cxf_service_register(&g_world, "iface/fs.wrote", 1, &fs_iface_wrote) != CXF_OK)
            die("iface/fs.wrote");
    }
    if (want_create) {
        g_wn_impl = fs_capimpl_write_new;   /* 模板：契约已定，ctx 由部署填 */
        g_wn_impl.ctx = &g_wfs;

        if (ctl_register_tool("cap/fs.write_new", &fs_capdef_write_new) != CXF_OK)
            die("cap/fs.write_new");
        if (ctl_register_impl("impl/fs.write_new.fs", &g_wn_impl) != CXF_OK)
            die("impl/fs.write_new");
    }
    if (want_apply) {
        g_ap_impl = fs_capimpl_apply;
        g_ap_impl.ctx = &g_wfs;

        if (ctl_register_tool("cap/fs.write_apply", &fs_capdef_apply) != CXF_OK)
            die("cap/fs.write_apply");
        if (ctl_register_impl("impl/fs.write_apply.fs", &g_ap_impl) != CXF_OK)
            die("impl/fs.write_apply");
    }
    /*
     * 追加是**独立的一档**，与 `--create` / `--write` 都不同时排斥：
     *   --append 单独给   = "能往已有日志里追，不能建也不能覆盖"（审计追加器）
     *   --create --append = "能建 + 能追，但改不了已有内容"
     *   --write  --append = "能改 + 能追"（覆盖与追加是两种动作，不是程度差别）
     * 它同样需要 WRITE 天花板：重复追加会真的多写一份（§4.18.9）。
     */
    if (want_append) {
        g_app_impl = fs_capimpl_append;
        g_app_impl.ctx = &g_wfs;

        if (ctl_register_tool("cap/fs.append", &fs_capdef_append) != CXF_OK)
            die("cap/fs.append");
        if (ctl_register_impl("impl/fs.append.fs", &g_app_impl) != CXF_OK)
            die("impl/fs.append");
    }
    /*
     * 出网档：`--net HOST:PORT`。它与写族**完全正交** —— "能改文件"与
     * "能往某台机器发"是两件互不相干的事，所以既不互斥也不隐含。
     *
     * 凭据只从 `CXF_NET_AUTH` 来（不进命令行，因为命令行会进 shell 历史与
     * `ps`；更不会进 payload）。天花板同样是 WRITE：出网的最坏后果是
     * **别人的账被改了**，与毁掉本地内容同级。
     */
    if (want_net) {
        g_trust = CAP_TRUST_WRITE;
        if (!parse_host_port(r->net, g_net_host, sizeof g_net_host, &g_net_cfg.port))
            die("--net 要写成 HOST:PORT（例如 127.0.0.1:8080）；不接受 URL —— "
                "path 由模型给，凭据走 CXF_NET_AUTH");
        g_net_cfg.host       = g_net_host;
        g_net_cfg.auth       = getenv("CXF_NET_AUTH");
        g_net_cfg.timeout_ms = 0u;
        g_net_cfg.policy=&g_policy;g_net_cfg.scope=g_net_scope;
        if(!agent_policy_secret(&g_policy,g_net_cfg.auth?g_net_cfg.auth:""))die("network policy registration failed");

        g_net_impl = http_capimpl_post;
        g_net_impl.ctx = &g_net_cfg;

        if (cxf_service_register(&g_world, "iface/http.resp", 1, &http_iface_resp) != CXF_OK)
            die("iface/http.resp");
        if (ctl_register_tool("cap/http.post", &http_capdef_post) != CXF_OK)
            die("cap/http.post");
        if (ctl_register_impl("impl/http.post.net", &g_net_impl) != CXF_OK)
            die("impl/http.post");
    }
    if (cxf_service_register(&g_world, "iface/agent.action", 1, &iface_action) != CXF_OK)
        die("iface/agent.action");
    if (cxf_service_register(&g_world, "cap/agent.step", 1, &capdef_step) != CXF_OK)
        die("cap/agent.step");
    if (cxf_service_register(&g_world, "impl/agent.step.llm", 1, &g_step_impl) != CXF_OK)
        die("impl/agent.step");

    if (cxf_module_load(&g_world, &agent_store_def, &g_ids[0]) != CXF_OK) die("load store");
    if (cxf_module_load(&g_world, &agent_inject_def, &g_ids[1]) != CXF_OK) die("load inject");
    if (cxf_module_load(&g_world, &agent_match_def, &g_ids[2]) != CXF_OK) die("load match");
    if (cxf_module_load(&g_world, &agent_execute_def, &g_ids[3]) != CXF_OK) die("load execute");
    if (cxf_module_load(&g_world, &agent_recycle_def, &g_ids[4]) != CXF_OK) die("load recycle");
    if (cxf_world_start(&g_world) != CXF_OK) die("world_start");

    g_store = (AgentStore *)cxf_module_state(&g_world, g_ids[0]);
    if (g_store == NULL) die("store state");

    agent_args_budget_set(g_store, budget);
    agent_set_max_rounds(g_store, rounds);

    /*
     * 配额（§4.17.8）。**不声明就是不限**，所以这里默认什么都不做 —— 引入闸门
     * 不改变任何既有部署的行为。两个数都是部署事实（买来的契约），不是这里算
     * 出来的：云端有 429/RPM 时由人来声明，本地推理按 slot 数声明。
     */
    if (conc != 0u || rpm != 0u) agent_quota_set(g_store, conc, rpm);

    /*
     * 只给 --conc 是**静默无效**的：今天的并发上限只体现为令牌桶容量，而速率
     * 不声明（rpm=0）时桶根本不用。不说出来，用户会以为配上了 8 路。
     * 这类"看起来配了"的失效必须当场喊，不能等它以后变成一次事故。
     */
    if (conc != 0u && rpm == 0u) {
        fprintf(stderr,
                "agentctl: --conc %u 单独声明没有任何效果（并发上限今天只体现为令牌桶"
                "容量，未声明 --rpm 时桶不生效）。要限速请同时给 --rpm。\n", conc);
    }

    reg = (const AgentRegistry *)cxf_service_find(&g_world, AGENT_REGISTRY_SVC, 1);
    if (reg == NULL || reg->sync == NULL || reg->tools == NULL) die("registry svc");
    if (reg->sync(g_store) != CXF_OK) die("registry sync");

    /*
     * 清单从**真实目录**渲染，不是手写的（§4.16 接入层）。
     * 于是"模型看得见"与"MATCH 选得中"共用同一个 trust 判据 —— 不会出现
     * 清单里有、路由里没有这类错位。顺序必须晚于 sync：清单读的是目录表。
     */
    g_tools_len = ctl_render_tools();
    if (g_tools_len == 0u || g_tools_len >= (uint32_t)sizeof(g_tools)) die("tools 渲染失败");
    g_llm.tools     = g_tools;
    g_llm.tools_len = ctl_task_only() ? 0 : g_tools_len;
#ifdef CXF_NATIVE_LLAMA
    if (g_native_model && !g_native && !g_tool_prepare) {
        cxf_llama_params p = cxf_llama_default_params();
        p.model_path = g_native_model; p.backend_path = g_native_backend;
        p.context_tokens = g_native_context; p.batch_tokens = g_native_batch; p.gpu_layers = g_native_gpu; p.threads = g_native_threads;
        cxf_result rc = g_tool_prepare_enabled
            ? agent_tool_prepare_open(&p, g_native_max, "/tmp", UINT64_C(1073741824), g_prepare_options, &g_tool_prepare)
            : agent_native_open(&p, g_native_max, &g_native);
        if (rc != CXF_OK) {
            fprintf(stderr, "agentctl: 原生模型加载失败：%s\n", cxf_result_str(rc));
            exit(2);
        }
        atexit(ctl_native_close);
        if(g_tool_output_profile)fprintf(stderr,"[tool-output] {\"kind\":\"ready\",\"enabled\":%s}\n",g_tool_output_enabled?"true":"false");
    }
#endif
}

static void ctl_submit(const char *text, uint32_t len) {
    InjectMsg m;
    g_local_calls = 0;

    memset(&m, 0, sizeof(m));
    m.kind      = AGENT_PLAN_LOOP;
    m.trust_max = g_trust;
    m.text_len  = len;

    if (cxf_ring_write(&g_store->inbox, &m, (uint32_t)sizeof(m)) != (uint32_t)sizeof(m))
        die("入口 ring 写失败（帧头）");
    if (len > 0u &&
        cxf_ring_write(&g_store->inbox, text, len) != len)
        die("入口 ring 写失败（载荷）");
}

static bool ctl_idle(void) {
    return cxf_ring_size(&g_store->inbox) == 0 && g_store->items.table.count == 0u;
}

/*
 * 段化读数（§4.18.16 八）。见调用点的说明：分子分母一起印，
 * 因为单看任何一个都会把"批次大小分布"与"失败位置分布"混成一个数。
 */
static void print_seg_stats(const AgentStore *s) {
    char     nbuf[256], kbuf[256];
    int      wn = 0, wk = 0;
    uint32_t nN = 0, nK = 0;
    unsigned long long sumN = 0, sumK = 0;

    /*
     * 先报"这次产出有没有变成批次"（§4.18.17 六）。**顺序不能反**：
     * 若 `bad + rejected` 占了大头，后面两个直方图的分母接近 0，
     * 那时再谈 k 与 E[N-k] 就是在谈空集合。所以这一行排在前面，
     * 它是否决项而不是补充项。
     */
    {
        uint32_t tried = s->stat_batch_bad + s->stat_batch_rejected + s->stat_batch_ok;

        if (tried == 0u) {
            fprintf(stderr, "[batch] 当前统计区间没有任何产出尝试 ⇒ p̂ 无定义，后面两个直方图也不可信\n");
        } else {
            fprintf(stderr,
                    "[batch] ok %u | bad %u | rejected %u | p̂ %.2f"
                    "（<0.90 该降 N；bad 多 = 格式能力，rejected 多 = 目录/天花板）\n",
                    s->stat_batch_ok, s->stat_batch_bad, s->stat_batch_rejected,
                    (double)s->stat_batch_ok / (double)tried);
        }
    }

    nbuf[0] = '\0';
    kbuf[0] = '\0';
    for (uint32_t i = 0; i <= AGENT_FANOUT_MAX; i++) {
        if (s->stat_seg_hist[i] != 0u) {
            wn += snprintf(nbuf + wn, sizeof(nbuf) - (size_t)wn, " %u=%u",
                           i, s->stat_seg_hist[i]);
            nN += s->stat_seg_hist[i];
            sumN += (unsigned long long)i * s->stat_seg_hist[i];
        }
        if (s->stat_fail_seq[i] != 0u) {
            wk += snprintf(kbuf + wk, sizeof(kbuf) - (size_t)wk, " %u=%u",
                           i, s->stat_fail_seq[i]);
            nK += s->stat_fail_seq[i];
            sumK += (unsigned long long)i * s->stat_fail_seq[i];
        }
        if (wn < 0 || wk < 0) return;          /* 截断就整行不打，不印半句 */
    }
    if (wn == 0) { snprintf(nbuf, sizeof(nbuf), " (无)"); }

    fprintf(stderr, "[seg] 批大小 n:%s\n", nbuf);
    if (nK == 0u) {
        fprintf(stderr, "[seg] 失败位置 k: (无失败)  ⇒ 这一跑不提供 k 的证据\n");
        return;
    }
    fprintf(stderr, "[seg] 失败位置 k:%s\n", kbuf);

    /*
     * 最后一个数只在**单批**时印 —— 这是本文件上一版的一处错。
     *
     * 上一版印的是 `E[N] - E[k]`，并把它叫"平均能省几段（上界）"。它错在
     * **两个边际的分母不同**：`E[N]` 除的是全部批次，`E[k]` 除的是**有失败的**
     * 批次。失败率越低，这个差越大 —— 于是"只失败一次"会看起来像"能省很多段"。
     * 方向恰好反了：失败越少，回退能省的总量越少。
     *
     * 单批时没有这个问题（此刻只有一个 N 和一个 k，相减就是那一批的 N-k），
     * 所以**能精确就精确**；多批时如实说"这里算不了"，并指向按批配对的探针
     * （tools/seg_probe.py）。宁可少一个数，不可多一个看起来对的数。
     */
    if (nN == 1u) {
        fprintf(stderr, "[seg] 只有一批（N=%llu, k=%llu）⇒ N-k = %lld 是**精确值**"
                        "（这一跑回退能省的段数）\n",
                sumN, sumK, (long long)sumN - (long long)sumK);
    } else {
        fprintf(stderr,
                "[seg] E[N] %.2f（%u 批） | E[k|失败] %.2f（%u 次失败）\n"
                "[seg] N-k **不在这里算**：两个数分母不同（全部批 vs 失败批），"
                "相减不是 E[N-k]。按批配对见 tools/seg_probe.py\n",
                (double)sumN / (double)nN, nN,
                (double)sumK / (double)nK, nK);
    }
}

/* 一次提交的差量结果。store 的终值与统计是累计日志，前端不能把旧值重放一遍。 */
typedef struct CtlRun {
    cxf_str  final;       /* 仅本次新增的终值字节；指向 store，下一次提交前有效 */
    uint32_t ticks;
    uint32_t calls;
    uint32_t executed;
    uint32_t failed;
    uint32_t capped;
    uint32_t quota_wait;
    bool     idle;        /* false = 撞上限后仍有活条目，禁止继续叠加任务 */
} CtlRun;

static CtlRun ctl_run(const char *text, uint32_t len) {
#ifdef CXF_NATIVE_LLAMA
    char output_input[AGENT_INPUT_MAX+1u];
    if(g_tool_output_profile&&len&&text[len-1]!='\n'){
        if(len>=sizeof output_input)die("工具输出通道输入过长");
        memcpy(output_input,text,len);output_input[len++]='\n';text=output_input;
    }
    if ((g_native || g_tool_prepare) && (g_tool_prepare ? agent_tool_prepare_reset(g_tool_prepare) : agent_native_reset(g_native)) != CXF_OK) die("原生 session 重置失败");
#endif
    CtlRun r;
    cxf_str before = agent_final(g_store);
    cxf_str fin;
    uint32_t calls = g_calls;
    uint32_t executed = g_store->stat_executed;
    uint32_t failed = g_store->stat_failed;
    uint32_t capped = g_store->stat_capped;
    uint32_t quota_wait = g_store->stat_quota_wait;
    struct timespec ts;

    memset(&r, 0, sizeof(r));
    memset(g_ph_ns, 0, sizeof(g_ph_ns));
    ctl_context_begin();  /* 配置已重载但尚未执行：此刻才是旧任务的元数据快照。 */
    ctl_submit(text, len);

    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_t0_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;

    while (!ctl_idle() && r.ticks < CTL_TICKS) {
        cxf_world_tick(&g_world);
        r.ticks++;
        for (uint32_t p = 0; p < CXF_PHASE_COUNT; p++)
            g_ph_ns[p] += cxf_world_phase_ns(&g_world, (cxf_phase)p);
        if (cxf_world_check(&g_world) != true) die("world 不变式被破坏");
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_wall_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec - g_t0_ns;
    g_total_ticks += r.ticks;

    fin = agent_final(g_store);
    if (fin.len >= before.len) {
        r.final.p = fin.p != NULL ? fin.p + before.len : NULL;
        r.final.len = fin.len - before.len;
    } else {
        die("终值日志变短，无法划分本次结果");
    }
    r.calls = g_calls - calls;
    r.executed = g_store->stat_executed - executed;
    r.failed = g_store->stat_failed - failed;
    r.capped = g_store->stat_capped - capped;
    r.quota_wait = g_store->stat_quota_wait - quota_wait;
    r.idle = ctl_idle();
    g_context.final_len = r.final.len;

    if (r.ticks >= CTL_TICKS) fprintf(stdout, "[未收敛] 撞 tick 上限 %u\n", CTL_TICKS);
    if (g_trace != NULL) {
        fprintf(g_trace, "=== summary ===\nfinal %u B, failed %u, ticks %u, calls %u\n",
                r.final.len, r.failed, r.ticks, r.calls);
        fflush(g_trace);
    }
#ifdef CTL_TASK_FINISHED
    CTL_TASK_FINISHED();
#endif
    return r;
}

static void ctl_print_final(cxf_str fin) {
    fwrite(fin.p != NULL ? fin.p : "", 1u, fin.len, stdout);
    if (fin.len == 0u || fin.p[fin.len - 1u] != '\n') fputc('\n', stdout);
    fflush(stdout);
}

static bool ctl_trace_set(const char *path) {
    size_t n=strlen(path);char copy[AGENT_POLICY_PATH_CAP];
    if(!n||n>=sizeof g_trace_path||n>=sizeof copy)return false;
    AgentPolicy updated=g_policy;
    if(!agent_policy_protect(&updated,path))return false;
    memcpy(copy,path,n+1);char *slash=strrchr(copy,'/');const char *parent=".",*leaf=copy;
    if(slash){leaf=slash+1;if(slash==copy)parent="/";else{*slash=0;parent=copy;}}
    AgentPolicyPath at;
    if(agent_policy_parent(&g_policy,parent,leaf,(uint32_t)strlen(leaf),&at)!=CXF_OK)return false;
    bool created=false;
    int fd=openat(at.parent,at.leaf,O_WRONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0&&errno==ENOENT){fd=openat(at.parent,at.leaf,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);created=fd>=0;}
    if(fd<0){close(at.parent);return false;}
    struct stat st;bool valid=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&agent_policy_stat(&g_policy,&st);
    FILE *f=NULL;
    if(valid&&agent_policy_protect(&updated,path))f=fdopen(fd,"wb");
    if(!f){close(fd);if(created)unlinkat(at.parent,at.leaf,0);close(at.parent);return false;}
    if(ftruncate(fd,0)){fclose(f);if(created)unlinkat(at.parent,at.leaf,0);close(at.parent);return false;}
    close(at.parent);g_policy=updated;
    if(g_trace)fclose(g_trace);g_trace=f;memcpy(g_trace_path,path,n+1);return true;
}

static void ctl_trace_off(void) {
    if (g_trace != NULL) fclose(g_trace);
    g_trace = NULL;
    g_trace_path[0] = '\0';
}

static void ctl_repl_context(void) {
    if (!g_context.valid) {
        fputs("[context] 尚无任务\n[context] end\n", stdout);
        return;
    }

#ifdef CXF_NATIVE_LLAMA
    if (g_native || g_tool_prepare) printf("[context] task #%u | model %s | backend libllama\n", g_context.task, g_context.model);
    else
#endif
    printf("[context] task #%u | model %s | endpoint http://%s:%u%s\n",
           g_context.task, g_context.model, g_context.host,
           (unsigned)g_context.port, g_context.path);
    printf("[context] snapshot budget %u B | rounds %u | tools %u B | final %u B\n",
           g_context.budget, g_context.rounds, g_context.tools_len, g_context.final_len);
    printf("[context] events %u | dropped events %u | dropped prompt %llu B | dropped act %llu B\n",
           g_context.events, g_context.dropped_events,
           (unsigned long long)g_context.dropped_prompt,
           (unsigned long long)g_context.dropped_act);
    printf("[context] window prompt %u/%u B | act %u/%u B | event capacity %u\n",
           g_context.prompt_used, CTL_CONTEXT_PROMPT_CAP,
           g_context.act_used, CTL_CONTEXT_ACT_CAP, CTL_CONTEXT_EVENTS);

    for (uint32_t i = 0u; i < g_context.events; i++) {
        const CtlContextEvent *e = &g_context.event[i];

        printf("[context] call %u | rc %d (%s)\n", e->call, (int)e->rc,
               cxf_result_str((cxf_result)e->rc));
        printf("-- prompt (%u B kept, %u B lost) --\n", e->prompt_len, e->prompt_dropped);
        fwrite(g_context.prompt + e->prompt_off, 1u, e->prompt_len, stdout);
        printf("\n-- act (%u B kept, %u B lost) --\n", e->act_len, e->act_dropped);
        if (e->rc == CXF_OK) ctl_print_actions(stdout, g_context.act + e->act_off, e->act_len);
    }
    fputs("[context] end\n", stdout);
}

static void ctl_repl_help(void) {
    fputs("/help                 显示本帮助\n"
          "/status               查看当前部署与运行配置\n"
          "/stats                查看累计执行统计\n"
          "/tools                查看实际发给模型的工具清单\n"
          "/context              复盘最近一次任务的真实 LLM 上下文与动作\n"
          "/reload               重新读取 JSON 配置\n"
          "/model [NAME]         查询或设置模型名（可能被 JSON 重载覆盖）\n"
          "/budget [N]           查询或设置上下文字节预算；0 = 引擎默认\n"
          "/rounds [N]           查询或设置轮数上限；0 = 引擎默认\n"
          "/conc [N]             查询或设置并发声明\n"
          "/rpm [N]              查询或设置每分钟请求上限\n"
          "/trace [FILE|off]     查询、切换或关闭 trace\n"
          "/quit                 退出会话\n"
          "普通文本 = 提交独立任务；当前不会自动携带上一任务对话。\n", stdout);
}

static void ctl_repl_status(const CtlRoots *roots, uint32_t tasks) {
    printf("root     %s\n", roots->read);
    printf("test     %s (timeout %u ms)\n", roots->test_command ? roots->test_command : "off",
           roots->test_timeout_ms);
#ifdef CXF_NATIVE_LLAMA
    if (g_native || g_tool_prepare) printf("backend  libllama (Qwen3), model %s\n", g_native_model);
    else
#endif
    printf("endpoint http://%s:%u%s\n", g_llm.host, (unsigned)g_llm.port, g_llm.path);
    printf("model    %s\n", g_llm.model);
    printf("local    %s (calls %u/2)\n", g_local_enabled ? g_local.model : "off", g_local_calls);
    printf("test env %s\n", roots->test_env_mode ? "clean" : "inherit");
    printf("test isolation %s; net %u; read %u write %u\n",g_test_sandbox.mode?"off":"on",g_test_sandbox.net,g_test_sandbox.reads,g_test_sandbox.writes);
#ifdef CXF_NATIVE_LLAMA
    if (!g_native && !g_tool_prepare) {
#endif
    printf("timeout  %u ms\n", g_llm.timeout_ms);
    printf("api key  %s\n", g_llm.api_key[0] != '\0' ? "已设置" : "未设置");
    printf("config   %s\n", g_config_path);
#ifdef CXF_NATIVE_LLAMA
    }
#endif
    printf("budget   %u B\n", g_store->args_budget);
    printf("rounds   %u\n", g_store->max_rounds);
    printf("quota    conc %u, rpm %u%s\n", g_store->quota.conc_max, g_store->quota.rpm,
           g_store->quota.conc_max == 0u && g_store->quota.rpm == 0u ? "（不限）" : "");
    printf("data     model-scope %u, net-scope %u, cloud-egress %u\n",g_llm.scope,g_net_scope,g_policy.cloud_egress);
    printf("trace    %s\n", g_trace != NULL ? g_trace_path : "off");
    printf("trust    %s\n", g_trust == CAP_TRUST_WRITE ? "WRITE" : "IO");
    printf("tasks    %u\n", tasks);
}

static bool ctl_parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v;

    if (s == NULL || s[0] == '\0' || s[0] == '-') return false;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static void ctl_quota_warning(void) {
    if (g_store->quota.conc_max != 0u && g_store->quota.rpm == 0u)
        fputs("agentctl: 警告：只设 conc 且 rpm=0 时闸门不生效；请同时设置 /rpm。\n", stderr);
}

static void ctl_repl_stats(void) {
    fputs("[stats] 以下为当前进程的累计值\n", stderr);
    fprintf(stderr,
            "[stats] run %ld ticks %u rounds_max %u | in %lluB out %lluB copy %lluB lost %lluB\n"
            "[stats] injected %u executed %u failed %u capped %u retried %u resumed %u"
            " | args_lost %u input_lost %u fanout_kids %u\n"
            "[stats] llm calls %u exec_calls %u | cost %u (按实际调用) | denied %u resolve %u max_tries %u stalled %u\n"
            "[stats] tools %uB | blob live %llu peak %llu | gates conc %u rpm %u quota_wait %u\n",
            (long)getpid(), g_total_ticks, g_store->max_rounds,
            (unsigned long long)g_store->bytes_in, (unsigned long long)g_store->bytes_out,
            (unsigned long long)g_store->bytes_copy, (unsigned long long)g_store->bytes_lost,
            g_store->stat_injected, g_store->stat_executed, g_store->stat_failed,
            g_store->stat_capped, g_store->stat_retried, g_store->stat_resumed,
            g_store->stat_args_lost, g_store->stat_input_lost, g_store->stat_fanout_kids,
            g_calls, g_store->stat_exec_calls, g_store->stat_cost, g_store->stat_denied,
            g_store->stat_resolve, g_store->stat_max_tries, g_store->stat_stalled,
            g_tools_len, (unsigned long long)g_store->blob_bytes,
            (unsigned long long)g_store->blob_peak,
            g_store->quota.conc_max, g_store->quota.rpm, g_store->stat_quota_wait);
    print_seg_stats(g_store);
}

/* 命令只作用于任务边界。line 可写；返回值为是否退出。 */
static bool ctl_repl_command(char *line, const CtlRoots *roots, uint32_t tasks) {
    char *cmd = line + 1;
    char *arg = cmd;
    uint32_t v;

    while (*arg != '\0' && *arg != ' ' && *arg != '\t') arg++;
    if (*arg != '\0') *arg++ = '\0';
    while (*arg == ' ' || *arg == '\t') arg++;
    {
        size_t n = strlen(arg);
        while (n > 0u && (arg[n - 1u] == ' ' || arg[n - 1u] == '\t')) arg[--n] = '\0';
    }
    if (*arg == '\0') arg = NULL;

#define CTL_NOARG() do { if (arg != NULL) { \
        fprintf(stderr, "agentctl: /%s 不接受参数\n", cmd); return false; } } while (0)

    if (!strcmp(cmd, "quit")) {
        CTL_NOARG();
        return true;
    } else if (!strcmp(cmd, "help")) {
        CTL_NOARG();
        ctl_repl_help();
    } else if (!strcmp(cmd, "status")) {
        CTL_NOARG();
        ctl_repl_status(roots, tasks);
    } else if (!strcmp(cmd, "stats")) {
        CTL_NOARG();
        ctl_repl_stats();
    } else if (!strcmp(cmd, "tools")) {
        CTL_NOARG();
        fwrite(g_tools, 1u, g_tools_len, stdout);
        fputc('\n', stdout);
    } else if (!strcmp(cmd, "context")) {
        CTL_NOARG();
        ctl_repl_context();
    } else if (!strcmp(cmd, "reload")) {
        CTL_NOARG();
#ifdef CXF_NATIVE_LLAMA
        if (g_native || g_tool_prepare) { fputs("原生模型参数在启动时固定，请重启以更换。\n", stderr); return false; }
#endif
        (void)ctl_config_reload(false, true);
    } else if (!strcmp(cmd, "tool-output-policy")) {
#ifdef CXF_NATIVE_LLAMA
        if(!arg||!g_tool_prepare||!ctl_idle())return false;
#ifdef CTL_OUTPUT_POLICY_OVERRIDE
        arg=CTL_OUTPUT_POLICY_OVERRIDE(arg);
#endif
        int policy=!strcmp(arg,"old")?1:!strcmp(arg,"thread")?2:!strcmp(arg,"token")?3:!strcmp(arg,"progress")?4:!strcmp(arg,"on-event")?5:!strcmp(arg,"progress-event")?6:!strcmp(arg,"off-event")?7:!strcmp(arg,"progress-race")?8:!strcmp(arg,"off-race")?9:!strcmp(arg,"idle-race")?10:!strcmp(arg,"thread-race")?11:!strcmp(arg,"token-race")?12:!strcmp(arg,"lazy-race")?13:!strcmp(arg,"off-drain")?14:!strcmp(arg,"progress-drain")?15:!strcmp(arg,"off-split")?16:!strcmp(arg,"progress-split")?17:!strcmp(arg,"off-coalesce")?18:!strcmp(arg,"progress-coalesce")?19:!strcmp(arg,"thread-coalesce")?20:!strcmp(arg,"token-coalesce")?21:!strcmp(arg,"progress-coarse")?22:0;
        if(agent_tool_output_policy(g_tool_prepare,policy)!=CXF_OK)die("output policy rejected");
#endif
    } else if (!strcmp(cmd, "tool-output-prefill")) {
#ifdef CXF_NATIVE_LLAMA
        if(!g_tool_output_profile||!g_tool_prepare||!ctl_idle()||!arg||(strcmp(arg,"on")&&strcmp(arg,"off"))){fputs("请在空闲Native流式通道中使用 /tool-output-prefill on|off\n",stderr);return false;}
        g_tool_output_enabled=!strcmp(arg,"on");printf("tool-output-prefill %s\n",arg);
#else
        fputs("该选项需要Native工具输出通道\n",stderr);
#endif
    } else if (!strcmp(cmd, "model")) {
#ifdef CXF_NATIVE_LLAMA
        if (g_native || g_tool_prepare) {
            if (arg) fputs("原生模型参数在启动时固定，请重启以更换。\n", stderr);
            else printf("model %s\n", g_native_model);
            return false;
        }
#endif
        if (arg == NULL) {
            printf("model %s\n", g_llm.model);
        } else {
            size_t n = strlen(arg);
            if (n >= sizeof(g_llm.model) || strpbrk(arg, " \t") != NULL) {
                fputs("agentctl: 模型名不能含空白或超过 63 B\n", stderr);
                return false;
            }
            memcpy(g_llm.model, arg, n + 1u);
            printf("model = %s（下一任务生效）\n", g_llm.model);
        }
    } else if (!strcmp(cmd, "budget") || !strcmp(cmd, "rounds") ||
               !strcmp(cmd, "conc") || !strcmp(cmd, "rpm")) {
        if (arg != NULL && !ctl_parse_u32(arg, &v)) {
            fprintf(stderr, "agentctl: /%s 需要一个 0..%u 的十进制整数\n", cmd, UINT32_MAX);
            return false;
        }
        if (!strcmp(cmd, "budget")) {
            if (arg != NULL) agent_args_budget_set(g_store, v);
            printf("budget = %u B%s\n", g_store->args_budget, arg != NULL ? "（下一任务生效）" : "");
        } else if (!strcmp(cmd, "rounds")) {
            if (arg != NULL) agent_set_max_rounds(g_store, v);
            printf("rounds = %u%s\n", g_store->max_rounds, arg != NULL ? "（下一任务生效）" : "");
        } else {
            uint32_t conc = g_store->quota.conc_max;
            uint32_t rpm = g_store->quota.rpm;

            if (arg != NULL) {
                if (!strcmp(cmd, "conc")) conc = v;
                else rpm = v;
                agent_quota_set(g_store, conc, rpm);
            }
            printf("quota = conc %u, rpm %u%s\n", conc, rpm,
                   conc == 0u && rpm == 0u ? "（不限）" : "");
            ctl_quota_warning();
        }
    } else if (!strcmp(cmd, "trace")) {
        if (arg == NULL) {
            printf("trace %s\n", g_trace != NULL ? g_trace_path : "off");
        } else if (!strcmp(arg, "off")) {
            ctl_trace_off();
            fputs("trace = off\n", stdout);
        } else if (!ctl_trace_set(arg)) {
            fprintf(stderr, "agentctl: 开不了 trace 文件 '%s'；原 trace 保持不变\n", arg);
        } else {
            printf("trace = %s\n", g_trace_path);
        }
    } else {
        fprintf(stderr, "agentctl: 未知命令 /%s；输入 /help 查看命令\n", cmd);
    }
#undef CTL_NOARG
    fflush(stdout);  /* 管道/前端没有 tty 提示符，也必须立即可见。 */
    return false;
}

static void ctl_repl(const CtlRoots *roots) {
    char q[AGENT_INPUT_MAX + 8u];
    uint32_t tasks = 0u;
    bool interactive = isatty(STDIN_FILENO) != 0;
    bool blocked = false;

    fputs("cxf agent 会话；/help 查看命令，/quit 退出。\n", stdout);
    for (;;) {
        size_t qlen;

        if (interactive) {
            fputs("cxf> ", stdout);
            fflush(stdout);
        }
        if (fgets(q, (int)sizeof(q), stdin) == NULL) {
            if (interactive) fputc('\n', stdout);
            return;
        }
        if (strchr(q, '\n') == NULL && !feof(stdin)) {
            int ch;
            do { ch = fgetc(stdin); } while (ch != '\n' && ch != EOF);
            fputs("agentctl: 输入超过 AGENT_INPUT_MAX；本行未提交\n", stderr);
            continue;
        }
        qlen = strlen(q);
        while (qlen > 0u && (q[qlen - 1u] == '\n' || q[qlen - 1u] == '\r')) q[--qlen] = '\0';
        if (qlen == 0u) continue;
        if (q[0] == '/') {
            if (ctl_repl_command(q, roots, tasks)) return;
            continue;
        }
        if (blocked) {
            fputs("agentctl: 上一任务未收敛且仍有活条目；请 /quit 后重开，不能继续叠加任务\n", stderr);
            continue;
        }
        if (qlen > AGENT_INPUT_MAX) {
            fputs("agentctl: 问题超过 AGENT_INPUT_MAX，入口会拒帧\n", stderr);
            continue;
        }
        /* 文件是持续真源：每次提交前重读；坏配置不进入引擎，也不覆盖旧值。 */
        if (!ctl_config_reload(false, false)) {
            fputs("agentctl: 本行未提交；修复 JSON 后重试，或用 /reload 查看结果\n", stderr);
            continue;
        }

        tasks++;
        {
            CtlRun r = ctl_run(q, (uint32_t)qlen);
            ctl_print_final(r.final);
            fprintf(stderr,
                    "[task] #%u ticks %u final %uB | llm calls %u executed %u failed %u capped %u"
                    "（/stats 查看累计值）\n",
                    tasks, r.ticks, r.final.len, r.calls, r.executed, r.failed, r.capped);
            /* 与一次性模式相同：只有 last_rc 非 OK 才能报失败，不能只看 HTTP 状态。 */
            if (g_llm.last_rc != CXF_OK) {
                fprintf(stderr, "[llm] 最后一次调用没成：transport_status %u | rc %d (%s)\n",
                        (unsigned)g_llm.last_status, (int)g_llm.last_rc,
                        cxf_result_str((cxf_result)g_llm.last_rc));
                if (g_raw_head[0] != '\0')
                    fprintf(stderr, "      ↳ 响应正文前 120B：%s\n", g_raw_head);
            }
            blocked = !r.idle;
        }
    }
}

static uint32_t ctl_scope(const char *s) {
    if(!s)return AGENT_SCOPE_UNKNOWN;
    if(!strcmp(s,"local"))return AGENT_SCOPE_LOCAL;
    if(!strcmp(s,"cloud"))return AGENT_SCOPE_CLOUD;
    die("scope requires local/cloud");return 0;
}
static uint32_t ctl_egress(const char *s) {
    if(!s||!strcmp(s,"deny"))return AGENT_EGRESS_DENY;
    if(!strcmp(s,"task"))return AGENT_EGRESS_TASK;
    if(!strcmp(s,"workspace"))return AGENT_EGRESS_WORKSPACE;
    die("cloud egress requires deny/task/workspace");return 0;
}
int main(int argc, char **argv) {
    const char *trace = NULL;
    const char *config = "agentctl.json";
    CtlRoots roots = { .test_timeout_ms = 30000u, .test_env_mode = 1 };
    uint32_t budget = 0u, rounds = CTL_ROUNDS_DEFAULT;
    uint32_t conc = 0u, rpm = 0u;
    char q[AGENT_INPUT_MAX + 8u];
    size_t qlen;
    uint32_t ticks = 0u;
    cxf_str fin;
    bool repl = false;
    bool config_explicit = false;
#ifdef CXF_NATIVE_LLAMA
    bool native_options = false;
#endif

    g_model_scope=ctl_scope(getenv("CXF_MODEL_SCOPE"));g_net_scope=ctl_scope(getenv("CXF_NET_SCOPE"));
    g_policy.cloud_egress=ctl_egress(getenv("CXF_CLOUD_EGRESS"));
    for (int i = 1; i < argc; i++) {
#ifdef CXF_NATIVE_LLAMA
        if (!strcmp(argv[i], "--tool-snapshot-release") && i+1<argc) {
            const char *v=argv[++i];native_options=true;g_tool_prepare_enabled=true;
            if(!strcmp(v,"early"))g_prepare_options.release_snapshot_early=true;
            else if(!strcmp(v,"return"))g_prepare_options.release_snapshot_early=false;
            else die("--tool-snapshot-release requires early/return");
            continue;
        }
        if(!strcmp(argv[i],"--tool-output-prefill")&&i+1<argc){const char *v=argv[++i];if(strcmp(v,"off")&&strcmp(v,"on"))die("--tool-output-prefill requires off/on");g_tool_output_profile=true;g_tool_output_enabled=!strcmp(v,"on");g_tool_prepare_enabled=true;native_options=true;continue;}
        if ((!strcmp(argv[i], "--tool-prepare") || !strcmp(argv[i], "--tool-kv")) && i+1<argc) {
            bool policy=!strcmp(argv[i], "--tool-prepare");const char *v=argv[++i];
            native_options=true;g_tool_prepare_enabled=true;
            if(policy) {
                if(!strcmp(v,"late"))g_prepare_options.policy=TOOL_PREPARE_LATE;
                else if(!strcmp(v,"immediate"))g_prepare_options.policy=TOOL_PREPARE_IMMEDIATE;
                else if(!strcmp(v,"timed"))g_prepare_options.policy=TOOL_PREPARE_TIMED;
                else if(!strcmp(v,"progress"))g_prepare_options.policy=TOOL_PREPARE_PROGRESS;
                else die("--tool-prepare requires late/immediate/timed/progress");
            } else {
                if(!strcmp(v,"resident"))g_prepare_options.kv_mode=TOOL_KV_RESIDENT;
                else if(!strcmp(v,"recompute"))g_prepare_options.kv_mode=TOOL_KV_RECOMPUTE;
                else if(!strcmp(v,"restore"))g_prepare_options.kv_mode=TOOL_KV_RESTORE;
                else die("--tool-kv requires resident/recompute/restore");
            }
            continue;
        }
        if (!strcmp(argv[i], "--llama-model") && i + 1 < argc) { g_native_model = argv[++i]; native_options = true; continue; }
        if (!strcmp(argv[i], "--llama-backend") && i + 1 < argc) { g_native_backend = argv[++i]; native_options = true; continue; }
        if ((!strcmp(argv[i], "--llama-context") || !strcmp(argv[i], "--llama-max-tokens") ||
             !strcmp(argv[i], "--llama-gpu-layers") || !strcmp(argv[i], "--llama-batch") || !strcmp(argv[i], "--llama-threads")) && i + 1 < argc) {
            const char *option = argv[i]; uint32_t v;
            native_options = true;
            if (!ctl_parse_u32(argv[++i], &v) || v > INT32_MAX) die("原生参数需要非负整数");
            if (!strcmp(option, "--llama-context")) g_native_context = v;
            else if (!strcmp(option, "--llama-batch")) g_native_batch = v;
            else if (!strcmp(option, "--llama-max-tokens")) g_native_max = v;
            else if (!strcmp(option, "--llama-gpu-layers")) g_native_gpu = (int32_t)v;
            else g_native_threads = (int32_t)v;
            continue;
        }
#endif
        if (!strcmp(argv[i],"--model-scope")&&i+1<argc) g_model_scope=ctl_scope(argv[++i]);
        else if(!strcmp(argv[i],"--net-scope")&&i+1<argc)g_net_scope=ctl_scope(argv[++i]);
        else if(!strcmp(argv[i],"--cloud-egress")&&i+1<argc)g_policy.cloud_egress=ctl_egress(argv[++i]);
        else if (!strcmp(argv[i], "--repl"))                            repl = true;
        else if (!strcmp(argv[i], "--root") && i + 1 < argc)       roots.read   = argv[++i];
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc)       roots.diff   = argv[++i];
        else if (!strcmp(argv[i], "--create") && i + 1 < argc)     roots.create = argv[++i];
        else if (!strcmp(argv[i], "--write") && i + 1 < argc)      roots.write  = argv[++i];
        else if (!strcmp(argv[i], "--append") && i + 1 < argc)     roots.append = argv[++i];
        else if (!strcmp(argv[i], "--net") && i + 1 < argc)        roots.net    = argv[++i];
        else if (!strcmp(argv[i], "--local-config") && i + 1 < argc) g_local_path = argv[++i];
        else if(!strcmp(argv[i],"--test-isolation")&&i+1<argc) {
            const char *v=argv[++i];if(!strcmp(v,"on"))g_test_sandbox.mode=TEST_SANDBOX_ON;
            else if(!strcmp(v,"off"))g_test_sandbox.mode=TEST_SANDBOX_OFF;else die("--test-isolation requires on/off");
        } else if(!strcmp(argv[i],"--test-net")&&i+1<argc) {
            const char *v=argv[++i];if(!strcmp(v,"deny"))g_test_sandbox.net=TEST_NET_DENY;
            else if(!strcmp(v,"allow"))g_test_sandbox.net=TEST_NET_ALLOW;else die("--test-net requires deny/allow");
        } else if(!strcmp(argv[i],"--test-toolchain")&&i+1<argc) {
            g_test_sandbox.toolchain_bin=argv[++i];
        } else if(!strcmp(argv[i],"--test-sdk")&&i+1<argc) {
            g_test_sandbox.sdkroot=argv[++i];
        } else if((!strcmp(argv[i],"--test-read")||!strcmp(argv[i],"--test-write"))&&i+1<argc) {
            bool write=!strcmp(argv[i],"--test-write");uint32_t *count=write?&g_test_sandbox.writes:&g_test_sandbox.reads;
            if(*count>=TEST_SANDBOX_GRANTS)die("test directory grant limit exceeded");
            if(write)g_test_sandbox.write[(*count)++]=argv[++i];else g_test_sandbox.read[(*count)++]=argv[++i];
        }
        else if (!strcmp(argv[i], "--test-env") && i + 1 < argc) {
            const char *mode = argv[++i];
            if (!strcmp(mode, "clean")) roots.test_env_mode = 1;
            else if (!strcmp(mode, "inherit")) roots.test_env_mode = 0;
            else die("--test-env requires clean/inherit");
        }
        else if (!strcmp(argv[i], "--test-command") && i + 1 < argc) roots.test_command = argv[++i];
        else if (!strcmp(argv[i], "--test-timeout-ms") && i + 1 < argc) {
            if (!ctl_parse_u32(argv[++i], &roots.test_timeout_ms) || !roots.test_timeout_ms)
                die("--test-timeout-ms requires a positive integer");
        }
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc)    budget = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)    rounds = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--conc") && i + 1 < argc)      conc   = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--rpm") && i + 1 < argc)       rpm    = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc)     trace  = argv[++i];
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            config = argv[++i]; config_explicit = true;
        } else {
            fprintf(stderr,
                    "用法: agentctl [--repl] --root DIR [--config FILE=agentctl.json]"
                    " [--diff DIR=只看] [--create DIR=只建]"
                    " [--write DIR=能改(含 diff)] [--append DIR=能追] [--net HOST:PORT=能发]"
                    " [--test-command COMMAND] [--test-timeout-ms N=30000] [--test-env clean|inherit] [--test-isolation on|off] [--test-read DIR] [--test-write DIR] [--test-net deny|allow] [--test-toolchain BIN_DIR --test-sdk SDK_DIR]"
                    " [--local-config FILE] [--model-scope local|cloud] [--net-scope local|cloud] [--cloud-egress deny|task|workspace]"
                    " [--budget B=不限] [--rounds N=%u]"
                    " [--conc N=不限] [--rpm N=不限] [--trace FILE] < 问题\n"
#ifdef CXF_NATIVE_LLAMA
                    "原生: --llama-model FILE [--llama-backend DIR] [--llama-context N=4096]"
                    " [--llama-batch N=512] [--llama-max-tokens N=512] [--llama-gpu-layers N=99] [--llama-threads N=4]\n"
                    "工具准备: --tool-prepare late|immediate|timed|progress [--tool-kv resident|recompute|restore] [--tool-snapshot-release early|return]（需 --test-command）\n"
                    "工具输出: --tool-output-prefill off|on（Native/resident；同一种正文优先收据，默认不启用）\n"
#endif
                    ,
                    CTL_ROUNDS_DEFAULT);
            return 2;
        }
    }
#ifdef CXF_NATIVE_LLAMA
    if (g_tool_prepare_enabled && !roots.test_command) die("--tool-prepare requires --test-command");
    if(g_tool_output_profile&&g_prepare_options.kv_mode!=TOOL_KV_RESIDENT)die("--tool-output-prefill requires resident KV");
    if (native_options && !g_native_model) die("原生参数必须与 --llama-model 一起使用");
#endif
    if (roots.read == NULL) {
        fprintf(stderr, "agentctl: 缺 --root（这个进程能看哪片盘是部署事实，不给默认值）\n");
        return 2;
    }
    if((g_test_sandbox.toolchain_bin!=NULL)!=(g_test_sandbox.sdkroot!=NULL))
        die("--test-toolchain and --test-sdk must be provided together");

    if(roots.test_command&&g_test_sandbox.mode==TEST_SANDBOX_ON && agent_sandbox_host_protect()!=CXF_OK)
        die("test.run host environment protection unavailable; isolation refused");
    const char *credential_env[]={"CXF_LLM_KEY","CXF_NET_AUTH","OPENAI_API_KEY","ANTHROPIC_API_KEY","DEEPSEEK_API_KEY",NULL};
    for(unsigned k=0;credential_env[k];k++) {
        const char *v=getenv(credential_env[k]);
        if(v&&!agent_policy_secret(&g_policy,v))die("credential policy capacity exceeded");
    }
    ctl_config_init(config, config_explicit);
    if(!agent_policy_protect(&g_policy,config)||!agent_policy_secret(&g_policy,g_llm.api_key))die("model data policy registration failed");
    g_llm.policy=&g_policy;g_llm.scope=g_model_scope;
#ifdef CXF_NATIVE_LLAMA
    if(g_native_model)g_llm.scope=AGENT_SCOPE_LOCAL;
#endif
    ctl_local_init();
    if (trace != NULL && !ctl_trace_set(trace)) die("开不了 trace 文件");
    if (repl) {
        ctl_setup(&roots, budget, rounds, conc, rpm);
        ctl_repl(&roots);
        ctl_trace_off();
        cxf_world_stop(&g_world);
        cxf_world_release(&g_world);
        return 0;
    }

    /* 非 REPL 模式保持原契约：stdin 的一行就是一次任务，执行后退出。 */
    if (fgets(q, (int)sizeof(q), stdin) == NULL) die("stdin 没有输入");
    qlen = strlen(q);
    while (qlen > 0u && (q[qlen - 1u] == '\n' || q[qlen - 1u] == '\r')) q[--qlen] = '\0';
    if (qlen == 0u) die("空问题");
    if (qlen > AGENT_INPUT_MAX) die("问题超过 AGENT_INPUT_MAX，入口会拒帧");

    ctl_setup(&roots, budget, rounds, conc, rpm);
    {
        CtlRun r = ctl_run(q, (uint32_t)qlen);
        ticks = r.ticks;
        fin = r.final;
    }
    ctl_print_final(fin);

    fprintf(stderr,
            "[stats] run %ld ticks %u rounds_max %u | in %lluB out %lluB copy %lluB lost %lluB\n"
            "[stats] injected %u executed %u failed %u capped %u retried %u resumed %u"
            " | args_lost %u input_lost %u fanout_kids %u\n"
            "[stats] llm calls %u exec_calls %u | cost %u (按实际调用) | denied %u resolve %u max_tries %u stalled %u\n"
            "[stats] tools %uB | blob live %llu peak %llu | gates conc %u rpm %u quota_wait %u\n",
            (long)getpid(), ticks, g_store->max_rounds,
            (unsigned long long)g_store->bytes_in, (unsigned long long)g_store->bytes_out,
            (unsigned long long)g_store->bytes_copy, (unsigned long long)g_store->bytes_lost,
            g_store->stat_injected, g_store->stat_executed, g_store->stat_failed,
            g_store->stat_capped, g_store->stat_retried, g_store->stat_resumed,
            g_store->stat_args_lost, g_store->stat_input_lost,
            g_store->stat_fanout_kids,
            g_calls, g_store->stat_exec_calls, g_store->stat_cost,
            g_store->stat_denied,
            g_store->stat_resolve, g_store->stat_max_tries, g_store->stat_stalled,
            g_tools_len,
            (unsigned long long)g_store->blob_bytes, (unsigned long long)g_store->blob_peak,
            g_store->quota.conc_max, g_store->quota.rpm, g_store->stat_quota_wait);

    /*
     * 出网失败的原因（§4.18.17 的**前置**读数）。
     *
     * 为什么它排在最前面、而且必须单独一行：`[stats] failed` 只说明"有东西没成"，
     * 而云端开测时失败的三种可能（凭据错 / 被限速 / 对端抖动）在
     * `agent_llm` 的错误码里是**同一个 FORMAT** —— 因为协议层把非 2xx 全归成
     * 格式错（那是刻意的，协议层不做重试策略）。
     *
     * 归成一个码是对的，把事实也抹掉是错的：三种情况的下一步动作完全相反。
     * 所以这里把协议层记下的 (status, rc) 原样印出来，并把映射写成一句话 ——
     * 判据可以粗糙，方向不能错。没跑这件事的代价是：一次云端开测 15 次全红，
     * 而人以为是"模型不肯分片"。
     */
    /*
     * 判据只有 `last_rc != CXF_OK` 一条 —— 不要写成 `|| last_status != 0`。
     *
     * 第一版正是后者，于是**成功调用也进了这个分支**，印出
     * `[llm] 最后一次调用没成：http 200 | rc 0 (ok)` —— 一行自相矛盾的诊断。
     * 这比不印更坏：它把一个成功的运行标成失败，而人会觉得"200 都算错，
     * 那是不是提示词有问题"。**诊断的第一要务是不许把成功说成失败。**
     * status 只在"已经判定没成"之后用来分档，它不是判据。
     */
    if (g_llm.last_rc != CXF_OK) {
        uint16_t st = g_llm.last_status;
        const char *why;

        /*
         * 判据是 **(status, rc) 二元组**，不是 status 一个。
         *
         * 第一版只看 status，于是在真实端点上连错三次（2026-10-23 一次实测里全撞上）：
         *
         *   (0,    FULL)  印成"查 CXF_LLM_URL 形状"  —— 真因是请求体超 16KB
         *   (200,  RANGE) 印成"真·格式问题"          —— 真因是模型给了 9 段、超 FANOUT_MAX 被拒
         *   (200,  OK)    印成"没成"                  —— 那条压根是成功的调用
         *
         * 三次都不是打字错误，是**同一个判断错误**：把"HTTP 那层怎么说"当成了
         * "这一层为什么没成"。`rc` 才是我们自己的结论，status 只是对端的态度。
         */
#ifdef CXF_NATIVE_LLAMA
        if (g_native || g_tool_prepare) {
            why = "原生推理失败；检查上下文／生成预算、模型模板和本地引擎诊断。未自动切换 HTTP。";
        } else
#endif
        if (g_llm.last_rc == CXF_ERR_RANGE && st >= 200u && st < 300u) {
            /* 2xx + RANGE：对端答得好好的，是我们自己的上限把它拒了 */
            why = "**段数超过 AGENT_FANOUT_MAX（8）**：模型给的 tool_calls 比上限多，"
                  "被响亮拒绝（不截断）。这不是故障，是上限 —— 要看它的形状就先抬上限";
        } else if (g_llm.last_rc == CXF_ERR_FULL && st == 0u) {
            /* 0 + FULL：请求体还没发出去就放不下了 */
            why = "**请求体超过 AGENT_LLM_REQ_CAP（16KB）**：上下文太长（多半是上一轮的工具结果"
                  "很大）。调小 --budget 或收紧 --root 的搜索面，不要改 URL";
        } else if (st == 0u) {
            why = (g_llm.last_rc == CXF_ERR_IO)
                  ? "没走到 HTTP（连接失败/超时）：查 host:port 与 CXF_LLM_TIMEOUT_MS"
                  : "请求构造失败但也不是 FULL：查 CXF_LLM_URL 形状（只收 http://）";
        } else if (st == 400u) {
            why = "请求体被拒：多半是 model 名不认识，或 tools 的 schema 不被这个端点接受";
        } else if (st == 401u || st == 403u) {
            why = "**凭据**：改 CXF_LLM_KEY（这一条别去改提示词）";
        } else if (st == 404u) {
            /*
             * ★ 这一条在真实端点上被证伪过一次，改法值得记住。
             *
             * 第一版写的是"路径不对"。实测（2026-10-23，121 那台 new-api）：
             * 同一台、同一个 URL，五个模型里四个成功、`deepseek-v4-flash` 回 404
             * ⇒ **路径是对的**。new-api 对"该 token 分组里没有这个模型名"也回 404，
             * 而且这在真实端点上比路径错**常见得多**。
             * 反过来，路径真的少写 `/v1` 时它回的是 **200 + 前端 HTML**（也实测过）——
             * 那个根本不会走到这一条。
             */
            why = "**模型名/分组**（最常见）：这个 token 的分组里没有该模型，"
                  "或名字拼错。路径错的典型表现是 200+HTML，不会走到 404";
        } else if (st == 429u) {
            why = "**对端限速**：等一会儿再发。这一层不做重试是有意的（见 agent_llm.h 边界）";
        } else if (st >= 500u) {
            why = "对端故障：同一请求逐字节相同，可以直接重发";
        } else if (st >= 200u && st < 300u) {
            if (g_llm.last_rc == CXF_ERR_FULL) {
                why = "2xx 但放不下：调大 scratch（AGENT_LLM_RAW_CAP 只是建议值）";
            } else if (g_raw_head[0] == '<') {
                /* 正文以 `<` 开头 = 打到了前端页面，不是 API。这条**不是**模型的错。 */
                why = "**打到了前端页面**（正文以 `<` 开头）：URL 少写 `/v1` 或路径拼错，"
                      "不是模型问题";
            } else {
                why = "**真·格式问题**：200 且正文不是前端页，却解不出 choices/message";
            }
        } else {
            why = "其它非 2xx：先看对端的访问日志";
        }

        fprintf(stderr, "[llm] 最后一次调用没成：transport_status %u | rc %d (%s)\n      ↳ %s\n",
                (unsigned)st, (int)g_llm.last_rc,
                cxf_result_str((cxf_result)g_llm.last_rc), why);

        /*
         * 正文头部：唯一能区分"HTML"与"JSON error"的证据。
         * 没有它，上面那句 `why` 仍然只是猜测（本轮已经猜错过两次：
         * 把 200 说成失败、把 404 说成路径错）。
         */
        if (g_raw_head[0] != '\0')
            fprintf(stderr, "      ↳ 响应正文前 120B：%s\n", g_raw_head);
    }

    /*
     * 段化收益的读数（§4.18.16 八）。**分子分母一起印**。
     *
     * 只印 `E[k]`（失败平均落在第几段）会把"批次大小分布"混进结论：
     * 一个总是只派 2 条的模型与一个总是派 4 条却总在第 2 段失败的模型，
     * 单看 E[k] 都是 2，而前者根本无段可省。
     *
     * 所以两个**原始分布**都印，把"该不该得结论"留给读的人；
     * 由它们派生的 `N-k` 只在单批时印（理由见函数末尾那段）。
     */
    print_seg_stats(g_store);

    /*
     * 分段耗时（§4.17）。这一行是"回路到底在等什么"的唯一硬证据：
     * 四个 phase 里只有 EXECUTE 会出网，于是 `exec` 的占比直接等于
     * "墙钟里有多少是纯等待"。同步 RPC 的形状下它必然接近 100% ——
     * 这个数不是为了好看，是为了给"要不要做多路 I/O"一个可比较的基线。
     */
    fprintf(stderr,
            "[time] wall %.2fs | in %.0fms match %.0fms exec %.0fms recycle %.0fms"
            " | wait %.1f%%\n",
            (double)g_wall_ns / 1e9,
            (double)g_ph_ns[0] / 1e6, (double)g_ph_ns[1] / 1e6,
            (double)g_ph_ns[2] / 1e6, (double)g_ph_ns[3] / 1e6,
            g_wall_ns > 0u ? 100.0 * (double)g_ph_ns[2] / (double)g_wall_ns : 0.0);

    /*
     * 「轮数用尽」必须与「工具坏了」分开报（§4.16.3）。
     *
     * 两者都是 `failed`，但处置相反：一个是**证据已经到手、只是没收口**，
     * 另一个是**这条路根本没走通**。合成一行数字的时候，读者只能猜。
     * 实测两种模型都会把轮数用光，所以这行诊断不是防御性代码，是常态输出。
     */
    if (g_store->stat_capped > 0u) {
        uint32_t tools_done = (g_store->stat_executed > g_calls)
                              ? g_store->stat_executed - g_calls : 0u;

        fprintf(stderr,
                "[未收敛] 轮数用尽 ×%u：模型在 %u 轮内没有给出终值。"
                "已拿到工具结果 %u 份（模型被问 %u 次）——"
                "证据在上下文里，缺的是收口。下一步可调大 --rounds。\n",
                g_store->stat_capped, g_store->max_rounds, tools_done, g_calls);
    }

    /*
     * 「被闸门挡住」与「跑得慢」也要分开报。`quota_wait` 非零意味着 tick 在
     * **空转等令牌** —— 忙循环里等，等于把 CPU 烧在睡眠上。它不是失败，也不
     * 影响正确性，但它是"这个 tick 该有一个最早唤醒时刻"的直接证据（A5）。
     */
    if (g_store->stat_quota_wait > 0u) {
        fprintf(stderr,
                "[闸门] quota_wait ×%u：瞬时并发 %u / RPM %u 把 EXECUTE 挡回去了 %u 次"
                "（不是失败，是排队）。tick 是忙循环，这段在空转 —— "
                "调大 --conc/--rpm，或者等 A5 给 tick 一个最早唤醒时刻。\n",
                g_store->stat_quota_wait, g_store->quota.conc_max, g_store->quota.rpm,
                g_store->stat_quota_wait);
    }

    ctl_trace_off();
    cxf_world_stop(&g_world);
    cxf_world_release(&g_world);
    return (fin.len > 0u) ? 0 : 1;
}
