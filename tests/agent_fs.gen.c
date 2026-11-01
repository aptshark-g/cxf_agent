/* 由 tools/gen_registry.py 生成，勿手改。源：agent_fs.c */

#include "cxf/service.h"

/* 服务注册项：符号由 CXF_SERVICE 定义，此处只引用。 */
extern const cxf_service_def cxf_svc_iface_fs_hits;
extern const cxf_service_def cxf_svc_capdef_fs_grep;

static const cxf_service_def *const cxf_svc_entries_agent_fs[] = {
    &cxf_svc_iface_fs_hits,
    &cxf_svc_capdef_fs_grep,
};
const cxf_service_bundle cxf_services_agent_fs = { cxf_svc_entries_agent_fs, 2, "agent_fs.c" };
