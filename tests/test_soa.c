#include <stdio.h>
#include <string.h>

#include "cxf/soa.h"

/* 类型描述符来自 tools/gen_registry.py，不再是手写的。 */
#include "soa_types.h"
#include "soa_types.gen.h"

static int g_fail = 0;

#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

/* 生成物与编译器的一致性：描述符里的 size/align 必须就是 sizeof/_Alignof。
   生成器不长数值，这两条是对它唯一的实质校验。 */
static void test_generated_matches_compiler(void) {
    CHECK(cxf_type_Particle.size == sizeof(Particle));
    CHECK(cxf_type_Particle.align == _Alignof(Particle));
    CHECK(cxf_type_Vec3.size == sizeof(Vec3));
    CHECK(cxf_type_Wide.size == sizeof(Wide));
    CHECK(cxf_type_Node.size == sizeof(Node));

    CHECK(cxf_type_check(&cxf_type_Particle));
    CHECK(cxf_type_check(&cxf_type_Vec3));
    CHECK(cxf_type_check(&cxf_type_Node));
    CHECK(cxf_type_check(&cxf_type_Wide));

    /* 指针叶子的 target 指向自身，且不递归（Y3） */
    CHECK(cxf_type_Node.fields[1].type->kind == CXF_K_PTR);
    CHECK(cxf_type_Node.fields[1].type->target == &cxf_type_Node);
}

static void test_flatten(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_layout l;
    const cxf_leaf *lf;

    CHECK(cxf_layout_build(&l, &cxf_type_Particle, alloc) == CXF_OK);

    /* pos 展开成 3 个叶子，加 vx / vy / hp / owner */
    CHECK(l.leaf_count == 7);

    CHECK(strcmp(l.leaves[0].path, "pos.x") == 0);
    CHECK(l.leaves[0].offset == 0);
    CHECK(strcmp(l.leaves[2].path, "pos.z") == 0);
    CHECK(l.leaves[2].offset == offsetof(Particle, pos) + offsetof(Vec3, z));
    CHECK(strcmp(l.leaves[3].path, "vx") == 0);
    CHECK(l.leaves[3].offset == offsetof(Particle, vx));

    lf = cxf_layout_find(&l, "pos.y");
    CHECK(lf != NULL && lf->offset == 4);
    CHECK(cxf_layout_find(&l, "nope") == NULL);

    /* 每个叶子的偏移都不越界（Y4） */
    for (uint32_t i = 0; i < l.leaf_count; i++)
        CHECK(l.leaves[i].offset + l.leaves[i].type->size <= sizeof(Particle));

    cxf_layout_release(&l, alloc);
}

/* 你最初要的"树形筛选"：先按大小分组，再看具体类型。 */
static void test_select(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_layout l;
    const cxf_leaf *out[16];
    uint32_t n;

    CHECK(cxf_layout_build(&l, &cxf_type_Particle, alloc) == CXF_OK);

    n = cxf_layout_select_size(&l, 4, out, 16);
    CHECK(n == 6);   /* pos.x/y/z, vx, vy, hp —— owner 是 8 字节 */
    for (uint32_t i = 0; i < n; i++) CHECK(out[i]->type->size == 4);

    n = cxf_layout_select_size(&l, 8, out, 16);
    CHECK(n == 1);
    CHECK(strcmp(out[0]->path, "owner") == 0);

    n = cxf_layout_select_kind(&l, CXF_K_F32, out, 16);
    CHECK(n == 5);   /* pos.x/y/z, vx, vy */

    n = cxf_layout_select_kind(&l, CXF_K_PTR, out, 16);
    CHECK(n == 1);

    cxf_layout_release(&l, alloc);
}

static void test_soa_roundtrip(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_layout l;
    cxf_soa s;
    cxf_handle h;
    Particle p, q;

    CHECK(cxf_layout_build(&l, &cxf_type_Particle, alloc) == CXF_OK);
    CHECK(cxf_soa_init(&s, &l, alloc, 8) == CXF_OK);
    CHECK(cxf_soa_check(&s));
    CHECK(s.table.col_count == 7);

    h = cxf_soa_add(&s);
    CHECK(cxf_soa_live(&s, h));

    p.pos.x = 1.0f; p.pos.y = 2.0f; p.pos.z = 3.0f;
    p.vx = 4.0f; p.vy = 5.0f; p.hp = 6; p.owner = (void *)(uintptr_t)0x1234;

    /* 冷路径：整体写入再整体读出，必须逐字节相同 */
    cxf_soa_scatter(&s, h, &p);
    memset(&q, 0xAA, sizeof(q));
    cxf_soa_gather(&s, h, &q);
    CHECK(memcmp(&p, &q, sizeof(p)) == 0);

    /* 热路径：直接改一列，gather 时应看到该字段变化、其余不变 */
    {
        float *xs = cxf_soa_col(&s, 0);
        uint32_t row = cxf_table_row(&s.table, h);
        xs[row] = 99.0f;
        cxf_soa_gather(&s, h, &q);
        CHECK(q.pos.x == 99.0f);
        CHECK(q.pos.y == 2.0f && q.pos.z == 3.0f);
        CHECK(q.hp == 6);
    }

    /* 单个元素地址访问 */
    {
        uint32_t *hp = cxf_soa_get(&s, h, 5);
        CHECK(hp != NULL && *hp == 6);
        *hp = 77;
        cxf_soa_gather(&s, h, &q);
        CHECK(q.hp == 77);
    }

    /* 删除后 handle 失效，gather 不应改动目标 */
    {
        Particle keep;
        memcpy(&keep, &q, sizeof(keep));
        CHECK(cxf_soa_remove(&s, h) == CXF_OK);
        CHECK(!cxf_soa_live(&s, h));
        cxf_soa_gather(&s, h, &q);
        CHECK(memcmp(&keep, &q, sizeof(keep)) == 0);
    }

    CHECK(cxf_soa_check(&s));
    cxf_soa_release(&s);
    cxf_layout_release(&l, alloc);
}

/* 自引用结构体：递归必须终止在指针上，不能栈溢出。 */
static void test_self_reference(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_layout l;

    CHECK(cxf_layout_build(&l, &cxf_type_Node, alloc) == CXF_OK);
    CHECK(l.leaf_count == 2);
    CHECK(strcmp(l.leaves[0].path, "v") == 0);
    CHECK(strcmp(l.leaves[1].path, "next") == 0);
    CHECK(l.leaves[1].type->kind == CXF_K_PTR);

    cxf_layout_release(&l, alloc);
}

/* 叶子数超过列上限必须报错，而不是悄悄截断。 */
static void test_too_many_leaves(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_layout l;
    cxf_soa s;

    CHECK(cxf_layout_build(&l, &cxf_type_Wide, alloc) == CXF_OK);
    CHECK(l.leaf_count == 17);
    CHECK(cxf_soa_init(&s, &l, alloc, 4) == CXF_ERR_RANGE);

    cxf_layout_release(&l, alloc);
}

int main(void) {
    test_generated_matches_compiler();
    test_flatten();
    test_select();
    test_soa_roundtrip();
    test_self_reference();
    test_too_many_leaves();

    if (g_fail == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_fail);
    return 1;
}
