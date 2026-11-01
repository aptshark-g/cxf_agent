#ifndef CXF_SOA_H
#define CXF_SOA_H

#include "base.h"
#include "alloc.h"
#include "table.h"
#include "type.h"

/*
 * 结构体 → SoA 列的自动布局。建立在 cxf_table 之上，不另造存储。
 *
 * 一个结构体展平成 N 个叶子，每个叶子就是 table 的一列。
 * 于是"实体"这个 AoS 概念在内存里变成 N 条独立的连续数组：
 * 只处理位置的系统只碰位置那几列，不必把整个结构体读进缓存。
 *
 * 不变式（全部在 cxf_soa_check 中断言）：
 *   O1  table.col_count == layout->leaf_count
 *   O2  table 第 i 列的 col_size == leaves[i].type->size
 *
 * 两条访问路径，用途不同：
 *   热路径  cxf_soa_col / cxf_soa_get —— 直接拿列指针或元素地址，无查找
 *   冷路径  cxf_soa_gather / scatter  —— 按 offset 逐字段搬，组装回结构体
 *
 * gather 是便利设施不是主路径。它按叶子数做多次 memcpy，
 * 结构体字段一多就不便宜。让人愿意用它，恰恰会抵消 SoA 的收益，
 * 所以它只在确实需要完整结构体时出现（跨系统边界、序列化、调试）。
 *
 * 不适用：列数受 CXF_TABLE_MAX_COLS 限制（见 table.h）。
 * 展平后超过上限会在 init 时返回 CXF_ERR_RANGE，而不是悄悄截断。
 */

typedef struct cxf_soa {
    const cxf_layout *layout;
    cxf_table         table;
    cxf_alloc        *alloc;
} cxf_soa;

cxf_result cxf_soa_init(cxf_soa *s, const cxf_layout *l, cxf_alloc *a, uint32_t cap);

/* 同 cxf_soa_init，但给底层表指定 class（见 table.h / handle.h H3）。
 * SoA 的 handle 与 table 的 handle 是同一套 —— 不另造标识，否则两套 id 必然对不上。 */
cxf_result cxf_soa_init_class(cxf_soa *s, const cxf_layout *l, cxf_alloc *a,
                              uint32_t cap, uint32_t class_id);

void       cxf_soa_release(cxf_soa *s);

cxf_handle cxf_soa_add(cxf_soa *s);
cxf_result cxf_soa_remove(cxf_soa *s, cxf_handle h);
bool       cxf_soa_live(const cxf_soa *s, cxf_handle h);

/* 热路径：整列基址，用于顺序扫描。 */
void *cxf_soa_col(cxf_soa *s, uint32_t leaf);

/* 热路径：单个元素地址。handle 失效时返回 NULL。 */
void *cxf_soa_get(cxf_soa *s, cxf_handle h, uint32_t leaf);

/* 冷路径：按 offset 组装回宿主结构体 / 反向写入。out 必须指向该结构体。 */
void  cxf_soa_gather(cxf_soa *s, cxf_handle h, void *out);
void  cxf_soa_scatter(cxf_soa *s, cxf_handle h, const void *in);

/* 校验 O1-O2。NDEBUG 下编译为常量 true。 */
#ifndef NDEBUG
bool cxf_soa_check(const cxf_soa *s);
#else
#define cxf_soa_check(s) ((void)(s), true)
#endif

#endif
