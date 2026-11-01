#include <stdlib.h>

#include "cxf/alloc.h"

/* 契约与不变式见 alloc.h。 */

/* 对齐规范到 >= 16 且为 2 的幂。不满足是调用方错误，debug 下直接炸。 */
static size_t norm_align(size_t align) {
    size_t a = align < CXF_MIN_ALIGN ? CXF_MIN_ALIGN : align;
    CXF_ASSERT(CXF_IS_POW2(a));
    return a;
}

/*
 * 统计在这里统一更新，因此底层实现不必关心记账（A4）。
 * 失败时不碰任何计数器，调用方可以 rely on "失败 = 状态不变"。
 */
void *cxf_alloc_alloc(cxf_alloc *a, size_t size, size_t align) {
    void *p;
    CXF_ASSERT(a != NULL && a->alloc != NULL);
    p = a->alloc(a->ctx, size, norm_align(align));
    if (p == NULL) return NULL;

    a->stat.alloc_count++;
    a->stat.bytes_live += size;
    a->stat.bytes_total += size;
    if (a->stat.bytes_live > a->stat.bytes_peak)
        a->stat.bytes_peak = a->stat.bytes_live;
    return p;
}

/* size 必须与实际分配时一致（A2），否则 bytes_live 会漂移成错误值。 */
void cxf_alloc_free(cxf_alloc *a, void *ptr, size_t size) {
    CXF_ASSERT(a != NULL && ptr != NULL);
    a->free(a->ctx, ptr, size);
    a->stat.free_count++;
    a->stat.bytes_live -= size;
}

void cxf_alloc_stat_reset(cxf_alloc *a) {
    a->stat.alloc_count = 0;
    a->stat.free_count = 0;
    a->stat.bytes_live = 0;
    a->stat.bytes_peak = 0;
    a->stat.bytes_total = 0;
}

static void *heap_alloc(void *ctx, size_t size, size_t align) {
    (void)ctx;
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0) return NULL;
    return p;
}

static void heap_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

/*
 * 进程级单例。stat 的读改写在多线程下无保护 —— 有意如此：
 * 加锁会让每个分配都付一次原子操作，而正确做法是每个线程持有一个自己的
 * cxf_alloc。见 alloc.h 中 cxf_alloc_heap 的说明。
 */
static cxf_alloc g_heap = { heap_alloc, heap_free, NULL, { 0, 0, 0, 0, 0 } };

cxf_alloc *cxf_alloc_heap(void) {
    return &g_heap;
}
