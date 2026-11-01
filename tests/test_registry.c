#include <stdio.h>
#include <string.h>

#include "cxf/world.h"
#include "cxf/soa.h"

#include "soa_types.h"
#include "soa_types.gen.h"

/*
 * 注册期的端到端验证。
 *
 * 关键不是"能注册能查到"，而是三件事被观测到：
 *   1. 字符串查找只发生在装配期（调用期计数不变）
 *   2. 类型缺失在装配期就被抓到，不会拖到运行期
 *   3. 类型不随模块卸载消失（K3）
 */

static int g_fail = 0;

#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

/* ---- 一个真实模块：装配期 find 一次，调用期只用 state 里的指针 ---- */

typedef struct {
    const cxf_type *particle;   /* 固化：装配期查一次，之后不再查 */
    cxf_layout      layout;
    cxf_soa         table;
    uint32_t        col_vx;     /* 固化的列号，连 path 都不用再解析 */
    uint32_t        ticks;
    uint32_t        rows;
} Sim;

static cxf_result sim_init(cxf_world *w, void *st) {
    Sim *s = (Sim *)st;
    const cxf_leaf *vx;

    /* 注册期的产物在这里变成"固化"：字符串查找，一次。 */
    s->particle = cxf_type_find(w, "Particle");
    if (s->particle == NULL) return CXF_ERR_DEP;

    if (cxf_layout_build(&s->layout, s->particle, w->alloc) != CXF_OK)
        return CXF_ERR_NOMEM;
    if (cxf_soa_init(&s->table, &s->layout, w->alloc, 64) != CXF_OK)
        return CXF_ERR_NOMEM;

    vx = cxf_layout_find(&s->layout, "vx");
    if (vx == NULL) return CXF_ERR_DEP;
    s->col_vx = vx->col;

    for (uint32_t i = 0; i < 64; i++) {
        Particle p;
        cxf_handle h;
        memset(&p, 0, sizeof(p));
        p.hp = i;
        h = cxf_soa_add(&s->table);
        cxf_soa_scatter(&s->table, h, &p);
        s->rows++;
    }
    return CXF_OK;
}

/* 调用期：world 与 frame 都不碰，只用 state 里已固化的东西。 */
static void sim_tick(cxf_world *w, void *st, cxf_arena *frame) {
    Sim *s = (Sim *)st;
    float *vx = cxf_soa_col(&s->table, s->col_vx);

    (void)w;
    (void)frame;

    for (uint32_t r = 0; r < s->table.table.count; r++) vx[r] += 1.0f;
    s->ticks++;
}

static void sim_deinit(cxf_world *w, void *st) {
    Sim *s = (Sim *)st;
    cxf_soa_release(&s->table);
    cxf_layout_release(&s->layout, w->alloc);
}

static const cxf_module_def sim_def = {
    "sim", CXF_PHASE_UPDATE, sizeof(Sim), 0, 0, 0,
    sim_init, sim_tick, sim_deinit
};

/* ---- 测试 ---- */

static void test_register_and_find(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;

    CHECK(cxf_world_init(&w, alloc, 1 << 16) == CXF_OK);
    CHECK(cxf_type_register_bundle(&w, &cxf_types_soa_types) == CXF_OK);
    CHECK(w.type_count == 4);

    CHECK(cxf_type_find(&w, "Particle") == &cxf_type_Particle);
    CHECK(cxf_type_find(&w, "Vec3") == &cxf_type_Vec3);
    CHECK(cxf_type_find(&w, "Node") == &cxf_type_Node);
    CHECK(cxf_type_find(&w, "nope") == NULL);

    /* K1：重名一律报错，即使指向同一个描述符 */
    CHECK(cxf_type_register(&w, "Particle", &cxf_type_Particle) == CXF_ERR_RANGE);
    CHECK(cxf_type_register(&w, "Particle", &cxf_type_Vec3) == CXF_ERR_RANGE);
    CHECK(w.type_count == 4);

    CHECK(cxf_world_check(&w));
    cxf_world_release(&w);
}

/* 整组注册要原子：失败时不能留下半张表（K3 只增不减） */
static void test_bundle_atomic(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;

    static const cxf_type_entry dup_entries[] = {
        { "Fresh",    &cxf_type_Vec3     },
        { "Particle", &cxf_type_Particle },   /* 与已注册的重名 */
    };
    static const cxf_type_bundle dup_bundle = { dup_entries, 2, "dup" };

    CHECK(cxf_world_init(&w, alloc, 1 << 16) == CXF_OK);
    CHECK(cxf_type_register_bundle(&w, &cxf_types_soa_types) == CXF_OK);
    CHECK(cxf_type_register_bundle(&w, &dup_bundle) == CXF_ERR_RANGE);

    CHECK(w.type_count == 4);                     /* Fresh 也没有混进去 */
    CHECK(cxf_type_find(&w, "Fresh") == NULL);

    cxf_world_release(&w);
}

/* 类型没注册就在 start 时被抓到，不会拖到某一帧才炸 */
static void test_missing_type_fails_start(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;
    uint32_t id = 0;

    CHECK(cxf_world_init(&w, alloc, 1 << 16) == CXF_OK);
    CHECK(cxf_module_load(&w, &sim_def, &id) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_ERR_DEP);
    CHECK(!w.started);

    cxf_world_release(&w);
}

static void test_freeze_then_tick(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;
    uint32_t id = 0;
    uint64_t after_init;
    Sim *s;

    CHECK(cxf_world_init(&w, alloc, 1 << 16) == CXF_OK);
    CHECK(cxf_type_register_bundle(&w, &cxf_types_soa_types) == CXF_OK);
    CHECK(cxf_module_load(&w, &sim_def, &id) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_OK);

    /* 装配期只查了一次 */
    after_init = w.type_find_count;
    CHECK(after_init == 1);

    for (uint32_t i = 0; i < 100; i++) {
        cxf_world_tick(&w);
        CHECK(cxf_world_check(&w));
    }

    /* 调用期零查找：跑 100 帧，计数器一动没动 */
    CHECK(w.type_find_count == after_init);

    s = (Sim *)cxf_module_state(&w, id);
    CHECK(s->ticks == 100);
    CHECK(s->rows == 64);
    {
        float *vx = cxf_soa_col(&s->table, s->col_vx);
        CHECK(vx[0] == 100.0f);
        CHECK(vx[63] == 100.0f);
    }

    cxf_world_stop(&w);
    cxf_world_release(&w);
}

/* K3：类型不随模块卸载消失。模块能卸载是因为 state 是运行时分配的，
   类型的存储压根不在 world 里。 */
static void test_type_outlives_module(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;
    uint32_t id = 0;

    CHECK(cxf_world_init(&w, alloc, 1 << 16) == CXF_OK);
    CHECK(cxf_type_register_bundle(&w, &cxf_types_soa_types) == CXF_OK);
    CHECK(cxf_module_load(&w, &sim_def, &id) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_OK);
    cxf_world_tick(&w);
    cxf_world_stop(&w);

    CHECK(cxf_module_unload(&w, id) == CXF_OK);
    CHECK(w.type_count == 4);
    CHECK(cxf_type_find(&w, "Particle") == &cxf_type_Particle);

    cxf_world_release(&w);
}

int main(void) {
    test_register_and_find();
    test_bundle_atomic();
    test_missing_type_fails_start();
    test_freeze_then_tick();
    test_type_outlives_module();

    if (g_fail == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_fail);
    return 1;
}
