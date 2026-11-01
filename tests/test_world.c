#include <stdio.h>
#include <string.h>

#include "cxf/world.h"

static int g_fail = 0;

#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

#define CAP_MEM (1u << 0)
#define CAP_LOG (1u << 1)
#define CAP_NET (1u << 2)

static uint32_t g_seq[32];
static uint32_t g_seq_n;
static int32_t  g_pos_mem = -1, g_pos_net = -1, g_pos_late = -1;

static void seq_reset(void) {
    g_seq_n = 0;
    g_pos_mem = g_pos_net = g_pos_late = -1;
}

/* 只在缓冲区内记录；计数器照常递增，因此长跑测试不会越界。 */
static void seq_push(uint32_t id, int32_t *pos) {
    if (g_seq_n < 32) g_seq[g_seq_n] = id;
    if (pos != NULL) *pos = (int32_t)g_seq_n;
    g_seq_n++;
}

typedef struct { uint32_t allocs; } mem_state;

static void mem_tick(cxf_world *w, void *state, cxf_arena *frame) {
    (void)w; (void)frame;
    ((mem_state *)state)->allocs++;
    seq_push(0, &g_pos_mem);
}

static void net_tick(cxf_world *w, void *state, cxf_arena *frame) {
    (void)w;
    (*(uint32_t *)state)++;
    cxf_arena_alloc(frame, 64, 16);
    seq_push(1, &g_pos_net);
}

static void late_tick(cxf_world *w, void *state, cxf_arena *frame) {
    (void)w; (void)state; (void)frame;
    seq_push(2, &g_pos_late);
}

static const cxf_module_def mem_def = {
    .name = "mem", .phase = CXF_PHASE_INPUT, .state_size = sizeof(mem_state),
    .provides = CAP_MEM, .tick = mem_tick,
};

static const cxf_module_def net_def = {
    .name = "net", .phase = CXF_PHASE_INPUT, .state_size = sizeof(uint32_t),
    .requires_hard = CAP_MEM, .requires = CAP_LOG, .tick = net_tick,
};

static const cxf_module_def late_def = {
    .name = "late", .phase = CXF_PHASE_LATE, .state_size = 0,
    .tick = late_tick,
};

/* 无状态且不参与 tick —— 颗粒度随意的下限。 */
static uint32_t g_util_calls;
static void util_tick(cxf_world *w, void *state, cxf_arena *frame) {
    (void)w; (void)state; (void)frame;
    g_util_calls++;
}
static const cxf_module_def util_def = {
    .name = "util", .phase = CXF_PHASE_NONE, .state_size = 0,
    .provides = CAP_LOG, .tick = util_tick,
};

static void test_order_and_deps(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;
    uint32_t id;

    cxf_world_init(&w, alloc, 4096);
    seq_reset();

    CHECK(cxf_module_load(&w, &net_def, NULL) == CXF_OK);   /* 故意先加载依赖方 */
    CHECK(cxf_module_load(&w, &mem_def, &id) == CXF_OK);
    CHECK(cxf_module_load(&w, &late_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &util_def, NULL) == CXF_OK);
    CHECK(cxf_world_check(&w));

    CHECK(cxf_world_start(&w) == CXF_OK);
    CHECK(cxf_world_check(&w));

    cxf_world_tick(&w);
    CHECK(g_seq_n == 3);

    /* HARD 依赖：mem 必须排在 net 之前，与加载顺序无关 */
    CHECK(g_pos_mem >= 0 && g_pos_net >= 0);
    CHECK(g_pos_mem < g_pos_net);
    /* phase 分段：INPUT 段整体早于 LATE 段 */
    CHECK(g_pos_net < g_pos_late);
    /* phase == NONE 的模块不参与 tick */
    CHECK(g_util_calls == 0);
    /* 无状态模块的 state 为 NULL 是合法的 */
    CHECK(cxf_module_get(&w, 3)->state == NULL);

    cxf_world_tick(&w);
    CHECK(((mem_state *)cxf_module_state(&w, id))->allocs == 2);

    cxf_world_release(&w);
}

