#ifndef CXF_BITMAP_H
#define CXF_BITMAP_H

#include "base.h"

/*
 * 定长位图：调用方给内存，本文件只做形状。零分配、无 world 依赖 —— 与 str 同类。
 *
 * 为什么必须有自己的位图：能力位 / 类型集 / visited 现在都用 u32 掩码顶，
 * 而 u32 在第 33 个类型上**静默截断**。位图把"上限"变成一个显式的 nbits，
 * 越界是断言而不是静默丢位。
 *
 * 为什么是 uint64 字：位数与 popcount/ctz 的机器字对齐，一次扫描 64 位。
 * 逐字节实现能省事，但把"找下一个"从一条指令变成八条。
 *
 * 不变式：
 *   M1  nwords == ceil(nbits / 64)，w 指向 nwords 个 u64
 *   M2  位置 >= nbits 的位永远为 0（set/unset 断言，不写）
 *   M3  nbits == 0 时 nwords == 0 且 w 可为 NULL，所有查询都安全返回"没有"
 *
 * 复杂度：get/set/unset O(1)；popcount O(nwords)；next / first_zero O(扫描字数)，
 * 命中在首字时是 O(1) —— 迭代整个位图是 O(nwords + 置位数)，不是 O(nbits)。
 *
 * 不适用：不做自动扩容（那是 buf/vector 的事；位图扩容会改变位置语义，必须显式重建）；
 * 不保证跨字节序可移植（要落盘请先定端序，别直接把字写进文件）。
 */
typedef struct cxf_bitmap {
    uint64_t *w;
    uint32_t  nbits;
    uint32_t  nwords;
} cxf_bitmap;

/* 容纳 nbits 位所需字节数（64 位对齐）。 */
static inline uint32_t cxf_bitmap_bytes(uint32_t nbits) {
    return ((nbits + 63u) / 64u) * 8u;
}

/* mem 至少 cxf_bitmap_bytes(nbits) 字节且 8 字节对齐；本函数会清零。 */
void     cxf_bitmap_init(cxf_bitmap *b, void *mem, uint32_t nbits);
void     cxf_bitmap_clear(cxf_bitmap *b);

bool     cxf_bitmap_get(const cxf_bitmap *b, uint32_t i);
void     cxf_bitmap_set(cxf_bitmap *b, uint32_t i);
void     cxf_bitmap_unset(cxf_bitmap *b, uint32_t i);

uint32_t cxf_bitmap_popcount(const cxf_bitmap *b);

/* 从 from 起（含）找下一个置位，返回下标；没有返回 CXF_NO_INDEX。 */
uint32_t cxf_bitmap_next(const cxf_bitmap *b, uint32_t from);

/* 从 from 起（含）找下一个清零位 —— 稠密 id 分配用；没有返回 CXF_NO_INDEX。 */
uint32_t cxf_bitmap_first_zero(const cxf_bitmap *b, uint32_t from);

#ifndef NDEBUG
bool cxf_bitmap_check(const cxf_bitmap *b);
#else
#define cxf_bitmap_check(b) ((void)(b), true)
#endif

#endif
