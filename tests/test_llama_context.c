#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cxf/llama.h"
#include "../src/llama_internal.h"
#include "cxf/world.h"
#include "agent.h"
#include "agent_action.h"
#include "agent_caps.h"
#include "agent_caps.gen.h"
#include "agent_types.gen.h"
#include "llama.h"

#define OK(expr) do { cxf_result rc_ = (expr); if (rc_ != CXF_OK) { \
    fprintf(stderr, "FAIL %s:%d %s -> %d\n", __FILE__, __LINE__, #expr, rc_); abort(); } } while (0)

typedef struct FaultAlloc { int remaining; } FaultAlloc;
static void *fault_alloc(void *ctx, size_t n, size_t align) {
    FaultAlloc *f = ctx;
    assert(align <= 16);
    if (!f->remaining) return NULL;
    if (f->remaining > 0) f->remaining--;
    return malloc(n);
}
static void fault_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }

static cxf_llama_view view(cxf_llama *e, cxf_llama_session h) {
    cxf_llama_view v; OK(cxf_llama_session_view(e, h, &v)); return v;
}
static cxf_llama_stats stats(cxf_llama *e) {
    cxf_llama_stats s; cxf_llama_get_stats(e, &s); return s;
}
static void append(cxf_llama *e, cxf_llama_session h, const int32_t *t, uint32_t n) {
    OK(cxf_llama_append(e, h, view(e, h).revision, t, n));
}
static void finish(cxf_llama *e, cxf_llama_session h) {
    uint32_t n;
    OK(cxf_llama_prefill(e, h, view(e, h).revision, UINT32_MAX, true, &n));
    cxf_llama_view v = view(e, h); assert(v.committed == v.evaluated);
}
static int32_t greedy(cxf_llama *e, cxf_llama_session h) {
    int32_t t; OK(cxf_llama_greedy(e, h, view(e, h).revision, &t)); return t;
}
static int32_t fresh(cxf_llama *e, const int32_t *t, uint32_t n) {
    cxf_llama_session h; OK(cxf_llama_session_new(e, &h));
    append(e, h, t, n); finish(e, h); int32_t result = greedy(e, h);
    OK(cxf_llama_session_close(e, h)); return result;
}

