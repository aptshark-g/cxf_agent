#ifndef CXF_HANDLE_H
#define CXF_HANDLE_H

#include "base.h"

/*
 * 跨模块引用一律走 handle，不用裸指针。理由有三：
 *   1. 热插拔安全：模块可以整块移除，handle 失效可检测而非静默错指
 *   2. 槽位可复用：删除后 index 回收，内存不泄漏
 *   3. 可序列化：8 字节整数能落盘、能过 ring buffer、能原子交换
 *
 * ── 位布局 ───────────────────────────────────────────────────────────
 *   gen 32 位 = [ class 8 位 | 世代计数器 24 位 ]
 *
 * 为什么高 8 位要放 class：没有它，"这张 handle 是不是本表的"只能靠
 * index 在界内 + gen 恰好相等来**概率拦截** —— 旧版 H3 自认"跨表比较无定义"，
 * 于是把 A 表的 handle 喂给 B 表，只要 index 撞上且世代数相同就静默成功。
 * 一旦表在装配期拿到 class，跨表误用就变成**确定性拒绝**，不再是巧合。
 *
 * 代价是计数器 32 → 24 位（1677 万次复用）。这个代价划算：否决 22+10 方案的理由是
 * 10 位只有 1024 次就回绕，而 24 位仍远超热插拔的回绕需求；且回绕被 H2 兜住 ——
 * 低 24 位回到 0 是偶数，只表示"死"，不会制造"假活"。
 *
 * 不变式：
 *   H1  gen == 0（class 0 且计数器 0）表示无效 handle，永不由 cxf_table_add 产生
 *   H2  gen 的低 24 位为奇数表示该槽位存活，偶数表示已回收
 *   H3  同一 class 内相等当且仅当同一槽位同一世代；**不同 class 的 handle 永不相等**
 *       （高位不同必然不等）—— 跨表比较从此是"有定义：必不相等"，不再是 UB
 *   H4  表只产出 class 等于自身 class_id 的 handle；class 0 = 未标注，不做跨表保护
 *       （保留给尚未分型的表，也是唯一的残余碰撞面）
 *   H5  计数器在低 24 位内回绕，回绕点必然落在偶数上；但跨越 2^24 次复用之后，
 *       极老的 handle 可能重新等于新 handle —— 与 32 位方案的差别是概率，不是性质。
 *       设计接受这一点：比"10 位回绕"低四个数量级，而再加位数就要挤占 class。
 *
 * 不适用：handle 不是智能指针，不管理生命周期，不保证目标存活。
 * 每次解引用前由表判定，见 cxf_table_live。
 */
#define CXF_HANDLE_CLASS_BITS 8u
#define CXF_HANDLE_GEN_BITS   24u
#define CXF_HANDLE_GEN_MASK   0x00FFFFFFu
#define CXF_HANDLE_CLASS_NONE 0u
#define CXF_HANDLE_CLASS_MAX  255u

typedef struct cxf_handle {
    uint32_t index;  /* 槽位下标，删除后被复用 */
    uint32_t gen;    /* [class 8 | 计数器 24]，见 H1/H2 */
} cxf_handle;

/*
 * 原样构造：gen 已经是"class | 计数器"的打包值，**不做掩码**。
 * 这是"从存储里恢复 handle"的路径 —— SoA 会把 gen 拍平成一列，读回来的正是打包值；
 * 在这里掩掉高位会让恢复出的 handle 与表里存的 gen 不等，那是最坏的一种错。
 * 需要按 class + 计数器拼一个时用 cxf_handle_typed。
 */
static inline cxf_handle cxf_handle_make(uint32_t index, uint32_t gen) {
    cxf_handle h = { index, gen };
    return h;
}

static inline cxf_handle cxf_handle_typed(uint32_t index, uint32_t counter,
                                          uint32_t class_id) {
    CXF_ASSERT(class_id <= CXF_HANDLE_CLASS_MAX);
    return cxf_handle_make(index, (class_id << CXF_HANDLE_GEN_BITS) |
                                  (counter & CXF_HANDLE_GEN_MASK));
}

static inline bool cxf_handle_null(cxf_handle h) {
    return h.gen == 0;
}

/* 世代计数器（不含 class）。调试与统计用；判活请用 cxf_handle_alive。 */
static inline uint32_t cxf_handle_gen(cxf_handle h) {
    return h.gen & CXF_HANDLE_GEN_MASK;
}

static inline uint32_t cxf_handle_class(cxf_handle h) {
    return h.gen >> CXF_HANDLE_GEN_BITS;
}

/* H2：低 24 位为奇数。只对"本表产出的 handle"有意义 —— 跨表还要比 class。 */
static inline bool cxf_handle_alive(cxf_handle h) {
    return (h.gen & 1u) != 0u;
}

/* 全等比较，含 class。两个 handle 相等当且仅当同一槽位的同一世代。 */
static inline bool cxf_handle_eq(cxf_handle a, cxf_handle b) {
    return a.index == b.index && a.gen == b.gen;
}

/*
 * 推进世代：只动低 24 位，class 位原样保留。
 * 回绕点落在 0（偶数 ⇒ 死），这是 H5 —— 把所有"翻世代"的操作收在这一个函数里，
 * 避免"某处忘了掩码"把 class 位加进位掉。
 */
static inline uint32_t cxf_handle_gen_next(uint32_t gen) {
    return (gen & ~CXF_HANDLE_GEN_MASK) | ((gen + 1u) & CXF_HANDLE_GEN_MASK);
}

#endif
