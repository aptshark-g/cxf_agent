#ifndef AGENT_KV_WORKER_H
#define AGENT_KV_WORKER_H
#include "cxf/llama.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AgentKvWorker AgentKvWorker;
typedef uint64_t AgentKvSnapshot;
typedef uint64_t AgentKvTicket;
typedef cxf_result (*AgentKvOwnerFn)(cxf_llama *, void *);
typedef struct AgentKvReport {
    uint64_t queued_ns, started_ns, ready_ns, finished_ns;
    uint64_t restore_start_ns, restore_end_ns;
    uint64_t verify_ns, restore_ns, restored_bytes, wasted_bytes;
    uint32_t restored_tokens;
    bool cancelled;
    uint64_t finish_enter_ns, finish_owner_ns, finish_return_ns;
} AgentKvReport;
/* Optional macOS/Linux C++ host adapter. All llama/cxf calls, including open and
 * close, run on one owner thread. No engine calls from host assembly callbacks.
 * At most 16 queued requests, 16 outstanding tickets and 8 private snapshots.
 * params/path are copied at open; supplied allocator belongs to the owner.
 * Model/backend global initialization must remain serialized across engines. */
cxf_result agent_kv_worker_open(const cxf_llama_params *, const char *scratch_parent,
                                 uint64_t disk_budget, AgentKvWorker **);
void agent_kv_worker_close(AgentKvWorker *);
/* close may drain live tickets, but must not race other host API calls. */
/* Synchronous owner callback; ctx may be borrowed until this function returns.
 * Do not retain engine pointers or reenter this worker from its callback. */
cxf_result agent_kv_worker_run(AgentKvWorker *, AgentKvOwnerFn, void *);
cxf_result agent_kv_worker_save(AgentKvWorker *, cxf_llama_session, uint64_t revision,
                               AgentKvSnapshot *);
cxf_result agent_kv_worker_drop(AgentKvWorker *, AgentKvSnapshot);
/* Diagnostic isolation: unlink only after ticket drain, retain private fd and
 * budget/slot until normal drop performs last close. No descriptor escapes. */
cxf_result agent_kv_worker_unlink_hold(AgentKvWorker *, AgentKvSnapshot);
cxf_result agent_kv_worker_evict(AgentKvWorker *, cxf_llama_session, uint64_t revision);
/* Hint is metadata only and returns without waiting for restore. Cancellation
 * prevents final commit; an in-flight native restore stops at its boundary.
 * finish consumes the ticket, waiting even on cancellation, then checks the
 * snapshot/session version again before the optional owner callback. */
cxf_result agent_kv_worker_prepare(AgentKvWorker *, AgentKvSnapshot, AgentKvTicket *);
cxf_result agent_kv_worker_cancel(AgentKvWorker *, AgentKvTicket);
cxf_result agent_kv_worker_poll(AgentKvWorker *, AgentKvTicket, AgentKvReport *, bool *ready);
cxf_result agent_kv_worker_finish(AgentKvWorker *, AgentKvTicket, AgentKvOwnerFn,
                                 void *, AgentKvReport *);
#ifdef __cplusplus
}
#endif
#endif