static void test_sessions(cxf_llama *e, const int32_t *t, uint32_t count, FaultAlloc *fault) {
    assert(count > 768);
    cxf_llama_session a, b;
    OK(cxf_llama_session_new(e, &a));
    assert(view(e, a).revision == 1);
    cxf_llama_view old = view(e, a);
    append(e, a, t, 512);
    uint32_t done;
    assert(cxf_llama_prefill(e, a, old.revision, 512, true, &done) == CXF_ERR_BAD_HANDLE);
    OK(cxf_llama_prefill(e, a, view(e, a).revision, 0, true, &done));
    assert(!done && view(e, a).evaluated == 0);
    OK(cxf_llama_prefill(e, a, view(e, a).revision, 64, false, &done));
    assert(!done && stats(e).deferred_prefills > 0);
    OK(cxf_llama_prefill(e, a, view(e, a).revision, 192, false, &done));
    assert(done == 192 && view(e, a).evaluated == 192);
    int32_t sample;
    assert(cxf_llama_greedy(e, a, view(e, a).revision, &sample) == CXF_ERR_EMPTY);
    finish(e, a);

    cxf_llama_stats before = stats(e);
    OK(cxf_llama_fork(e, a, view(e, a).revision, 333, &b));
    cxf_llama_stats after = stats(e);
    assert(after.token_payload_capacity == before.token_payload_capacity);
    assert(after.stored_tokens == before.stored_tokens);
    assert(after.logical_tokens == before.logical_tokens + 333);
    assert(view(e, b).evaluated == 333);
    append(e, b, t + 333, 200);
    assert(stats(e).stored_tokens == before.stored_tokens + 200);
    int32_t copied[768];
    OK(cxf_llama_read(e, b, 0, copied, 533));
    assert(!memcmp(copied, t, 533 * sizeof *t));
    before = stats(e);
    finish(e, b);
    assert(stats(e).evaluated_tokens - before.evaluated_tokens == 200);
    int32_t b_sample = greedy(e, b);
    assert(b_sample == fresh(e, t, 533));
    /* The reference changed the context-wide logits. Switching back must
     * replay only this session's last token, never consume another's logits. */
    before = stats(e);
    assert(greedy(e, b) == b_sample);
    assert(stats(e).logits_replay_tokens == before.logits_replay_tokens + 1);

    cxf_llama_view preserved = view(e, b);
    before = stats(e);
    int32_t invalid = -1;
    assert(cxf_llama_append(e, b, preserved.revision, &invalid, 1) == CXF_ERR_RANGE);
    assert(cxf_llama_replace(e, b, preserved.revision, 534, t, 1) == CXF_ERR_RANGE);
    assert(cxf_llama_append(e, b, preserved.revision, t, UINT32_MAX) == CXF_ERR_RANGE);
    fault->remaining = 0;
    assert(cxf_llama_append(e, b, preserved.revision, t, 257) == CXF_ERR_NOMEM);
    fault->remaining = -1;
    fault->remaining = 1;
    assert(cxf_llama_append(e, b, preserved.revision, t, 512) == CXF_ERR_NOMEM);
    fault->remaining = -1;
    assert(view(e, b).revision == preserved.revision);
    assert(view(e, b).committed == preserved.committed);
    assert(view(e, b).evaluated == preserved.evaluated);
    assert(stats(e).token_payload_capacity == before.token_payload_capacity);
    assert(greedy(e, b) == b_sample);

    /* Replace inside a partially shared block, keeping the parent unmodified. */
    int32_t changed[533]; memcpy(changed, t, sizeof changed);
    changed[330] = t[0];
    if (changed[330] == t[330]) changed[330] = t[1];
    OK(cxf_llama_replace(e, b, view(e, b).revision, 330, changed + 330, 203));
    assert(view(e, b).evaluated == 330);
    finish(e, b);
    assert(greedy(e, b) == fresh(e, changed, 533));
    OK(cxf_llama_read(e, a, 0, copied, 512));
    assert(!memcmp(copied, t, 512 * sizeof *t));
    /* Child owns no parent's lifetime. */
    OK(cxf_llama_session_close(e, a));
    assert(cxf_llama_session_view(e, a, &old) == CXF_ERR_BAD_HANDLE);
    assert(cxf_llama_session_close(e, a) == CXF_ERR_BAD_HANDLE);
    assert(greedy(e, b) == fresh(e, changed, 533));

    /* Late fork can inherit committed tokens not yet evaluated. */
    append(e, b, t + 533, 100);
    cxf_llama_session late;
    OK(cxf_llama_fork(e, b, view(e, b).revision, 600, &late));
    assert(view(e, late).committed == 600 && view(e, late).evaluated == 533);
    OK(cxf_llama_session_close(e, b));
    finish(e, late);
    OK(cxf_llama_session_close(e, late));
    assert(stats(e).sessions == 0 && stats(e).stored_tokens == 0 && stats(e).token_payload_capacity == 0);

    /* Empty snapshots, explicit flush, slot generations and identity. */
    OK(cxf_llama_session_new(e, &a));
    cxf_llama_session forged = a; forged.owner++;
    assert(cxf_llama_session_view(e, forged, &old) == CXF_ERR_BAD_HANDLE);
    assert(cxf_llama_greedy(e, a, view(e, a).revision, &sample) == CXF_ERR_EMPTY);
    append(e, a, t, 20);
    OK(cxf_llama_prefill(e, a, view(e, a).revision, 20, false, &done)); assert(!done);
    OK(cxf_llama_prefill(e, a, view(e, a).revision, 7, true, &done)); assert(done == 7);
    finish(e, a);
    OK(cxf_llama_replace(e, a, view(e, a).revision, 0, NULL, 0));
    assert(!view(e, a).committed && !view(e, a).evaluated && stats(e).stored_tokens == 0);
    OK(cxf_llama_session_close(e, a));
    OK(cxf_llama_session_new(e, &b));
    assert(cxf_llama_append(e, a, 1, t, 1) == CXF_ERR_BAD_HANDLE);
    OK(cxf_llama_session_close(e, b));

    /* Streaming generation amortizes storage; an older branch must not see
     * newly appended tokens even when it shares the physical tail block. */
    OK(cxf_llama_session_new(e, &a)); append(e, a, t, 20);
    OK(cxf_llama_fork(e, a, view(e, a).revision, 20, &b));
    for (uint32_t i = 20; i < 220; i++) append(e, b, t + i, 1);
    assert(stats(e).chunks == 1 && stats(e).stored_tokens == 220);
    assert(view(e, a).committed == 20);
    assert(cxf_llama_read(e, a, 20, copied, 1) == CXF_ERR_RANGE);
    append(e, a, t + 300, 10);
    OK(cxf_llama_read(e, a, 0, copied, 30));
    assert(!memcmp(copied, t, 20 * sizeof *t));
    assert(!memcmp(copied + 20, t + 300, 10 * sizeof *t));
    OK(cxf_llama_read(e, b, 0, copied, 220));
    assert(!memcmp(copied, t, 220 * sizeof *t));
    finish(e, b); assert(greedy(e, b) == fresh(e, t, 220));
    OK(cxf_llama_session_close(e, a)); OK(cxf_llama_session_close(e, b));

    for (int i = 0; i < 50; i++) {
        OK(cxf_llama_session_new(e, &a)); append(e, a, t, 260);
        OK(cxf_llama_fork(e, a, view(e, a).revision, 257, &b));
        append(e, b, t, 10);
        OK(cxf_llama_session_close(e, a)); OK(cxf_llama_session_close(e, b));
    }
    assert(!stats(e).stored_tokens && !stats(e).chunks && !stats(e).token_payload_capacity);
    puts("PASS sessions: shared storage, explicit revisions, budgets, branching, rollback, OOM, lifetime, logits ownership");
}

