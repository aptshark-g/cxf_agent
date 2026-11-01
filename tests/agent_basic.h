#ifndef AGENT_BASIC_H
#define AGENT_BASIC_H

#include "agent_caps.h"

/* Basic workspace tools; all roots and executable commands belong to deployment.
 * fs.read args: relative path, optionally followed by '\n' and a byte offset.
 * fs.list args: relative directory ('.' for root).
 * test.run args: empty; the model cannot select a command or its working directory.
 * Read/list reject symlink components. test.run is an opt-in process capability,
 * not an OS sandbox: the configured command runs with the caller's permissions.
 */
#define BASIC_READ_BYTES 4096u
#define BASIC_LIST_MAX   128u
#define BASIC_OUT_HINT   8192u

typedef struct BasicFsCfg { const char *root; } BasicFsCfg;
typedef struct TestRunCfg {
    const char *root;
    const char *command;
    uint32_t timeout_ms; /* 0 = 30 seconds */
} TestRunCfg;
/* Optional parent-process timestamps. Observation never executes a command
 * again and never invokes a callback from the fork child. */
typedef struct TestRunTrace {
    uint64_t entered_ns, spawn_begin_ns, spawn_end_ns;
    uint64_t first_output_ns, reaped_ns, finished_ns;
    int backend, launch_error;
    uint64_t eof_ns,eof_poll_ns,eof_polls,callback_ns;
    uint64_t total_output_bytes;
    uint32_t retained_output_bytes;
} TestRunTrace;
typedef enum TestRunBackend { TEST_RUN_AUTO=0, TEST_RUN_FORK=1, TEST_RUN_SPAWN=2 } TestRunBackend;
/* Trusted synchronous parent observer; no callbacks in the fork child. */
typedef void (*TestRunProgressFn)(void *,const TestRunTrace *);
/* Streaming channel: bounded retained/sanitized stdout prefix, borrowed only
 * during callback. Same command/timeout/once semantics; final status is a tail. */
typedef void (*TestRunOutputFn)(void *,const char *,uint32_t,bool final);
cxf_result test_run_exec_output(const void *,const char *,uint32_t,char *,uint32_t,uint32_t *,
                               TestRunTrace *,TestRunOutputFn,void *);

/* Optional macOS exit-event wait; portable fallback retains polling. */
cxf_result test_run_exec_output_event(const void *,const char *,uint32_t,char *,uint32_t,uint32_t *,
                               TestRunTrace *,TestRunOutputFn,void *);

/* Exit-in-progress ESRCH/NOTE_EXIT: bounded 1ms retry, otherwise event wait. */
cxf_result test_run_exec_output_race(const void *,const char *,uint32_t,char *,uint32_t,uint32_t *,
                               TestRunTrace *,TestRunOutputFn,void *);

cxf_result basic_read_exec(const void *, const char *, uint32_t,
                           char *, uint32_t, uint32_t *);
cxf_result basic_list_exec(const void *, const char *, uint32_t,
                           char *, uint32_t, uint32_t *);
cxf_result test_run_exec(const void *, const char *, uint32_t,
                         char *, uint32_t, uint32_t *);
cxf_result test_run_exec_traced(const void *, const char *, uint32_t,
                                char *, uint32_t, uint32_t *, TestRunTrace *);
cxf_result test_run_exec_observed(const void *,const char *,uint32_t,char *,uint32_t,uint32_t *,
    TestRunTrace *,TestRunBackend,TestRunProgressFn,void *);

extern const IfaceDef basic_iface_view;
extern const IfaceDef basic_iface_test;
extern const CapDef basic_capdef_read, basic_capdef_list, basic_capdef_test;
extern const CapImpl basic_impl_read, basic_impl_list, basic_impl_test;

#endif
