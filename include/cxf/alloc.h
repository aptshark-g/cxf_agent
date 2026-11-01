#ifndef CXF_ALLOC_H
#define CXF_ALLOC_H

#include "base.h"

/*
 * 内核里唯一允许出现函数指针的地方。
 * 理由：热路径根本不走它 —— 热路径走 arena 的 bump，一次指针加法。
 * 它只在大块分配和 release 时被调用，间接跳转成本可以忽略。
 * 若将来发现热路径上出现 cxf_alloc_alloc，那是设计出错，不是优化点。
 *
 * 不变式：
 *   A1  alloc 返回的指针至少按 align 对齐；align 会向上规范到 >= 16 且为 2 的幂
 *   A2  free 必须与 alloc 成对，且传入相同的 size —— size 参与统计，传错会污染 stat
 *   A3  bytes_live == bytes_total - 已释放量；bytes_peak >= bytes_live 恒成立
 *   A4  alloc 失败返回 NULL，不改变任何 stat
 *
 * stat 是白盒性的第一层：任何时候都能回答"现在活了多少字节、峰值多少、累计多少"。
 * 它带来的成本是每次分配 3 次计数器更新 + 1 次比较，实测在热路径上不可忽略
 * （arena 因此把统计移出了热路径，见 arena.h）。这里不优化，因为它不在热路径上。
 *
 * 不适用：不做内存池、不做线程缓存、不做越界检测。需要这些请换底层实现，
 * 接口不变 —— 这正是把它做成函数指针而非直接调 malloc 的原因。
 */
typedef struct cxf_alloc_stat {
    uint64_t alloc_count;  /* 累计成功分配次数 */
    uint64_t free_count;   /* 累计释放次数 */
    uint64_t bytes_live;   /* 当前存活字节，见 A3 */
    uint64_t bytes_peak;   /* bytes_live 的历史最大值，单调不减 */
    uint64_t bytes_total;  /* 累计分配字节，单调不减 */
} cxf_alloc_stat;

typedef struct cxf_alloc {
    void *(*alloc)(void *ctx, size_t size, size_t align);
    void (*free)(void *ctx, void *ptr, size_t size);
    void *ctx;
    cxf_alloc_stat stat;
} cxf_alloc;

/*
 * 分配 size 字节。align 会被规范（见 A1）。
 * 返回 NULL 表示底层失败，此时 stat 不变（A4）。
 * size 为 0 的行为由底层实现定义，本层不拦截。
 */
void  *cxf_alloc_alloc(cxf_alloc *a, size_t size, size_t align);

/* 释放。ptr 必须来自本 allocator 的 alloc，size 必须与当时一致（A2）。 */
void   cxf_alloc_free(cxf_alloc *a, void *ptr, size_t size);

/* 清零五个计数器。bytes_live 不清零会让后续峰值统计失真。 */
void   cxf_alloc_stat_reset(cxf_alloc *a);

/*
 * 进程级堆分配器，返回静态单例。
 * 它不是线程安全的：stat 的更新是无保护的读改写。
 * 多线程场景下给每个线程一个自己的 cxf_alloc，stat 各自独立。
 */
cxf_alloc *cxf_alloc_heap(void);

#endif
