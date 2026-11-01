#ifndef CXF_TYPE_H
#define CXF_TYPE_H

#include "base.h"
#include "alloc.h"
#include "arena.h"

/*
 * 类型树：把 C 结构体描述成一棵树，叶子是原子类型或指针。
 *
 * 存在理由是自动化 SoA —— 手写列定义在有嵌套的结构体上很快失控，
 * 而结构体里的偏移与大小都是编译期常量，可以让工具代劳。
 *
 * 不变式（全部在 cxf_type_check 中断言）：
 *   Y1  kind != STRUCT 的类型，field_count == 0 且 fields == NULL
 *   Y2  kind == STRUCT 的类型，fields != NULL 且 field_count > 0
 *   Y3  kind == PTR 时，target 可以为 NULL（void*）或指向某个类型。
 *       注意：检查不递归 target —— 指针可以自引用（struct A { A *next; }），
 *       递归会成环。target 的合法性由生成器保证，运行时不验证。
 *   Y4  展平后 leaf_count == 该类型的叶子数，且每个叶子的 offset + size <= type->size
 *
 * 终止性：递归展开必定停。C 的 struct 不能含自身实例（不完整类型），
 * 自引用只能经指针，因此类型图是有限深度的有向无环图。
 * 见 docs/type-layout.md 第 3 节 —— 这是不需要深度上限的理由。
 *
 * 复杂度：展平 O(叶子数)，装配期一次。查询按 path 是线性扫描，
 * 同样只在装配期用；热路径直接用展平结果的列号。
 *
 * 不适用：不支持 union（活跃成员不可知）、位域（offsetof 未定义）、
 * 柔性数组（大小不可知）。这些在生成期拒绝，不静默跳过。
 */

typedef enum cxf_kind {
    CXF_K_I8, CXF_K_U8, CXF_K_I16, CXF_K_U16,
    CXF_K_I32, CXF_K_U32, CXF_K_F32,
    CXF_K_I64, CXF_K_U64, CXF_K_F64,
    CXF_K_PTR,      /* 叶子：递归到此终止，不追踪指向的内容 */
    CXF_K_STRUCT,   /* 内部节点 */
} cxf_kind;

typedef struct cxf_type cxf_type;

typedef struct cxf_field {
    const char     *name;
    uint32_t        offset;   /* offsetof(宿主, 字段) */
    const cxf_type *type;
} cxf_field;

struct cxf_type {
    const char     *name;
    uint32_t        size;
    uint32_t        align;
    cxf_kind        kind;
    uint32_t        field_count;
    const cxf_field *fields;  /* Y2 */
    const cxf_type  *target;  /* Y3 */
};

/* 展平后的一个叶子，即 SoA 的一列。 */
typedef struct cxf_leaf {
    uint32_t        offset;   /* 在原结构体内的绝对字节偏移 */
    const cxf_type *type;
    uint32_t        col;      /* 列号，与 cxf_soa 的列一一对应 */
    const char     *path;     /* "pos.x"，供诊断与按名查询，热路径不用 */
} cxf_leaf;

typedef struct cxf_layout {
    const cxf_type *type;
    cxf_leaf       *leaves;
    uint32_t        leaf_count;
    cxf_arena       paths;   /* path 字符串都从这里分配，release 时一次归还 */
} cxf_layout;

/*
 * 展平。分配 leaves 与 path 字符串，均由 alloc 持有，release 时统一归还。
 * 装配期调用一次；热路径用结果的 col 号，不再查 path。
 */
cxf_result cxf_layout_build(cxf_layout *l, const cxf_type *t, cxf_alloc *a);
void       cxf_layout_release(cxf_layout *l, cxf_alloc *a);

/* 按路径查找叶子，如 "pos.x"。找不到返回 NULL。线性扫描，装配期用。 */
const cxf_leaf *cxf_layout_find(const cxf_layout *l, const char *path);

/* 按大小或种类筛选叶子，写入 out，返回命中数。这是"树形筛选"的查询接口。 */
uint32_t cxf_layout_select_size(const cxf_layout *l, uint32_t size,
                                const cxf_leaf **out, uint32_t cap);
