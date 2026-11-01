#include <string.h>

#include "cxf/bitmap.h"

/* 契约与不变式见 bitmap.h，这里只写位技巧上的取舍。 */

#if defined(__GNUC__) || defined(__clang__)
#define CXF_CTZ64(x)      ((uint32_t)__builtin_ctzll(x))
#define CXF_POPCNT64(x)   ((uint32_t)__builtin_popcountll(x))
#else
static uint32_t ctz64_fallback(uint64_t x) {
    uint32_t n = 0;
    while ((x & 1u) == 0) { x >>= 1; n++; }
    return n;
}
static uint32_t popcnt64_fallback(uint64_t x) {
    uint32_t n = 0;
    while (x != 0) { x &= x - 1u; n++; }  /* Kernighan */
    return n;
}
#define CXF_CTZ64(x)    ctz64_fallback(x)
#define CXF_POPCNT64(x) popcnt64_fallback(x)
#endif

void cxf_bitmap_init(cxf_bitmap *b, void *mem, uint32_t nbits) {
    CXF_ASSERT(mem != NULL || nbits == 0);
    CXF_ASSERT(((uintptr_t)mem & 7u) == 0);   /* 字对齐是调用方的义务 */

    b->w = (uint64_t *)mem;
    b->nbits = nbits;
    b->nwords = (nbits + 63u) / 64u;
    cxf_bitmap_clear(b);
}

void cxf_bitmap_clear(cxf_bitmap *b) {
    if (b->nwords != 0) memset(b->w, 0, (size_t)b->nwords * 8u);
}

bool cxf_bitmap_get(const cxf_bitmap *b, uint32_t i) {
    CXF_ASSERT(i < b->nbits);
    return ((b->w[i >> 6] >> (i & 63u)) & 1u) != 0;
}

void cxf_bitmap_set(cxf_bitmap *b, uint32_t i) {
    CXF_ASSERT(i < b->nbits);   /* M2：越界不写，断言兜住 */
    b->w[i >> 6] |= (uint64_t)1u << (i & 63u);
}

void cxf_bitmap_unset(cxf_bitmap *b, uint32_t i) {
    CXF_ASSERT(i < b->nbits);
    b->w[i >> 6] &= ~((uint64_t)1u << (i & 63u));
}

uint32_t cxf_bitmap_popcount(const cxf_bitmap *b) {
    uint32_t n = 0, i;

    for (i = 0; i < b->nwords; i++) n += CXF_POPCNT64(b->w[i]);
    return n;
}

/*
 * 首字先掩掉 from 之前的位，再逐字扫 —— 命中在首字时只做两条指令。
 * 循环条件写作"m == 0"而不是"逐位判断"，是为了让编译器把整个字当作条件。
 */
uint32_t cxf_bitmap_next(const cxf_bitmap *b, uint32_t from) {
    uint32_t i, r;
    uint64_t m;

    if (from >= b->nbits) return CXF_NO_INDEX;

    i = from >> 6;
    m = b->w[i] & (~(uint64_t)0 << (from & 63u));
    while (m == 0) {
        if (++i >= b->nwords) return CXF_NO_INDEX;
        m = b->w[i];
    }
    r = (i << 6) + CXF_CTZ64(m);
    return r < b->nbits ? r : CXF_NO_INDEX;
}

uint32_t cxf_bitmap_first_zero(const cxf_bitmap *b, uint32_t from) {
    uint32_t i, r;
    uint64_t m;

    if (from >= b->nbits) return CXF_NO_INDEX;

    i = from >> 6;
    m = ~b->w[i] & (~(uint64_t)0 << (from & 63u));
    while (m == 0) {
        if (++i >= b->nwords) return CXF_NO_INDEX;
        m = ~b->w[i];
    }
    r = (i << 6) + CXF_CTZ64(m);
    return r < b->nbits ? r : CXF_NO_INDEX;   /* 末字尾部超界的 0 位不算 */
}

#ifndef NDEBUG
bool cxf_bitmap_check(const cxf_bitmap *b) {
    uint32_t tail, i;

    if (b->nbits == 0) return b->nwords == 0;
    if (b->w == NULL) return false;
    if (b->nwords != (b->nbits + 63u) / 64u) return false;   /* M1 */

    /* M2：末字里超出 nbits 的位必须为 0 */
    tail = b->nbits & 63u;
    if (tail != 0) {
        uint64_t mask = ~(uint64_t)0 << tail;
        if ((b->w[b->nwords - 1] & mask) != 0) return false;
    }
    (void)i;
    return true;
}
#endif
