#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "cxf/llama.h"
#include "cxf/blob.h"
#include "cxf/table.h"
#include "llama.h"
#include "llama_internal.h"

#define TOKEN_BLOCK 256u
#define CLASS_TOKENS 201u
#define CLASS_CHUNKS 202u
#define CLASS_SESSIONS 203u

typedef struct Chunk {
    cxf_handle parent, data;
    uint32_t start, len;
    bool marked;
} Chunk;
typedef struct Session {
    cxf_handle tail;
    uint64_t revision;
    uint32_t committed, evaluated;
} Session;

struct cxf_llama {
    cxf_alloc *alloc;
    uint64_t owner;
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    struct llama_batch batch;
    cxf_blobpool payload;
    cxf_table chunks, sessions;
    uint32_t context_tokens, max_sessions, batch_tokens, minimum, prefill_cap;
    cxf_handle logits_session;
    uint32_t logits_position;
    cxf_llama_stats stats;
    bool backend_acquired,trace_enabled,defer_sync,pending_sync,coalesce_tail;
    cxf_llama_decode_trace trace;
    void (*trace_hook)(int);
    void (*batch_hook)(uint32_t,uint32_t);
};

/* Process identity/lifecycle only, no model or policy state. This module and
 * cxf's allocator are deliberately single-threaded; caller serializes calls. */
static uint64_t next_owner;
static uint32_t backend_users;

const struct llama_model *cxf_llama_native_model(cxf_llama *e) { return e ? e->model : NULL; }

