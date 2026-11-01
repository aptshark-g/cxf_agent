#include <string.h>

#include "cxf/csr.h"

/* 契约与不变式见 csr.h，这里只写"零临时数组"的建图技巧。 */

void cxf_csr_init(cxf_csr *g, void *mem, uint32_t n, uint32_t m) {
    uint8_t *p = (uint8_t *)mem;

    CXF_ASSERT(mem != NULL || (n == 0 && m == 0));
    CXF_ASSERT(((uintptr_t)mem & 3u) == 0);

    g->off = (uint32_t *)p;
    g->adj = (uint32_t *)(p + (size_t)(n + 1u) * 4u);
    g->n = n;
    g->m = m;
    memset(g->off, 0, (size_t)(n + 1u) * 4u);
}

static uint32_t edge_from(const cxf_csr_edge *e, bool rev) {
    return rev ? e->to : e->from;
}

static uint32_t edge_to(const cxf_csr_edge *e, bool rev) {
    return rev ? e->from : e->to;
}

/*
 * 建图用"计数排序"的老办法，但要**零临时数组**（本库不许分配）。
 * 常规写法要一个 n 项的游标数组，这里用两步绕开：
 *
 *   1) off[i+1] 存节点 i 的出度；前缀和后 off[i] 就是节点 i 的起点。
 *   2) 逆序扫描边表，用 `--off[f+1]` 作为**递减的尾游标**往里填。
 *      因为逆序 + 尾插，同一节点的边在 adj 里仍保持输入顺序（C3）。
 *      填完后 off[j+1] 恰好回落到"节点 j 的起点"。
 *   3) 左移一位：off[j] = off[j+1]，off[n] = m，回到"off[i] 是起点"。
 *
 * 代价是两次额外遍历（O(n+m)），收益是**没有分配**，因此能在帧里建图。
 */
static cxf_result build(cxf_csr *g, void *mem, uint32_t n,
                        const cxf_csr_edge *edges, uint32_t m, bool rev) {
    uint32_t i;

    for (i = 0; i < m; i++) {
        if (edge_from(&edges[i], rev) >= n || edge_to(&edges[i], rev) >= n)
            return CXF_ERR_RANGE;   /* C2：越界边不静默丢弃 */
    }

    cxf_csr_init(g, mem, n, m);     /* off 清零 */

    for (i = 0; i < m; i++) g->off[edge_from(&edges[i], rev) + 1u]++;
    for (i = 1; i <= n; i++) g->off[i] += g->off[i - 1u];

    for (i = m; i-- > 0;) {
        uint32_t f = edge_from(&edges[i], rev);

        g->adj[--g->off[f + 1u]] = edge_to(&edges[i], rev);
    }

    for (i = 0; i < n; i++) g->off[i] = g->off[i + 1u];
    g->off[n] = m;
    return CXF_OK;
}

cxf_result cxf_csr_from_edges(cxf_csr *g, void *mem, uint32_t n,
                              const cxf_csr_edge *edges, uint32_t m) {
    CXF_ASSERT(m == 0 || edges != NULL);
    return build(g, mem, n, edges, m, false);
}

cxf_result cxf_csr_from_edges_rev(cxf_csr *g, void *mem, uint32_t n,
                                  const cxf_csr_edge *edges, uint32_t m) {
    CXF_ASSERT(m == 0 || edges != NULL);
    return build(g, mem, n, edges, m, true);
}

#ifndef NDEBUG
bool cxf_csr_check(const cxf_csr *g) {
    uint32_t i;

    if (g->n == 0 && g->m == 0) return true;   /* 空图没有可校验的形状 */
    if (g->off == NULL || g->adj == NULL) return false;
    if (g->off[0] != 0) return false;                       /* C1 */
    if (g->off[g->n] != g->m) return false;                 /* C1 */

    for (i = 0; i < g->n; i++)
        if (g->off[i] > g->off[i + 1u]) return false;        /* C1 单调 */
    for (i = 0; i < g->m; i++)
        if (g->adj[i] >= g->n) return false;                 /* C2 */
    return true;
}
#endif