typedef struct NativeCtx { cxf_llama *engine; cxf_llama_session session; uint32_t calls; } NativeCtx;
static cxf_result native_step(const void *ctx, const char *args, uint32_t n,
                              char *out, uint32_t cap, uint32_t *out_len) {
    NativeCtx *c = (NativeCtx *)ctx;
    int32_t tokens[2048], chosen;
    char text[128]; uint32_t count, bytes, processed;
    *out_len = 0;
    if (cap < 256) return CXF_ERR_RANGE; /* never re-run inference for output growth */
    cxf_result rc = cxf_llama_tokenize(c->engine, args, n, false, tokens, 2048, &count);
    if (rc != CXF_OK) return rc;
    cxf_llama_view v = view(c->engine, c->session);
    rc = cxf_llama_append(c->engine, c->session, v.revision, tokens, count);
    if (rc != CXF_OK) return rc;
    v = view(c->engine, c->session);
    rc = cxf_llama_prefill(c->engine, c->session, v.revision, UINT32_MAX, true, &processed);
    if (rc != CXF_OK) return rc;
    rc = cxf_llama_greedy(c->engine, c->session, v.revision, &chosen);
    if (rc != CXF_OK) return rc;
    rc = cxf_llama_piece(c->engine, chosen, text, sizeof text, &bytes);
    if (rc != CXF_OK) return rc;
    *out_len = agent_act_encode(AGENT_ACT_DONE, NULL, 0, text, bytes, out, cap);
    c->calls++;
    return *out_len ? CXF_OK : CXF_ERR_FORMAT;
}

