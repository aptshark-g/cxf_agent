/* Direct libllama experiment; no cxf runtime policy changes.
 * Compile against the EXACT headers shipped with the selected library build.
 * Fixtures deliberately end on stable text/token boundaries and verify that
 * incremental tokenization equals full tokenization before measuring anything.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "llama.h"

typedef struct Fixture {
    char *text;
    llama_token *tokens;
    int *chars, *ends;
    int steps, total, bytes;
} Fixture;

typedef struct Engine {
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    struct llama_batch batch;
    int vocab_n;
} Engine;

static double clock_ms(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static void until_ms(double target) {
    for (;;) {
        double left = target - clock_ms();
        if (left <= 0) return;
        struct timespec t = { (time_t)(left / 1000), (long)(fmod(left, 1000) * 1e6) };
        nanosleep(&t, NULL);
    }
}

static uint64_t resident_bytes(void) {
#ifdef __APPLE__
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return (uint64_t)info.resident_size;
#endif
    return 0; /* Do not confuse peak RSS with current resident memory. */
}

static void silent_log(enum ggml_log_level level, const char *text, void *unused) {
    (void)unused;
    if (level >= GGML_LOG_LEVEL_ERROR || strstr(text, "offloaded") ||
        strstr(text, "Metal") || strstr(text, "KV buffer")) fputs(text, stderr);
}

static int tokenize(Engine *e, const char *text, int bytes, llama_token *out, int cap) {
    int n = llama_tokenize(e->vocab, text, bytes, out, cap, false, true);
    assert(n >= 0 && n <= cap);
    return n;
}

static Fixture fixture(Engine *e, int lines, int delta_lines, int steps) {
    Fixture f = {0};
    f.steps = steps;
    int count = lines + delta_lines * steps;
    int capacity = count * 120 + 1;
    f.text = malloc((size_t)capacity);
    f.tokens = malloc((size_t)capacity * sizeof *f.tokens);
    f.chars = calloc((size_t)steps + 1, sizeof *f.chars);
    f.ends = calloc((size_t)steps + 1, sizeof *f.ends);
    assert(f.text && f.tokens && f.chars && f.ends);
    for (int i = 0; i < count; i++) {
        int n = snprintf(f.text + f.bytes, (size_t)(capacity - f.bytes),
                         "Item %04d: stable context for local inference and cache reuse.\n", i);
        assert(n > 0 && f.bytes + n < capacity);
        f.bytes += n;
        if (i + 1 >= lines && (i + 1 - lines) % delta_lines == 0)
            f.chars[(i + 1 - lines) / delta_lines] = f.bytes;
    }
    f.total = tokenize(e, f.text, f.bytes, f.tokens, capacity);
    llama_token *tmp = malloc((size_t)capacity * sizeof *tmp);
    assert(tmp);
    for (int s = 0; s <= steps; s++) {
        f.ends[s] = tokenize(e, f.text, f.chars[s], tmp, capacity);
        assert(memcmp(tmp, f.tokens, (size_t)f.ends[s] * sizeof *tmp) == 0);
        if (s) {
            int n = tokenize(e, f.text + f.chars[s-1], f.chars[s] - f.chars[s-1], tmp, capacity);
            assert(n == f.ends[s] - f.ends[s-1]);
            assert(memcmp(tmp, f.tokens + f.ends[s-1], (size_t)n * sizeof *tmp) == 0);
        }
    }
    free(tmp);
    return f;
}

/* Explicit token positions and sequence id; submit only the requested range. */
static void evaluate(Engine *e, const llama_token *tokens, int begin, int end, int seq) {
    for (int pos = begin; pos < end;) {
        int n = end - pos;
        if (n > e->batch.n_tokens) n = e->batch.n_tokens;
        struct llama_batch b = e->batch;
        b.n_tokens = n;
        for (int j = 0; j < n; j++) {
            b.token[j] = tokens[pos + j];
            b.pos[j] = pos + j;
            b.n_seq_id[j] = 1;
            b.seq_id[j][0] = seq;
            b.logits[j] = pos + j == end - 1;
        }
        int rc = llama_decode(e->ctx, b);
        if (rc) { fprintf(stderr, "decode failed rc=%d pos=%d n=%d\n", rc, pos, n); exit(2); }
        pos += n;
    }
    llama_synchronize(e->ctx);
}

