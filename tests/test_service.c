#include <stdio.h>
#include <string.h>

#include "cxf/world.h"

#include "services.h"
#include "services.gen.h"

/*
 * 服务注册的端到端验证。与类型注册同模式：装配期按名查一次并固化，
 * 调用期直接用 state 里的函数指针表。
 *
 * 和类型不同的地方只有两点，这里都覆盖到了：
 *   1. 服务有版本号，且版本必须精确匹配（S2）
 *   2. 同一个 name 可以注册多个版本
 */

static int g_fail = 0;

#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

typedef struct {
    const MathApi *math;   /* 固化 */
    const LogApi  *log;
    int32_t        acc;
    uint32_t       ticks;
} Consumer;

static cxf_result consumer_init(cxf_world *w, void *st) {
    Consumer *c = (Consumer *)st;

    c->math = (const MathApi *)cxf_service_find(w, "math", 1);
    c->log  = (const LogApi  *)cxf_service_find(w, "log", 1);
    if (c->math == NULL || c->log == NULL) return CXF_ERR_DEP;
    return CXF_OK;
}

/* 调用期：world 与 frame 都不碰，只用 state 里已固化的函数指针表。 */
static void consumer_tick(cxf_world *w, void *st, cxf_arena *frame) {
    Consumer *c = (Consumer *)st;

    (void)w;
    (void)frame;

    c->acc = c->math->add(c->acc, 1);
    c->log->write("tick");
    c->ticks++;
}

static const cxf_module_def consumer_def = {
    "consumer", CXF_PHASE_UPDATE, sizeof(Consumer), 0, 0, 0,
    consumer_init, consumer_tick, NULL
};

static void test_register_and_find(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;
    const MathApi *m1;
    const MathApi *m0;
    const LogApi  *lv1;

    CHECK(cxf_world_init(&w, alloc, 1 << 16) == CXF_OK);
    CHECK(cxf_service_register_bundle(&w, &cxf_services_services) == CXF_OK);
    CHECK(w.service_count == 3);   /* math v1 / math v0 / log v1 */

    m1 = (const MathApi *)cxf_service_find(&w, "math", 1);
    CHECK(m1 != NULL);
    CHECK(m1->add(2, 3) == 5);
    CHECK(m1->mul(2, 3) == 6);

    /* 同一 name，不同版本：两个条目，同一张表 */
    m0 = (const MathApi *)cxf_service_find(&w, "math", 0);
    CHECK(m0 != NULL && m0 == m1);

    /* S2：版本必须精确匹配，不做向上兼容 */
    CHECK(cxf_service_find(&w, "math", 2) == NULL);
    CHECK(cxf_service_find(&w, "nope", 1) == NULL);

    lv1 = (const LogApi *)cxf_service_find(&w, "log", 1);
    CHECK(lv1 != NULL);
    CHECK(cxf_service_find(&w, "log", 0) == NULL);

    /* S1：重名重版本拒绝 */
    CHECK(cxf_service_register(&w, "math", 1, m1) == CXF_ERR_RANGE);
    /* 同名不同版本是允许的 */
    CHECK(cxf_service_register(&w, "math", 7, m1) == CXF_OK);

    CHECK(cxf_world_check(&w));
    cxf_world_release(&w);
}

/* 整组注册要原子：失败时不留下半张表（S3） */
static void test_bundle_atomic(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;

    CHECK(cxf_world_init(&w, alloc, 1 << 16) == CXF_OK);
    CHECK(cxf_service_register_bundle(&w, &cxf_services_services) == CXF_OK);
    CHECK(cxf_service_register_bundle(&w, &cxf_services_services) == CXF_ERR_RANGE);
    CHECK(w.service_count == 3);   /* 第二次一个都没进去 */

    cxf_world_release(&w);
}

/* 服务缺失在装配期就被抓到，不会拖到某一帧才炸 */
static void test_missing_service_fails_start(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;
    uint32_t id = 0;

    CHECK(cxf_world_init(&w, alloc, 1 << 16) == CXF_OK);
    CHECK(cxf_module_load(&w, &consumer_def, &id) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_ERR_DEP);
    CHECK(!w.started);

    cxf_world_release(&w);
}

static void test_freeze_then_tick(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_world w;
    uint32_t id = 0;
    uint64_t after_init;
    Consumer *c;

    CHECK(cxf_world_init(&w, alloc, 1 << 16) == CXF_OK);
    CHECK(cxf_service_register_bundle(&w, &cxf_services_services) == CXF_OK);
    CHECK(cxf_module_load(&w, &consumer_def, &id) == CXF_OK);
    CHECK(cxf_world_start(&w) == CXF_OK);

    /* 装配期查了两次（math 与 log 各一次） */
    after_init = w.service_find_count;
    CHECK(after_init == 2);

    for (uint32_t i = 0; i < 100; i++) {
        cxf_world_tick(&w);
        CHECK(cxf_world_check(&w));
    }

    /* 调用期零查找：100 帧过去了，计数器一动没动 */
    CHECK(w.service_find_count == after_init);

    c = (Consumer *)cxf_module_state(&w, id);
    CHECK(c->ticks == 100);
    CHECK(c->acc == 100);
    CHECK(c->log->lines() == 100);

    cxf_world_stop(&w);
    cxf_world_release(&w);
}

int main(void) {
    test_register_and_find();
    test_bundle_atomic();
    test_missing_service_fails_start();
    test_freeze_then_tick();

    if (g_fail == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_fail);
    return 1;
}