uint32_t cxf_layout_select_kind(const cxf_layout *l, cxf_kind k,
                                const cxf_leaf **out, uint32_t cap);

typedef struct cxf_world cxf_world;   /* 定义在 world.h */

/*
 * 注册期：把编译期常量接到运行时查找上。
 *
 * 类型描述符本身是静态常量（生成物里的 `const cxf_type`），编译期就存在。
 * 但"给我 Particle 的描述符"这件事在编译期没法表达 —— 除非到处 extern 声明，
 * 那等于把模块之间的名字写死，热插拔就没了。注册期解决的就是这一米的距离：
 * 装配期把常量登记进 world，模块用字符串问一次，拿到指针存进 state，
 * 从此调用期再没有查找。
 *
 * 三阶段纪律（与 service.h 同源，那里有完整论证）：
 *   注册期  cxf_type_register / register_bundle，允许字符串查找，慢无所谓
 *   固化    模块 init 里 find 一次，结果存进 state
 *   调用期  直接用 state 里的指针，零查找
 *
 * 所有权：world 只存指针，不拥有类型。描述符的存储是静态常量，
 * 生命周期长于 world，因此 release 不 free 它们，也永远不会失效（K3）。
 *
 * 与模块表的关键差别：类型不卸载。
 * 模块能卸载是因为它的 state 是运行时分配的，卸掉就回收；
 * 类型的存储压根不在 world 里，"卸载一个类型"等于让一个结构体定义消失，
 * 那没有意义。运行时加载带来的是新类型，不是旧类型的消失。
 *
 * 不变式：
 *   K1 同名不注册两次 —— 即使指针相同也报错。两个 bundle 导出同名类型
 *      是链接期就该发现的错误，静默接受会掩盖它
 *   K2 find 只按名字精确匹配，不做前缀、模糊、版本协商
 *   K3 注册后指针在 world 生命周期内稳定；条目只增不减
 */
typedef struct cxf_type_entry {
    const char     *name;
    const cxf_type *type;
} cxf_type_entry;

/*
 * 成组注册项。由生成器产出（见 tools/gen_registry.py），是静态常量表。
 * 手写一个头文件里有几百个类型时不现实 —— 注册代码必须和描述符同源生成。
 */
typedef struct cxf_type_bundle {
    const cxf_type_entry *entries;
    uint32_t              count;
    const char           *source;   /* 只用于诊断：来自哪个头文件 */
} cxf_type_bundle;

/* 装配期注册。重名返回 CXF_ERR_RANGE（K1），OOM 返回 CXF_ERR_NOMEM。 */
cxf_result cxf_type_register(cxf_world *w, const char *name, const cxf_type *t);

/*
 * 整组注册。先整体校验再提交 —— 条目只增不减（K3），
 * 不允许失败时留下半张表，否则调用方无从判断哪个注册了。
 */
cxf_result cxf_type_register_bundle(cxf_world *w, const cxf_type_bundle *b);

/*
 * 按名字查找，找不到返回 NULL。
 * 只在装配期调用：它是线性 strcmp，不是给热路径用的 —— 见三阶段纪律。
 */
const cxf_type *cxf_type_find(const cxf_world *w, const char *name);

/*
 * 生成器的标记。CXF_TYPE(T) 展开成一个真实的 typedef，而不是空宏。
 *
 * 这是故意的：名字写错时编译期就报错，生成器不会静默跳过一个类型。
 * 标记本身不携带任何布局信息 —— tools/gen_registry.py 只从源码文本里
 * 取出名字，结构体内部交给 clang 的 AST 解析。两边对不上就是编译错误，
 * 不需要生成器自己去校验一致性。
 */
#define CXF_TYPE(T) typedef T T##_cxf_type

/* 校验 Y1-Y4。NDEBUG 下编译为常量 true。 */
#ifndef NDEBUG
bool cxf_type_check(const cxf_type *t);
#else
#define cxf_type_check(t) ((void)(t), true)
#endif

#endif
