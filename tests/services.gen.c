/* 由 tools/gen_registry.py 生成，勿手改。源：services.c */

#include "cxf/service.h"

/* 服务注册项：符号由 CXF_SERVICE 定义，此处只引用。 */
extern const cxf_service_def cxf_svc_math_v1;
extern const cxf_service_def cxf_svc_math_v0;
extern const cxf_service_def cxf_svc_log_v1;

static const cxf_service_def *const cxf_svc_entries_services[] = {
    &cxf_svc_math_v1,
    &cxf_svc_math_v0,
    &cxf_svc_log_v1,
};
const cxf_service_bundle cxf_services_services = { cxf_svc_entries_services, 3, "services.c" };