/* SOFT 依赖缺失不影响加载与运行；HARD 缺失直接失败。 */
static void test_hard_vs_soft(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;

    cxf_world_init(&w, alloc, 4096);

    /* net 的 SOFT 依赖 CAP_LOG 没有提供者，仍应成功 */
    CHECK(cxf_module_load(&w, &net_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &mem_def, NULL) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_OK);
    CHECK(cxf_world_check(&w));
    cxf_world_release(&w);

    /* 缺 mem 时，net 的 HARD 依赖无提供者 —— start 必须失败 */
    cxf_world_init(&w, alloc, 4096);
    CHECK(cxf_module_load(&w, &net_def, NULL) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_ERR_DEP);
    CHECK(!w.started);
    cxf_world_release(&w);
}

static const cxf_module_def cyc_a_def = {
    .name = "a", .phase = CXF_PHASE_INPUT, .state_size = 0,
    .provides = 1u, .requires_hard = 2u, .tick = NULL,
};
static const cxf_module_def cyc_b_def = {
    .name = "b", .phase = CXF_PHASE_INPUT, .state_size = 0,
    .provides = 2u, .requires_hard = 1u, .tick = NULL,
};

static void test_cycle(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;

    cxf_world_init(&w, alloc, 4096);
    CHECK(cxf_module_load(&w, &cyc_a_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &cyc_b_def, NULL) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_ERR_DEP);
    CHECK(!w.started);
    cxf_world_release(&w);
}

/* 被 HARD 依赖的模块卸不掉；解除依赖后可以。 */
static void test_unload_refcount(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;
    uint32_t mem_id = 0, net_id = 0;

    cxf_world_init(&w, alloc, 4096);
    CHECK(cxf_module_load(&w, &mem_def, &mem_id) == CXF_OK);
    CHECK(cxf_module_load(&w, &net_def, &net_id) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_OK);

    CHECK(cxf_module_get(&w, mem_id)->refcount == 1);
    CHECK(cxf_module_unload(&w, mem_id) == CXF_ERR_RANGE);

    CHECK(cxf_module_unload(&w, net_id) == CXF_OK);
    CHECK(cxf_module_get(&w, mem_id)->refcount == 0);
    CHECK(cxf_module_unload(&w, mem_id) == CXF_OK);
    CHECK(cxf_world_check(&w));

    cxf_world_release(&w);
}

/* 同名模块不允许重复加载（M1）。 */
static void test_dup_name(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;

    cxf_world_init(&w, alloc, 4096);
    CHECK(cxf_module_load(&w, &mem_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &mem_def, NULL) == CXF_ERR_RANGE);
    CHECK(cxf_module_find(&w, "mem") != NULL);
    CHECK(cxf_module_find(&w, "nope") == NULL);
    cxf_world_release(&w);
}

/* ---- DI：装配期查找并固化，调用期零查找 ---- */

typedef struct { void (*write)(const char *s); } log_api_v1;
static uint32_t g_log_calls;

static void log_write_impl(const char *s) { (void)s; g_log_calls++; }
static const log_api_v1 g_log_api = { log_write_impl };

typedef struct { const log_api_v1 *log; uint32_t ticks; } user_state;

static cxf_result user_init(cxf_world *w, void *state) {
    /* 装配期：字符串查找只发生这一次 */
    ((user_state *)state)->log = cxf_service_find(w, "log", 1);
    return ((user_state *)state)->log != NULL ? CXF_OK : CXF_ERR_DEP;
}

static void user_tick(cxf_world *w, void *state, cxf_arena *frame) {
    (void)w; (void)frame;
    /* 调用期：直接用固化好的指针，一次跳转直达 */
    ((user_state *)state)->log->write("hi");
    ((user_state *)state)->ticks++;
}

