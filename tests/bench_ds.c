#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cxf/algo.h"
#include "cxf/alloc.h"
#include "cxf/arena.h"
#include "cxf/pqueue.h"
#include "cxf/ring.h"
#include "cxf/table.h"

#define N       1000000
#define ROUNDS  3

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/*
 * 每个用例先完整跑一轮预热：让 32MB 工作集的 page fault 与 chunk 分配
 * 发生在计时之外。之后取 ROUNDS 轮最小值。
 * sink 用 volatile 承接，避免编译器把整段循环消除掉。
 */
static volatile uint64_t g_sink;

static uint8_t g_raw[64u * 1024 * 1024];

static void run_raw_bump(void) {
    size_t off = 0;
    for (int i = 0; i < N; i++) {
        volatile uint32_t *q = (volatile uint32_t *)(g_raw + off);
        off += 32;
        *q = (uint32_t)i;
        g_sink += *q;
    }
}

static void run_malloc(void) {
    for (int i = 0; i < N; i++) {
        volatile uint32_t *q = malloc(32);
        *q = (uint32_t)i;
        g_sink += *q;
        free((void *)q);
    }
}

static cxf_arena g_arena;

static void run_arena_alloc(void) {
    for (int i = 0; i < N; i++) {
        volatile uint32_t *q = cxf_arena_alloc(&g_arena, 32, 16);
        *q = (uint32_t)i;
        g_sink += *q;
    }
}

static void run_arena_frame(void) {
    for (int i = 0; i < N; i++) {
        cxf_arena_push(&g_arena);
        volatile uint32_t *q = cxf_arena_alloc(&g_arena, 32, 16);
        *q = (uint32_t)i;
        g_sink += *q;
        cxf_arena_pop(&g_arena);
    }
}

static cxf_table g_table;
static cxf_handle *g_handles;

static void run_table_scan(void) {
    uint32_t *ids = cxf_table_col(&g_table, 0);
    float *xs = cxf_table_col(&g_table, 1);
    double sum = 0;
    for (int i = 0; i < N; i++) sum += (double)ids[i] + (double)xs[i];
    g_sink += (uint64_t)sum;
}

static void run_table_get(void) {
    for (int i = 0; i < N; i++)
        g_sink += *(uint32_t *)cxf_table_get(&g_table, g_handles[i], 0);
}

static cxf_ring g_ring;
static uint8_t g_msg[64];

static void run_ring(void) {
    for (int i = 0; i < N; i++) {
        uint32_t w = cxf_ring_write(&g_ring, g_msg, sizeof(g_msg));
        g_sink += cxf_ring_read(&g_ring, g_msg, w);
    }
}

/*
 * 指针 chase vs 稠密索引。四种形态隔开两个变量：
 *   "有没有指针" 与 "下一个地址能不能提前算出"
 *
 * 要盯的不是"顺序 vs 随机"，而是"这一次 load 的地址要不要等上一次 load 完成"。
 * 链表顺序链接也走得很慢，就是因为依赖链把访存并行度压成了 1。
 */
typedef struct LNode { uint32_t v; struct LNode *next; } LNode;

static LNode   *g_nodes;
static uint32_t *g_vals;
static uint32_t *g_perm;
static LNode   *g_head_seq;
static LNode   *g_head_rand;

static uint32_t g_rng = 2463534242u;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void setup_lists(void) {
    g_nodes = malloc(sizeof(LNode) * N);
    g_vals  = malloc(sizeof(uint32_t) * N);
    g_perm  = malloc(sizeof(uint32_t) * N);
    if (g_nodes == NULL || g_vals == NULL || g_perm == NULL) exit(1);

    for (int i = 0; i < N; i++) {
        g_nodes[i].v = (uint32_t)i;
        g_nodes[i].next = (i + 1 < N) ? &g_nodes[i + 1] : NULL;
        g_vals[i] = (uint32_t)i;
        g_perm[i] = (uint32_t)i;
    }
    g_head_seq = &g_nodes[0];

    /* 打乱链接顺序：内存里仍是连续节点，但走的路线是随机的。 */
    for (int i = N - 1; i > 0; i--) {
        uint32_t j = rnd() % (uint32_t)(i + 1);
        uint32_t t = g_perm[i]; g_perm[i] = g_perm[j]; g_perm[j] = t;
    }
    for (int i = 0; i < N; i++)
        g_nodes[g_perm[i]].next = (i + 1 < N) ? &g_nodes[g_perm[i + 1]] : NULL;
    g_head_rand = &g_nodes[g_perm[0]];
}

static void teardown_lists(void) {
    free(g_nodes); free(g_vals); free(g_perm);
}

