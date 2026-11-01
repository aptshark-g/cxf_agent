#include <string.h>

#include "cxf/buf.h"
#include "cxf/vec.h"

/* 契约与不变式见 buf.h，这里只写增长策略上的取舍。
 *
 * 增长本身不在这里实现：buf 是"elem == 1 的 vec"，倍增 / 溢出 / 释放大小都在
 * cxf_vec_grow 里。留在这里的只有字节语义（append / assign / view）。 */

cxf_result cxf_buf_reserve(cxf_buf *b, uint32_t need) {
    /* 用局部变量收结果再写回，而不是把 (void**)&b->p 传进去：
     * uint8_t** 与 void** 不是兼容类型，强转在严格别名规则下是 UB。 */
    void *p = b->p;
    uint32_t cap = b->cap;
    cxf_result rc = cxf_vec_grow(b->alloc, &p, &cap, b->len, need, 1u);

    if (rc != CXF_OK) return rc;
    b->p = (uint8_t *)p;
    b->cap = cap;
    return CXF_OK;
}

cxf_result cxf_buf_append(cxf_buf *b, const void *src, uint32_t n) {
    cxf_result rc;

    if (n == 0) return CXF_OK;
    if (n > UINT32_MAX - b->len) return CXF_ERR_RANGE;   /* 长度不许回绕 */

    rc = cxf_buf_reserve(b, b->len + n);
    if (rc != CXF_OK) return rc;
    memcpy(b->p + b->len, src, n);
    b->len += n;
    return CXF_OK;
}

cxf_result cxf_buf_append_byte(cxf_buf *b, uint8_t c) {
    return cxf_buf_append(b, &c, 1);
}

cxf_result cxf_buf_append_str(cxf_buf *b, cxf_str s) {
    return cxf_buf_append(b, s.p, s.len);   /* n == 0 时不碰 s.p */
}

cxf_result cxf_buf_assign(cxf_buf *b, const void *src, uint32_t n) {
    cxf_result rc = cxf_buf_reserve(b, n);

    if (rc != CXF_OK) return rc;
    if (n != 0) memmove(b->p, src, n);   /* 允许 src 指向自身：改写是原地语义 */
    b->len = n;
    return CXF_OK;
}

void cxf_buf_reset(cxf_buf *b) {
    b->len = 0;
}

void cxf_buf_free(cxf_buf *b) {
    if (b->p != NULL) cxf_alloc_free(b->alloc, b->p, b->cap);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

#ifndef NDEBUG
bool cxf_buf_check(const cxf_buf *b) {
    if (b->len > b->cap) return false;                        /* B1 */
    if ((b->p == NULL) != (b->cap == 0)) return false;        /* B1 */
    return true;
}
#endif
