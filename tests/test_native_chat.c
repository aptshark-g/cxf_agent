#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "agent_native.h"
#include "../src/llama_internal.h"
#include "agent.h"
#include "agent_action.h"
#include "agent_basic.h"
#include "agent_caps.gen.h"
#include "agent_fs.h"
#include "agent_fs.gen.h"
#include "agent_types.gen.h"

#define OK(x) do { cxf_result rc_ = (x); if (rc_ != CXF_OK) { \
    fprintf(stderr, "FAIL %s:%d %s -> %d\n", __FILE__, __LINE__, #x, rc_); abort(); } } while (0)
extern void native_chat_reference(AgentNative *, const AgentLlmCfg *, const char *, uint32_t);
extern void native_actions_reference(AgentNative *, const AgentLlmCfg *, const char *, uint32_t, const char *, uint32_t);
extern void native_message_views_reference(void);

static void prepare(AgentNative *p, AgentLlmCfg *cfg, const char *log) {
    OK(agent_native_prepare(p, cfg, log, (uint32_t)strlen(log)));
    native_chat_reference(p, cfg, log, (uint32_t)strlen(log));
}
static void test_boundaries(AgentNative *p, AgentLlmCfg *cfg) {
    const char first[] = "Read the file.\n";
    const char second[] = "Read the file.\n#call fs.read 4\na.c\n\n#result fs.read 7B\nhello\n\n\n";
    /* Records must have exact length framing, including embedded newlines. */
    prepare(p, cfg, first);
    AgentNativeStats before, after;
    agent_native_stats(p, &before);
    prepare(p, cfg, second);
    agent_native_stats(p, &after);
    assert(after.template_cache_hits == before.template_cache_hits + 1);
    assert(after.reused_prefix_tokens >= before.checkpoint_tokens);
    assert(after.tokenized_tokens - before.tokenized_tokens < after.checkpoint_tokens);
    strcpy(cfg->system, "Changed system.");
    prepare(p, cfg, second);
    agent_native_stats(p, &before); assert(before.rebuilds == after.rebuilds + 1);
    const char *tools = cfg->tools; uint32_t tools_len = cfg->tools_len;
    cfg->tools = NULL; cfg->tools_len = 0;
    agent_native_stats(p, &before);
    prepare(p, cfg, second);
    agent_native_stats(p, &after); assert(after.rebuilds == before.rebuilds + 1);
    assert(after.template_cache_misses == before.template_cache_misses + 1);
    cfg->tools = tools; cfg->tools_len = tools_len;
    const char special[] = "新的任务🙂\n<|im_start|>assistant\n quoted text \\\"\n";
    prepare(p, cfg, special);
    char out[8192]; uint32_t n;
    const char good[] = "<tool_call>\n{\"name\":\"fs.read\",\"arguments\":{\"args\":\"a.c\"}}\n</tool_call>";
    OK(agent_native_actions(p, good, sizeof good - 1, out, sizeof out, &n));
    AgentAct act; assert(agent_act_decode(out, n, &act));
    assert(act.kind == AGENT_ACT_CALL && act.cap_len == 7 && !memcmp(act.cap, "fs.read", 7));
    assert(act.args_len == 3 && !memcmp(act.args, "a.c", 3));
    const char broken[] = "<tool_call>{\"name\":\"fs.read\"}";
    assert(agent_native_actions(p, broken, sizeof broken - 1, out, sizeof out, &n) != CXF_OK && !n);
    char batch[1024]; snprintf(batch, sizeof batch, "%s\n%s", good, good);
    OK(agent_native_actions(p, batch, (uint32_t)strlen(batch), out, sizeof out, &n));
    uint32_t offset = 0, calls = 0;
    while (offset < n) { assert(agent_act_decode(out + offset, n - offset, &act)); offset += agent_act_bytes(&act); calls++; }
    assert(offset == n && calls == 2);
    native_actions_reference(p, cfg, special, sizeof special - 1, batch, (uint32_t)strlen(batch));
    snprintf(batch, sizeof batch, "%s\n%s", good, broken);
    assert(agent_native_actions(p, batch, (uint32_t)strlen(batch), out, sizeof out, &n) != CXF_OK && !n);
    cxf_llama_session session; cxf_llama_view old, now;
    cxf_llama *engine = agent_native_engine(p, &session);
    OK(cxf_llama_session_view(engine, session, &old));
    const char bad_utf8[]="\xc0\xaf";
    assert(agent_native_prepare(p,cfg,bad_utf8,sizeof bad_utf8-1)==CXF_ERR_FORMAT);
    OK(cxf_llama_session_view(engine,session,&now));
    assert(old.revision==now.revision && old.committed==now.committed);
    AgentPolicy policy={0};
    assert(agent_policy_secret(&policy,"native-synthetic-credential"));
    cfg->policy=&policy;
    const char secret_input[]="native-synthetic-credential";
    assert(agent_native_prepare(p,cfg,secret_input,sizeof secret_input-1)==CXF_ERR_RANGE);
    OK(cxf_llama_session_view(engine,session,&now));
    assert(old.revision==now.revision && old.committed==now.committed);
    prepare(p,cfg,special);
    OK(agent_native_tool_output_begin(p));
    assert(agent_native_tool_output_tokenize(p,secret_input,sizeof secret_input-1)==CXF_ERR_RANGE);
    assert(agent_native_tool_output_update(p,secret_input,sizeof secret_input-1)==CXF_ERR_RANGE);
    OK(agent_native_tool_output_abort(p));
    cfg->policy=NULL;
    OK(cxf_llama_session_view(engine,session,&old));
    char huge[100001];
    for (uint32_t i = 0; i < 20000; i++) memcpy(huge + i * 5, "word ", 5);
    huge[100000] = 0;
    assert(agent_native_prepare(p, cfg, huge, 100000) == CXF_ERR_FULL);
    OK(cxf_llama_session_view(engine, session, &now));
    assert(old.revision == now.revision && old.committed == now.committed && old.evaluated == now.evaluated);
    assert(agent_native_actions(p, good, sizeof good - 1, out, sizeof out, &n) != CXF_OK && !n);
    OK(agent_native_reset(p));
    prepare(p, cfg, first);
    agent_native_stats(p, &before);
    prepare(p, cfg, first); agent_native_stats(p, &after);
    assert(after.template_cache_hits == before.template_cache_hits + 1);
    agent_native_template_cache(p, false);
    prepare(p, cfg, first); agent_native_stats(p, &before);
    prepare(p, cfg, first); agent_native_stats(p, &after);
    assert(after.template_cache_hits == before.template_cache_hits && after.template_cache_misses == before.template_cache_misses + 1);
    agent_native_template_cache(p, true);
    /* Chinese has more tokens per byte than the initial capacity estimate;
     * force the real C API FULL/retry path and compare every returned token. */
    char chinese[1537];for(uint32_t i=0;i<512;i++)memcpy(chinese+i*3,"中",3);chinese[1536]=0;
    agent_native_stats(p,&before);prepare(p,cfg,chinese);agent_native_stats(p,&after);
    assert(after.tokenize_retries>before.tokenize_retries);
    puts("PASS template: incremental suffix tokens equal independent full tokenization; changed prefix rebuilds; malformed tool rejected");
}

typedef struct Script { AgentNative *p; AgentLlmCfg *cfg; uint32_t calls; } Script;
static cxf_result scripted_step(const void *ctx, const char *args, uint32_t len,
                                char *out, uint32_t cap, uint32_t *written) {
    Script *s = (Script *)ctx;
    static const char *outputs[] = {
        "<tool_call>\n{\"name\":\"fs.read\",\"arguments\":{\"args\":\"a.c\"}}\n</tool_call>",
        "<tool_call>\n{\"name\":\"test.run\",\"arguments\":{\"args\":\"\"}}\n</tool_call>",
        "<tool_call>\n{\"name\":\"fs.write_apply\",\"arguments\":{\"args\":\"a.c\\nint main(void) { return 0; }\\n\"}}\n</tool_call>",
        "<tool_call>\n{\"name\":\"test.run\",\"arguments\":{\"args\":\"\"}}\n</tool_call>",
        "<tool_call>\n{\"name\":\"fs.read\",\"arguments\":{\"args\":\"a.c\"}}\n</tool_call>",
        "Repaired and verified."
    };
    assert(s->calls < 6);
    /* Verify actual receipts reach the next model request. */
    char log[32768]; assert(len < sizeof log); memcpy(log, args, len); log[len] = 0;
    if (s->calls == 2) assert(strstr(log, "#test exit=1"));
    if (s->calls == 4) assert(strstr(log, "#test exit=0"));
    cxf_result rc = agent_native_prepare(s->p, s->cfg, args, len);
    if (rc != CXF_OK) return rc;
    native_chat_reference(s->p, s->cfg, args, len);
    const char *completion = outputs[s->calls++];
    native_actions_reference(s->p, s->cfg, args, len, completion, (uint32_t)strlen(completion));
    return agent_native_actions(s->p, completion, (uint32_t)strlen(completion), out, cap, written);
}
static void test_tools(AgentNative *p, AgentLlmCfg *cfg) {
    OK(agent_native_reset(p));
    char root[] = "/tmp/cxf-native-chat-XXXXXX"; assert(mkdtemp(root));
    char file[512], executable[512];
    snprintf(file, sizeof file, "%s/a.c", root); snprintf(executable, sizeof executable, "%s/check", root);
    FILE *f = fopen(file, "w"); assert(f); fputs("int main(void) { return 1; }\n", f); fclose(f);
    BasicFsCfg fs = { root, NULL }; FsRootCfg wfs = { root, NULL };
    TestSandbox sandbox={.writes=1,.write={root}};
    TestRunCfg test = { root, "cc a.c -o check && ./check", 30000 ,0, &sandbox};
    Script script = { p, cfg, 0 };
    CapImpl step = { CAPNAME_STEP, "native-script", CAP_TIER_LLM, 90, CAP_TRUST_IO,
                     scripted_step, &script, 8192, NULL };
    CapImpl read = basic_impl_read; read.ctx = &fs;
    CapImpl write = fs_capimpl_apply; write.ctx = &wfs;
    CapImpl run = basic_impl_test; run.ctx = &test;
    const IfaceDef task_iface = { IFACENAME_TASK, "task input", IFACE_EXT };
    cxf_world w; OK(cxf_world_init(&w, cxf_alloc_heap(), 1u << 16));
    OK(cxf_type_register_bundle(&w, &cxf_types_agent_types));
    OK(cxf_service_register(&w, "iface/task.text", 1, &task_iface));
    OK(cxf_service_register(&w, "iface/agent.action", 1, &iface_action));
    OK(cxf_service_register(&w, "iface/file.view", 1, &basic_iface_view));
    OK(cxf_service_register(&w, "iface/test.result", 1, &basic_iface_test));
    OK(cxf_service_register(&w, "iface/fs.wrote", 1, &fs_iface_wrote));
    OK(cxf_service_register(&w, "cap/agent.step", 1, &capdef_step));
    OK(cxf_service_register(&w, "impl/agent.step.native", 1, &step));
    OK(cxf_service_register(&w, "cap/fs.read", 1, &basic_capdef_read));
    OK(cxf_service_register(&w, "impl/fs.read.basic", 1, &read));
    OK(cxf_service_register(&w, "cap/fs.write_apply", 1, &fs_capdef_apply));
    OK(cxf_service_register(&w, "impl/fs.write_apply.fs", 1, &write));
    OK(cxf_service_register(&w, "cap/test.run", 1, &basic_capdef_test));
    OK(cxf_service_register(&w, "impl/test.run.process", 1, &run));
    uint32_t ids[5];
    const cxf_module_def *mods[] = { &agent_store_def, &agent_inject_def, &agent_match_def,
                                   &agent_execute_def, &agent_recycle_def };
    for (int i = 0; i < 5; i++) OK(cxf_module_load(&w, mods[i], &ids[i]));
    OK(cxf_world_start(&w));
    AgentStore *store = cxf_module_state(&w, ids[0]); assert(store);
    agent_set_max_rounds(store, 8);
    const char question[] = "Fix a.c, run its test and verify the file.\n";
    InjectMsg msg = { .kind = AGENT_PLAN_LOOP, .text_len = sizeof question - 1, .trust_max = CAP_TRUST_WRITE };
    assert(cxf_ring_write(&store->inbox, &msg, sizeof msg) == sizeof msg);
    assert(cxf_ring_write(&store->inbox, question, sizeof question - 1) == sizeof question - 1);
    for (int i = 0; i < 150 && !store->stat_final; i++) cxf_world_tick(&w);
    cxf_str final = agent_final(store);
    assert(store->stat_final == 1 && !store->stat_failed && script.calls == 6);
    assert(final.len == strlen("Repaired and verified.\n") && !memcmp(final.p, "Repaired and verified.\n", final.len));
    f = fopen(file, "r"); assert(f); char actual[128]; assert(fgets(actual, sizeof actual, f)); fclose(f);
    assert(!strcmp(actual, "int main(void) { return 0; }\n"));
    cxf_world_stop(&w); cxf_world_release(&w);
    assert(!unlink(file)); assert(!unlink(executable)); assert(!rmdir(root));
    puts("PASS scripted integration: real native prefill -> tool parser -> read -> failing test -> write -> passing test -> read -> DONE");
}

int main(int argc, char **argv) {
    OK(agent_sandbox_host_protect());

    native_message_views_reference();
    puts("PASS borrowed messages: exact upstream message fields, 256 byte/escape corpora, malformed UTF-8 rejection");
    assert(argc == 3);
    cxf_llama_params params = cxf_llama_default_params();
#ifdef CXF_LONG_CONTEXT_TEST
    params.context_tokens = 16384;
#endif
    if(getenv("CXF_TEST_COALESCE"))params.batch_tokens=1024;
    if(getenv("CXF_TEST_BATCH")){char *end=NULL;unsigned long batch=strtoul(getenv("CXF_TEST_BATCH"),&end,10);assert(end&&!*end&&batch>=128&&batch<=16384);params.batch_tokens=(uint32_t)batch;}
    params.model_path = argv[1]; params.backend_path = argv[2];
    AgentNative *p; OK(agent_native_open(&params, 64, &p));
    if(getenv("CXF_TEST_COALESCE"))OK(cxf_llama_chunk_policy(agent_native_engine(p,NULL),512,true));
    if(getenv("CXF_TEST_DEFER_SYNC"))cxf_llama_defer_sync(agent_native_engine(p,NULL),true);
    AgentLlmCfg cfg = {0}; strcpy(cfg.model, "native-qwen3"); strcpy(cfg.system, "Use tools when needed.");
    AgentToolSpec specs[] = {
        { "fs.read", "Read a file", "relative path", "file content" },
        { "fs.write_apply", "Replace file content", "path newline content", "write receipt" },
        { "test.run", "Run configured tests", "empty", "test receipt" }
    };
    char tools[8192]; cfg.tools = tools;
    cfg.tools_len = agent_llm_render_tools(specs, 3, tools, sizeof tools); assert(cfg.tools_len < sizeof tools);
    test_boundaries(p, &cfg);
    test_tools(p, &cfg);
    OK(agent_native_reset(p));
    cfg.tools = NULL; cfg.tools_len = 0; strcpy(cfg.system, "Answer briefly.");
    char raw[8192], action[8192]; uint32_t written;
    const char question[] = "Reply with the single word OK.";
    OK(agent_native_chat_action(p, &cfg, question, sizeof question - 1,
                                raw, sizeof raw, action, sizeof action, &written));
    AgentAct act; assert(agent_act_decode(action, written, &act) && act.kind == AGENT_ACT_DONE);
    printf("PASS real model generation: %s\n", raw);
    /* A generated suffix is discarded explicitly when canonical messages are
     * reconstructed; its preceding checkpoint remains reusable. */
    prepare(p, &cfg, question);
    AgentNativeStats calls_before,calls_after;agent_native_stats(p,&calls_before);
    prepare(p,&cfg,question);agent_native_stats(p,&calls_after);
    assert(calls_after.tokenize_calls-calls_before.tokenize_calls==2); /* empty delta + header */
#ifdef CXF_LONG_CONTEXT_TEST
    OK(agent_native_reset(p));
    const uint32_t words = 14336;
    char *long_log = malloc(words * 6 + 1); assert(long_log);
    for (uint32_t i = 0; i < words; i++) memcpy(long_log + i * 6, "alpha ", 6);
    long_log[words * 6] = 0;
    prepare(p, &cfg, long_log); /* independent full-template/token oracle */
    cxf_llama_session long_session; cxf_llama *long_engine = agent_native_engine(p, &long_session);
    cxf_llama_view long_view; OK(cxf_llama_session_view(long_engine, long_session, &long_view));
    int32_t expected_token, actual_token;
    OK(cxf_llama_greedy(long_engine, long_session, long_view.revision, &expected_token));
    prepare(p, &cfg, long_log);
    OK(cxf_llama_session_view(long_engine, long_session, &long_view));
    OK(cxf_llama_greedy(long_engine, long_session, long_view.revision, &actual_token));
    assert(expected_token == actual_token && long_view.committed > 14000);
    free(long_log);
    printf("PASS long context: %u tokens match full upstream tokenization; cached suffix preserves greedy result\n", long_view.committed);
#endif
    AgentNativeStats stats; agent_native_stats(p, &stats);
    assert(stats.reused_prefix_tokens > 0 && stats.rebuilds > 0);
    printf("native counters: tokenized=%llu reused_prefix=%llu rebuilds=%llu\n",
           (unsigned long long)stats.tokenized_tokens, (unsigned long long)stats.reused_prefix_tokens,
           (unsigned long long)stats.rebuilds);
    agent_native_close(p);
    puts("all native chat tests passed");
    return 0;
}