static void run_chase_seq(void) {
    uint64_t s = 0;
    for (LNode *p = g_head_seq; p != NULL; p = p->next) s += p->v;
    g_sink += s;
}

static void run_chase_rand(void) {
    uint64_t s = 0;
    for (LNode *p = g_head_rand; p != NULL; p = p->next) s += p->v;
    g_sink += s;
}

static void run_gather_rand(void) {
    uint64_t s = 0;
    for (int i = 0; i < N; i++) s += g_vals[g_perm[i]];
    g_sink += s;
}

static void run_dense_seq(void) {
    uint64_t s = 0;
    for (int i = 0; i < N; i++) s += g_vals[i];
    g_sink += s;
}

static double timed(void (*fn)(void), void (*reset)(void), const char *label) {
    double best = 1e30;

    fn();
    if (reset != NULL) reset();

    for (int r = 0; r < ROUNDS; r++) {
        double t0 = now_sec();
        fn();
        double t1 = now_sec();
        if (t1 - t0 < best) best = t1 - t0;
        if (reset != NULL) reset();
    }

    printf("%-24s %8.2f ns/op  %8.0f Mops/s\n",
           label, best * 1e9 / N, N / best / 1e6);
    return best;
}

static void reset_raw(void) { }
static void reset_arena(void) { cxf_arena_reset(&g_arena); }

/*
 * 事件队列的两种做法，**建堆与排空必须分开测**。
 * 合在一起测时，排空（随机访问、每层一次 cache miss）会淹没建堆的差别，
 * 于是"O(n) 建堆 vs n 次 O(log n) 插入"这个真正要对比的量看不到。
 *
 *   sort        全量排序 + 扫       —— 数组连续，cache 友好，是"全量取出"的下限
 *   make        自底向上建堆         —— O(n)
 *   n×push      逐个插入             —— O(n log n)
 *   make+全弹   建堆 + 排空          —— 排空占绝对主导，说明"全量排空"本就不该用堆
 *
 * 堆的用武之地是增量（长期只少量进出）与只取前 k 个，不是"全量进出一次"。
 */
static cxf_pqueue g_pq;
static uint32_t *g_pq_scratch;
static cxf_pqueue_ent *g_pq_mem;
static cxf_pqueue_ent *g_pq_src;

static void pq_drain(void) {
    uint64_t s = 0;
    uint32_t k = 0, id = 0;

    while (cxf_pqueue_pop(&g_pq, &k, &id)) s += k;
    g_sink += s;
}

static void run_pq_make(void) {
    cxf_pqueue_make(&g_pq, g_pq_src, N);
}

static void run_pq_push(void) {
    cxf_pqueue_clear(&g_pq);
    for (int i = 0; i < N; i++) cxf_pqueue_push(&g_pq, g_pq_src[i].key, g_pq_src[i].id);
}

static void run_pq_make_drain(void) {
    cxf_pqueue_make(&g_pq, g_pq_src, N);
    pq_drain();
}

static void run_sort_scan(void) {
    uint64_t s = 0;

    for (int i = 0; i < N; i++) g_pq_scratch[i] = g_pq_src[i].key;
    cxf_algo_sort_u32(g_pq_scratch, N);
    for (int i = 0; i < N; i++) s += g_pq_scratch[i];
    g_sink += s;
}

/*
 * 搬运基准：把 agent 层报的**字节账**换算成**纳秒**的汇率。
 *
 * 没有这个数，"bytes_copy 比 bytes_out 还多"只是一句描述；有了它才能回答
 * 真正的问题——那一次聚合拷贝占这次 handler 的百分之几。
 *
 * 四档覆盖 agent 的真实报文尺度：短报价 ~16B、一次工具返回 ~512B、
 * 一段 prompt 片段 ~4KiB。刻意用运行时变量作长度，使 memcpy 无法被内联成
 * 几条 move 指令——这和 `blob_append` 的实际调用形态一致（长度运行时才知）。
 */
static uint8_t *g_copy_src, *g_copy_dst;
static uint32_t g_copy_len;

static void run_copy(void) {
    for (int i = 0; i < N; i++) {
        memcpy(g_copy_dst, g_copy_src, g_copy_len);
        g_sink += g_copy_dst[0];
    }
}

