#include <string.h>

#include "cxf/blob.h"
#include "cxf/vec.h"

/* 契约与不变式见 blob.h，这里只写"为什么是一列 AoS"。 */

static cxf_blob_ent *ent_of(cxf_blobpool *bp, cxf_handle h) {
    return (cxf_blob_ent *)cxf_table_get(&bp->t, h, 0);
}

/* const 版本要 cast：cxf_table_get 的接口接受非 const 表，
 * 而 view 语义上是只读的。与 ring.c 里处理 const 是同一类妥协。 */
static const cxf_blob_ent *ent_of_c(const cxf_blobpool *bp, cxf_handle h) {
    return (const cxf_blob_ent *)cxf_table_get((cxf_table *)&bp->t, h, 0);
}

static cxf_result ent_reserve(cxf_blobpool *bp, cxf_blob_ent *e, uint32_t need) {
    /* 增长的边界（倍增 / 溢出 / 复制 / 释放大小）都在 cxf_vec_grow；
     * 这里只补它管不到的一件事：把容量变化计入池的字节账。 */
    void *p = e->p;
    uint32_t cap = e->cap;
    cxf_result rc = cxf_vec_grow(bp->t.alloc, &p, &cap, e->len, need, 1u);

    if (rc != CXF_OK) return rc;

    bp->bytes_live += (uint64_t)cap - e->cap;   /* L2：记容量，不是长度 */
    if (bp->bytes_live > bp->bytes_peak) bp->bytes_peak = bp->bytes_live;

    e->p = (uint8_t *)p;
    e->cap = cap;
    return CXF_OK;
}

cxf_result cxf_blobpool_init(cxf_blobpool *bp, cxf_alloc *alloc,
                             uint32_t cap, uint32_t class_id) {
    const size_t col = sizeof(cxf_blob_ent);   /* 一列装整个条目，见 blob.h */

    bp->bytes_live = 0;
    bp->bytes_peak = 0;
    bp->count = 0;
    return cxf_table_init_class(&bp->t, alloc, &col, 1, cap, class_id);
}

void cxf_blobpool_release(cxf_blobpool *bp) {
    uint32_t r;

    for (r = 0; r < bp->t.count; r++) {
        cxf_blob_ent *e = ent_of(bp, cxf_table_at(&bp->t, r));
        if (e != NULL && e->p != NULL)
            cxf_alloc_free(bp->t.alloc, e->p, e->cap);
    }
    cxf_table_release(&bp->t);
    bp->bytes_live = 0;
    bp->bytes_peak = 0;
    bp->count = 0;
}

cxf_handle cxf_blob_new(cxf_blobpool *bp, uint32_t hint) {
    cxf_handle h = cxf_table_add(&bp->t);
    cxf_blob_ent *e;

    if (cxf_handle_null(h)) return h;

    /* 槽位是复用的，列内容不保证是零 —— 必须自己初始化，否则会继承上一任的 p/cap，
     * 那不是"泄漏"，是**free 到别人的块**。 */
    e = ent_of(bp, h);
    e->p = NULL;
    e->len = 0;
    e->cap = 0;

    if (hint != 0 && ent_reserve(bp, e, hint) != CXF_OK) {
        cxf_table_remove(&bp->t, h);   /* 失败回滚：不留下空壳条目 */
        return cxf_handle_make(0, 0);
    }

    bp->count++;
    return h;
}

cxf_result cxf_blob_append(cxf_blobpool *bp, cxf_handle h, const void *src, uint32_t n) {
    cxf_blob_ent *e = ent_of(bp, h);
    cxf_result rc;

    if (e == NULL) return CXF_ERR_BAD_HANDLE;
    if (n == 0) return CXF_OK;
    if (n > UINT32_MAX - e->len) return CXF_ERR_RANGE;

    rc = ent_reserve(bp, e, e->len + n);
    if (rc != CXF_OK) return rc;

    memcpy(e->p + e->len, src, n);
    e->len += n;
    return CXF_OK;
}

cxf_result cxf_blob_assign(cxf_blobpool *bp, cxf_handle h, const void *src, uint32_t n) {
    cxf_blob_ent *e = ent_of(bp, h);
    cxf_result rc;

    if (e == NULL) return CXF_ERR_BAD_HANDLE;

    rc = ent_reserve(bp, e, n);
    if (rc != CXF_OK) return rc;

    if (n != 0) memmove(e->p, src, n);   /* 允许 src 指向条目自身 */
    e->len = n;
    return CXF_OK;
}

cxf_str cxf_blob_view(const cxf_blobpool *bp, cxf_handle h) {
    const cxf_blob_ent *e = ent_of_c(bp, h);

    if (e == NULL) return cxf_str_make(NULL, 0);
    return cxf_str_make((const char *)e->p, e->len);
}

cxf_result cxf_blob_free(cxf_blobpool *bp, cxf_handle h) {
    cxf_blob_ent *e = ent_of(bp, h);

    if (e == NULL) return CXF_ERR_BAD_HANDLE;

    if (e->p != NULL) cxf_alloc_free(bp->t.alloc, e->p, e->cap);
    bp->bytes_live -= e->cap;   /* 先减再清，避免顺序错时漏减 */

    e->p = NULL;
    e->len = 0;
    e->cap = 0;

    cxf_table_remove(&bp->t, h);   /* gen 翻转 ⇒ L4：二次 free 走 BAD_HANDLE */
    bp->count--;
    return CXF_OK;
}

#ifndef NDEBUG
bool cxf_blobpool_check(const cxf_blobpool *bp) {
    uint64_t sum = 0;
    uint32_t r;

    if (bp->count != bp->t.count) return false;

    for (r = 0; r < bp->t.count; r++) {
        const cxf_blob_ent *e = ent_of_c(bp, cxf_table_at(&bp->t, r));

        if (e == NULL) return false;
        if (e->len > e->cap) return false;                       /* L1 */
        if ((e->p == NULL) != (e->cap == 0)) return false;       /* L1 */
        sum += e->cap;
    }
    if (sum != bp->bytes_live) return false;                     /* L2 */
    if (bp->bytes_peak < bp->bytes_live) return false;           /* L3 */
    return cxf_table_check(&bp->t);
}
#endif
