#ifndef AGENT_STREAM_H
#define AGENT_STREAM_H
#include "agent_kv_worker.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AgentStream AgentStream;
typedef struct AgentStreamUnit {
    const char *id, *text; uint32_t version, bytes; uint64_t end_seq;
} AgentStreamUnit;
typedef cxf_result (*AgentStreamUnitFn)(const AgentStreamUnit *, void *, char *, uint32_t, uint32_t *);
/* Final prompt is borrowed only for this callback. Fork for generation; do not
 * edit/close the receiving session. Callback contexts live until stream_close. */
typedef cxf_result (*AgentStreamFinalFn)(cxf_llama *, cxf_llama_session, const char *, uint32_t, void *);
typedef struct AgentStreamParams {
    const char *system;
    const char *initial_context; uint32_t initial_bytes;
    uint32_t prefill_budget; /* 1..512; engine's minimum-batch policy still applies */
    bool defer_until_end;  /* batch baseline: same content, callbacks and budgets */
    bool flush_each_update; /* experiment/control: allow sub-minimum streaming batches */
    AgentStreamUnitFn process_unit; void *unit_ctx;
    AgentStreamFinalFn finalize; void *final_ctx;
} AgentStreamParams;
typedef struct AgentStreamReport {
    uint64_t started_ns, input_end_ns, model_ready_ns, final_started_ns, done_ns;
    uint64_t source_bytes, frames, render_ns, tokenize_ns, prefill_ns;
    uint64_t tokenized_tokens, tokenizer_calls, evaluated_tokens, decode_calls;
    uint64_t input_evaluated_tokens, input_decode_calls; /* excludes final callback generation */
    uint64_t tool_ns, tool_calls, tool_completed, stale_receipts, tool_scratch_peak;
    uint64_t backpressure_returns, pending_peak, source_storage_bytes;
    uint32_t initial_tokens, committed_tokens, evaluated_frontier;
    bool cancelled, failed, done;
    cxf_result result;
} AgentStreamReport;
typedef struct AgentStreamReceipt {
    char id[65]; uint32_t version; uint64_t end_seq, started_ns, finished_ns;
    bool stale; cxf_result result;
} AgentStreamReceipt;
/* Optional host adapter. Worker is borrowed and exclusively used by this
 * stream until close. Sources: ordered NDJSON seq=1..N, kind text/file_chunk/
 * file_end/end; text field for payload, id/version for files. Each payload is
 * an immutable independent message unit, not an arbitrary BPE/text fragment.
 * Limits: 16KiB wire frame, 64KiB source payload, 128 events, 8 file versions,
 * 16 uncommitted source events in streaming mode, 4KiB per receipt.
 * All host API calls are serialized by caller; close must not race them. */
cxf_result agent_stream_open(AgentKvWorker *, const AgentStreamParams *, AgentStream **);
/* Arbitrary byte splits, including within UTF-8/JSON, are buffered. consumed
 * includes a retained complete frame when FULL means backlog pressure: wait,
 * then feed(NULL,0) to retry it before continuing. failed in report marks a
 * terminal capacity/error condition. There is no silent truncation. */
cxf_result agent_stream_feed(AgentStream *, const char *, uint32_t, uint32_t *consumed);
cxf_result agent_stream_wait_input(AgentStream *, uint32_t timeout_ms);
/* finish requires an accepted end event and no incomplete wire frame. It
 * waits for valid file receipts, final prefill and final callback. On cancel,
 * close still joins in-flight native/file callbacks before releasing data. */
cxf_result agent_stream_finish(AgentStream *, AgentStreamReport *);
cxf_result agent_stream_cancel(AgentStream *);
void agent_stream_report(AgentStream *, AgentStreamReport *);
cxf_result agent_stream_receipt(AgentStream *, uint32_t index, AgentStreamReceipt *, char *, uint32_t, uint32_t *);
void agent_stream_close(AgentStream *);
#ifdef __cplusplus
}
#endif
#endif