static void clear(Engine *e) {
    llama_synchronize(e->ctx);
    /* Invalidate positions, as a cache miss does. Zeroing the whole allocated
     * KV buffer would add unrelated memory-clear work to the cold baseline. */
    llama_memory_clear(llama_get_memory(e->ctx), false);
}

static float *copy_logits(Engine *e) {
    float *p = malloc((size_t)e->vocab_n * sizeof *p);
    assert(p);
    memcpy(p, llama_get_logits_ith(e->ctx, -1), (size_t)e->vocab_n * sizeof *p);
    return p;
}

static double difference(Engine *e, const float *ref, int *same_top1) {
    const float *p = llama_get_logits_ith(e->ctx, -1);
    double max = 0;
    int a = 0, b = 0;
    for (int i = 0; i < e->vocab_n; i++) {
        double d = fabs((double)p[i] - ref[i]);
        assert(isfinite(p[i]) && isfinite(ref[i]));
        if (d > max) max = d;
        if (p[i] > p[a]) a = i;
        if (ref[i] > ref[b]) b = i;
    }
    *same_top1 = a == b;
    return max;
}

static void prefix_trial(Engine *e, Fixture *f, int mode, int rep, float **refs) {
    static const char *names[] = { "cold", "cached_scan", "explicit_delta" };
    int cap = f->bytes + 1;
    llama_token *tmp = malloc((size_t)cap * sizeof *tmp);
    char *wire = malloc((size_t)cap);
    assert(tmp && wire);
    clear(e);
    evaluate(e, f->tokens, 0, f->ends[0], 0); /* common warm prefix, excluded */
    double front = 0, eval = 0, max_diff = 0;
    uint64_t scans = 0, tokenized = 0, evaluated = 0, copied = 0;
    int all_top1 = 1;
    for (int s = 1; s <= f->steps; s++) {
        int old = f->ends[s-1], end = f->ends[s], begin = 0;
        double t = clock_ms();
        if (mode == 2) {
            int bytes = f->chars[s] - f->chars[s-1];
            int n = tokenize(e, f->text + f->chars[s-1], bytes, tmp, cap);
            tokenized += (uint64_t)n;
            begin = old; /* committed frontier supplied by the owner */
        } else {
            /* Full text materialization and tokenizer pass, no HTTP/JSON.
             * Cached-scan therefore excludes server transport overhead. */
            memcpy(wire, f->text, (size_t)f->chars[s]);
            copied += (uint64_t)f->chars[s];
            int n = tokenize(e, wire, f->chars[s], tmp, cap);
            assert(n == end);
            tokenized += (uint64_t)n;
            if (mode == 1) {
                while (begin < old && tmp[begin] == f->tokens[begin]) begin++;
                scans += (uint64_t)begin;
            }
        }
        double t1 = clock_ms();
        front += t1 - t;
        if (mode == 0) clear(e);
        evaluate(e, f->tokens, begin, end, 0);
        double t2 = clock_ms();
        eval += t2 - t1;
        evaluated += (uint64_t)(end - begin);
        /* Correctness checks are outside measured regions. Reference uses
         * exactly the same decode batch shapes as explicit_delta. */
        if (mode == 1 && !refs[s-1]) refs[s-1] = copy_logits(e);
        if (refs[s-1]) {
            int same;
            double d = difference(e, refs[s-1], &same);
            if (d > max_diff) max_diff = d;
            all_top1 &= same;
        }
    }
    if (mode == 2) assert(all_top1 && max_diff < 0.001);
    if (rep >= 0) printf("{\"kind\":\"prefix\",\"mode\":\"%s\",\"rep\":%d,"
        "\"frontend_ms\":%.6f,\"eval_ms\":%.6f,\"wall_ms\":%.6f,"
        "\"evaluated_tokens\":%llu,\"tokenized_tokens\":%llu,\"scanned_tokens\":%llu,"
        "\"materialized_text_bytes\":%llu,\"max_logit_diff\":%.8f,\"same_top1\":%s}\n",
        names[mode], rep, front, eval, front + eval,
        (unsigned long long)evaluated, (unsigned long long)tokenized, (unsigned long long)scans,
        (unsigned long long)copied, max_diff, all_top1 ? "true" : "false");
    free(tmp); free(wire);
    fflush(stdout);
}

