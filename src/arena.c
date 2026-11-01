#include <string.h>

#include "cxf/arena.h"

/* 实现侧注释只写"为什么"，契约与不变式一律写在 arena.h。 */

/*
 * 结算 peak。只在非热路径调用：慢路径、帧边界、release。
 * 放这里是为了让 alloc 的热路径少两次读改写 —— 详见 arena.h 顶部第二条实测说明。
 */
static void arena_touch_peak(cxf_arena *a) {
    size_t u;
    if (a->cur == NULL) return;
    u = a->cur->base + a->cur->used;
    if (u > a->stat.bytes_peak) a->stat.bytes_peak = u;
}

void cxf_arena_init(cxf_arena *a, cxf_alloc *alloc, size_t chunk_size) {
    memset(a, 0, sizeof(*a));
    a->alloc = alloc;
    /* chunk_size 为 0 会让后续每次分配都退化为一次 malloc，兜底给个默认值。 */
    a->chunk_size = chunk_size != 0 ? chunk_size : 4096;
}

void cxf_arena_release(cxf_arena *a) {
    cxf_chunk *c = a->chunks;
    arena_touch_peak(a);
    while (c != NULL) {
        cxf_chunk *next = c->next;
        cxf_alloc_free(a->alloc, c, CXF_ARENA_HDR + c->cap);
        c = next;
    }
    memset(a, 0, sizeof(*a));
}

static cxf_chunk *arena_chunk_new(cxf_arena *a, size_t need) {
    size_t cap = a->chunk_size;
    cxf_chunk *c;

    /* 超大请求单独开一个专用 chunk，避免它把常规 chunk 链表撑爆。 */
    if (need > cap) cap = need;
    c = cxf_alloc_alloc(a->alloc, CXF_ARENA_HDR + cap, CXF_MIN_ALIGN);
    if (c == NULL) return NULL;

    c->next = NULL;
    c->base = a->cur != NULL ? a->cur->base + a->cur->cap : 0;
    c->cap = cap;
    c->used = 0;

    if (a->cur != NULL) a->cur->next = c;
    else a->chunks = c;
    a->cur = c;

    a->stat.chunk_count++;
    a->stat.chunk_bytes += CXF_ARENA_HDR + cap;
    return c;
}

/*
 * 慢路径：当前 chunk 放不下。三选一，且选完一定放得下：
 *   1. 请求超过常规 chunk 尺寸 -> 专用大 chunk（cap 就是 need）
 *   2. 链表后面还有 chunk     -> 前进并清零其 used（这是 pop 留下的复用点）
 *   3. 否则                   -> 新开常规 chunk
 * 走完这里不再重试循环，因为三種情况都保证了容量足够。
 */
void *cxf_arena_alloc_slow(cxf_arena *a, size_t size, size_t align) {
    cxf_chunk *c;
    size_t off;

    if (align < CXF_MIN_ALIGN) align = CXF_MIN_ALIGN;
    arena_touch_peak(a);

    if (size + align > a->chunk_size) {
        if (arena_chunk_new(a, size + align) == NULL) return NULL;
    } else if (a->cur != NULL && a->cur->next != NULL) {
        a->cur->next->used = 0;
        a->cur = a->cur->next;
    } else {
        if (arena_chunk_new(a, a->chunk_size) == NULL) return NULL;
    }

    c = a->cur;
    off = CXF_ALIGN_UP(c->used, align);
    if (off > c->cap || size > c->cap - off) return NULL;

    c->used = off + size;
    a->stat.bytes_total += size;
    arena_touch_peak(a);
    return (uint8_t *)c + CXF_ARENA_HDR + off;
}

cxf_result cxf_arena_push(cxf_arena *a) {
    cxf_arena_frame *f;
    if (a->depth >= CXF_ARENA_MAX_FRAMES) return CXF_ERR_RANGE;

    f = &a->frames[a->depth];
    f->chunk = a->cur;
    f->used = a->cur != NULL ? a->cur->used : 0;
    a->depth++;
    a->stat.push_count++;
    return CXF_OK;
}

/*
 * 回退到入帧状态。只动 cur 和它的 used，绝不遍历链表 ——
 * 遍历的代价随 chunk 数增长，而帧的语义要求它是常数操作。
 * cur 之后那些 chunk 的 used 保持陈旧值，等 alloc 前进到它时自然清零（R2b）。
 */
void cxf_arena_pop(cxf_arena *a) {
    cxf_arena_frame *f;

    CXF_ASSERT(a->depth > 0);
    arena_touch_peak(a);
    f = &a->frames[--a->depth];

    if (f->chunk != NULL) {
        a->cur = f->chunk;
        f->chunk->used = f->used;
    } else {
        /* 入帧时还没有 chunk，说明链表里现有的都是帧内创建的，整条回收复用。 */
        a->cur = a->chunks;
        if (a->chunks != NULL) a->chunks->used = 0;
    }
    a->stat.pop_count++;
}

void cxf_arena_reset(cxf_arena *a) {
    arena_touch_peak(a);
    a->cur = a->chunks;
    if (a->chunks != NULL) a->chunks->used = 0;
    a->depth = 0;
}

#ifndef NDEBUG
/* 守住 arena.h 里声明的 R1-R4。NDEBUG 下整个不编译。 */
bool cxf_arena_check(const cxf_arena *a) {
    size_t base = 0;
    cxf_chunk *c;

    if (a == NULL) return false;
    if ((a->cur == NULL) != (a->chunks == NULL)) return false;          /* R1 */
    if (a->depth < 0 || a->depth > CXF_ARENA_MAX_FRAMES) return false;  /* R4 */

    for (c = a->chunks; c != NULL; c = c->next) {
        if (c->base != base) return false;                              /* R2 */
        if (c->used > c->cap) return false;                             /* R3 */
        base += c->cap;
    }
    return true;
}
#endif