static void bench_copy(void) {
    static const uint32_t sizes[] = { 16u, 64u, 512u, 4096u };
    char label[48];

    g_copy_src = malloc(4096);
    g_copy_dst = malloc(4096);
    if (g_copy_src == NULL || g_copy_dst == NULL) return;
    memset(g_copy_src, 0xA5, 4096);

    printf("\n-- 搬运汇率（1 字节多少钱）\n");
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        double best = 1e30;

        g_copy_len = sizes[k];
        run_copy();
        for (int r = 0; r < ROUNDS; r++) {
            double t0 = now_sec();
            run_copy();
            double t1 = now_sec();
            if (t1 - t0 < best) best = t1 - t0;
        }
        snprintf(label, sizeof(label), "memcpy %uB", sizes[k]);
        printf("%-22s %8.2f ns/op  %8.1f GB/s\n",
               label, best * 1e9 / N, (double)N * sizes[k] / best / 1e9);
    }

    free(g_copy_src);
    free(g_copy_dst);
}

int main(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    const size_t sizes[2] = { sizeof(uint32_t), sizeof(float) };

    printf("N = %d, 取 %d 轮最小值，预热一轮\n\n", N, ROUNDS);

    timed(run_raw_bump, reset_raw, "raw bump 无记账");
    timed(run_malloc, NULL, "malloc alloc+free");

    cxf_arena_init(&g_arena, alloc, 64 * 1024);
    timed(run_arena_alloc, reset_arena, "arena  alloc");
    printf("  %-22s chunks=%llu  chunk_bytes=%llu\n", "arena 状态",
           (unsigned long long)g_arena.stat.chunk_count,
           (unsigned long long)g_arena.stat.chunk_bytes);
    timed(run_arena_frame, reset_arena, "arena  alloc+push+pop");
    printf("  %-22s chunks=%llu  peak=%llu\n", "arena 状态",
           (unsigned long long)g_arena.stat.chunk_count,
           (unsigned long long)g_arena.stat.bytes_peak);
    cxf_arena_release(&g_arena);

    if (cxf_table_init(&g_table, alloc, sizes, 2, N) != CXF_OK) return 1;
    g_handles = malloc(sizeof(cxf_handle) * N);
    for (int i = 0; i < N; i++) {
        g_handles[i] = cxf_table_add(&g_table);
        *(uint32_t *)cxf_table_get(&g_table, g_handles[i], 0) = (uint32_t)i;
        *(float *)cxf_table_get(&g_table, g_handles[i], 1) = (float)i * 0.5f;
    }
    timed(run_table_scan, NULL, "table  SoA 顺序扫描");
    timed(run_table_get, NULL, "table  handle->ptr");
    free(g_handles);
    cxf_table_release(&g_table);

    setup_lists();
    timed(run_dense_seq,   NULL, "dense  顺序扫描 u32");
    timed(run_gather_rand, NULL, "dense  随机 gather");
    timed(run_chase_seq,   NULL, "ptr    链表(顺序链接)");
    timed(run_chase_rand,  NULL, "ptr    链表(随机链接)");
    teardown_lists();

    if (cxf_ring_init(&g_ring, alloc, 1 << 16) != CXF_OK) return 1;
    for (int i = 0; i < (int)sizeof(g_msg); i++) g_msg[i] = (uint8_t)i;
    timed(run_ring, NULL, "ring   write+read 64B");
    cxf_ring_release(&g_ring, alloc);

    /* 优先级队列：自带一份 key 序列 —— 不能用上面的 g_perm，它已随
     * teardown_lists() 释放（这里踩过一次 use-after-free：读到的"随机序"
     * 其实是复用后的内存，push 成本被测成 1.1 ns，看着比 make 还快）。 */
    g_pq_scratch = malloc(sizeof(uint32_t) * N);
    g_pq_mem     = malloc(sizeof(cxf_pqueue_ent) * N);
    g_pq_src     = malloc(sizeof(cxf_pqueue_ent) * N);
    if (g_pq_scratch == NULL || g_pq_mem == NULL || g_pq_src == NULL) return 1;
    for (int i = 0; i < N; i++) {
        g_pq_src[i].key = (uint32_t)i;
        g_pq_src[i].id  = (uint32_t)i;   /* id 唯一 ⇒ 全序无歧义 */
    }
    for (int i = N - 1; i > 0; i--) {
        uint32_t j = rnd() % (uint32_t)(i + 1);
        uint32_t t = g_pq_src[i].key;
        g_pq_src[i].key = g_pq_src[j].key;
        g_pq_src[j].key = t;
    }
    cxf_pqueue_init(&g_pq, g_pq_mem, N);
    timed(run_sort_scan, NULL, "sort   全量排序+扫");
    timed(run_pq_make, NULL, "pqueue 建堆 make");
    timed(run_pq_push, NULL, "pqueue 建堆 n×push");
    timed(run_pq_make_drain, NULL, "pqueue make+全弹");
    free(g_pq_src);
    free(g_pq_mem);
    free(g_pq_scratch);

    bench_copy();

    return 0;
}
