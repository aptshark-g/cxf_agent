#ifndef NATIVE_CONTEXT_AUDIT_H
#define NATIVE_CONTEXT_AUDIT_H
#include "agent_native.h"
#ifdef CXF_CONTEXT_AUDIT
#ifdef __cplusplus
extern "C" {
#endif
/* Diagnostic-only, never a production callback or retained text view.
 * Counts are logical payload lengths, not allocator bytes. JSON/messages/
 * normalized count content+reasoning bytes only, excluding keys/tool metadata;
 * this avoids allocating copies just to inspect upstream JSON string lengths. */
typedef struct NativeContextAuditEvent {
    const char *stage;
    uint64_t request, json_strings, messages, normalized, rendered, token_scratch;
} NativeContextAuditEvent;
typedef void (*NativeContextAuditFn)(void *, const NativeContextAuditEvent *);
typedef enum NativeContextInputMode {
    NATIVE_INPUT_DIRECT, NATIVE_INPUT_REQUEST_JSON, NATIVE_INPUT_RETAINED_JSON,
    NATIVE_INPUT_JINJA_VALUES
} NativeContextInputMode;
void agent_native_context_audit(AgentNative *, NativeContextAuditFn, void *, NativeContextInputMode);
#ifdef __cplusplus
}
#endif
#endif
#endif
