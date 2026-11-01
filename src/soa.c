#include <string.h>

#include "cxf/soa.h"

/* 契约与不变式见 soa.h 与 docs/type-layout.md。 */

cxf_result cxf_soa_init(cxf_soa *s, const cxf_layout *l, cxf_alloc *a, uint32_t cap) {
    return cxf_soa_init_class(s, l, a, cap, CXF_HANDLE_CLASS_NONE);
}

cxf_result cxf_soa_init_class(cxf_soa *s, const cxf_layout *l, cxf_alloc *a,
                              uint32_t cap, uint32_t class_id) {
    size_t sizes[CXF_TABLE_MAX_COLS];

    if (l->leaf_count == 0 || l->leaf_count > CXF_TABLE_MAX_COLS)
        return CXF_ERR_RANGE;

    for (uint32_t i = 0; i < l->leaf_count; i++)
        sizes[i] = l->leaves[i].type->size;

    s->layout = l;
    s->alloc = a;
    return cxf_table_init_class(&s->table, a, sizes, l->leaf_count, cap, class_id);
}

void cxf_soa_release(cxf_soa *s) {
    cxf_table_release(&s->table);
    memset(s, 0, sizeof(*s));
}

cxf_handle cxf_soa_add(cxf_soa *s) {
    cxf_handle h = cxf_table_add(&s->table);
    /* 新行的每一列清零：gather 出来的结构体不应含上一任的残留。 */
    if (cxf_table_live(&s->table, h)) {
        uint32_t row = cxf_table_row(&s->table, h);
        for (uint32_t i = 0; i < s->layout->leaf_count; i++) {
            size_t sz = s->layout->leaves[i].type->size;
            memset(s->table.cols[i] + (size_t)row * sz, 0, sz);
        }
    }
    return h;
}

cxf_result cxf_soa_remove(cxf_soa *s, cxf_handle h) {
    return cxf_table_remove(&s->table, h);
}

bool cxf_soa_live(const cxf_soa *s, cxf_handle h) {
    return cxf_table_live(&s->table, h);
}

void *cxf_soa_col(cxf_soa *s, uint32_t leaf) {
    return leaf < s->layout->leaf_count ? cxf_table_col(&s->table, leaf) : NULL;
}

void *cxf_soa_get(cxf_soa *s, cxf_handle h, uint32_t leaf) {
    return leaf < s->layout->leaf_count ? cxf_table_get(&s->table, h, leaf) : NULL;
}

void cxf_soa_gather(cxf_soa *s, cxf_handle h, void *out) {
    uint32_t row;

    if (out == NULL || !cxf_table_live(&s->table, h)) return;
    row = cxf_table_row(&s->table, h);

    for (uint32_t i = 0; i < s->layout->leaf_count; i++) {
        const cxf_leaf *lf = &s->layout->leaves[i];
        memcpy((uint8_t *)out + lf->offset,
               s->table.cols[i] + (size_t)row * lf->type->size,
               lf->type->size);
    }
}

void cxf_soa_scatter(cxf_soa *s, cxf_handle h, const void *in) {
    uint32_t row;

    if (in == NULL || !cxf_table_live(&s->table, h)) return;
    row = cxf_table_row(&s->table, h);

    for (uint32_t i = 0; i < s->layout->leaf_count; i++) {
        const cxf_leaf *lf = &s->layout->leaves[i];
        memcpy(s->table.cols[i] + (size_t)row * lf->type->size,
               (const uint8_t *)in + lf->offset,
               lf->type->size);
    }
}

#ifndef NDEBUG
bool cxf_soa_check(const cxf_soa *s) {
    if (s == NULL || s->layout == NULL) return false;
    if (s->table.col_count != s->layout->leaf_count) return false;   /* O1 */
    for (uint32_t i = 0; i < s->layout->leaf_count; i++) {
        if (s->table.col_size[i] != s->layout->leaves[i].type->size)
            return false;                                            /* O2 */
        if (s->layout->leaves[i].col != i) return false;
    }
    return cxf_table_check(&s->table);
}
#endif
