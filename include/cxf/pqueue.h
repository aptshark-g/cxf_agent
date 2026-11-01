#ifndef CXF_PQUEUE_H
#define CXF_PQUEUE_H

#include "base.h"

/*
 * 二叉小顶堆：`(key, id)` 数组视图，零分配。
 *
 * 为什么需要它：调度器每一 tick 都要问"下一个到期的任务是谁"，而现在的做法是
 * 每轮全排序（O(n log n)）或用有序数组插入（O(n) 搬移）。堆把这两件事都变成 O(log n)。
 * 注意这条只对**增量**成立：如果每轮都要全量取出，排序仍然更快（见 cxf_pqueue_make）。
 *
 * 为什么 key 之外还要 id：堆只回答"最小的是谁"，不回答"它是哪个任务"。
 * 只存 key 的堆在调度里没用 —— 除非 key 本身就是 id（那就退化成"按 id 排序"）。
 *
 * 为什么相等 key 要比 id：**确定性**。不比较 id 时，相等 key 的弹出顺序取决于
 * 内部移动的历史，同一组任务在不同插入顺序下给出不同调度序 —— 日志与断言都不可复现。
 * 加上 id 之后堆上是全序，同一集合永远给同一条序列。
 *
 * 生命周期：**视图**。数组由调用方给（通常是 arena 帧或世界级分配），本结构只切分它。
 * 满了返回 false 而不自动增长 —— 增长策略属于调用方（与 buf 分工：buf 管增长，堆管次序）。
 *
 * 不变式：
 *   P1  0 <= n <= cap
 *   P2  对任意 i > 0：less(a[i], a[(i-1)/2]) 为假（父不大于子）
 *   P3  less 是全序：key 小者优先，key 相等时 id 小者优先
 *
 * 复杂度：push / pop / update_at O(log n)；peek / size O(1)。
 *
 * 不适用：不做 id → 位置 的反查（decrease-key 需要调用方自己维护一张 pos 表，
 * 然后调 update_at）；不自动扩容；不合并两个堆（要合并请重新 push）。
 */
typedef struct cxf_pqueue_ent {
    uint32_t key;
    uint32_t id;
} cxf_pqueue_ent;

typedef struct cxf_pqueue {
    cxf_pqueue_ent *a;
    uint32_t        n;
    uint32_t        cap;
} cxf_pqueue;

/* mem 至少 cap * sizeof(cxf_pqueue_ent) 字节，4 字节对齐。容量为 0 时允许 mem == NULL。 */
void cxf_pqueue_init(cxf_pqueue *q, void *mem, uint32_t cap);

/* 入堆。满返回 false（不覆盖任何元素）。 */
bool cxf_pqueue_push(cxf_pqueue *q, uint32_t key, uint32_t id);

/*
 * 批量建堆：把 src 的 n 个元素搬进来并**自底向上下沉**成堆。n > cap 时取 cap。
 *
 * 为什么需要它：n 次 push 的建堆是 O(n log n)（最坏：升序输入，每次都浮到根），
 * 自底向下建堆是 O(n) —— 差别在于"每个元素都从叶子浮上来"与"一半元素根本不用动、
 * 另一半最多下沉一层"。构造期就已知全部元素时（每 tick 把新到期任务和存量一起就位）走这条路。
 * 随机 key 下实测约为逐个插入的一半（`make bench` 复现）。
 *
 * **但要注意边界**：堆不该用来做"全量排序取出"。把 n 个元素全部 pop 出来的代价由
 * 排空主导 —— 每次 pop 要沿树下沉、每层一次 cache miss，而全排序是连续扫描。
 * `make bench` 上"make + 全弹"明显慢于"全量排序 + 扫"。
 * 堆的用武之地是**增量**（长期只少量进出、反复取最小而不重排）与**只取前 k 个**。
 */
void cxf_pqueue_make(cxf_pqueue *q, const cxf_pqueue_ent *src, uint32_t n);

/* 弹出最小项。空返回 false，且不写 out。out 可为 NULL。 */
bool cxf_pqueue_pop(cxf_pqueue *q, uint32_t *key, uint32_t *id);

/* 只看不弹。空返回 false。out 可为 NULL。 */
bool cxf_pqueue_peek(const cxf_pqueue *q, uint32_t *key, uint32_t *id);

/*
 * 把位置 pos 的 key 改掉并恢复堆序（可上可下）。
 * pos >= n 时断言失败（debug）并在 release 下直接返回。
 */
void cxf_pqueue_update_at(cxf_pqueue *q, uint32_t pos, uint32_t key);

/*
 * 按 id 找堆内位置。找不到返回 false。复杂度 O(n)。
 *
 * 为什么需要它：update_at 要的是 pos，而 pos 在每次 push / pop 后都会变，**外部
 * 观察不到这些变化** —— 所以"调用方自己维护一张 pos 表"实际做不到，除非本库在
 * push / pop / sift 里回调它（那就成了回调，宪法第 4 条禁止）。
 * 因此 decrease-key 的可用路径是 find_pos + update_at：O(n) 搜索 + O(log n) 调整。
 *
 * 要严格 O(log n) 的 decrease-key，得让本结构自己持有一列 pos（多一块内存 +
 * 每次 sift 交换时同步两个数组）。当前没有调用方需要这个代价，所以不做 ——
 * 等调度器真的把堆当 Dijkstra 的 fringe 用时再加。
 */
bool cxf_pqueue_find_pos(const cxf_pqueue *q, uint32_t id, uint32_t *pos);

void cxf_pqueue_clear(cxf_pqueue *q);

static inline uint32_t cxf_pqueue_size(const cxf_pqueue *q) {
    return q->n;
}

#ifndef NDEBUG
bool cxf_pqueue_check(const cxf_pqueue *q);
#else
#define cxf_pqueue_check(q) ((void)(q), true)
#endif

#endif
