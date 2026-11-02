#ifndef AGENT_POLICY_H
#define AGENT_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include "cxf/base.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AGENT_POLICY_PATHS 8u
#define AGENT_POLICY_SECRETS 16u
#define AGENT_POLICY_PATH_CAP 1024u
#define AGENT_POLICY_SECRET_CAP 512u
/* Zero is deliberately unknown, never automatic local trust. */
enum { AGENT_SCOPE_UNKNOWN=0, AGENT_SCOPE_LOCAL=1, AGENT_SCOPE_CLOUD=2 };
enum { AGENT_EGRESS_DENY=0, AGENT_EGRESS_TASK=1, AGENT_EGRESS_WORKSPACE=2 };
enum { AGENT_DATA_TASK=1, AGENT_DATA_WORKSPACE=2 };
typedef struct AgentPolicyFile {
    char path[AGENT_POLICY_PATH_CAP];
    uint64_t dev, inode;
    bool has_inode;
} AgentPolicyFile;
/* Borrowed by tool/provider configs; keep alive and immutable through task/worker drain. */
typedef struct AgentPolicy {
    uint32_t cloud_egress, files, secrets;
    AgentPolicyFile file[AGENT_POLICY_PATHS];
    char secret[AGENT_POLICY_SECRETS][AGENT_POLICY_SECRET_CAP];
} AgentPolicy;
typedef struct AgentPolicyPath { int parent; char leaf[512]; } AgentPolicyPath;
bool agent_policy_protect(AgentPolicy *,const char *);
bool agent_policy_secret(AgentPolicy *,const char *);
bool agent_policy_path(const AgentPolicy *,const char *,const char *,uint32_t,bool);
bool agent_policy_stat(const AgentPolicy *,const struct stat *);
cxf_result agent_policy_parent(const AgentPolicy *,const char *,const char *,uint32_t,AgentPolicyPath *);
int agent_policy_open(const AgentPolicy *,const char *,const char *,uint32_t,bool,int);
uint32_t agent_policy_sensitive_patterns(void);
bool agent_policy_sensitive_regex(uint32_t,char *,uint32_t);
bool agent_policy_text(const AgentPolicy *,const char *,uint32_t);
bool agent_policy_egress(const AgentPolicy *,uint32_t,uint32_t,const char *,uint32_t);
bool agent_policy_loopback(const char *);
#ifdef __cplusplus
}
#endif
#endif
