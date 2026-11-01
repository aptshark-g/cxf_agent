#include <string.h>

#include "cxf/vec.h"

/* 契约与不变式见 vec.h，这里只写增长策略上的取舍。 */

/*
 * 起步容量按"至少 32 字节、至少 8 个元素"取：
 *   - 至少 32 字节：elem == 1 时（buf / blob 条目）与旧行为一致，小字符串不必
 *     连续增长三四次；
 *   - 至少 8 个元素：宽元素（world 的模块表、dep_edge）时 32 字节不足一个元素，
 *     要兜住"起步容量为 0 导致除零或死循环"。
 * 两条取大值，比"给每个调用方传一个起步容量"少一个参数、少一处误配。
 */
static uint32_t start_cap(uint32_t elem) {
    uint32_t n = 32u / elem;

    return n < 8u ? 8u : n;
}

cxf_result cxf_vec_grow(cxf_alloc *a, void **p, uint32_t *cap,
                        uint32_t len, uint32_t need, uint32_t elem) {
    uint32_t ncap;
    void *np;

    CXF_ASSERT(a != NULL && p != NULL && cap != NULL && elem > 0);
    if (need <= *cap) return CXF_OK;                    /* V4：不增长就什么都不动 */
    if ((uint64_t)need * elem > (uint64_t)UINT32_MAX) return CXF_ERR_RANGE;

    /* 翻倍而不是 +need：push 的摊还成本才是 O(1)。逐元素精确扩容是 O(n²) ——
     * 这是所有"越写越慢"的来源。 */
    ncap = *cap != 0 ? *cap : start_cap(elem);
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2u) { ncap = need; break; }   /* 再翻就溢出 */
        ncap *= 2u;
    }
    if ((uint64_t)ncap * elem > (uint64_t)UINT32_MAX) {
        ncap = need;                                          /* 退回最小满足 */
        if ((uint64_t)ncap * elem > (uint64_t)UINT32_MAX) return CXF_ERR_RANGE;
    }

    np = cxf_alloc_alloc(a, (size_t)ncap * elem, CXF_MIN_ALIGN);
    if (np == NULL) return CXF_ERR_NOMEM;

    if (len != 0) memcpy(np, *p, (size_t)len * elem);
    if (*p != NULL) cxf_alloc_free(a, *p, (size_t)(*cap) * elem);   /* A2：与申请时同一算式 */

    *p = np;
    *cap = ncap;
    return CXF_OK;
}

cxf_result cxf_vec_reserve(cxf_vec *v, uint32_t need) {
    return cxf_vec_grow(v->alloc, &v->p, &v->cap, v->len, need, v->elem);
}

cxf_result cxf_vec_push(cxf_vec *v, const void *e) {
    cxf_result rc;

    if (v->len == UINT32_MAX) return CXF_ERR_RANGE;
    rc = cxf_vec_reserve(v, v->len + 1u);
    if (rc != CXF_OK) return rc;

    memcpy((uint8_t *)v->p + (size_t)v->len * v->elem, e, v->elem);
    v->len++;
    return CXF_OK;
}

void *cxf_vec_push_slot(cxf_vec *v) {
    void *slot;

    if (v->len == UINT32_MAX) return NULL;
    if (cxf_vec_reserve(v, v->len + 1u) != CXF_OK) return NULL;

    slot = (uint8_t *)v->p + (size_t)v->len * v->elem;
    v->len++;
    return slot;
}

void cxf_vec_clear(cxf_vec *v) {
    v->len = 0;
}

void cxf_vec_free(cxf_vec *v) {
    if (v->p != NULL) cxf_alloc_free(v->alloc, v->p, (size_t)v->cap * v->elem);
    v->p = NULL;
    v->len = 0;
    v->cap = 0;
}

#ifndef NDEBUG
bool cxf_vec_check(const cxf_vec *v) {
    if (v->elem == 0) return false;                     /* V1 */
    if (v->len > v->cap) return false;                  /* V1 */
    if ((v->p == NULL) != (v->cap == 0)) return false;  /* V1 */
    return true;
}
#endif
