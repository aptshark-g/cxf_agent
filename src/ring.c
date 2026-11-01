#include <string.h>

#include "cxf/ring.h"

/* 内存序与不变式见 ring.h，这里只写环形折返的机制。 */

/*
 * const 版本需要 cast：C11 的 atomic_load_explicit 只接受非 const 的
 * _Atomic 指针，而 size/peek 语义上是只读的。
 * 这是唯一一处为接口干净而做的强制转换。
 */
static _Atomic uint32_t *head_of(const cxf_ring *r) {
    return (_Atomic uint32_t *)&r->head;
}

static _Atomic uint32_t *tail_of(const cxf_ring *r) {
    return (_Atomic uint32_t *)&r->tail;
}

cxf_result cxf_ring_init(cxf_ring *r, cxf_alloc *alloc, uint32_t cap) {
    uint32_t c = 1;

    memset(r, 0, sizeof(*r));
    CXF_ASSERT(cap > 0);

    while (c < cap) c <<= 1;
    r->buf = cxf_alloc_alloc(alloc, c, 1);
    if (r->buf == NULL) return CXF_ERR_NOMEM;

    r->cap = c;
    r->mask = c - 1;
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    return CXF_OK;
}

void cxf_ring_release(cxf_ring *r, cxf_alloc *alloc) {
    if (r->buf != NULL) cxf_alloc_free(alloc, r->buf, r->cap);
    memset(r, 0, sizeof(*r));
}

/*
 * 写入。acquire 读 tail 拿到读端最新位置，release 写 head 发布数据 ——
 * 顺序不能反：先写数据再发布 head，否则读端可能看到 head 已推进而数据未落。
 * 环形折返时拆成两段 memcpy，尾部一段 + 头部一段。
 */
uint32_t cxf_ring_write(cxf_ring *r, const void *src, uint32_t len) {
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(tail_of(r), memory_order_acquire);
    uint32_t space = r->cap - (head - tail);
    uint32_t n = len < space ? len : space;
    uint32_t off, first;

    if (n == 0 || src == NULL) return 0;

    off = head & r->mask;
    first = r->cap - off;
    if (first > n) first = n;

    memcpy(r->buf + off, src, first);
    if (n > first) memcpy(r->buf, (const uint8_t *)src + first, n - first);

    atomic_store_explicit(&r->head, head + n, memory_order_release);
    return n;
}

uint32_t cxf_ring_read(cxf_ring *r, void *dst, uint32_t len) {
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(head_of(r), memory_order_acquire);
    uint32_t size = head - tail;
    uint32_t n = len < size ? len : size;
    uint32_t off, first;

    if (n == 0 || dst == NULL) return 0;

    off = tail & r->mask;
    first = r->cap - off;
    if (first > n) first = n;

    memcpy(dst, r->buf + off, first);
    if (n > first) memcpy((uint8_t *)dst + first, r->buf, n - first);

    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
    return n;
}

/* 与 read 同，但不推进 tail。用于"先读长度头、再决定读多少"的两段式协议。 */
uint32_t cxf_ring_peek(const cxf_ring *r, void *dst, uint32_t len) {
    uint32_t tail = atomic_load_explicit(tail_of(r), memory_order_relaxed);
    uint32_t head = atomic_load_explicit(head_of(r), memory_order_acquire);
    uint32_t size = head - tail;
    uint32_t n = len < size ? len : size;
    uint32_t off, first;

    if (n == 0 || dst == NULL) return 0;

    off = tail & r->mask;
    first = r->cap - off;
    if (first > n) first = n;

    memcpy(dst, r->buf + off, first);
    if (n > first) memcpy((uint8_t *)dst + first, r->buf, n - first);
    return n;
}

uint32_t cxf_ring_size(const cxf_ring *r) {
    return atomic_load_explicit(head_of(r), memory_order_acquire)
         - atomic_load_explicit(tail_of(r), memory_order_acquire);
}

uint32_t cxf_ring_space(const cxf_ring *r) {
    return r->cap - cxf_ring_size(r);
}

#ifndef NDEBUG
/* 守住 ring.h 里声明的 G1-G3。NDEBUG 下整个不编译。 */
bool cxf_ring_check(const cxf_ring *r) {
    if (r == NULL) return false;
    if (r->cap == 0 || !CXF_IS_POW2(r->cap)) return false;   /* G1 */
    if (r->mask != r->cap - 1) return false;
    if (cxf_ring_size(r) > r->cap) return false;             /* G2 */
    return true;
}
#endif