static void test_world(cxf_llama *e) {
    const char question[] = "The capital of France is";
    int32_t toks[128]; uint32_t nt;
    OK(cxf_llama_tokenize(e, question, sizeof question - 1, false, toks, 128, &nt));
    int32_t expected = fresh(e, toks, nt);
    char piece[128]; uint32_t plen;
    OK(cxf_llama_piece(e, expected, piece, sizeof piece, &plen));
    NativeCtx ctx = { .engine = e };
    OK(cxf_llama_session_new(e, &ctx.session));
    CapImpl impl = { CAPNAME_STEP, "native", CAP_TIER_LLM, 90, CAP_TRUST_IO,
                     native_step, &ctx, 256, NULL };
    cxf_world w;
    OK(cxf_world_init(&w, cxf_alloc_heap(), 1u << 16));
    OK(cxf_type_register_bundle(&w, &cxf_types_agent_types));
    OK(cxf_service_register_bundle(&w, &cxf_services_agent_caps));
    OK(cxf_service_register(&w, "iface/agent.action", 1, &iface_action));
    OK(cxf_service_register(&w, "cap/agent.step", 1, &capdef_step));
    OK(cxf_service_register(&w, "impl/agent.step.native", 1, &impl));
    uint32_t ids[5];
    const cxf_module_def *mods[] = { &agent_store_def, &agent_inject_def, &agent_match_def,
                                    &agent_execute_def, &agent_recycle_def };
    for (int i = 0; i < 5; i++) OK(cxf_module_load(&w, mods[i], &ids[i]));
    OK(cxf_world_start(&w));
    AgentStore *s = cxf_module_state(&w, ids[0]); assert(s);
    InjectMsg msg = { .kind = AGENT_PLAN_LOOP, .text_len = sizeof question - 1, .trust_max = CAP_TRUST_IO };
    assert(cxf_ring_write(&s->inbox, &msg, sizeof msg) == sizeof msg);
    assert(cxf_ring_write(&s->inbox, question, sizeof question - 1) == sizeof question - 1);
    for (int i = 0; i < 20 && !s->stat_final; i++) cxf_world_tick(&w);
    cxf_str final = agent_final(s);
    assert(s->stat_final == 1 && s->stat_failed == 0 && ctx.calls == 1);
    assert(final.len == plen + 1 && !memcmp(final.p, piece, plen) && final.p[plen] == '\n');
    assert(view(e, ctx.session).committed == nt);
    assert(agent_audit(s) == 0);
    cxf_world_stop(&w); cxf_world_release(&w);
    OK(cxf_llama_session_close(e, ctx.session));
    puts("PASS integration: native CapImpl executed by cxf WorkItem and delivered its real model result");
}

static void test_exhaustion(cxf_llama_params p, const int32_t *t) {
    p.context_tokens = 512; p.max_sessions = 2; p.batch_tokens = 128; p.prefill_min_tokens = 64;
    cxf_llama *e; OK(cxf_llama_open(&p, &e));
    cxf_llama_session a, b, full;
    OK(cxf_llama_session_new(e, &a)); OK(cxf_llama_session_new(e, &b));
    assert(cxf_llama_session_new(e, &full) == CXF_ERR_FULL);
    append(e, a, t, 512); finish(e, a);
    int32_t original = greedy(e, a);
    append(e, b, t, 128); uint32_t done = 999;
    assert(cxf_llama_prefill(e, b, view(e, b).revision, 128, true, &done) == CXF_ERR_IO);
    assert(!done && !view(e, b).evaluated && view(e, b).committed == 128);
    assert(stats(e).decode_failures == 1 && stats(e).last_native_error != 0);
    assert(greedy(e, a) == original);
    OK(cxf_llama_session_close(e, a)); finish(e, b);
    assert(greedy(e, b) == fresh(e, t, 128));
    OK(cxf_llama_session_close(e, b));
    /* A failed native file restore must not corrupt a different live sequence.
     * Exercise real KV-capacity failure, then restore after freeing capacity. */
    char directory[] = "/tmp/cxf-kv-native-XXXXXX";
    assert(mkdtemp(directory));
    char path[128]; (void)snprintf(path, sizeof path, "%s/image.kv", directory);
    OK(cxf_llama_session_new(e, &a)); append(e, a, t, 128); finish(e, a);
    int32_t expected = greedy(e, a); cxf_llama_kv_image image;
    assert(cxf_llama_kv_save_file(e, a, view(e,a).revision, path, 1, &image) == CXF_ERR_FULL);
    assert(view(e,a).evaluated == 128);
    OK(cxf_llama_kv_save_file(e, a, view(e,a).revision, path, 64u*1024u*1024u, &image));
    OK(cxf_llama_kv_evict(e, a, view(e,a).revision));
    OK(cxf_llama_session_new(e, &b)); append(e, b, t, 512); finish(e, b);
    original = greedy(e,b);
    assert(cxf_llama_kv_restore_file(e, &image, path) == CXF_ERR_IO);
    assert(!view(e,a).evaluated && view(e,a).committed == 128);
    assert(view(e,b).evaluated == 512 && greedy(e,b) == original);
    OK(cxf_llama_session_close(e,b));
    OK(cxf_llama_kv_restore_file(e,&image,path));
    cxf_llama_stats before = stats(e);
    assert(greedy(e,a) == expected && stats(e).logits_replay_tokens == before.logits_replay_tokens+1);
    assert(stats(e).evaluated_tokens == before.evaluated_tokens+1);
    OK(cxf_llama_kv_evict(e,a,view(e,a).revision));
    cxf_llama_kv_image foreign = image; foreign.session.owner++;
    assert(cxf_llama_kv_restore_file(e,&foreign,path) == CXF_ERR_BAD_HANDLE);
    append(e,a,t,1);
    assert(cxf_llama_kv_restore_file(e,&image,path) == CXF_ERR_BAD_HANDLE);
    OK(cxf_llama_session_close(e,a)); assert(!unlink(path) && !rmdir(directory));
    cxf_llama_close(e);
    puts("PASS exhaustion: native KV failure preserved tokens and explicit retry recovered after capacity was freed");
    puts("PASS KV files: capacity failure preserved other sequence, explicit restore recovered, only final logits token replayed, stale/foreign images refused");
}

