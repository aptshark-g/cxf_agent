#ifndef CXF_RING_H
#define CXF_RING_H

#include <stdatomic.h>
#include "base.h"
#include "alloc.h"

/*
 * 单写单读无锁字节流 FIFO。
 *
 * 不变式（全部在 cxf_ring_check 中断言）：
 *   G1  cap 是 2 的幂，mask == cap - 1
 *   G2  size == head - tail（无符号减法），且 size <= cap
 *   G3  head 只由写端更新，tail 只由读端更新，两者单调递增
 *
 * head/tail 单调递增，索引时才取掩码。好处有两个：
 *   一是 size/space 一次减法得出，无需考虑回绕；
 *   二是 32 位回绕安全 —— 无符号差值在跨越 2^32 时依然正确，
 *   前提是读写速率差不超过 2^32 字节，实际不可能。
 * 由此 cap 字节全部可用，不像"留一格判满空"的写法浪费一个槽位。
 *
 * 内存序：写端读 tail 用 acquire、写 head 用 release；读端读 head 用 acquire、
 * 写 tail 用 release。这保证读端看到数据时，写端的数据写入一定已可见。
 * relaxed 只用于读自己那一端 —— 只有自己会改它。
 *
 * head 与 tail 各自 _Alignas 到独立 cache line。这不是优化而是正确性：
 * 同处一行会在多核下造成 false sharing，代价是数量级的。
 *
 * 不适用：
 *   不是 MPMC。多写者或多读者必须在外层加锁，或改用每个写者一条 ring。
 *   不做消息边界。ring 是字节流，长度头归上层 —— 边界判定放到内核会
 *   把一种消息格式强加给所有使用者，那是模块该干的事。
 */
typedef struct cxf_ring {
    uint8_t *buf;
    uint32_t cap;
    uint32_t mask;

    _Alignas(CXF_CACHE_LINE) _Atomic uint32_t head;  /* 写端独占，见 G3 */
    _Alignas(CXF_CACHE_LINE) _Atomic uint32_t tail;  /* 读端独占 */
} cxf_ring;

/* 建 ring。cap 向上取到 2 的幂，实际容量可用 cxf_ring_space 查询。 */
cxf_result cxf_ring_init(cxf_ring *r, cxf_alloc *alloc, uint32_t cap);
void       cxf_ring_release(cxf_ring *r, cxf_alloc *alloc);

/*
 * 写入至多 len 字节，返回实际写入数，可能为 0（空间不足）。
 * 部分写入是允许的：返回 n < len 表示只写进了 n 字节，调用方需自行处理。
 * 单写者语义：同一时刻只能有一个线程调用本函数。
 */
uint32_t   cxf_ring_write(cxf_ring *r, const void *src, uint32_t len);

/* 读出至多 len 字节，返回实际读出数。单读者语义。 */
uint32_t   cxf_ring_read(cxf_ring *r, void *dst, uint32_t len);

/* 读但不推进 tail。用于先看长度头再决定读多少。 */
uint32_t   cxf_ring_peek(const cxf_ring *r, void *dst, uint32_t len);

uint32_t   cxf_ring_size(const cxf_ring *r);
uint32_t   cxf_ring_space(const cxf_ring *r);

/* 校验 G1-G3，测试与调试期调用。NDEBUG 下编译为常量 true。 */
#ifndef NDEBUG
bool cxf_ring_check(const cxf_ring *r);
#else
#define cxf_ring_check(r) ((void)(r), true)
#endif

#endif
