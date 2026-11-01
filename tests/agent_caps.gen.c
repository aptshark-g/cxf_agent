/* 由 tools/gen_registry.py 生成，勿手改。源：agent_caps.c */

#include "cxf/service.h"

/* 服务注册项：符号由 CXF_SERVICE 定义，此处只引用。 */
extern const cxf_service_def cxf_svc_iface_task;
extern const cxf_service_def cxf_svc_iface_quote;
extern const cxf_service_def cxf_svc_iface_plan;
extern const cxf_service_def cxf_svc_capdef_travel_train;
extern const cxf_service_def cxf_svc_capdef_market_secondhand;
extern const cxf_service_def cxf_svc_capdef_price_quote;
extern const cxf_service_def cxf_svc_capdef_plan_merge;
extern const cxf_service_def cxf_svc_capimpl_price_quote_llm;
extern const cxf_service_def cxf_svc_capimpl_price_quote_parse;
extern const cxf_service_def cxf_svc_capimpl_travel_train_api;
extern const cxf_service_def cxf_svc_capimpl_market_secondhand_llm;
extern const cxf_service_def cxf_svc_capimpl_plan_merge_rules;

static const cxf_service_def *const cxf_svc_entries_agent_caps[] = {
    &cxf_svc_iface_task,
    &cxf_svc_iface_quote,
    &cxf_svc_iface_plan,
    &cxf_svc_capdef_travel_train,
    &cxf_svc_capdef_market_secondhand,
    &cxf_svc_capdef_price_quote,
    &cxf_svc_capdef_plan_merge,
    &cxf_svc_capimpl_price_quote_llm,
    &cxf_svc_capimpl_price_quote_parse,
    &cxf_svc_capimpl_travel_train_api,
    &cxf_svc_capimpl_market_secondhand_llm,
    &cxf_svc_capimpl_plan_merge_rules,
};
const cxf_service_bundle cxf_services_agent_caps = { cxf_svc_entries_agent_caps, 12, "agent_caps.c" };
