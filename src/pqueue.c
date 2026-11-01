#include "cxf/pqueue.h"

/* 契约与不变式见 pqueue.h，这里只写"为什么两处都用同一套比较"。 */

/* P3：全序。key 优先，相等时比 id —— 没有这一条，相等 key 的弹出顺序
 * 就取决于内部移动历史（同一集合不同插入序给出不同序列），调度不可复现。 */
static bool less(const cxf_pqueue_ent *a, const cxf_pqueue_ent *b) {
    if (a->key != b->key) return a->key < b->key;
    return a->id < b->id;
}

static void swap_ent(cxf_pqueue_ent *a, cxf_pqueue_ent *b) {
    cxf_pqueue_ent t = *a;

    *a = *b;
    *b = t;
}

static void sift_up(cxf_pqueue *q, uint32_t i) {
    while (i > 0) {
        uint32_t p = (i - 1u) / 2u;

        if (!less(&q->a[i], &q->a[p])) return;
        swap_ent(&q->a[i], &q->a[p]);
        i = p;
    }
}

static void sift_down(cxf_pqueue *q, uint32_t i) {
    for (;;) {
        uint32_t l = i * 2u + 1u, r = l + 1u, m = i;

        if (l < q->n && less(&q->a[l], &q->a[m])) m = l;
        if (r < q->n && less(&q->a[r], &q->a[m])) m = r;
        if (m == i) return;
        swap_ent(&q->a[i], &q->a[m]);
        i = m;
    }
}

void cxf_pqueue_init(cxf_pqueue *q, void *mem, uint32_t cap) {
    CXF_ASSERT(mem != NULL || cap == 0);
    CXF_ASSERT(((uintptr_t)mem & 3u) == 0);

    q->a = (cxf_pqueue_ent *)mem;
    q->n = 0;
    q->cap = cap;
}

bool cxf_pqueue_push(cxf_pqueue *q, uint32_t key, uint32_t id) {
    if (q->n >= q->cap) return false;   /* 不自动增长：策略在调用方 */

    q->a[q->n].key = key;
    q->a[q->n].id = id;
    sift_up(q, q->n);
    q->n++;
    return true;
}

/* 自底向上下沉：从最后一个非叶结点开始。O(n) —— 多数结点在叶子附近，
 * 最多下沉一两层；只有根会走满 log n。逐个 push 做不到这一点。 */
void cxf_pqueue_make(cxf_pqueue *q, const cxf_pqueue_ent *src, uint32_t n) {
    uint32_t i;

    CXF_ASSERT(src != NULL || n == 0);
    if (n > q->cap) n = q->cap;

    /* 用 memmove 的语义（含 src == q->a 的自拷贝场景），不假设不重叠 */
    if (src != q->a) {
        for (i = 0; i < n; i++) q->a[i] = src[i];
    }
    q->n = n;

    if (n < 2u) return;
    for (i = n / 2u; i > 0; i--) sift_down(q, i - 1u);
}

bool cxf_pqueue_pop(cxf_pqueue *q, uint32_t *key, uint32_t *id) {
    if (q->n == 0) return false;

    if (key != NULL) *key = q->a[0].key;
    if (id != NULL) *id = q->a[0].id;

    q->n--;
    if (q->n != 0) {
        /* 尾元素补到根再下沉：比"把根逐层下移"少一半写操作 */
        q->a[0] = q->a[q->n];
        sift_down(q, 0);
    }
    return true;
}

bool cxf_pqueue_peek(const cxf_pqueue *q, uint32_t *key, uint32_t *id) {
    if (q->n == 0) return false;

    if (key != NULL) *key = q->a[0].key;
    if (id != NULL) *id = q->a[0].id;
    return true;
}

void cxf_pqueue_update_at(cxf_pqueue *q, uint32_t pos, uint32_t key) {
    uint32_t i = pos;

    CXF_ASSERT(pos < q->n);
    if (pos >= q->n) return;   /* release 下的安全出口 */

    q->a[i].key = key;

    /* 变小就上浮；否则下沉。先判断方向可以省掉一次无用的下沉遍历。 */
    if (i > 0 && less(&q->a[i], &q->a[(i - 1u) / 2u])) sift_up(q, i);
    else sift_down(q, i);
}

void cxf_pqueue_clear(cxf_pqueue *q) {
    q->n = 0;
}

bool cxf_pqueue_find_pos(const cxf_pqueue *q, uint32_t id, uint32_t *pos) {
    uint32_t i;

    /* 线性扫：堆的顺序是按 key 排的，不按 id，所以没有可用的剪枝。
     * 这也是"为什么严格 O(log n) 需要额外一列 pos"的原因。 */
    for (i = 0; i < q->n; i++) {
        if (q->a[i].id != id) continue;
        if (pos != NULL) *pos = i;
        return true;
    }
    return false;
}

#ifndef NDEBUG
bool cxf_pqueue_check(const cxf_pqueue *q) {
    uint32_t i;

    if (q->n > q->cap) return false;                             /* P1 */
    if (q->cap != 0 && q->a == NULL) return false;
    for (i = 1; i < q->n; i++)                                   /* P2 */
        if (less(&q->a[i], &q->a[(i - 1u) / 2u])) return false;
    return true;
}
#endif