static void scan_only(Fixture *f) {
    llama_token *copy = malloc((size_t)f->total * sizeof *copy);
    assert(copy); memcpy(copy, f->tokens, (size_t)f->total * sizeof *copy);
    volatile uint64_t checksum = 0;
    int loops = 20000, n = f->ends[f->steps-1];
    double t = clock_ms();
    for (int i = 0; i < loops; i++) {
        int j = 0;
        while (j < n && copy[j] == f->tokens[j]) j++;
        checksum += (uint64_t)j;
    }
    double elapsed = clock_ms() - t;
    printf("{\"kind\":\"scan_only\",\"tokens\":%d,\"loops\":%d,\"per_scan_us\":%.6f,\"checksum\":%llu}\n",
           n, loops, elapsed * 1000 / loops, (unsigned long long)checksum);
    free(copy);
}

static void stream_trial(Engine *e, Fixture *f, int mode, int gap_ms, int rep, float **ref) {
    int begin = f->ends[0], amount = f->total - begin;
    if (amount > 512) amount = 512;
    int chunk = 32, chunks = (amount + chunk - 1) / chunk;
    clear(e); evaluate(e, f->tokens, 0, begin, 0);
    double t = clock_ms(), source_end = t + gap_ms * chunks;
    double compute = 0;
    if (!mode) {
        until_ms(source_end);
        double a = clock_ms();
        evaluate(e, f->tokens, begin, begin + amount, 0);
        compute += clock_ms() - a;
    } else {
        for (int i = 0; i < chunks;) {
            int next = i + (mode == 2 ? 4 : 1);
            if (next > chunks) next = chunks;
            until_ms(t + next * gap_ms);
            if (mode == 2) {
                int available = gap_ms ? (int)((clock_ms() - t) / gap_ms) : chunks;
                if (available > chunks) available = chunks;
                if (available > next) next = available;
            }
            int end = begin + next * chunk;
            if (end > begin + amount) end = begin + amount;
            double a = clock_ms();
            evaluate(e, f->tokens, begin + i * chunk, end, 0);
            compute += clock_ms() - a;
            i = next;
        }
    }
    double done = clock_ms();
    if (!*ref) *ref = copy_logits(e);
    int same;
    double diff = difference(e, *ref, &same);
    assert(same); /* record numerical drift caused by different batch shapes */
    if (rep >= 0) printf("{\"kind\":\"stream\",\"mode\":\"%s\",\"rep\":%d,"
        "\"gap_ms\":%d,\"chunk_tokens\":%d,\"new_tokens\":%d,\"source_ms\":%d,"
        "\"compute_ms\":%.6f,\"wall_ms\":%.6f,\"post_input_ms\":%.6f,"
        "\"max_logit_diff\":%.8f,\"same_top1\":true}\n",
        mode == 2 ? "coalesced_128" : mode == 1 ? "eager_32" : "wait_all",
        rep, gap_ms, chunk, amount, chunks * gap_ms,
        compute, done - t, fmax(0, done - source_end), diff);
    fflush(stdout);
}

