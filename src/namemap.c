#include <string.h>

#include "cxf/namemap.h"

/* 契约与不变式见 namemap.h，这里只写二分与扩容的细节。 */

void cxf_namemap_init(cxf_namemap *m) {
    m->ents = NULL;
    m->count = 0;
    m->cap = 0;
}

uint32_t cxf_namemap_lower_bound(const cxf_namemap *m, cxf_str key) {
    uint32_t lo = 0, hi = m->count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;

        if (cxf_str_order(m->ents[mid].name, key) < 0) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

/*
 * 扩容：另申请一块更大的再复制。arena 没有 free，所以旧块不回收 ——
 * 这是有意的：注册期的垃圾随世界一起消失，换来"没有失败回滚路径"（N2 因此永远成立）。
 */
static bool namemap_grow(cxf_namemap *m, cxf_arena *a) {
    uint32_t ncap = m->cap != 0 ? m->cap + m->cap / 2u + 1u : 8u;
    cxf_namemap_ent *ne = cxf_arena_alloc(a, (size_t)ncap * sizeof(*ne), CXF_MIN_ALIGN);

    if (ne == NULL) return false;
    if (m->count != 0) memcpy(ne, m->ents, (size_t)m->count * sizeof(*ne));

    m->ents = ne;
    m->cap = ncap;
    return true;
}

bool cxf_namemap_add(cxf_namemap *m, cxf_arena *a, cxf_str name, uint32_t id) {
    uint32_t pos = cxf_namemap_lower_bound(m, name);

    if (pos < m->count && cxf_str_eq(m->ents[pos].name, name))
        return false;                       /* N1：名字唯一，重复注册是错误 */

    if (m->count == m->cap && !namemap_grow(m, a))
        return false;

    if (pos < m->count)
        memmove(&m->ents[pos + 1], &m->ents[pos],
                (size_t)(m->count - pos) * sizeof(*m->ents));

    m->ents[pos].name = name;
    m->ents[pos].id = id;
    m->count++;
    return true;
}

bool cxf_namemap_find(const cxf_namemap *m, cxf_str name, uint32_t *id) {
    uint32_t pos = cxf_namemap_lower_bound(m, name);

    if (pos < m->count && cxf_str_eq(m->ents[pos].name, name)) {
        if (id != NULL) *id = m->ents[pos].id;
        return true;
    }
    return false;
}

#ifndef NDEBUG
bool cxf_namemap_check(const cxf_namemap *m) {
    uint32_t i;

    if (m->count > m->cap) return false;                    /* N2 */
    if (m->cap != 0 && m->ents == NULL) return false;
    for (i = 1; i < m->count; i++)
        if (cxf_str_order(m->ents[i - 1].name, m->ents[i].name) >= 0) return false;  /* N1 */
    return true;
}
#endif
