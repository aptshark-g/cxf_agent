#include "cxf/algo.h"

/* 契约与复杂度见 algo.h，这里只写"为什么这样选"。 */

static void swap_u32(uint32_t *a, uint32_t *b) {
    uint32_t t = *a;
    *a = *b;
    *b = t;
}

/* 插入排序：n 小时常数最小，且对"已基本有序"的输入是 O(n)。 */
static void insertion_sort(uint32_t *a, uint32_t n) {
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        uint32_t v = a[i];
        for (j = i; j > 0 && a[j - 1] > v; j--) a[j] = a[j - 1];
        a[j] = v;
    }
}

/* 大根堆下沉。用循环而不是递归：递归的每次调用都是一次依赖链。 */
static void sift_down(uint32_t *a, uint32_t root, uint32_t n) {
    for (;;) {
        uint32_t child = root * 2u + 1u;

        if (child >= n) return;
        if (child + 1u < n && a[child + 1u] > a[child]) child++;
        if (a[root] >= a[child]) return;
        swap_u32(&a[root], &a[child]);
        root = child;
    }
}

void cxf_algo_sort_u32(uint32_t *a, uint32_t n) {
    uint32_t i;

    if (n < 2) return;
    if (n < 16) { insertion_sort(a, n); return; }

    for (i = n / 2u; i-- > 0;) sift_down(a, i, n);   /* 建堆：从最后一个非叶节点回退 */
    for (i = n; i-- > 1;) {
        swap_u32(&a[0], &a[i]);
        sift_down(a, 0, i);
    }
}

uint32_t cxf_algo_lower_bound_u32(const uint32_t *a, uint32_t n, uint32_t key) {
    uint32_t lo = 0, hi = n;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;

        if (a[mid] < key) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

/* 小根堆（存下标，按 keys 比较）。选 k 个最大 —— 所以堆里放的是"当前最大的 k 个"，
 * 堆顶是其中最小的那个，正是下一个候选的淘汰线。 */
static void min_sift_down(uint32_t *heap, uint32_t i, uint32_t n, const uint32_t *keys) {
    for (;;) {
        uint32_t child = i * 2u + 1u;

        if (child >= n) return;
        if (child + 1u < n && keys[heap[child + 1u]] < keys[heap[child]]) child++;
        if (keys[heap[i]] <= keys[heap[child]]) return;
        swap_u32(&heap[i], &heap[child]);
        i = child;
    }
}

static void min_sift_up(uint32_t *heap, uint32_t i, const uint32_t *keys) {
    while (i > 0) {
        uint32_t parent = (i - 1u) / 2u;

        if (keys[heap[parent]] <= keys[heap[i]]) return;
        swap_u32(&heap[parent], &heap[i]);
        i = parent;
    }
}

uint32_t cxf_algo_topk_u32(const uint32_t *keys, uint32_t n, uint32_t k, uint32_t *out) {
    uint32_t sz = 0, i, m;

    if (k == 0 || n == 0) return 0;
    if (k > n) k = n;

    for (i = 0; i < n; i++) {
        if (sz < k) {
            out[sz] = i;
            min_sift_up(out, sz, keys);
            sz++;
        } else if (keys[i] > keys[out[0]]) {
            out[0] = i;
            min_sift_down(out, 0, k, keys);
        }
    }

    /* 逐个弹出最小值，从数组尾部往前填 —— 最终 out 是降序，
     * 且不需要第二块缓冲（pop 出去的槽位正好是它的最终位置）。 */
    m = sz;
    while (sz > 1) {
        swap_u32(&out[0], &out[sz - 1u]);
        sz--;
        min_sift_down(out, 0, sz, keys);
    }
    return m;
}

uint32_t cxf_algo_hash_bytes(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u, i;   /* FNV offset basis */

    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;            /* FNV prime */
    }
    return h;
}

uint32_t cxf_algo_hash_str(cxf_str s) {
    return cxf_algo_hash_bytes(s.p, s.len);   /* len == 0 时不碰 p */
}

cxf_result cxf_algo_toposort(const cxf_csr *g, uint32_t *indeg, uint32_t *order) {
    uint32_t n = g->n, i, head = 0, tail = 0, cnt = 0;

    for (i = 0; i < n; i++) indeg[i] = 0;
    for (i = 0; i < n; i++) {
        uint32_t k;

        for (k = g->off[i]; k < g->off[i + 1u]; k++) indeg[g->adj[k]]++;
    }

    /* 升序入队 ⇒ 确定性（见 algo.h）。order 同时充当 FIFO 队列与结果数组：
     * head 之前的项已定序且不再修改，tail 之后仍为空 —— 两者不重叠。 */
    for (i = 0; i < n; i++)
        if (indeg[i] == 0) order[tail++] = i;

    while (head < tail) {
        uint32_t u = order[head++], k;

        cnt++;
        for (k = g->off[u]; k < g->off[u + 1u]; k++) {
            uint32_t v = g->adj[k];

            if (--indeg[v] == 0) order[tail++] = v;
        }
    }

    return cnt == n ? CXF_OK : CXF_ERR_DEP;   /* 剩下的节点都在环上或环的下游 */
}

bool cxf_algo_reach(const cxf_csr *g, uint32_t src, uint32_t dst,
                    cxf_bitmap *visited, uint32_t *queue) {
    uint32_t head = 0, tail = 0;

    if (src >= g->n || dst >= g->n) return false;
    CXF_ASSERT(visited->nbits >= g->n);

    cxf_bitmap_clear(visited);
    if (src == dst) return true;

    queue[tail++] = src;
    cxf_bitmap_set(visited, src);

    while (head < tail) {
        uint32_t u = queue[head++], k;

        for (k = g->off[u]; k < g->off[u + 1u]; k++) {
            uint32_t v = g->adj[k];

            if (cxf_bitmap_get(visited, v)) continue;
            if (v == dst) return true;       /* 命中即返回，不把整张图跑完 */
            cxf_bitmap_set(visited, v);
            queue[tail++] = v;
        }
    }
    return false;
}
