#ifndef AGENT_SANDBOX_H
#define AGENT_SANDBOX_H
#include "agent_policy.h"
#define TEST_SANDBOX_GRANTS 4u
#define TEST_SANDBOX_PROFILE_CAP 65536u
/* Zero/default is restricted. No unsupported-platform fallback. */
enum { TEST_SANDBOX_ON=0, TEST_SANDBOX_OFF=1 };
enum { TEST_NET_DENY=0, TEST_NET_LOOPBACK_UNSUPPORTED=1, TEST_NET_ALLOW=2 };
typedef struct TestSandbox {
    uint32_t mode,net,reads,writes;
    const AgentPolicy *policy;
    const char *read[TEST_SANDBOX_GRANTS],*write[TEST_SANDBOX_GRANTS];
    /* Optional paired deployment bindings; environment only, never grants. */
    const char *toolchain_bin,*sdkroot;
} TestSandbox;
typedef struct AgentSandboxJob {
    char profile[TEST_SANDBOX_PROFILE_CAP],tmp[AGENT_POLICY_PATH_CAP];
    char tmpenv[AGENT_POLICY_PATH_CAP+16],homeenv[AGENT_POLICY_PATH_CAP+16],cacheenv[AGENT_POLICY_PATH_CAP+40];
    char pathenv[AGENT_POLICY_PATH_CAP+64],sdkenv[AGENT_POLICY_PATH_CAP+16];
    char *env[512];
    char *argv[8];const char *executable;
    uint32_t used,entries;
} AgentSandboxJob;
/* Sticky per-process protection; call before loading large threaded engines. */
cxf_result agent_sandbox_host_protect(void);
cxf_result agent_sandbox_prepare(AgentSandboxJob *,const TestSandbox *,const char *,const char *,char *const *);
bool agent_sandbox_cleanup(AgentSandboxJob *);
#endif
