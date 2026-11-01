#include <stdio.h>
#include <string.h>

#include "cxf/type.h"
#include "cxf/vec.h"
#include "cxf/world.h"

/* 契约与不变式见 type.h 与 docs/type-layout.md。 */

static uint32_t count_leaves(const cxf_type *t) {
    uint32_t n = 0;
    if (t->kind != CXF_K_STRUCT) return 1;
    for (uint32_t i = 0; i < t->field_count; i++)
        n += count_leaves(t->fields[i].type);
    return n;
}

static const char *path_join(cxf_arena *a, const char *prefix, const char *name) {
    size_t n;
    char *p;

    if (prefix == NULL || prefix[0] == '\0') {
        n = strlen(name) + 1;
        p = cxf_arena_alloc(a, n, 1);
        if (p == NULL) return "";
        memcpy(p, name, n);
        return p;
    }

    n = strlen(prefix) + 1 + strlen(name) + 1;
    p = cxf_arena_alloc(a, n, 1);
    if (p == NULL) return prefix;
    snprintf(p, n, "%s.%s", prefix, name);
    return p;
}

static void flatten_rec(const cxf_type *t, uint32_t off, const char *prefix,
                        cxf_layout *l, uint32_t *n) {
    if (t->kind != CXF_K_STRUCT) {
        cxf_leaf *lf = &l->leaves[*n];
        lf->offset = off;
        lf->type = t;
        lf->col = *n;
        lf->path = prefix != NULL ? prefix : "";
        (*n)++;
        return;
    }

    for (uint32_t i = 0; i < t->field_count; i++) {
        const cxf_field *f = &t->fields[i];
        const char *p = path_join(&l->paths, prefix, f->name);
        flatten_rec(f->type, off + f->offset, p, l, n);
    }
}

cxf_result cxf_layout_build(cxf_layout *l, const cxf_type *t, cxf_alloc *a) {
    uint32_t n = 0, cnt;

    memset(l, 0, sizeof(*l));
    l->type = t;
    cxf_arena_init(&l->paths, a, 1024);

    cnt = count_leaves(t);
    l->leaves = cxf_alloc_alloc(a, (size_t)cnt * sizeof(cxf_leaf), CXF_MIN_ALIGN);
    if (l->leaves == NULL) return CXF_ERR_NOMEM;
    l->leaf_count = cnt;

    flatten_rec(t, 0, NULL, l, &n);
    return n == cnt ? CXF_OK : CXF_ERR_RANGE;
}

void cxf_layout_release(cxf_layout *l, cxf_alloc *a) {
    if (l->leaves != NULL)
        cxf_alloc_free(a, l->leaves, (size_t)l->leaf_count * sizeof(cxf_leaf));
    cxf_arena_release(&l->paths);
    memset(l, 0, sizeof(*l));
}

const cxf_leaf *cxf_layout_find(const cxf_layout *l, const char *path) {
    for (uint32_t i = 0; i < l->leaf_count; i++) {
        if (strcmp(l->leaves[i].path, path) == 0) return &l->leaves[i];
    }
    return NULL;
}

uint32_t cxf_layout_select_size(const cxf_layout *l, uint32_t size,
                                const cxf_leaf **out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < l->leaf_count && n < cap; i++) {
        if (l->leaves[i].type->size == size) out[n++] = &l->leaves[i];
    }
    return n;
}

uint32_t cxf_layout_select_kind(const cxf_layout *l, cxf_kind k,
                                const cxf_leaf **out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < l->leaf_count && n < cap; i++) {
        if (l->leaves[i].type->kind == k) out[n++] = &l->leaves[i];
    }
    return n;
}

#ifndef NDEBUG
/*
 * 递归字段但不递归 target：结构体嵌套是无环的（DAG），而指针可以自引用，
 * 递归 target 会成环。见 type.h 的 Y3。
 */
bool cxf_type_check(const cxf_type *t) {
    if (t == NULL) return false;

    if (t->kind != CXF_K_STRUCT)
        return t->fields == NULL && t->field_count == 0;          /* Y1 */

    if (t->fields == NULL || t->field_count == 0) return false;   /* Y2 */

    for (uint32_t i = 0; i < t->field_count; i++) {
        const cxf_field *f = &t->fields[i];
        if (f->type == NULL) return false;
        if (f->offset + f->type->size > t->size) return false;    /* Y4 */
        if (!cxf_type_check(f->type)) return false;
    }
    return true;
}
#endif

/* ---- 注册期：装配期登记，调用期零查找 ---- */

static cxf_result type_reserve(cxf_world *w, uint32_t need) {
    /* 增长走唯一实现（cxf_vec_grow），与 service 表同一组边界。 */
    void *p = w->types;
    uint32_t cap = w->type_cap;
    cxf_result rc = cxf_vec_grow(w->alloc, &p, &cap, w->type_count, need,
                                 (uint32_t)sizeof(cxf_type_entry));

    if (rc != CXF_OK) return rc;
    w->types = (cxf_type_entry *)p;
    w->type_cap = cap;
    return CXF_OK;
}

static bool type_name_taken(const cxf_world *w, const char *name) {
    for (uint32_t i = 0; i < w->type_count; i++)
        if (strcmp(w->types[i].name, name) == 0) return true;
    return false;
}

cxf_result cxf_type_register(cxf_world *w, const char *name, const cxf_type *t) {
    cxf_result r;

    CXF_ASSERT(w != NULL && name != NULL && t != NULL);
    if (type_name_taken(w, name)) return CXF_ERR_RANGE;          /* K1 */

    r = type_reserve(w, w->type_count + 1);
    if (r != CXF_OK) return r;

    w->types[w->type_count].name = name;
    w->types[w->type_count].type = t;
    w->type_count++;
    return CXF_OK;
}

cxf_result cxf_type_register_bundle(cxf_world *w, const cxf_type_bundle *b) {
    cxf_result r;

    CXF_ASSERT(w != NULL && b != NULL && b->entries != NULL);

    /* 先整体校验再提交：条目只增不减（K3），失败时留下半张表会让调用方
       无从判断哪些注册了。两组校验合一，出错只报 CXF_ERR_RANGE。 */
    for (uint32_t i = 0; i < b->count; i++) {
        const cxf_type_entry *e = &b->entries[i];
        if (e->name == NULL || e->type == NULL) return CXF_ERR_RANGE;
        if (type_name_taken(w, e->name)) return CXF_ERR_RANGE;   /* K1 */
        for (uint32_t j = 0; j < i; j++)
            if (strcmp(b->entries[j].name, e->name) == 0)
                return CXF_ERR_RANGE;                            /* K1 */
    }

    r = type_reserve(w, w->type_count + b->count);
    if (r != CXF_OK) return r;

    for (uint32_t i = 0; i < b->count; i++) {
        w->types[w->type_count].name = b->entries[i].name;
        w->types[w->type_count].type = b->entries[i].type;
        w->type_count++;
    }
    return CXF_OK;
}

const cxf_type *cxf_type_find(const cxf_world *w, const char *name) {
    CXF_ASSERT(w != NULL && name != NULL);

    /* 计数槽位让"调用期零查找"成为可观测事实，而不是一条注释。
       测试断言 tick 前后它不变 —— 见 tests/test_registry.c。
       const 是给调用方的承诺，槽位不参与其中，故此处去掉 const。 */
    ((cxf_world *)w)->type_find_count++;

    for (uint32_t i = 0; i < w->type_count; i++)
        if (strcmp(w->types[i].name, name) == 0)
            return w->types[i].type;                             /* K2 */
    return NULL;
}
