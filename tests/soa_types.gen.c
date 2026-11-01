/* 由 tools/gen_registry.py 生成，勿手改。源：soa_types.h */

#include "soa_types.h"
#include "cxf/type.h"
#include "cxf/service.h"

/* 内置类型的宽度假设。不符就编译失败，不静默错标。 */
_Static_assert(sizeof(float) == 4, "cxf: float 宽度与 CXF_K_F32 不符");
_Static_assert(sizeof(int) == 4, "cxf: int 宽度与 CXF_K_I32 不符");
_Static_assert(sizeof(unsigned int) == 4, "cxf: unsigned int 宽度与 CXF_K_U32 不符");

/* 结构体描述符的前向声明：自引用与相互引用都要先有名字。 */
extern const cxf_type cxf_type_Vec3;
extern const cxf_type cxf_type_Particle;
extern const cxf_type cxf_type_Node;
extern const cxf_type cxf_type_Wide;

static const cxf_type cxf_bt_float = { "float", (uint32_t)sizeof(float), (uint32_t)_Alignof(float), CXF_K_F32, 0, NULL, NULL };
static const cxf_type cxf_bt_int = { "int", (uint32_t)sizeof(int), (uint32_t)_Alignof(int), CXF_K_I32, 0, NULL, NULL };
static const cxf_type cxf_bt_unsigned_int = { "unsigned int", (uint32_t)sizeof(unsigned int), (uint32_t)_Alignof(unsigned int), CXF_K_U32, 0, NULL, NULL };

static const cxf_type cxf_pt_Node_ptr = { "Node*", (uint32_t)sizeof(struct Node *), (uint32_t)_Alignof(struct Node *), CXF_K_PTR, 0, NULL, &cxf_type_Node };
static const cxf_type cxf_pt_void_ptr = { "void*", (uint32_t)sizeof(void *), (uint32_t)_Alignof(void *), CXF_K_PTR, 0, NULL, NULL };

static const cxf_field cxf_fields_Vec3[] = {
    { "x", (uint32_t)offsetof(Vec3, x), &cxf_bt_float },
    { "y", (uint32_t)offsetof(Vec3, y), &cxf_bt_float },
    { "z", (uint32_t)offsetof(Vec3, z), &cxf_bt_float },
};

static const cxf_field cxf_fields_Particle[] = {
    { "pos", (uint32_t)offsetof(Particle, pos), &cxf_type_Vec3 },
    { "vx", (uint32_t)offsetof(Particle, vx), &cxf_bt_float },
    { "vy", (uint32_t)offsetof(Particle, vy), &cxf_bt_float },
    { "hp", (uint32_t)offsetof(Particle, hp), &cxf_bt_unsigned_int },
    { "owner", (uint32_t)offsetof(Particle, owner), &cxf_pt_void_ptr },
};

static const cxf_field cxf_fields_Node[] = {
    { "v", (uint32_t)offsetof(struct Node, v), &cxf_bt_int },
    { "next", (uint32_t)offsetof(struct Node, next), &cxf_pt_Node_ptr },
};

static const cxf_field cxf_fields_Wide[] = {
    { "f0", (uint32_t)offsetof(Wide, f0), &cxf_bt_unsigned_int },
    { "f1", (uint32_t)offsetof(Wide, f1), &cxf_bt_unsigned_int },
    { "f2", (uint32_t)offsetof(Wide, f2), &cxf_bt_unsigned_int },
    { "f3", (uint32_t)offsetof(Wide, f3), &cxf_bt_unsigned_int },
    { "f4", (uint32_t)offsetof(Wide, f4), &cxf_bt_unsigned_int },
    { "f5", (uint32_t)offsetof(Wide, f5), &cxf_bt_unsigned_int },
    { "f6", (uint32_t)offsetof(Wide, f6), &cxf_bt_unsigned_int },
    { "f7", (uint32_t)offsetof(Wide, f7), &cxf_bt_unsigned_int },
    { "f8", (uint32_t)offsetof(Wide, f8), &cxf_bt_unsigned_int },
    { "f9", (uint32_t)offsetof(Wide, f9), &cxf_bt_unsigned_int },
    { "f10", (uint32_t)offsetof(Wide, f10), &cxf_bt_unsigned_int },
    { "f11", (uint32_t)offsetof(Wide, f11), &cxf_bt_unsigned_int },
    { "f12", (uint32_t)offsetof(Wide, f12), &cxf_bt_unsigned_int },
    { "f13", (uint32_t)offsetof(Wide, f13), &cxf_bt_unsigned_int },
    { "f14", (uint32_t)offsetof(Wide, f14), &cxf_bt_unsigned_int },
    { "f15", (uint32_t)offsetof(Wide, f15), &cxf_bt_unsigned_int },
    { "f16", (uint32_t)offsetof(Wide, f16), &cxf_bt_unsigned_int },
};

const cxf_type cxf_type_Vec3 = { "Vec3", (uint32_t)sizeof(Vec3), (uint32_t)_Alignof(Vec3), CXF_K_STRUCT, 3, cxf_fields_Vec3, NULL };

const cxf_type cxf_type_Particle = { "Particle", (uint32_t)sizeof(Particle), (uint32_t)_Alignof(Particle), CXF_K_STRUCT, 5, cxf_fields_Particle, NULL };

const cxf_type cxf_type_Node = { "Node", (uint32_t)sizeof(struct Node), (uint32_t)_Alignof(struct Node), CXF_K_STRUCT, 2, cxf_fields_Node, NULL };

const cxf_type cxf_type_Wide = { "Wide", (uint32_t)sizeof(Wide), (uint32_t)_Alignof(Wide), CXF_K_STRUCT, 17, cxf_fields_Wide, NULL };

/* 注册期：整组登记进 world，见 type.h 的注册期说明。 */
static const cxf_type_entry cxf_entries_soa_types[] = {
    { "Vec3", &cxf_type_Vec3 },
    { "Particle", &cxf_type_Particle },
    { "Node", &cxf_type_Node },
    { "Wide", &cxf_type_Wide },
};
const cxf_type_bundle cxf_types_soa_types = { cxf_entries_soa_types, 4, "soa_types.h" };
