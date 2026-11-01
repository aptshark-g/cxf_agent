#ifndef AGENT_TOOL_PREPARE_H
#define AGENT_TOOL_PREPARE_H
#include "agent_native.h"
#include "agent_kv_worker.h"
#include "agent_resource.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "agent_basic.h"
typedef struct AgentToolPrepare AgentToolPrepare;
typedef enum AgentToolKvMode { TOOL_KV_RESIDENT=0, TOOL_KV_RECOMPUTE=1, TOOL_KV_RESTORE=2 } AgentToolKvMode;
typedef enum AgentToolPreparePolicy { TOOL_PREPARE_LATE=0, TOOL_PREPARE_IMMEDIATE=1, TOOL_PREPARE_TIMED=2, TOOL_PREPARE_PROGRESS=3 } AgentToolPreparePolicy;
typedef struct AgentToolPrepareOptions {
    AgentToolKvMode kv_mode; AgentToolPreparePolicy policy; double margin_ms;
    bool release_snapshot_early; /* optional: drop only after ticket finish */
    bool hold_snapshot_file; /* isolate unlink from last close; requires early restore */
    bool trace_resources; /* sparse optional diagnostics; default false */
} AgentToolPrepareOptions;
typedef struct AgentToolPrepareReport {
    uint64_t entered_ns, tool_start_ns, tool_end_ns, finished_ns;
    uint64_t prepare_start_ns, prefix_ready_ns, checkpoint_ns, save_ns, evict_ns;
    uint64_t wasted_tokens;
    double predicted_tool_ms, predicted_prepare_ms, planned_delay_ms;
    uint32_t prefix_tokens, tool_observations_before, prepare_observations_before;
    cxf_result prepare_rc, tool_rc;
    bool cancelled;
    AgentKvReport kv;
    uint64_t thread_enter_ns, timer_deadline_ns, timer_wake_ns;
    uint64_t prepare_api_return_ns, join_return_ns, drop_start_ns, drop_end_ns;
    uint64_t epoch, progress_ns;
    double initial_tool_estimate_ms, initial_delay_ms, predicted_post_spawn_ms;
    uint32_t progress_observations_before;
    uint64_t thread_launch_start_ns, thread_launch_end_ns, tool_fn_start_ns;
    uint64_t early_drop_start_ns, early_drop_end_ns;
    cxf_result early_drop_rc, drop_rc;
    AgentResourceSample early_drop_before,early_drop_after,drop_before,drop_after;
} AgentToolPrepareReport;
typedef cxf_result (*AgentToolExec)(const void *,const char *,uint32_t,char *,uint32_t,uint32_t *);
typedef cxf_result (*AgentToolNativeFn)(AgentNative *,void *);
/* Optional single-tool host. Owns one worker, borrows its engine for Native.
 * All APIs serialized except cancel; close must follow the completed execute.
 * No tool permission changes. Engine calls only on the worker owner. */
cxf_result agent_tool_prepare_open(const cxf_llama_params *,uint32_t max_generate,
    const char *scratch_parent,uint64_t disk_budget,AgentToolPrepareOptions,AgentToolPrepare **);
void agent_tool_prepare_close(AgentToolPrepare *);
cxf_result agent_tool_prepare_options(AgentToolPrepare *,AgentToolPrepareOptions);
/* Up to 32 deployment keys; numerical warm-start only, units ms/token.
 * Caller isolates hardware/model/batch/tool identity. No raw history stored. */
cxf_result agent_tool_prepare_seed(AgentToolPrepare *,uint64_t key,double tool_ms,double prepare_ms_per_token);
/* Same completed calibration labels may seed the after-spawn phase. */
cxf_result agent_tool_prepare_seed_progress(AgentToolPrepare *,uint64_t key,double post_spawn_ms);
/* Synchronous parent progress for this execute epoch only. May race cancel.
 * A progress-policy tool without an event falls back to prepare after return. */
cxf_result agent_tool_prepare_progress(AgentToolPrepare *,uint64_t epoch,uint64_t event_ns);
cxf_result agent_tool_prepare_run(AgentToolPrepare *,AgentToolNativeFn,void *);
/* exec is invoked once; preparation errors are diagnostic, never tool retries.
 * Borrow args/output/ctx through return. Keep adequate capacity for the real
 * executor; its own permissions, receipts and FULL semantics still apply. */
cxf_result agent_tool_prepare_execute(AgentToolPrepare *,uint64_t key,AgentToolExec,const void *,
    const char *,uint32_t,char *,uint32_t,uint32_t *,AgentToolPrepareReport *);
/* Cancels speculative preparation only; the tool executor owns process timeout.
 * execute drains native work before returning and before releasing its inputs. */
cxf_result agent_tool_prepare_cancel(AgentToolPrepare *);
cxf_result agent_tool_prepare_reset(AgentToolPrepare *);
cxf_result agent_tool_prepare_action(AgentToolPrepare *,AgentLlmCfg *,const char *,uint32_t,
    char *,uint32_t,char *,uint32_t,uint32_t *);
/* Diagnostic policy:0 E30,1 E29,2 owner-only,3 tokenize-only,4 output-growth gate;5 timer-event,6 growth-event,7 off-event,8 growth-race,9 off-race;10 idle-race,11 thread-race,12 token-race diagnostics;13 lazy-race;14 off-drain,15 growth-drain. Idle only. */
cxf_result agent_tool_output_policy(AgentToolPrepare *,int);
void agent_tool_prepare_stats(AgentToolPrepare *,AgentNativeStats *);
typedef struct AgentToolOutputReport {
    uint64_t entered_ns,tool_start_ns,tool_end_ns,joined_ns,returned_ns,first_output_ns;
    uint64_t observed,submitted,overwritten,body_bytes,evaluated,tokenized,updates,replayed;
    uint64_t front_ns,prefill_ns;
    TestRunTrace process;
    uint64_t owner_queue_ns,owner_work_ns,first_native_ns,last_native_ns;
    uint64_t consumer_threads,consumer_launch_ns;
    uint64_t decode_ns,sync_ns,decode_cpu_ns,sync_cpu_ns,decode_calls,sync_calls;
    cxf_result stream_rc,tool_rc;
    bool enabled,cancelled;
} AgentToolOutputReport;
/* Resident-only real test.run channel. Both settings use body-first/status-tail
 * receipts; enabled adds bounded latest-only prefill. No tool retries. */
cxf_result agent_tool_prepare_output(AgentToolPrepare *,const TestRunCfg *,bool,
    const char *,uint32_t,char *,uint32_t,uint32_t *,AgentToolOutputReport *);
#ifdef __cplusplus
}
#endif
#endif