/* Independent ground truth: no cxf token blocks, cursors or copy_range here. */
static int32_t raw_reference(const char *path, const char *backends, const int32_t *tokens, int32_t n) {
    ggml_backend_load_all_from_path(backends); llama_backend_init();
    struct llama_model_params mp = llama_model_default_params(); mp.n_gpu_layers = 99;
    struct llama_model *model = llama_model_load_from_file(path, mp); assert(model);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 1024; cp.n_batch = 512; cp.n_ubatch = 512;
    cp.n_threads = 4; cp.n_threads_batch = 4;
    struct llama_context *ctx = llama_init_from_model(model, cp); assert(ctx);
    struct llama_batch batch = llama_batch_init(n, 0, 1); batch.n_tokens = n;
    for (int32_t i = 0; i < n; i++) {
        batch.token[i] = tokens[i]; batch.pos[i] = i;
        batch.n_seq_id[i] = 1; batch.seq_id[i][0] = 0; batch.logits[i] = i == n - 1;
    }
    assert(llama_decode(ctx, batch) == 0); llama_synchronize(ctx);
    float *logits = llama_get_logits_ith(ctx, -1);
    int32_t best = 0, size = llama_vocab_n_tokens(llama_model_get_vocab(model));
    for (int32_t i = 1; i < size; i++) if (logits[i] > logits[best]) best = i;
    llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free();
    return best;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s model.gguf backend-dir\n", argv[0]); return 2; }
    FaultAlloc fault = { -1 };
    cxf_alloc allocator = { fault_alloc, fault_free, &fault, {0} };
    cxf_llama_params p = cxf_llama_default_params();
    p.model_path = argv[1]; p.backend_path = argv[2]; p.alloc = &allocator;
    cxf_llama *e;
    OK(cxf_llama_open(&p, &e));
    char text[8000]; size_t len = 0;
    for (int i = 0; i < 90; i++) len += (size_t)snprintf(text + len, sizeof text - len,
        "Item %04d: stable context for local inference and cache reuse.\n", i);
    uint32_t count; int32_t tokens[8000];
    OK(cxf_llama_tokenize(e, text, (uint32_t)len, false, tokens, 8000, &count));
    uint32_t required;
    assert(cxf_llama_tokenize(e, text, (uint32_t)len, false, NULL, 0, &required) == CXF_ERR_FULL);
    assert(required == count);
    test_sessions(e, tokens, count, &fault);
    test_world(e);
    int32_t native_reference = fresh(e, tokens, 512);
    cxf_llama_session stale; OK(cxf_llama_session_new(e, &stale));
    OK(cxf_llama_session_close(e, stale)); cxf_llama_close(e);
    assert(allocator.stat.bytes_live == 0);
    assert(native_reference == raw_reference(argv[1], argv[2], tokens, 512));
    puts("PASS independent reference: native backend agrees with direct libllama prefill");
    test_exhaustion(p, tokens);
    assert(allocator.stat.bytes_live == 0);
    OK(cxf_llama_open(&p, &e));
    cxf_llama_view v;
    assert(cxf_llama_session_view(e, stale, &v) == CXF_ERR_BAD_HANDLE);
    cxf_llama_close(e);
    assert(allocator.stat.bytes_live == 0);
    puts("all native llama context tests passed");
    return 0;
}
