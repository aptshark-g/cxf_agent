#ifndef AGENT_NATIVE_MESSAGES_H
#define AGENT_NATIVE_MESSAGES_H
#include "agent_native.h"
#include "chat.h"
/* One output only: cold/upstream messages OR normalized Jinja messages.
 * Visitor borrows transcript; upstream output owns its strings for this call.
 * content_bytes is an optional audit count, not an owned payload. */
cxf_result agent_native_collect_messages(const AgentLlmCfg *, const char *, uint32_t,
    common_chat_templates_inputs *, common_json *, uint64_t *content_bytes = nullptr);
/* Same visitor, directly owned per-call Jinja values; input provenance kept. */
cxf_result agent_native_collect_values(const AgentLlmCfg *, const char *, uint32_t,
    jinja::value_array *, uint64_t *content_bytes = nullptr);
std::string agent_native_flatten_values(const jinja::value &, size_t byte_limit);
/* Internal shared entry: production cache-hit rendering and host overlap tests. */
std::string agent_native_render_values(const common_chat_template &, jinja::value_array, common_json tools, bool thinking=false);
bool agent_native_verified_template(const char *source);
bool agent_native_input_utf8(const char *,uint32_t);
#endif