static void rollback_control(Engine *e, Fixture *f) {
    int end = f->ends[1], pivot = f->ends[0];
    llama_token *changed = malloc((size_t)end * sizeof *changed); assert(changed);
    memcpy(changed, f->tokens, (size_t)end * sizeof *changed);
    changed[pivot] = (changed[pivot] + 1) % e->vocab_n;
    clear(e); evaluate(e, f->tokens, 0, end, 0);
    float *original = copy_logits(e);
    assert(llama_memory_seq_rm(llama_get_memory(e->ctx), 0, pivot, -1));
    evaluate(e, changed, pivot, end, 0);
    int original_same;
    double effect = difference(e, original, &original_same);
    /* Mutation must alter actual model output, otherwise equality checks could
     * be accidentally observing a stale logits pointer or a dead benchmark. */
    assert(effect > 0.00001);
    float *after = copy_logits(e);
    clear(e); evaluate(e, changed, 0, pivot, 0); evaluate(e, changed, pivot, end, 0);
    int same; double error = difference(e, after, &same);
    assert(same && error < 0.001);
    printf("{\"kind\":\"rollback_control\",\"changed_at\":%d,\"mutation_logit_effect\":%.8f,"
           "\"rollback_vs_fresh_diff\":%.8f,\"same_top1\":true}\n", pivot, effect, error);
    free(changed); free(original); free(after);
}

static void branch_trial(Engine *e, Fixture *f, int rep) {
    int prefix = f->ends[0], end = f->ends[1];
    clear(e); evaluate(e, f->tokens, 0, prefix, 0);
    double t = clock_ms();
    llama_memory_seq_cp(llama_get_memory(e->ctx), 0, 1, 0, prefix);
    llama_synchronize(e->ctx);
    double fork_ms = clock_ms() - t;
    t = clock_ms();
    evaluate(e, f->tokens, prefix, end, 1);
    double fork_suffix_ms = clock_ms() - t;
    float *ref = copy_logits(e);
    clear(e);
    double start = clock_ms();
    evaluate(e, f->tokens, 0, prefix, 1);
    double recompute_prefix = clock_ms() - start;
    evaluate(e, f->tokens, prefix, end, 1);
    double recompute = clock_ms() - start;
    int same;
    double diff = difference(e, ref, &same);
    assert(same);
    if (rep >= 0) printf("{\"kind\":\"branch\",\"rep\":%d,\"prefix_tokens\":%d,"
        "\"fork_ms\":%.6f,\"fork_then_suffix_ms\":%.6f,\"recompute_prefix_ms\":%.6f,"
        "\"recompute_full_ms\":%.6f,\"max_logit_diff\":%.8f,\"same_top1\":true}\n",
        rep, prefix, fork_ms, fork_ms + fork_suffix_ms, recompute_prefix, recompute, diff);
    free(ref); fflush(stdout);
}