static const cxf_module_def user_def = {
    .name = "user", .phase = CXF_PHASE_UPDATE, .state_size = sizeof(user_state),
    .init = user_init, .tick = user_tick,
};

static void test_service_di(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;
    uint32_t user_id = 0;

    cxf_world_init(&w, alloc, 4096);
    g_log_calls = 0;

    CHECK(cxf_service_register(&w, "log", 1, &g_log_api) == CXF_OK);
    CHECK(cxf_service_register(&w, "log", 1, &g_log_api) == CXF_ERR_RANGE);  /* S1 */

    /* 版本不匹配则找不到（S2） */
    CHECK(cxf_service_find(&w, "log", 2) == NULL);
    CHECK(cxf_service_find(&w, "log", 1) == &g_log_api);
    CHECK(cxf_service_find(&w, "nope", 1) == NULL);

    CHECK(cxf_module_load(&w, &user_def, &user_id) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_OK);

    cxf_world_tick(&w);
    cxf_world_tick(&w);
    cxf_world_tick(&w);
    CHECK(g_log_calls == 3);
    CHECK(((user_state *)cxf_module_state(&w, user_id))->ticks == 3);

    /* 缺服务时 init 失败，不进入半启动状态 */
    cxf_world_release(&w);

    cxf_world_init(&w, alloc, 4096);
    CHECK(cxf_module_load(&w, &user_def, NULL) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_ERR_DEP);
    CHECK(!w.started);
    cxf_world_release(&w);
}

/* 帧内分配整帧回收：多帧之后不应持续增长。 */
static void test_frame_recycle(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;

    cxf_world_init(&w, alloc, 4096);
    CHECK(cxf_module_load(&w, &mem_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &net_def, NULL) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_OK);

    for (int i = 0; i < 100; i++) cxf_world_tick(&w);

    /* 每帧 64 字节，100 帧若不回收需 6400 字节；复用后仍停在一个 chunk 内 */
    CHECK(w.arena_frame.stat.chunk_count == 1);
    CHECK(w.tick_count == 100);
    CHECK(cxf_arena_check(&w.arena_frame));
    CHECK(cxf_world_check(&w));

    cxf_world_release(&w);
}

/* 阶段计时是槽位：内核只写数，不解释。 */
static uint64_t g_busy_sink;

static void busy_tick(cxf_world *w, void *state, cxf_arena *frame) {
    (void)w; (void)state; (void)frame;
    volatile uint64_t s = 0;
    for (int i = 0; i < 50000; i++) s += (uint64_t)i;
    g_busy_sink = s;
}

static const cxf_module_def busy_def = {
    .name = "busy", .phase = CXF_PHASE_UPDATE, .state_size = 0, .tick = busy_tick,
};

static void test_phase_timing(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;

    cxf_world_init(&w, alloc, 4096);
    CHECK(cxf_module_load(&w, &mem_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &late_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &busy_def, NULL) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_OK);

    cxf_world_tick(&w);
    /* 只有耗时超过时钟粒度的阶段才测得到非零值 —— 这是计时本身的性质，
     * 不是 bug。极快模块的 0 表示"低于分辨率"，语义正确。 */
    CHECK(cxf_world_phase_ns(&w, CXF_PHASE_UPDATE) > 0);
    CHECK(cxf_world_phase_ns(&w, CXF_PHASE_INPUT) >= 0);
    CHECK(cxf_world_phase_ns(&w, CXF_PHASE_LATE) >= 0);
    CHECK(g_busy_sink != 0);

    /* 运行时加载后重新 start，新模块应被纳入 */
    CHECK(cxf_module_load(&w, &net_def, NULL) == CXF_OK);
    CHECK(!w.started);
    CHECK(cxf_world_start(&w) == CXF_OK);
    seq_reset();
    cxf_world_tick(&w);
    CHECK(g_seq_n == 3);
    CHECK(cxf_world_check(&w));

    cxf_world_release(&w);
}

