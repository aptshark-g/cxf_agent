#ifndef AGENT_NATIVE_H
#define AGENT_NATIVE_H
#include "cxf/llama.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "agent_llm.h"

typedef struct AgentNative AgentNative;
typedef struct AgentNativeStats {
    uint64_t tokenized_tokens, reused_prefix_tokens, rebuilds, generations;
    uint64_t render_ns, tokenize_ns, guard_ns, commit_ns, prefill_ns;
    uint64_t template_cache_hits, template_cache_misses;
    uint64_t tokenize_calls, tokenize_retries, tokenize_scratch_peak_bytes;
    uint32_t checkpoint_tokens, checkpoint_bytes;
    uint64_t output_updates,output_tokenized,output_evaluated,output_replayed,output_adopted;
    uint64_t output_front_ns,output_prefill_ns,output_discarded;
} AgentNativeStats;
/* Owner-only, fixed test.run(empty args), bounded4096-byte retained stdout.
 * Provisional tail shares the existing session; only final normal prepare may
 * publish actions. Final render/guard reconciles tail BPE before adoption. */
cxf_result agent_native_tool_output_begin(AgentNative *);
cxf_result agent_native_tool_output_tokenize(AgentNative *,const char *,uint32_t);
cxf_result agent_native_tool_output_update(AgentNative *,const char *,uint32_t);
cxf_result agent_native_tool_output_abort(AgentNative *);
cxf_result agent_native_open(const cxf_llama_params *, uint32_t max_generate, AgentNative **);
/* Owner-thread only. Borrow engine; close releases this object's session only.
 * Caller keeps engine alive and serializes every Native/engine operation. */
cxf_result agent_native_bind(cxf_llama *, uint32_t context_tokens,
                             uint32_t max_generate, AgentNative **);
/* Trim to the last verified pre-header checkpoint, retaining canonical tokens.
 * EMPTY before any prepare. Does not evict KV or close the engine. */
cxf_result agent_native_checkpoint(AgentNative *, cxf_llama_session *, uint64_t *revision,
                                   uint32_t *tokens);
void agent_native_close(AgentNative *);
cxf_result agent_native_reset(AgentNative *);
/* Diagnostic switch; disabling preserves the original upstream preparation.
 * Cache contains template/parser metadata, never conversation history. */
void agent_native_template_cache(AgentNative *, bool enabled);
/* Phased operations also permit deterministic template/parser acceptance.
 * prepare performs real KV prefill; actions parses a finished completion.
 * Production uses chat_action, which requires a real generation stop. */
cxf_result agent_native_prepare(AgentNative *, const AgentLlmCfg *, const char *, uint32_t);
/* Experimental D-SPEC host only; default prepare/chat_action remain non-thinking.
 * Same verified Qwen3 template, explicit mode included in parser-cache identity. */
cxf_result agent_native_prepare_thinking(AgentNative *,const AgentLlmCfg *,const char *,uint32_t,bool);
cxf_result agent_native_actions(AgentNative *, const char *, uint32_t,
                                char *, uint32_t, uint32_t *);
cxf_result agent_native_chat_action(AgentNative *, AgentLlmCfg *, const char *, uint32_t,
                                    char *, uint32_t, char *, uint32_t, uint32_t *);
void agent_native_stats(AgentNative *, AgentNativeStats *);
cxf_llama *agent_native_engine(AgentNative *, cxf_llama_session *);
#ifdef __cplusplus
}
#endif
#endif
