#ifndef AGENTCTL_CONFIG_H
#define AGENTCTL_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "agent_llm.h"

#define CTL_CFG_URL_CAP 256u
#define CTL_CFG_PATH_CAP 1024u

enum {
    CTL_CFG_API_URL     = 1u << 0,
    CTL_CFG_API_MODEL   = 1u << 1,
    CTL_CFG_API_KEY     = 1u << 2,
    CTL_CFG_API_TIMEOUT = 1u << 3,
    CTL_CFG_BUDGET      = 1u << 4,
    CTL_CFG_ROUNDS      = 1u << 5,
    CTL_CFG_CONC        = 1u << 6,
    CTL_CFG_RPM         = 1u << 7
};

typedef struct CtlFileConfig {
    uint32_t present;
    char url[CTL_CFG_URL_CAP];
    char model[AGENT_LLM_MODEL_CAP];
    char key[AGENT_LLM_KEY_CAP];
    uint32_t timeout_ms;
    uint32_t budget;
    uint32_t rounds;
    uint32_t conc;
    uint32_t rpm;
} CtlFileConfig;

typedef enum CtlConfigReadResult {
    CTL_CONFIG_OK = 0,
    CTL_CONFIG_NOT_FOUND,
    CTL_CONFIG_ERROR
} CtlConfigReadResult;

/* 完整读取并严格解析；失败时 out 不可使用，err 说明字节偏移或 I/O 原因。 */
CtlConfigReadResult ctl_config_read(const char *path, CtlFileConfig *out,
                                    char *err, uint32_t err_cap);

#endif
