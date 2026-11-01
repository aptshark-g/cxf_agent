/* 由 tools/gen_registry.py 生成，勿手改。源：agent_types.h */

#include "agent_types.h"
#include "cxf/type.h"
#include "cxf/service.h"

/* 内置类型的宽度假设。不符就编译失败，不静默错标。 */
_Static_assert(sizeof(unsigned char) == 1, "cxf: unsigned char 宽度与 CXF_K_U8 不符");
_Static_assert(sizeof(unsigned int) == 4, "cxf: unsigned int 宽度与 CXF_K_U32 不符");

/* 结构体描述符的前向声明：自引用与相互引用都要先有名字。 */
extern const cxf_type cxf_type_WorkItem;
extern const cxf_type cxf_type_cxf_handle;
extern const cxf_type cxf_type_Message;

static const cxf_type cxf_bt_unsigned_char = { "unsigned char", (uint32_t)sizeof(unsigned char), (uint32_t)_Alignof(unsigned char), CXF_K_U8, 0, NULL, NULL };
static const cxf_type cxf_bt_unsigned_int = { "unsigned int", (uint32_t)sizeof(unsigned int), (uint32_t)_Alignof(unsigned int), CXF_K_U32, 0, NULL, NULL };

static const cxf_field cxf_fields_WorkItem[] = {
    { "cap_id", (uint32_t)offsetof(WorkItem, cap_id), &cxf_bt_unsigned_int },
    { "status", (uint32_t)offsetof(WorkItem, status), &cxf_bt_unsigned_char },
    { "args", (uint32_t)offsetof(WorkItem, args), &cxf_type_cxf_handle },
    { "result", (uint32_t)offsetof(WorkItem, result), &cxf_type_cxf_handle },
    { "parent", (uint32_t)offsetof(WorkItem, parent), &cxf_type_cxf_handle },
    { "pending", (uint32_t)offsetof(WorkItem, pending), &cxf_bt_unsigned_int },
    { "origin_rank", (uint32_t)offsetof(WorkItem, origin_rank), &cxf_bt_unsigned_int },
    { "owner_rank", (uint32_t)offsetof(WorkItem, owner_rank), &cxf_bt_unsigned_int },
    { "trust_max", (uint32_t)offsetof(WorkItem, trust_max), &cxf_bt_unsigned_char },
    { "tries", (uint32_t)offsetof(WorkItem, tries), &cxf_bt_unsigned_char },
    { "rounds", (uint32_t)offsetof(WorkItem, rounds), &cxf_bt_unsigned_int },
    { "seq", (uint32_t)offsetof(WorkItem, seq), &cxf_bt_unsigned_char },
    { "err", (uint32_t)offsetof(WorkItem, err), &cxf_bt_unsigned_char },
};

static const cxf_field cxf_fields_cxf_handle[] = {
    { "index", (uint32_t)offsetof(cxf_handle, index), &cxf_bt_unsigned_int },
    { "gen", (uint32_t)offsetof(cxf_handle, gen), &cxf_bt_unsigned_int },
};

static const cxf_field cxf_fields_Message[] = {
    { "agent", (uint32_t)offsetof(Message, agent), &cxf_type_cxf_handle },
    { "role", (uint32_t)offsetof(Message, role), &cxf_bt_unsigned_char },
    { "tag", (uint32_t)offsetof(Message, tag), &cxf_bt_unsigned_int },
};

const cxf_type cxf_type_WorkItem = { "WorkItem", (uint32_t)sizeof(WorkItem), (uint32_t)_Alignof(WorkItem), CXF_K_STRUCT, 13, cxf_fields_WorkItem, NULL };

const cxf_type cxf_type_cxf_handle = { "cxf_handle", (uint32_t)sizeof(cxf_handle), (uint32_t)_Alignof(cxf_handle), CXF_K_STRUCT, 2, cxf_fields_cxf_handle, NULL };

const cxf_type cxf_type_Message = { "Message", (uint32_t)sizeof(Message), (uint32_t)_Alignof(Message), CXF_K_STRUCT, 3, cxf_fields_Message, NULL };

/* 注册期：整组登记进 world，见 type.h 的注册期说明。 */
static const cxf_type_entry cxf_entries_agent_types[] = {
    { "WorkItem", &cxf_type_WorkItem },
    { "cxf_handle", &cxf_type_cxf_handle },
    { "Message", &cxf_type_Message },
};
const cxf_type_bundle cxf_types_agent_types = { cxf_entries_agent_types, 3, "agent_types.h" };