static uint64_t trace_clock(bool cpu){struct timespec t;clock_gettime(cpu?CLOCK_THREAD_CPUTIME_ID:CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
void cxf_llama_trace_observer(cxf_llama *e,void (*fn)(int)){if(e)e->trace_hook=fn;}
void cxf_llama_trace_batch_observer(cxf_llama *e,void (*fn)(uint32_t,uint32_t)){if(e)e->batch_hook=fn;}
static void metal_phase(cxf_llama *e,int phase){if(e->trace_hook)e->trace_hook(phase);}
void cxf_llama_defer_sync(cxf_llama *e,bool on){if(e)e->defer_sync=on;}
cxf_result cxf_llama_chunk_policy(cxf_llama *e,uint32_t cap,bool coalesce){
    if(!e||cap>e->batch_tokens)return CXF_ERR_RANGE;
    e->prefill_cap=cap;e->coalesce_tail=coalesce;return CXF_OK;
}
void cxf_llama_trace_enable(cxf_llama *e,bool on){if(e)e->trace_enabled=on;}
void cxf_llama_trace_get(cxf_llama *e,cxf_llama_decode_trace *out){if(e&&out)*out=e->trace;}
static cxf_handle none(void) { return cxf_handle_make(0, 0); }
static Chunk *chunk(cxf_llama *e, cxf_handle h) { return cxf_table_get(&e->chunks, h, 0); }
static Session *session(cxf_llama *e, cxf_llama_session h) {
    return e && h.owner == e->owner ? cxf_table_get(&e->sessions, h.slot, 0) : NULL;
}
static Session *versioned(cxf_llama *e, cxf_llama_session h, uint64_t revision) {
    Session *s = session(e, h);
    return s && s->revision == revision ? s : NULL;
}

/* Engine is the only owner. Session handles and parent edges borrow blocks.
 * Mutation/drop is a cold boundary: mark roots and free unreachable blocks.
 * No reference counting, no cycles, no full-prefix copy-on-write. */
static void collect(cxf_llama *e) {
    Chunk *all = cxf_table_col(&e->chunks, 0);
    for (uint32_t i = 0; i < e->chunks.count; i++) all[i].marked = false;
    Session *ss = cxf_table_col(&e->sessions, 0);
    for (uint32_t i = 0; i < e->sessions.count; i++) {
        cxf_handle h = ss[i].tail;
        while (!cxf_handle_null(h)) {
            Chunk *c = chunk(e, h);
            CXF_ASSERT(c);
            if (c->marked) break;
            c->marked = true;
            h = c->parent;
        }
    }
    for (uint32_t i = e->chunks.count; i-- > 0;) {
        Chunk *c = (Chunk *)cxf_table_col(&e->chunks, 0) + i;
        if (!c->marked) {
            cxf_blob_free(&e->payload, c->data);
            cxf_table_remove(&e->chunks, cxf_table_at(&e->chunks, i));
        }
    }
}

static cxf_handle prefix_tail(cxf_llama *e, cxf_handle tail, uint32_t keep) {
    if (!keep) return none();
    while (!cxf_handle_null(tail)) {
        Chunk *c = chunk(e, tail);
        CXF_ASSERT(c);
        if (c->start < keep) return tail;
        tail = c->parent;
    }
    CXF_ASSERT(false);
    return none();
}

static void copy_range(cxf_llama *e, cxf_handle tail, uint32_t begin, uint32_t end, int32_t *out) {
    uint32_t right = end;
    while (right > begin) {
        Chunk *c = chunk(e, tail);
        CXF_ASSERT(c);
        if (c->start < right) {
            uint32_t left = c->start > begin ? c->start : begin;
            cxf_str data = cxf_blob_view(&e->payload, c->data);
            CXF_ASSERT(right <= c->start + c->len && data.len == c->len * sizeof(int32_t));
            memcpy(out + left - begin, data.p + (left - c->start) * sizeof(int32_t),
                   (size_t)(right - left) * sizeof(int32_t));
            right = left;
        }
        tail = c->parent;
    }
}

cxf_llama_params cxf_llama_default_params(void) {
    cxf_llama_params p = {0};
    p.context_tokens = 4096; p.max_sessions = 4;
    p.batch_tokens = 512; p.prefill_min_tokens = 128;
    p.gpu_layers = 99; p.threads = 4;
    return p;
}

void cxf_llama_close(cxf_llama *e) {
    if (!e) return;
    cxf_alloc *a = e->alloc;
    if (e->batch.token) llama_batch_free(e->batch);
    if (e->ctx) llama_free(e->ctx);
    if (e->model) llama_model_free(e->model);
    cxf_blobpool_release(&e->payload);
    cxf_table_release(&e->chunks);
    cxf_table_release(&e->sessions);
    if (e->backend_acquired && --backend_users == 0) llama_backend_free();
    cxf_alloc_free(a, e, sizeof *e);
}

cxf_result cxf_llama_open(const cxf_llama_params *p, cxf_llama **out) {
    if (!out) return CXF_ERR_RANGE;
    *out = NULL;
    if (!p || !p->model_path || !p->context_tokens || p->context_tokens > INT32_MAX ||
        !p->max_sessions || p->max_sessions > 64 || !p->batch_tokens ||
        p->batch_tokens > p->context_tokens || !p->prefill_min_tokens ||
        p->prefill_min_tokens > p->batch_tokens || p->gpu_layers < 0 || p->threads <= 0 ||
        next_owner == UINT64_MAX) return CXF_ERR_RANGE;
    cxf_alloc *a = p->alloc ? p->alloc : cxf_alloc_heap();
    cxf_llama *e = cxf_alloc_alloc(a, sizeof *e, CXF_MIN_ALIGN);
    if (!e) return CXF_ERR_NOMEM;
    memset(e, 0, sizeof *e); e->alloc = a;
    size_t chunk_size = sizeof(Chunk), session_size = sizeof(Session);
    cxf_result rc = cxf_blobpool_init(&e->payload, a, 16, CLASS_TOKENS);
    if (rc == CXF_OK) rc = cxf_table_init_class(&e->chunks, a, &chunk_size, 1, 16, CLASS_CHUNKS);
    if (rc == CXF_OK) rc = cxf_table_init_class(&e->sessions, a, &session_size, 1,
                                              p->max_sessions, CLASS_SESSIONS);
    if (rc != CXF_OK) { cxf_llama_close(e); return rc; }
    if (!backend_users) {
        if (p->backend_path) ggml_backend_load_all_from_path(p->backend_path);
        else ggml_backend_load_all();
        llama_backend_init();
    }
    backend_users++; e->backend_acquired = true;
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = p->gpu_layers;
    e->model = llama_model_load_from_file(p->model_path, mp);
    if (!e->model) { cxf_llama_close(e); return CXF_ERR_IO; }
    if (llama_model_has_encoder(e->model) || llama_model_is_recurrent(e->model) ||
        llama_model_is_hybrid(e->model) || llama_model_is_diffusion(e->model)) {
        cxf_llama_close(e); return CXF_ERR_DEP;
    }
    e->vocab = llama_model_get_vocab(e->model);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = p->context_tokens; cp.n_seq_max = p->max_sessions;
    cp.n_batch = p->batch_tokens; cp.n_ubatch = p->batch_tokens;
    cp.n_threads = p->threads; cp.n_threads_batch = p->threads;
    cp.kv_unified = true;
    e->ctx = llama_init_from_model(e->model, cp);
    if (!e->ctx) { cxf_llama_close(e); return CXF_ERR_IO; }
    e->batch = llama_batch_init((int32_t)p->batch_tokens, 0, 1);
    if (!e->batch.token || !e->batch.pos || !e->batch.n_seq_id || !e->batch.seq_id || !e->batch.logits) {
        cxf_llama_close(e); return CXF_ERR_NOMEM;
    }
    e->context_tokens = p->context_tokens; e->max_sessions = p->max_sessions;
    e->batch_tokens = p->batch_tokens; e->minimum = p->prefill_min_tokens;
    e->owner = ++next_owner;
    *out = e;
    return CXF_OK;
}

cxf_result cxf_llama_session_new(cxf_llama *e, cxf_llama_session *out) {
    if (!e || !out) return CXF_ERR_RANGE;
    *out = (cxf_llama_session){0};
    if (e->sessions.count >= e->max_sessions) return CXF_ERR_FULL;
    cxf_handle h = cxf_table_add(&e->sessions);
    if (cxf_handle_null(h)) return CXF_ERR_NOMEM;
    Session *s = cxf_table_get(&e->sessions, h, 0);
    *s = (Session){ .revision = 1 };
    *out = (cxf_llama_session){ e->owner, h };
    return CXF_OK;
}

cxf_result cxf_llama_session_view(cxf_llama *e, cxf_llama_session h, cxf_llama_view *out) {
    Session *s = session(e, h);
    if (!s) return CXF_ERR_BAD_HANDLE;
    if (!out) return CXF_ERR_RANGE;
    *out = (cxf_llama_view){ s->revision, s->committed, s->evaluated };
    return CXF_OK;
}

cxf_result cxf_llama_session_close(cxf_llama *e, cxf_llama_session h) {
    Session *s = session(e, h);
    if (!s) return CXF_ERR_BAD_HANDLE;
    llama_synchronize(e->ctx);
    if (!llama_memory_seq_rm(llama_get_memory(e->ctx), (llama_seq_id)h.slot.index, -1, -1))
        return CXF_ERR_IO;
    if (cxf_handle_eq(e->logits_session, h.slot)) e->logits_session = none();
    cxf_table_remove(&e->sessions, h.slot);
    collect(e);
    return CXF_OK;
}

cxf_result cxf_llama_replace(cxf_llama *e, cxf_llama_session h, uint64_t revision,
                            uint32_t keep, const int32_t *tokens, uint32_t n) {
    Session *s = versioned(e, h, revision);
    if (!s) return CXF_ERR_BAD_HANDLE;
    if (keep > s->committed || keep > e->context_tokens || n > e->context_tokens - keep ||
        (n && !tokens) || revision == UINT64_MAX) return CXF_ERR_RANGE;
    int32_t vocab = llama_vocab_n_tokens(e->vocab);
    for (uint32_t i = 0; i < n; i++) if (tokens[i] < 0 || tokens[i] >= vocab) return CXF_ERR_RANGE;
    if (!n && keep == s->committed) return CXF_OK;
    cxf_handle tail = prefix_tail(e, s->tail, keep);
    cxf_handle extend = none();
    uint32_t grow = 0;
    Chunk *last = chunk(e, tail);
    if (keep == s->committed && last && last->start + last->len == keep && last->len < TOKEN_BLOCK) {
        grow = TOKEN_BLOCK - last->len;
        if (grow > n) grow = n;
        extend = tail;
    }
    /* No old data or len is changed until every additional block is allocated. */
    for (uint32_t off = grow; off < n;) {
        uint32_t amount = n - off < TOKEN_BLOCK ? n - off : TOKEN_BLOCK;
        cxf_handle data = cxf_blob_new(&e->payload, TOKEN_BLOCK * sizeof(int32_t));
        if (cxf_handle_null(data)) { collect(e); return CXF_ERR_NOMEM; }
        cxf_result rc = cxf_blob_append(&e->payload, data, tokens + off, amount * sizeof(int32_t));
        if (rc != CXF_OK) { cxf_blob_free(&e->payload, data); collect(e); return rc; }
        cxf_handle node = cxf_table_add(&e->chunks);
        if (cxf_handle_null(node)) { cxf_blob_free(&e->payload, data); collect(e); return CXF_ERR_NOMEM; }
        Chunk *c = chunk(e, node);
        *c = (Chunk){ .parent = tail, .data = data, .start = keep + off, .len = amount };
        tail = node; off += amount;
    }
    /* Tables may have grown; only handles survive allocation boundaries. */
    s = session(e, h);
    if (keep < s->evaluated) {
        llama_synchronize(e->ctx);
        if (!llama_memory_seq_rm(llama_get_memory(e->ctx), (llama_seq_id)h.slot.index, keep, -1)) {
            collect(e); return CXF_ERR_DEP;
        }
        e->stats.invalidated_tokens += s->evaluated - keep;
        s->evaluated = keep;
        if (cxf_handle_eq(e->logits_session, h.slot)) e->logits_session = none();
    }
    if (grow) {
        Chunk *c = chunk(e, extend);
        cxf_result rc = cxf_blob_append(&e->payload, c->data, tokens, grow * sizeof(int32_t));
        CXF_ASSERT(rc == CXF_OK); /* fixed reserve: this step cannot allocate */
        (void)rc;
        c->len += grow;
    }
    bool discarded = keep < s->committed;
    s->tail = tail; s->committed = keep + n; s->revision++;
    if (discarded) collect(e);
    return CXF_OK;
}

cxf_result cxf_llama_append(cxf_llama *e, cxf_llama_session h, uint64_t revision,
                           const int32_t *tokens, uint32_t n) {
    Session *s = versioned(e, h, revision);
    if (!s) return CXF_ERR_BAD_HANDLE;
    return cxf_llama_replace(e, h, revision, s->committed, tokens, n);
}

cxf_result cxf_llama_read(cxf_llama *e, cxf_llama_session h, uint32_t start,
                         int32_t *out, uint32_t n) {
    Session *s = session(e, h);
    if (!s) return CXF_ERR_BAD_HANDLE;
    if (start > s->committed || n > s->committed - start || (n && !out)) return CXF_ERR_RANGE;
    copy_range(e, s->tail, start, start + n, out);
    return CXF_OK;
}

cxf_result cxf_llama_fork(cxf_llama *e, cxf_llama_session parent, uint64_t revision,
                         uint32_t keep, cxf_llama_session *out) {
    Session *p = versioned(e, parent, revision);
    if (!p) return CXF_ERR_BAD_HANDLE;
    if (keep > p->committed || !out) return CXF_ERR_RANGE;
    Session child = { .tail = prefix_tail(e, p->tail, keep), .revision = 1,
                      .committed = keep, .evaluated = p->evaluated < keep ? p->evaluated : keep };
    cxf_result rc = cxf_llama_session_new(e, out);
    if (rc != CXF_OK) return rc;
    Session *s = session(e, *out); *s = child;
    if (child.evaluated) {
        llama_synchronize(e->ctx);
        llama_memory_seq_cp(llama_get_memory(e->ctx), (llama_seq_id)parent.slot.index,
                            (llama_seq_id)out->slot.index, 0, child.evaluated);
        e->stats.shared_kv_tokens += child.evaluated;
    }
    return CXF_OK;
}

static void drain(cxf_llama *e){
    if(!e->pending_sync)return;
    uint64_t wall=0,cpu=0;if(e->trace_enabled){wall=trace_clock(false);cpu=trace_clock(true);}
    metal_phase(e,2);llama_synchronize(e->ctx);metal_phase(e,0);e->pending_sync=false;
    if(e->trace_enabled){e->trace.sync_ns+=trace_clock(false)-wall;e->trace.sync_cpu_ns+=trace_clock(true)-cpu;e->trace.sync_calls++;}
}

static cxf_result decode(cxf_llama *e, cxf_llama_session h, Session *s, uint32_t amount,bool sync_now) {
    struct llama_batch b = e->batch;
    b.n_tokens = (int32_t)amount;
    copy_range(e, s->tail, s->evaluated, s->evaluated + amount, b.token);
    for (uint32_t i = 0; i < amount; i++) {
        b.pos[i] = (llama_pos)(s->evaluated + i); b.n_seq_id[i] = 1;
        b.seq_id[i][0] = (llama_seq_id)h.slot.index; b.logits[i] = i == amount - 1;
    }
    uint64_t wall=0,cpu=0;if(e->trace_enabled){wall=trace_clock(false);cpu=trace_clock(true);}
    if(e->batch_hook)e->batch_hook(s->evaluated,amount);
    metal_phase(e,1);int rc = llama_decode(e->ctx, b);metal_phase(e,0);
    if(e->trace_enabled){e->trace.decode_ns+=trace_clock(false)-wall;e->trace.decode_cpu_ns+=trace_clock(true)-cpu;e->trace.decode_calls++;}
    e->pending_sync=true;if(sync_now||rc)drain(e);
    e->stats.decode_calls++;
    e->logits_session = none(); /* any decode can overwrite context-wide outputs */
    if (rc) {
        e->stats.last_native_error = rc; e->stats.decode_failures++;
        e->stats.invalidated_tokens += s->evaluated;
        (void)llama_memory_seq_rm(llama_get_memory(e->ctx), (llama_seq_id)h.slot.index, -1, -1);
        s->evaluated = 0;
        return CXF_ERR_IO;
    }
    s->evaluated += amount;
    e->stats.evaluated_tokens += amount;
    e->logits_session = h.slot; e->logits_position = s->evaluated;
    return CXF_OK;
}

cxf_result cxf_llama_prefill(cxf_llama *e, cxf_llama_session h, uint64_t revision,
                            uint32_t budget, bool flush, uint32_t *processed) {
    if (processed) *processed = 0;
    Session *s = versioned(e, h, revision);
    if (!s) return CXF_ERR_BAD_HANDLE;
    if (!processed) return CXF_ERR_RANGE;
    while (budget && s->evaluated < s->committed) {
        uint32_t remaining = s->committed - s->evaluated;
        uint32_t cap = e->prefill_cap ? e->prefill_cap : e->batch_tokens;
        if(e->coalesce_tail && remaining<=e->batch_tokens && remaining<=budget)cap=e->batch_tokens;
        uint32_t n = remaining;
        if (n > budget) n = budget;
        if (n > cap) n = cap;
        if (!flush && n < e->minimum) { e->stats.deferred_prefills++; break; }
        cxf_result rc = decode(e, h, s, n,!e->defer_sync);
        if (rc != CXF_OK) { *processed = 0; return rc; }
        *processed += n; budget -= n;
    }
    drain(e);
    return CXF_OK;
}

cxf_result cxf_llama_greedy(cxf_llama *e, cxf_llama_session h, uint64_t revision, int32_t *token) {
    Session *s = versioned(e, h, revision);
    if (!s) return CXF_ERR_BAD_HANDLE;
    if (!token) return CXF_ERR_RANGE;
    if (!s->committed || s->evaluated != s->committed) return CXF_ERR_EMPTY;
    if (!cxf_handle_eq(e->logits_session, h.slot) || e->logits_position != s->evaluated) {
        if (!llama_memory_seq_rm(llama_get_memory(e->ctx), (llama_seq_id)h.slot.index,
                                 s->evaluated - 1, -1)) return CXF_ERR_DEP;
        s->evaluated--;
        cxf_result rc = decode(e, h, s, 1,true);
        if (rc != CXF_OK) return rc;
        e->stats.logits_replay_tokens++;
    }
    const float *logits = llama_get_logits_ith(e->ctx, -1);
    if (!logits) return CXF_ERR_IO;
    int32_t best = 0, n = llama_vocab_n_tokens(e->vocab);
    for (int32_t i = 1; i < n; i++) if (logits[i] > logits[best]) best = i;
    *token = best;
    return CXF_OK;
}

cxf_result cxf_llama_kv_save_file(cxf_llama *e, cxf_llama_session h, uint64_t revision,
                                 const char *path, uint64_t budget, cxf_llama_kv_image *out) {
    if (out) memset(out, 0, sizeof *out);
    Session *s = versioned(e, h, revision);
    if (!s) return CXF_ERR_BAD_HANDLE;
    if (!path || !*path || !out) return CXF_ERR_RANGE;
    if (!s->evaluated || s->evaluated != s->committed) return CXF_ERR_EMPTY;
    llama_synchronize(e->ctx);
    size_t native = llama_state_seq_get_size(e->ctx, (llama_seq_id)h.slot.index);
    /* get_data has 8 framing bytes; the file has 12, including a zero token count. */
    if (!native || native > UINT64_MAX - 4) return CXF_ERR_IO;
    if ((uint64_t)native + 4 > budget) return CXF_ERR_FULL;
    size_t bytes = llama_state_seq_save_file(e->ctx, path, (llama_seq_id)h.slot.index, NULL, 0);
    if (!bytes || bytes > budget) { (void)remove(path); return CXF_ERR_IO; }
    *out = (cxf_llama_kv_image){ h, revision, bytes, s->evaluated };
    return CXF_OK;
}

cxf_result cxf_llama_kv_evict(cxf_llama *e, cxf_llama_session h, uint64_t revision) {
    Session *s = versioned(e, h, revision);
    if (!s) return CXF_ERR_BAD_HANDLE;
    llama_synchronize(e->ctx);
    if (!llama_memory_seq_rm(llama_get_memory(e->ctx), (llama_seq_id)h.slot.index, -1, -1))
        return CXF_ERR_IO;
    s->evaluated = 0;
    if (cxf_handle_eq(e->logits_session, h.slot)) e->logits_session = none();
    return CXF_OK;
}

cxf_result cxf_llama_kv_restore_file(cxf_llama *e, const cxf_llama_kv_image *image, const char *path) {
    if (!image || !path) return CXF_ERR_RANGE;
    Session *s = versioned(e, image->session, image->revision);
    if (!s) return CXF_ERR_BAD_HANDLE;
    if (!image->evaluated || image->evaluated != s->committed || s->evaluated || !image->bytes)
        return CXF_ERR_RANGE;
    int32_t unused = 0; size_t count = 0;
    size_t bytes = llama_state_seq_load_file(e->ctx, path, (llama_seq_id)image->session.slot.index,
                                            &unused, 0, &count);
    llama_synchronize(e->ctx); /* readiness means device state is usable, not merely an I/O return */
    llama_memory_t mem = llama_get_memory(e->ctx);
    if (bytes != image->bytes || count || llama_memory_seq_pos_min(mem, (llama_seq_id)image->session.slot.index) != 0 ||
        llama_memory_seq_pos_max(mem, (llama_seq_id)image->session.slot.index) != (llama_pos)image->evaluated - 1) {
        (void)llama_memory_seq_rm(mem, (llama_seq_id)image->session.slot.index, -1, -1);
        s->evaluated = 0;
        if (cxf_handle_eq(e->logits_session, image->session.slot)) e->logits_session = none();
        return CXF_ERR_IO;
    }
    s->evaluated = image->evaluated;
    /* Sequence files do not save logits; greedy replays the last token. */
    if (cxf_handle_eq(e->logits_session, image->session.slot)) e->logits_session = none();
    return CXF_OK;
}

bool cxf_llama_is_eog(cxf_llama *e, int32_t token) {
    return e && token >= 0 && token < llama_vocab_n_tokens(e->vocab) && llama_vocab_is_eog(e->vocab, token);
}

cxf_result cxf_llama_tokenize(cxf_llama *e, const char *text, uint32_t bytes, bool special,
                             int32_t *out, uint32_t cap, uint32_t *count) {
    if (!e || !count || bytes > INT32_MAX || cap > INT32_MAX || (bytes && !text) || (cap && !out))
        return CXF_ERR_RANGE;
    int32_t n = llama_tokenize(e->vocab, text ? text : "", (int32_t)bytes, out, (int32_t)cap, special, true);
    *count = n < 0 ? (uint32_t)(-(int64_t)n) : (uint32_t)n;
    return n < 0 ? CXF_ERR_FULL : CXF_OK;
}

cxf_result cxf_llama_piece(cxf_llama *e, int32_t token, char *out, uint32_t cap, uint32_t *bytes) {
    if (!e || !bytes || cap > INT32_MAX || (cap && !out) || token < 0 ||
        token >= llama_vocab_n_tokens(e->vocab)) return CXF_ERR_RANGE;
    int32_t n = llama_token_to_piece(e->vocab, token, out, (int32_t)cap, 0, true);
    *bytes = n < 0 ? (uint32_t)(-(int64_t)n) : (uint32_t)n;
    return n < 0 ? CXF_ERR_FULL : CXF_OK;
}

void cxf_llama_get_stats(cxf_llama *e, cxf_llama_stats *out) {
    if (!e || !out) return;
    *out = e->stats;
    out->token_payload_capacity = cxf_blobpool_bytes(&e->payload);
    out->sessions = e->sessions.count; out->chunks = e->chunks.count;
    const Session *ss = cxf_table_col(&e->sessions, 0);
    for (uint32_t i = 0; i < e->sessions.count; i++) out->logical_tokens += ss[i].committed;
    const Chunk *cc = cxf_table_col(&e->chunks, 0);
    for (uint32_t i = 0; i < e->chunks.count; i++) out->stored_tokens += cc[i].len;
}
