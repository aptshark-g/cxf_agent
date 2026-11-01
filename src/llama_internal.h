#ifndef CXF_LLAMA_INTERNAL_H
#define CXF_LLAMA_INTERNAL_H
/* Private integration boundary, not part of the stable cxf API. */
struct llama_model;
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cxf_llama_decode_trace {
    uint64_t decode_calls,sync_calls,decode_ns,sync_ns,decode_cpu_ns,sync_cpu_ns;
} cxf_llama_decode_trace;
/* Private owner-only diagnostic; defaults off, counters are cumulative. */
void cxf_llama_trace_observer(cxf_llama *,void (*)(int));
void cxf_llama_trace_batch_observer(cxf_llama *,void (*)(uint32_t,uint32_t));
void cxf_llama_defer_sync(cxf_llama *,bool);
/* Owner-only chunk cap; zero resets to allocated batch capacity. */
cxf_result cxf_llama_chunk_policy(cxf_llama *,uint32_t,bool);
void cxf_llama_trace_enable(cxf_llama *,bool);
void cxf_llama_trace_get(cxf_llama *,cxf_llama_decode_trace *);
const struct llama_model *cxf_llama_native_model(cxf_llama *);
/* Private, same-engine snapshots only. File contains KV, no token list.
 * Caller owns a fresh private path, validates file identity before restore and
 * serializes all calls. Canonical tokens and revision survive eviction. */
typedef struct cxf_llama_kv_image {
    cxf_llama_session session;
    uint64_t revision, bytes;
    uint32_t evaluated;
} cxf_llama_kv_image;
cxf_result cxf_llama_kv_save_file(cxf_llama *, cxf_llama_session, uint64_t,
                                 const char *, uint64_t budget, cxf_llama_kv_image *);
cxf_result cxf_llama_kv_evict(cxf_llama *, cxf_llama_session, uint64_t);
cxf_result cxf_llama_kv_restore_file(cxf_llama *, const cxf_llama_kv_image *, const char *);
#ifdef __cplusplus
}
#endif
#endif