static void state_trial(Engine *e, Fixture *f, int rep) {
    int prefix = f->ends[0], end = f->ends[1];
    clear(e); evaluate(e, f->tokens, 0, end, 0);
    float *ref = copy_logits(e);
    assert(llama_memory_seq_rm(llama_get_memory(e->ctx), 0, prefix, -1));
    size_t bytes = llama_state_seq_get_size(e->ctx, 0);
    uint8_t *state = malloc(bytes); assert(state);
    double t = clock_ms();
    size_t saved = llama_state_seq_get_data(e->ctx, state, bytes, 0);
    llama_synchronize(e->ctx);
    double export_ms = clock_ms() - t;
    assert(saved == bytes);
    uint64_t before = resident_bytes();
    t = clock_ms();
    assert(llama_memory_seq_rm(llama_get_memory(e->ctx), 0, -1, -1));
    llama_synchronize(e->ctx);
    double clear_ms = clock_ms() - t;
    uint64_t after = resident_bytes();
    t = clock_ms();
    size_t restored = llama_state_seq_set_data(e->ctx, state, bytes, 0);
    llama_synchronize(e->ctx);
    double restore_ms = clock_ms() - t;
    assert(restored == bytes);
    evaluate(e, f->tokens, prefix, end, 0);
    int same; double diff = difference(e, ref, &same);
    assert(same && diff < 0.001);
    if (rep >= 0) printf("{\"kind\":\"state\",\"rep\":%d,\"state_bytes\":%zu,"
        "\"export_ms\":%.6f,\"clear_ms\":%.6f,\"restore_ms\":%.6f,"
        "\"resident_before_clear\":%llu,\"resident_after_clear\":%llu,"
        "\"max_logit_diff\":%.8f,\"same_top1\":true}\n",
        rep, bytes, export_ms, clear_ms, restore_ms, (unsigned long long)before,
        (unsigned long long)after, diff);
    free(state); free(ref); fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s model.gguf backend-library-dir [repeats=5] [prefix-lines=128] [stream-gap-ms=20] [stream-only=0]\n", argv[0]);
        return 2;
    }
    int repeats = argc > 3 ? atoi(argv[3]) : 5;
    int prefix_lines = argc > 4 ? atoi(argv[4]) : 128;
    int stream_gap = argc > 5 ? atoi(argv[5]) : 20;
    bool stream_only = argc > 6 && atoi(argv[6]);
    assert(repeats > 0 && repeats <= 30 && prefix_lines >= 8 && prefix_lines <= 512);
    assert(stream_gap >= 0 && stream_gap <= 1000);
    llama_log_set(silent_log, NULL);
    ggml_backend_load_all_from_path(argv[2]);
    llama_backend_init();
    Engine e = {0};
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 99;
    e.model = llama_model_load_from_file(argv[1], mp);
    assert(e.model);
    e.vocab = llama_model_get_vocab(e.model);
    e.vocab_n = llama_vocab_n_tokens(e.vocab);
    Fixture f = fixture(&e, prefix_lines, 8, 6);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)((f.total + 255) / 256 * 256 + 512);
    cp.n_batch = 512; cp.n_ubatch = 512; cp.n_seq_max = 2;
    cp.n_threads = 4; cp.n_threads_batch = 4;
    cp.kv_unified = true;
    e.ctx = llama_init_from_model(e.model, cp); assert(e.ctx);
    e.batch = llama_batch_init(512, 0, 1);
    /* llama_batch_init returns n_tokens == capacity; preserve as our capacity. */
    e.batch.n_tokens = 512;
    printf("{\"kind\":\"metadata\",\"prefix_tokens\":%d,\"final_tokens\":%d,"
        "\"steps\":%d,\"text_bytes\":%d,\"token_bytes\":%zu,\"context_capacity\":%u,"
        "\"resident_after_load\":%llu,\"weights_bytes\":%llu,\"boundary_verified\":true}\n",
        f.ends[0], f.total, f.steps, f.bytes, (size_t)f.total * sizeof(llama_token),
        llama_n_ctx(e.ctx), (unsigned long long)resident_bytes(),
        (unsigned long long)llama_model_size(e.model));
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        printf("{\"kind\":\"device\",\"name\":\"%s\",\"type\":%d}\n",
               ggml_backend_dev_name(dev), (int)ggml_backend_dev_type(dev));
    }
    float **refs = calloc((size_t)f.steps, sizeof *refs); assert(refs);
    /* Warm every scenario before collecting samples; rotate measurement order. */
    if (!stream_only) {
        prefix_trial(&e, &f, 1, -1, refs);
        prefix_trial(&e, &f, 2, -1, refs);
        prefix_trial(&e, &f, 0, -1, refs);
        for (int r = 0; r < repeats; r++)
            for (int k = 0; k < 3; k++) prefix_trial(&e, &f, (r + k) % 3, r, refs);
        scan_only(&f);
    }
    for (int g = stream_only ? 1 : 0; g < 2; g++) {
        int gap = g ? stream_gap : 0;
        float *ref = NULL;
        stream_trial(&e, &f, 0, gap, -1, &ref);
        stream_trial(&e, &f, 1, gap, -1, &ref);
        stream_trial(&e, &f, 2, gap, -1, &ref);
        for (int r = 0; r < repeats; r++)
            for (int k = 0; k < 3; k++) stream_trial(&e, &f, (r + k) % 3, gap, r, &ref);
        free(ref);
    }
    if (!stream_only) {
        rollback_control(&e, &f);
        branch_trial(&e, &f, -1);
        state_trial(&e, &f, -1);
        for (int r = 0; r < repeats; r++) {
            branch_trial(&e, &f, r);
            state_trial(&e, &f, r);
        }
    }
    for (int i = 0; i < f.steps; i++) free(refs[i]);
    free(refs); free(f.text); free(f.tokens); free(f.chars); free(f.ends);
    llama_batch_free(e.batch); llama_free(e.ctx); llama_model_free(e.model); llama_backend_free();
    return 0;
}
