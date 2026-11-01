#ifndef CXF_BLOB_H
#define CXF_BLOB_H

#include "alloc.h"
#include "base.h"
#include "str.h"
#include "table.h"

/*
 * 变长数据池：table 提供标识与生命周期，buf 的规则提供增长。
 *
 * 这是 §2.1 里"所有权交接"的落地形态：SoA 行必须定宽，变长数据只能 handle 指出去；
 * 于是"谁拥有这段字节"必须由协议说清，而不是由指针图说清。
 *   new（创建方持有） → handle 随 WorkItem 传递（所有权转移） → free 一次（集中回收）
 * 单一所有者、无引用计数：所有权方向由数据流决定，因此**所有权图是 DAG**，
 * 不存在"互相持有"的环 —— 这不是纪律，是形状（构造保证 > 运行时检测 > 约定）。
 *
 * 为什么用 table 而不是数组：需要 handle（失效可检测）+ 槽位复用（长跑不泄漏）。
 * 为什么每条目是一列 AoS 而不是拆成 p/len/cap 三列：条目只有 16 字节，
 * 而访问单位永远是"整条目"；拆三列只多两次列基址计算，还让 ent_reserve 没法原地改。
 *
 * 不变式：
 *   L1  每个存活条目的 cap >= len；cap > 0 时 p != NULL
 *   L2  bytes_live == 所有存活条目 cap 之和（记容量不记长度：占内存的是容量）
 *   L3  bytes_peak >= bytes_live，单调不减
 *   L4  free 之后该 handle 立即失效（gen 翻转），二次 free 必返 CXF_ERR_BAD_HANDLE
 *   L5  条目的 p 在 reserve 之后才可能变；调用方不得跨增长持有 p
 *
 * 复杂度：new/free O(1)（摊还）；append 摊还 O(1)；view O(1)；check O(rows)。
 *
 * 不适用：不做引用计数，不共享所有权（要共享请共享 handle 并约定单一回收点）；
 * 不做碎片整理（容量只增不减，直到 free）；不保证同一 handle 的 p 地址稳定。
 */
typedef struct cxf_blob_ent {
    uint8_t *p;
    uint32_t len;
    uint32_t cap;
} cxf_blob_ent;

typedef struct cxf_blobpool {
    cxf_table t;
    uint64_t  bytes_live;
    uint64_t  bytes_peak;
    uint32_t  count;      /* 存活条目数，便于和 table.count 对账 */
} cxf_blobpool;

/*
 * class_id 写进本池产出的 handle（见 handle.h H3）：把 blob 的 handle 喂进 items 表
 * 会被确定性拒绝，而不是"index 撞上就静默成功"。
 */
cxf_result cxf_blobpool_init(cxf_blobpool *bp, cxf_alloc *alloc,
                             uint32_t cap, uint32_t class_id);
void       cxf_blobpool_release(cxf_blobpool *bp);

/* 创建一个空条目，容量至少 hint。失败返回 gen == 0 的无效 handle。 */
cxf_handle cxf_blob_new(cxf_blobpool *bp, uint32_t hint);

/* 追加 / 改写。失败时条目内容不变（L1/L5）。 */
cxf_result cxf_blob_append(cxf_blobpool *bp, cxf_handle h, const void *src, uint32_t n);
cxf_result cxf_blob_assign(cxf_blobpool *bp, cxf_handle h, const void *src, uint32_t n);

/* 只读视图。handle 失效返回空切片（与 cxf_table_get 同一条：失效是正常事件不断言）。 */
cxf_str    cxf_blob_view(const cxf_blobpool *bp, cxf_handle h);

/* 归还。二次 free 返回 CXF_ERR_BAD_HANDLE（L4）。 */
cxf_result cxf_blob_free(cxf_blobpool *bp, cxf_handle h);

static inline uint64_t cxf_blobpool_bytes(const cxf_blobpool *bp) {
    return bp->bytes_live;
}

#ifndef NDEBUG
bool cxf_blobpool_check(const cxf_blobpool *bp);
#else
#define cxf_blobpool_check(bp) ((void)(bp), true)
#endif

#endif
