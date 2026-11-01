#ifndef CXF_ARENA_H
#define CXF_ARENA_H

#include "base.h"
#include "alloc.h"

/*
 * Chunked bump allocator + 固定深度帧栈。
 *
 * 帧栈是"热路径零分配"的落地方式：tick 开始 push，结束 pop 整帧回收。
 * 深度是编译期常量，用尽返回 CXF_ERR_RANGE 而不是悄悄扩容 —— 帧深是设计意图，
 * 越界说明调用方的帧结构写错了，不是容量不够。
 *
 * 不变式（全部在 cxf_arena_check 中断言）：
 *   R1  cur == NULL 当且仅当 chunks == NULL，即从未分配过
 *   R2  沿链表 base 严格递增：base[0] == 0 且 base[i+1] == base[i] + cap[i]
 *   R2b cur 是"当前水位"所在 chunk，分配从 cur 向链表尾推进。
 *       cur 之后仍可能有 chunk —— 它们是上一帧的遗留，used 是陈旧值，
 *       在 alloc 前进到它时才清零。因此 pop 之后 cur 未必是 base 最大的。
 *   R3  任意 chunk 满足 used <= cap
 *   R4  0 <= depth <= CXF_ARENA_MAX_FRAMES，frames[0..depth-1] 有效
 *   R5  pop 之后，帧内分配出去的指针全部失效，不可再读写（调用方义务，无法断言）
 *
 * 复杂度（全部 O(1)，除了 release）：
 *   alloc      摊还 O(1)
 *   push/pop   O(1) —— 关键：pop 绝不遍历链表
 *   reset      O(1)
 *   release    O(chunk_count)
 *
 * 两处反直觉的实现，共同思想是"把随规模增长的成本换成常数成本"：
 *
 * 一、pop 不清理后续 chunk。若 pop 遍历 frame 之后的 chunk，代价随 chunk 数增长，
 *     与"帧是 O(1)"的语义直接冲突 —— 帧越多反而越慢。
 *     改为只回退 cur 与其 used，后续 chunk 的 used 在 alloc 前进到它时才清零。
 *
 * 二、统计不在热路径。每次 alloc 更新三条计数器会形成串行读改写，
 *     把 bump 拖离理论下倍数倍。改为 chunk 带 base 偏移，
 *     当前用量由 cur->base + cur->used 直接算出，热路径只留两次写。
 *
 * 代价要认：由此 bytes_used 的语义是"触及到的内存水位"，
 * 含已完成 chunk 的未用尾部，不是精确分配字节数。
 * 要精确值就得回热路径付那三条 RMW —— 我们选择不付。
 *
 * 倍率数字随机器与编译器浮动，本文件不再写死任何 ns 值，跑 make bench 复现。
 *
 * 不适用：不保证对齐超出 CXF_MIN_ALIGN 时的地址稳定，不保证跨 release 的指针有效，
 * 不做越界检测。需要这些请用 ASan 构建，别改运行时。
 */
#define CXF_ARENA_MAX_FRAMES 8
#define CXF_ARENA_HDR        CXF_ALIGN_UP(sizeof(cxf_chunk), CXF_MIN_ALIGN)

typedef struct cxf_chunk {
    struct cxf_chunk *next;
    size_t            base;  /* 本 chunk 在整条链中的起始偏移，使用量可 O(1) 算出，见 R2 */
    size_t            cap;   /* 载荷容量，不含 CXF_ARENA_HDR */
    size_t            used;  /* 载荷已用字节，满足 R3 */
} cxf_chunk;

typedef struct cxf_arena_frame {
    cxf_chunk *chunk;  /* 入帧时的 cur，可能为 NULL（R1 允许） */
    size_t     used;   /* 入帧时该 chunk 的 used */
} cxf_arena_frame;

typedef struct cxf_arena_stat {
    uint64_t bytes_total;   /* 累计分配字节，单调不减，热路径累加 */
    uint64_t bytes_peak;    /* 水位历史最大值，在慢路径/帧边界/release 结算 */
    uint64_t chunk_bytes;   /* 向底层 allocator 申请的总字节，含 HDR */
    uint64_t chunk_count;   /* 已申请 chunk 数，reset 后不减（内存保留复用） */
    uint64_t push_count;
    uint64_t pop_count;
} cxf_arena_stat;

typedef struct cxf_arena {
    cxf_alloc       *alloc;
    cxf_chunk       *chunks;  /* 链表头，base == 0 */
    cxf_chunk       *cur;     /* 当前水位所在 chunk，见 R2b */
    size_t           chunk_size;
    cxf_arena_frame  frames[CXF_ARENA_MAX_FRAMES];
    int              depth;
    cxf_arena_stat   stat;
} cxf_arena;

void      cxf_arena_init(cxf_arena *a, cxf_alloc *alloc, size_t chunk_size);
void      cxf_arena_release(cxf_arena *a);
void     *cxf_arena_alloc_slow(cxf_arena *a, size_t size, size_t align);
cxf_result cxf_arena_push(cxf_arena *a);
void      cxf_arena_pop(cxf_arena *a);
void      cxf_arena_reset(cxf_arena *a);

/* 当前水位字节数 = cur->base + cur->used，O(1)。语义见上方"代价要认"。 */
static inline size_t cxf_arena_bytes_used(const cxf_arena *a) {
    return a->cur != NULL ? a->cur->base + a->cur->used : 0;
}

/*
 * 分配 size 字节，align 向上规范到 >= CXF_MIN_ALIGN。
 * 返回 NULL 表示底层失败；除此之外一定成功。
 *
 * 热路径：无函数调用，align 为编译期常量时对齐计算被完全折叠，
 * 因此它接近裸 bump 的理论下限，远快于 malloc —— 倍率见 make bench。
 *
 * 失败后 arena 状态不变，可以重试 —— 但既然是 OOM，重试通常也没用。
 */
static inline void *cxf_arena_alloc(cxf_arena *a, size_t size, size_t align) {
    cxf_chunk *c = a->cur;
    size_t off;

    if (align < CXF_MIN_ALIGN) align = CXF_MIN_ALIGN;

    if (c != NULL) {
        off = CXF_ALIGN_UP(c->used, align);
        if (off <= c->cap && size <= c->cap - off) {
            uint8_t *p = (uint8_t *)c + CXF_ARENA_HDR + off;
            c->used = off + size;
            a->stat.bytes_total += size;
            return p;
        }
    }
    return cxf_arena_alloc_slow(a, size, align);
}

/*
 * 校验 R1-R4，测试与调试期调用。
 * NDEBUG 下整个消失，不为运行时付一分钱 —— 这与"注释不腐烂"并不矛盾：
 * 注释声明不变式，check 在 debug 构建里守住它，两者是同一件事的两面。
 */
#ifndef NDEBUG
bool cxf_arena_check(const cxf_arena *a);
#else
#define cxf_arena_check(a) ((void)(a), true)
#endif

#endif