/*
 * 释放必须把 allocator 的记账还回原点。
 *
 * 这条断言是"用 A 算式申请、用 B 算式释放"这类错误的**唯一**可观察后果：
 * 堆分配器拿到 size 就丢掉了（heap_free 忽略它），所以 free 用错大小不会崩、
 * 不会漏内存，只会让 bytes_live 单调漂移 —— 直到有人换成一个真正在意 size 的
 * allocator（池 / 共享段），那时才会变成堆损坏。
 *
 * 落地时 world 里就有两处：edges 用 cap 申请、用 count 释放；order 用 (n+1)*4
 * 申请、用 module_count*4 释放。两处都靠把"指针 + 容量"绑成 cxf_vec 消掉。
 */
static void test_release_accounting(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    uint64_t before = alloc->stat.bytes_live;
    cxf_world w;

    cxf_world_init(&w, alloc, 4096);
    seq_reset();
    CHECK(cxf_module_load(&w, &net_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &mem_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &late_def, NULL) == CXF_OK);
    CHECK(cxf_module_load(&w, &util_def, NULL) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_OK);
    cxf_world_tick(&w);

    /* 再 start 一次：order 与 edges 都会重新申请/释放，更容易暴露 size 不一致 */
    CHECK(cxf_world_start(&w) == CXF_OK);

    cxf_world_release(&w);
    CHECK(alloc->stat.bytes_live == before);
}

/* 与 mem 同 phase、零依赖：与 mem 一起构成"零入度并列"的情形。 */
static int32_t g_pos_alt = -1;
static void alt_tick(cxf_world *w, void *state, cxf_arena *frame) {
    (void)w; (void)frame;
    (*(uint32_t *)state)++;
    seq_push(3, &g_pos_alt);
}
static const cxf_module_def alt_def = {
    .name = "alt", .phase = CXF_PHASE_INPUT, .state_size = sizeof(uint32_t),
    .tick = alt_tick,
};

/*
 * 零入度并列时按**模块 id 升序** —— 这是"同一张图永远给同一条执行序列"里的
 * 并列规则。它不在 world 里实现了（Kahn 已下沉到 cxf_algo_toposort），
 * 但调用方依赖它：装载顺序一变，日志与断言就该跟着变，而不是随机变。
 *
 * 此前没有任何测试钉住这条 —— 只断言过"mem 在 net 之前"这类相对序。
 * 换排序实现时，相对序可能还对、并列序已经漂了。
 */
static void test_topo_determinism(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;

    cxf_world_init(&w, alloc, 4096);
    seq_reset();
    g_pos_alt = -1;

    CHECK(cxf_module_load(&w, &mem_def, NULL) == CXF_OK);   /* id 0，零入度 */
    CHECK(cxf_module_load(&w, &alt_def, NULL) == CXF_OK);   /* id 1，零入度 */
    CHECK(cxf_module_load(&w, &net_def, NULL) == CXF_OK);   /* id 2，hard 依赖 id 0 */

    CHECK(cxf_world_start(&w) == CXF_OK);
    cxf_world_tick(&w);

    CHECK(g_seq_n == 3);
    CHECK(g_seq[0] == 0 && g_seq[1] == 3 && g_seq[2] == 1);   /* mem, alt, net */
    CHECK(g_pos_mem >= 0 && g_pos_alt >= 0 && g_pos_net >= 0);
    CHECK(g_pos_mem < g_pos_alt && g_pos_alt < g_pos_net);

    cxf_world_release(&w);
}

int main(void) {
    test_order_and_deps();
    test_hard_vs_soft();
    test_cycle();
    test_unload_refcount();
    test_dup_name();
    test_service_di();
    test_frame_recycle();
    test_phase_timing();
    test_topo_determinism();
    test_release_accounting();

    if (g_fail == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_fail);
    return 1;
}
