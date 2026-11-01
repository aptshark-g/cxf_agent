#ifndef CXF_LLAMA_H
#define CXF_LLAMA_H

#include "alloc.h"
#include "handle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional backend, pinned to llama.cpp b10938. No llama headers in this API.
 * Contract/status: docs/llama-context-v1.md. All calls are serialized by caller.
 * Committed token history is owned once by this engine's blob pool. Sessions
 * borrow append-only blocks with fixed view ends; bounded scratch is not another history.
 */
typedef struct cxf_llama cxf_llama;
typedef struct cxf_llama_session { uint64_t owner; cxf_handle slot; } cxf_llama_session;

typedef struct cxf_llama_params {
    const char *model_path;
    const char *backend_path;
    cxf_alloc *alloc;               /* NULL -> cxf_alloc_heap; does not count libllama allocations */
    uint32_t context_tokens;        /* requested KV pool (engine may pad); per-session logical ceiling */
    uint32_t max_sessions;
    uint32_t batch_tokens;
    uint32_t prefill_min_tokens;
    int32_t gpu_layers;             /* 0 is explicit CPU execution */
    int32_t threads;
} cxf_llama_params;

typedef struct cxf_llama_view {
    uint64_t revision;
    uint32_t committed;
    uint32_t evaluated;
} cxf_llama_view;

typedef struct cxf_llama_stats {
    uint64_t evaluated_tokens;
    uint64_t decode_calls;
    uint64_t shared_kv_tokens;
    uint64_t invalidated_tokens;
    uint64_t logits_replay_tokens;
    uint64_t decode_failures;
    uint64_t deferred_prefills;
    uint64_t token_payload_capacity; /* cxf pool capacities; NOT KV bytes or VRAM */
    uint64_t stored_tokens;          /* physical canonical blocks, may include a clipped block tail */
    uint64_t logical_tokens;         /* sum of session views; shared prefixes counted per view */
    uint32_t sessions;
    uint32_t chunks;
    int32_t last_native_error;
} cxf_llama_stats;

cxf_llama_params cxf_llama_default_params(void);
cxf_result cxf_llama_open(const cxf_llama_params *, cxf_llama **out);
void cxf_llama_close(cxf_llama *);

cxf_result cxf_llama_session_new(cxf_llama *, cxf_llama_session *out);
cxf_result cxf_llama_session_view(cxf_llama *, cxf_llama_session, cxf_llama_view *out);
cxf_result cxf_llama_session_close(cxf_llama *, cxf_llama_session);

/* Stale revision -> BAD_HANDLE. All allocation/ID/range validation precedes KV
 * edits. append/replace copy only the supplied suffix into canonical blocks.
 * replace permits n=0 (truncate); a no-op does not increment revision.
 */
cxf_result cxf_llama_append(cxf_llama *, cxf_llama_session, uint64_t revision,
                           const int32_t *tokens, uint32_t n);
cxf_result cxf_llama_replace(cxf_llama *, cxf_llama_session, uint64_t revision,
                            uint32_t keep, const int32_t *tokens, uint32_t n);
cxf_result cxf_llama_fork(cxf_llama *, cxf_llama_session parent, uint64_t revision,
                         uint32_t keep, cxf_llama_session *out);
cxf_result cxf_llama_read(cxf_llama *, cxf_llama_session, uint32_t start,
                         int32_t *out, uint32_t n);

/* budget=0 does no work. !flush waits for prefill_min_tokens, including a small
 * final budget remainder. flush bypasses that threshold, never the token budget.
 * On decode failure canonical history survives; affected KV is invalidated and
 * evaluated resets to zero. Caller explicitly decides whether to retry.
 */
cxf_result cxf_llama_prefill(cxf_llama *, cxf_llama_session, uint64_t revision,
                            uint32_t budget, bool flush, uint32_t *processed);

/* Requires committed == evaluated > 0, otherwise EMPTY. If another session
 * overwrote context-wide logits, replay this session's last token first.
 * Sampling never appends tokens; caller checks EOG and commits chosen tokens.
 */
cxf_result cxf_llama_greedy(cxf_llama *, cxf_llama_session, uint64_t revision, int32_t *token);
bool cxf_llama_is_eog(cxf_llama *, int32_t token);

/* Transient helpers. Negative/native tokenizer capacity result -> FULL with
 * required token count; output is not promised on FULL. Tokenization alone
 * does not establish a stable append boundary across arbitrary text chunks.
 */
cxf_result cxf_llama_tokenize(cxf_llama *, const char *, uint32_t bytes, bool add_special,
                             int32_t *out, uint32_t cap, uint32_t *count);
cxf_result cxf_llama_piece(cxf_llama *, int32_t token, char *out, uint32_t cap, uint32_t *bytes);
void cxf_llama_get_stats(cxf_llama *, cxf_llama_stats *out);

#ifdef __cplusplus
}
#endif

#endif
