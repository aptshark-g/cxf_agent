#include <stdio.h>
#include <string.h>

#include "cxf/algo.h"
#include "cxf/alloc.h"
#include "cxf/arena.h"
#include "cxf/bitmap.h"
#include "cxf/blob.h"
#include "cxf/buf.h"
#include "cxf/csr.h"
#include "cxf/handle.h"
#include "cxf/namemap.h"
#include "cxf/pqueue.h"
#include "cxf/ring.h"
#include "cxf/sb.h"
#include "cxf/str.h"
#include "cxf/table.h"
#include "cxf/vec.h"

static int g_fail = 0;

#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

static void test_handle(void) {
    cxf_handle a = cxf_handle_make(7, 3);
    cxf_handle b = cxf_handle_make(7, 3);
    cxf_handle c = cxf_handle_make(7, 5);
    cxf_handle z = { 0, 0 };

    CHECK(cxf_handle_eq(a, b));
    CHECK(!cxf_handle_eq(a, c));
    CHECK(cxf_handle_null(z));
    CHECK(!cxf_handle_null(a));

    /* class | 计数器 打包：两者互不侵占 */
    {
        cxf_handle t1 = cxf_handle_typed(3, 41, 1);
        cxf_handle t2 = cxf_handle_typed(3, 41, 2);

        CHECK(cxf_handle_class(t1) == 1);
        CHECK(cxf_handle_gen(t1) == 41);
        CHECK(cxf_handle_alive(t1));
        CHECK(!cxf_handle_eq(t1, t2));   /* H3：不同 class 永不相等 */
        CHECK(!cxf_handle_null(t1));
        CHECK(cxf_handle_null(cxf_handle_typed(0, 0, 0)));

        /* H5：翻世代只动低 24 位，回绕落点为偶数（死），class 位保留 */
        CHECK(cxf_handle_gen_next(0x01000001u) == 0x01000002u);
        CHECK(cxf_handle_gen_next(0x01FFFFFFu) == 0x01000000u);
        CHECK(!cxf_handle_alive(cxf_handle_make(0, 0x01000000u)));
    }
}

static void test_table_class(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    const size_t sizes[1] = { sizeof(uint32_t) };
    cxf_table a, b;
    cxf_handle ha, hb;

    CHECK(cxf_table_init_class(&a, alloc, sizes, 1, 4, 1) == CXF_OK);
    CHECK(cxf_table_init_class(&b, alloc, sizes, 1, 4, 2) == CXF_OK);

    ha = cxf_table_add(&a);
    hb = cxf_table_add(&b);

    /* 同 index、同计数器，只有 class 不同 —— 旧实现（32 位裸 gen）会静默成功 */
    CHECK(ha.index == hb.index);
    CHECK(cxf_handle_gen(ha) == cxf_handle_gen(hb));
    CHECK(cxf_handle_class(ha) == 1);
    CHECK(cxf_handle_class(hb) == 2);

    CHECK(!cxf_table_live(&b, ha));
    CHECK(!cxf_table_live(&a, hb));
    CHECK(cxf_table_get(&b, ha, 0) == NULL);
    CHECK(cxf_table_remove(&b, ha) == CXF_ERR_BAD_HANDLE);
    CHECK(cxf_table_live(&a, ha));   /* 误用被拒，原表毫无影响 */
    CHECK(cxf_table_live(&b, hb));

    CHECK(cxf_table_check(&a));
    CHECK(cxf_table_check(&b));

    /* 边界：typed handle 防"拿错表"，不防"按内部布局伪造"——
       若伪造值与真 handle 逐位相同，那它本来就是那个 handle。 */
    cxf_table_release(&a);
    cxf_table_release(&b);
}

static void test_alloc(void) {
    cxf_alloc *a = cxf_alloc_heap();
    void *p, *q;

    cxf_alloc_stat_reset(a);
    p = cxf_alloc_alloc(a, 100, 16);
    q = cxf_alloc_alloc(a, 200, 16);
    CHECK(p != NULL && q != NULL);
    CHECK(a->stat.alloc_count == 2);
    CHECK(a->stat.bytes_live == 300);
    CHECK(a->stat.bytes_peak == 300);

    cxf_alloc_free(a, p, 100);
    CHECK(a->stat.bytes_live == 200);
    CHECK(a->stat.bytes_peak == 300);

    cxf_alloc_free(a, q, 200);
    CHECK(a->stat.bytes_live == 0);
}

static void test_arena(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_arena a;
    void *p1, *p2;

    cxf_arena_init(&a, alloc, 256);

    p1 = cxf_arena_alloc(&a, 64, 16);
    p2 = cxf_arena_alloc(&a, 64, 16);
    CHECK(p1 != NULL && p2 != NULL);
    CHECK((uint8_t *)p2 - (uint8_t *)p1 == 64);
    CHECK(a.stat.chunk_count == 1);
    CHECK(cxf_arena_bytes_used(&a) == 128);
    CHECK(cxf_arena_check(&a));

    CHECK(cxf_arena_push(&a) == CXF_OK);
    cxf_arena_alloc(&a, 1024, 16);
    CHECK(a.stat.chunk_count == 2);
    CHECK(cxf_arena_bytes_used(&a) == 256 + 1024);
    CHECK(cxf_arena_check(&a));

    cxf_arena_pop(&a);
    CHECK(cxf_arena_bytes_used(&a) == 128);
    CHECK(a.stat.chunk_count == 2);
    CHECK(a.stat.bytes_peak == 256 + 1024);
    CHECK(cxf_arena_check(&a));

    cxf_arena_alloc(&a, 64, 16);
    CHECK(cxf_arena_bytes_used(&a) == 192);
    CHECK(a.stat.chunk_count == 2);
    CHECK(cxf_arena_check(&a));

    cxf_arena_reset(&a);
    CHECK(cxf_arena_bytes_used(&a) == 0);
    CHECK(a.stat.chunk_count == 2);
    CHECK(cxf_arena_check(&a));

    cxf_arena_release(&a);
    CHECK(a.chunks == NULL);
}

static void test_arena_frame_depth(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_arena a;
    int i;

    cxf_arena_init(&a, alloc, 4096);
    for (i = 0; i < CXF_ARENA_MAX_FRAMES; i++)
        CHECK(cxf_arena_push(&a) == CXF_OK);
    CHECK(cxf_arena_push(&a) == CXF_ERR_RANGE);

    for (i = 0; i < CXF_ARENA_MAX_FRAMES; i++) cxf_arena_pop(&a);
    CHECK(a.depth == 0);

    cxf_arena_release(&a);
}

typedef struct {
    uint32_t id;
    float    x;
} ent_row;

static void test_table(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    const size_t sizes[2] = { sizeof(uint32_t), sizeof(float) };
    cxf_table t;
    cxf_handle h[8];
    uint32_t i;

    CHECK(cxf_table_init(&t, alloc, sizes, 2, 4) == CXF_OK);
    CHECK(t.cap == 4);
    CHECK(t.class_id == CXF_HANDLE_CLASS_NONE);  /* 不指定 = 未标注，行为与旧版一致 */

    for (i = 0; i < 4; i++) {
        h[i] = cxf_table_add(&t);
        CHECK(cxf_table_live(&t, h[i]));
        *(uint32_t *)cxf_table_get(&t, h[i], 0) = 100 + i;
        *(float *)cxf_table_get(&t, h[i], 1) = (float)i;
    }
    CHECK(t.count == 4);
    CHECK(cxf_table_check(&t));

    for (i = 0; i < 4; i++) {
        CHECK(cxf_table_live(&t, h[i]));
        CHECK(*(uint32_t *)cxf_table_get(&t, h[i], 0) == 100 + i);
    }

    CHECK(cxf_table_remove(&t, h[1]) == CXF_OK);
    CHECK(t.count == 3);
    CHECK(!cxf_table_live(&t, h[1]));
    CHECK(cxf_table_remove(&t, h[1]) == CXF_ERR_BAD_HANDLE);
    CHECK(cxf_table_get(&t, h[1], 0) == NULL);
    CHECK(cxf_table_check(&t));

    {
        cxf_handle reused = cxf_table_add(&t);
        CHECK(reused.index == h[1].index);
        CHECK(reused.gen != h[1].gen);
        CHECK(!cxf_table_live(&t, h[1]));
        CHECK(cxf_table_live(&t, reused));
    }
    CHECK(cxf_table_check(&t));

    for (i = 0; i < 4; i++) {
        if (i == 1) continue;
        CHECK(cxf_table_live(&t, h[i]));
        CHECK(*(uint32_t *)cxf_table_get(&t, h[i], 0) == 100 + i);
    }

    cxf_table_release(&t);
}

static void test_table_grow(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    const size_t sizes[1] = { sizeof(uint32_t) };
    cxf_table t;
    uint32_t i;
    cxf_handle h;

    CHECK(cxf_table_init(&t, alloc, sizes, 1, 2) == CXF_OK);

    for (i = 0; i < 100; i++) {
        h = cxf_table_add(&t);
        CHECK(cxf_table_live(&t, h));
        *(uint32_t *)cxf_table_get(&t, h, 0) = i;
    }
    CHECK(t.count == 100);
    CHECK(t.cap >= 100);
    CHECK(cxf_table_check(&t));

    for (i = 0; i < t.count; i++) {
        cxf_handle at = cxf_table_at(&t, i);
        CHECK(*(uint32_t *)cxf_table_get(&t, at, 0) == i);
    }

    cxf_table_release(&t);
}

static void test_table_remove_during_iteration(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    const size_t sizes[1] = { sizeof(uint32_t) };
    cxf_table t;
    uint32_t i, kept = 0;

    CHECK(cxf_table_init(&t, alloc, sizes, 1, 8) == CXF_OK);
    for (i = 0; i < 8; i++) {
        cxf_handle h = cxf_table_add(&t);
        *(uint32_t *)cxf_table_get(&t, h, 0) = i;
    }

    for (i = t.count; i-- > 0;) {
        cxf_handle h = cxf_table_at(&t, i);
        uint32_t v = *(uint32_t *)cxf_table_get(&t, h, 0);
        if ((v & 1) == 0) cxf_table_remove(&t, h);
    }

    for (i = 0; i < t.count; i++) {
        cxf_handle h = cxf_table_at(&t, i);
        uint32_t v = *(uint32_t *)cxf_table_get(&t, h, 0);
        CHECK((v & 1) == 1);
        kept++;
    }
    CHECK(t.count == 4);
    CHECK(kept == 4);
    CHECK(cxf_table_check(&t));

    cxf_table_release(&t);
}

static void test_table_clear(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    const size_t sizes[1] = { sizeof(uint32_t) };
    cxf_table t;
    cxf_handle h;

    CHECK(cxf_table_init(&t, alloc, sizes, 1, 4) == CXF_OK);
    h = cxf_table_add(&t);
    CHECK(cxf_table_live(&t, h));

    cxf_table_clear(&t);
    CHECK(t.count == 0);
    CHECK(!cxf_table_live(&t, h));
    CHECK(cxf_table_check(&t));

    cxf_table_release(&t);
}

static void test_ring(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_ring r;
    uint8_t in[200], out[200];
    uint32_t i;

    for (i = 0; i < sizeof(in); i++) in[i] = (uint8_t)i;

    CHECK(cxf_ring_init(&r, alloc, 100) == CXF_OK);
    CHECK(r.cap == 128);
    CHECK(cxf_ring_space(&r) == 128);
    CHECK(cxf_ring_size(&r) == 0);

    CHECK(cxf_ring_write(&r, in, 50) == 50);
    CHECK(cxf_ring_size(&r) == 50);
    CHECK(cxf_ring_read(&r, out, 50) == 50);
    CHECK(memcmp(in, out, 50) == 0);
    CHECK(cxf_ring_size(&r) == 0);

    CHECK(cxf_ring_write(&r, in, 200) == 128);
    CHECK(cxf_ring_space(&r) == 0);
    CHECK(cxf_ring_read(&r, out, 200) == 128);
    CHECK(memcmp(in, out, 128) == 0);

    for (i = 0; i < 1000; i++) {
        CHECK(cxf_ring_write(&r, in, 37) == 37);
        CHECK(cxf_ring_read(&r, out, 37) == 37);
        CHECK(memcmp(in, out, 37) == 0);
    }
    CHECK(cxf_ring_size(&r) == 0);

    CHECK(cxf_ring_write(&r, in, 10) == 10);
    CHECK(cxf_ring_peek(&r, out, 10) == 10);
    CHECK(cxf_ring_size(&r) == 10);
    CHECK(cxf_ring_read(&r, out, 10) == 10);
    CHECK(cxf_ring_size(&r) == 0);
    CHECK(cxf_ring_check(&r));

    cxf_ring_release(&r, alloc);
}

static void test_str(void) {
    cxf_str ab = CXF_STR_LIT("alpha");
    cxf_str abc = CXF_STR_LIT("alphabet");
    uint32_t u = 7;

    /* 编译期长度：字面量不该在运行期 strlen */
    CHECK(ab.len == 5);
    CHECK(abc.len == 8);
    CHECK(cxf_str_check(ab));
    CHECK(cxf_str_check(cxf_str_make(NULL, 0)));
#ifndef NDEBUG
    /* NDEBUG 下 check 恒真（零成本），这条断言只在 debug 有意义 */
    CHECK(!cxf_str_check(cxf_str_make(NULL, 3)));  /* S1 违例必须被 check 抓住 */
#endif

    CHECK(cxf_str_eq(ab, CXF_STR_LIT("alpha")));
    CHECK(!cxf_str_eq(ab, abc));
    CHECK(cxf_str_empty(cxf_str_make(NULL, 0)));

    /* 公共前缀相等时短的小 —— "前缀 < 全文"是一条不变式 */
    CHECK(cxf_str_order(ab, abc) == -1);
    CHECK(cxf_str_order(abc, ab) == 1);
    CHECK(cxf_str_order(ab, ab) == 0);
    CHECK(cxf_str_order(CXF_STR_LIT("b"), CXF_STR_LIT("a")) == 1);

    CHECK(cxf_str_starts_with(abc, ab));
    CHECK(!cxf_str_starts_with(ab, abc));
    CHECK(cxf_str_starts_with(ab, cxf_str_make(NULL, 0)));
    CHECK(cxf_str_ends_with(abc, CXF_STR_LIT("bet")));
    CHECK(!cxf_str_ends_with(abc, ab));

    CHECK(cxf_str_find(abc, CXF_STR_LIT("pha")) == 2);
    CHECK(cxf_str_find(abc, CXF_STR_LIT("zz")) == CXF_NO_INDEX);
    CHECK(cxf_str_find(abc, cxf_str_make(NULL, 0)) == 0);
    CHECK(cxf_str_find(ab, abc) == CXF_NO_INDEX);  /* needle 比 hay 长 */
    CHECK(cxf_str_find_char(abc, 'b') == 5);
    CHECK(cxf_str_find_char(abc, 'z') == CXF_NO_INDEX);

    /* sub 越界自动收敛 */
    CHECK(cxf_str_eq(cxf_str_sub(abc, 2, 3), CXF_STR_LIT("pha")));
    CHECK(cxf_str_empty(cxf_str_sub(abc, 99, 3)));
    CHECK(cxf_str_eq(cxf_str_sub(abc, 5, 99), CXF_STR_LIT("bet")));

    /* trim 只动两侧，中间的空白属于内容 */
    CHECK(cxf_str_eq(cxf_str_trim(CXF_STR_LIT("  a b \t\r\n")), CXF_STR_LIT("a b")));
    CHECK(cxf_str_empty(cxf_str_trim(CXF_STR_LIT(" \t "))));

    /* split：空字段保留，尾随分隔符产生空 token —— 协议列不能因空而错位 */
    {
        cxf_str in = CXF_STR_LIT("a,,b,");
        cxf_str tok;
        int n = 0;
        cxf_str_iter it;

        cxf_str_split(&it, in, ',');
        while (cxf_str_split_next(&it, &tok)) {
            switch (n) {
            case 0: CHECK(cxf_str_eq(tok, CXF_STR_LIT("a"))); break;
            case 1: CHECK(cxf_str_empty(tok)); break;
            case 2: CHECK(cxf_str_eq(tok, CXF_STR_LIT("b"))); break;
            case 3: CHECK(cxf_str_empty(tok)); break;
            default: CHECK(0); break;
            }
            n++;
        }
        CHECK(n == 4);
    }

    /* 严格解析：失败不写 *out */
    CHECK(cxf_str_parse_u32(CXF_STR_LIT("0"), &u) == CXF_OK && u == 0);
    CHECK(cxf_str_parse_u32(CXF_STR_LIT("4294967295"), &u) == CXF_OK &&
          u == 4294967295u);
    u = 7;
    CHECK(cxf_str_parse_u32(CXF_STR_LIT("4294967296"), &u) == CXF_ERR_RANGE && u == 7);
    CHECK(cxf_str_parse_u32(CXF_STR_LIT("12a"), &u) == CXF_ERR_FORMAT && u == 7);
    CHECK(cxf_str_parse_u32(CXF_STR_LIT("-1"), &u) == CXF_ERR_FORMAT);
    CHECK(cxf_str_parse_u32(CXF_STR_LIT(" 12"), &u) == CXF_ERR_FORMAT);
    CHECK(cxf_str_parse_u32(CXF_STR_LIT(""), &u) == CXF_ERR_FORMAT);
}

static void test_sb(void) {
    cxf_alloc *alloc = cxf_alloc_heap();
    cxf_arena a;
    cxf_sb sb, m;
    cxf_str out;
    char buf[32];

    /* 计数模式：只有 len 变化，一个字节都不写 */
    m = cxf_sb_measure();
    cxf_sb_puts(&m, CXF_STR_LIT("plan|"));
    cxf_sb_u32(&m, 601);
    CHECK(cxf_sb_len(&m) == 8);
    CHECK(cxf_sb_used(&m) == 0);
    CHECK(!cxf_sb_overflow(&m));  /* 计数模式没有"截断"这回事 */

    /* 精确缓冲：一次成功，无截断 */
    sb = cxf_sb_buf(buf, sizeof(buf));
    cxf_sb_puts(&sb, CXF_STR_LIT("plan|"));
    cxf_sb_u32(&sb, 601);
    CHECK(cxf_sb_len(&sb) == 8);
    CHECK(!cxf_sb_overflow(&sb));
    CHECK(cxf_str_eq(cxf_sb_view(&sb), CXF_STR_LIT("plan|601")));

    /* 小缓冲：截断可见，逻辑长度不变 —— len 就是第二遍要申请的大小 */
    sb = cxf_sb_buf(buf, 4);
    cxf_sb_puts(&sb, CXF_STR_LIT("plan|"));
    cxf_sb_u32(&sb, 601);
    CHECK(cxf_sb_len(&sb) == 8);
    CHECK(cxf_sb_used(&sb) == 4);
    CHECK(cxf_sb_overflow(&sb));
    CHECK(memcmp(buf, "plan", 4) == 0);

    /* 边界：u64 满值 20 位，零值也要写一位 */
    sb = cxf_sb_buf(buf, sizeof(buf));
    cxf_sb_u64(&sb, UINT64_MAX);
    CHECK(cxf_str_eq(cxf_sb_view(&sb), CXF_STR_LIT("18446744073709551615")));
    sb = cxf_sb_buf(buf, sizeof(buf));
    cxf_sb_u32(&sb, 0);
    CHECK(cxf_str_eq(cxf_sb_view(&sb), CXF_STR_LIT("0")));

    /* commit：从 arena 精确分配并复制；commitz 额外补 NUL 供 C API 使用 */
    cxf_arena_init(&a, alloc, 256);
    sb = cxf_sb_buf(buf, sizeof(buf));
    cxf_sb_puts(&sb, CXF_STR_LIT("plan|"));
    cxf_sb_u32(&sb, 601);
    out = cxf_str_make(NULL, 0);
    CHECK(cxf_sb_commit(&sb, &a, &out) == CXF_OK);
    CHECK(cxf_str_eq(out, CXF_STR_LIT("plan|601")));
    CHECK(cxf_arena_bytes_used(&a) == 8);  /* 精确大小，不是 cap */

    CHECK(cxf_sb_commitz(&sb, &a, &out) == CXF_OK);
    CHECK(out.len == 8);
    CHECK(out.p[out.len] == '\0');         /* 切片不含 NUL，但缓冲里有 */
    CHECK(cxf_arena_check(&a));

    /* 截断后提交：明确失败，不给半截字符串 */
    sb = cxf_sb_buf(buf, 2);
    cxf_sb_puts(&sb, CXF_STR_LIT("abcdef"));
    CHECK(cxf_sb_commit(&sb, &a, &out) == CXF_ERR_FULL);
    CHECK(cxf_str_empty(out));

    /* 计数模式提交是调用方的错：返回 EMPTY 而不是崩溃 */
    m = cxf_sb_measure();
    cxf_sb_puts(&m, CXF_STR_LIT("x"));
    CHECK(cxf_sb_commit(&m, &a, &out) == CXF_ERR_EMPTY);

    /* 两段式：先数，第二遍直接写进 arena 精确缓冲，零复制 */
    m = cxf_sb_measure();
    cxf_sb_puts(&m, CXF_STR_LIT("plan|"));
    cxf_sb_u32(&m, 601);
    {
        char *p = cxf_arena_alloc(&a, m.len, 1);
        cxf_sb sb2;

        CHECK(p != NULL);
        sb2 = cxf_sb_buf(p, m.len);
        cxf_sb_puts(&sb2, CXF_STR_LIT("plan|"));
        cxf_sb_u32(&sb2, 601);
        CHECK(!cxf_sb_overflow(&sb2));       /* 两段式下不该截断 */
        CHECK(cxf_sb_len(&sb2) == m.len);    /* 两遍长度必须一致 */
        CHECK(cxf_str_eq(cxf_sb_view(&sb2), CXF_STR_LIT("plan|601")));
    }
    CHECK(cxf_arena_check(&a));
    cxf_arena_release(&a);
}

static void test_bitmap(void) {
    uint64_t mem[2];
    cxf_bitmap b;
    uint32_t i;

    cxf_bitmap_init(&b, mem, 128);
    CHECK(b.nwords == 2);
    CHECK(cxf_bitmap_bytes(128) == 16);
    CHECK(cxf_bitmap_popcount(&b) == 0);
    CHECK(cxf_bitmap_next(&b, 0) == CXF_NO_INDEX);
    CHECK(cxf_bitmap_first_zero(&b, 0) == 0);

    cxf_bitmap_set(&b, 0);
    cxf_bitmap_set(&b, 63);
    cxf_bitmap_set(&b, 64);
    cxf_bitmap_set(&b, 127);
    CHECK(cxf_bitmap_get(&b, 0) && cxf_bitmap_get(&b, 63));
    CHECK(cxf_bitmap_popcount(&b) == 4);
    CHECK(cxf_bitmap_next(&b, 0) == 0);
    CHECK(cxf_bitmap_next(&b, 1) == 63);
    CHECK(cxf_bitmap_next(&b, 64) == 64);       /* 跨字边界 */
    CHECK(cxf_bitmap_next(&b, 65) == 127);
    CHECK(cxf_bitmap_next(&b, 128) == CXF_NO_INDEX);
    CHECK(cxf_bitmap_first_zero(&b, 0) == 1);
    CHECK(cxf_bitmap_first_zero(&b, 63) == 65);
    CHECK(cxf_bitmap_first_zero(&b, 127) == CXF_NO_INDEX);  /* 尾部超界不算 */

    cxf_bitmap_unset(&b, 64);
    CHECK(!cxf_bitmap_get(&b, 64));
    CHECK(cxf_bitmap_first_zero(&b, 64) == 64);
    CHECK(cxf_bitmap_check(&b));

    /* 非 64 倍数：尾部超界位必须保持 0（M2） */
    {
        uint64_t mem2[1];
        cxf_bitmap b2;

        cxf_bitmap_init(&b2, mem2, 3);
        cxf_bitmap_set(&b2, 2);
        CHECK(cxf_bitmap_popcount(&b2) == 1);
        CHECK(cxf_bitmap_next(&b2, 0) == 2);
        CHECK(cxf_bitmap_next(&b2, 3) == CXF_NO_INDEX);
        CHECK(cxf_bitmap_check(&b2));
    }

    /* 整图迭代 */
    cxf_bitmap_init(&b, mem, 128);
    for (i = 0; i < 128; i += 7) cxf_bitmap_set(&b, i);
    {
        uint32_t n = 0;
        for (i = cxf_bitmap_next(&b, 0); i != CXF_NO_INDEX;
             i = cxf_bitmap_next(&b, i + 1)) n++;
        CHECK(n == 19);
    }
}

static void test_buf(void) {
    cxf_alloc *al = cxf_alloc_heap();
    cxf_buf b = cxf_buf_make(al);

    CHECK(cxf_buf_check(&b));
    CHECK(cxf_buf_append(&b, "abc", 3) == CXF_OK);
    CHECK(b.len == 3 && b.cap >= 3);
    CHECK(cxf_buf_append_str(&b, CXF_STR_LIT("hello")) == CXF_OK);
    CHECK(cxf_str_eq(cxf_buf_view(&b), CXF_STR_LIT("abchello")));

    /* 增长后内容不丢（这是手写三份实现最容易漏的一处） */
    {
        uint32_t cap0 = b.cap;

        CHECK(cxf_buf_append(&b, "0123456789012345678901234567890123456789", 40) == CXF_OK);
        CHECK(b.cap > cap0);
        CHECK(b.len == 48);
        CHECK(memcmp(b.p, "abchello0123456789012345678901234567890123456789", 48) == 0);
    }

    CHECK(cxf_buf_assign(&b, "xy", 2) == CXF_OK);
    CHECK(b.len == 2 && cxf_str_eq(cxf_buf_view(&b), CXF_STR_LIT("xy")));

    cxf_buf_reset(&b);
    CHECK(b.len == 0 && b.cap >= 2);

    cxf_buf_free(&b);
    CHECK(b.p == NULL && b.cap == 0);
    cxf_buf_free(&b);   /* 重复 free 安全 */
}

static void test_blob(void) {
    cxf_alloc *al = cxf_alloc_heap();
    cxf_blobpool bp;
    cxf_handle h, h2;

    CHECK(cxf_blobpool_init(&bp, al, 4, 9) == CXF_OK);

    h = cxf_blob_new(&bp, 16);
    CHECK(!cxf_handle_null(h));
    CHECK(cxf_handle_class(h) == 9);       /* 池的 class 写进 handle */
    CHECK(cxf_blobpool_bytes(&bp) >= 16);

    CHECK(cxf_blob_append(&bp, h, "abc", 3) == CXF_OK);
    CHECK(cxf_str_eq(cxf_blob_view(&bp, h), CXF_STR_LIT("abc")));

    /* 增长后前缀保留 */
    CHECK(cxf_blob_append(&bp, h, "012345678901234567890123456789", 30) == CXF_OK);
    CHECK(cxf_str_eq(cxf_blob_view(&bp, h),
                     CXF_STR_LIT("abc012345678901234567890123456789")));
    CHECK(cxf_blobpool_check(&bp));

    h2 = cxf_blob_new(&bp, 0);
    CHECK(!cxf_handle_null(h2));
    CHECK(cxf_blob_append(&bp, h2, "x", 1) == CXF_OK);

    CHECK(cxf_blob_free(&bp, h) == CXF_OK);
    CHECK(cxf_blob_free(&bp, h) == CXF_ERR_BAD_HANDLE);   /* L4：二次 free 响亮失败 */
    CHECK(cxf_str_empty(cxf_blob_view(&bp, h)));
    CHECK(cxf_blobpool_check(&bp));

    CHECK(cxf_blob_free(&bp, h2) == CXF_OK);
    CHECK(cxf_blobpool_bytes(&bp) == 0);                  /* 全部归还 ⇒ 零 */
    CHECK(bp.count == 0);
    CHECK(cxf_blobpool_check(&bp));

    cxf_blobpool_release(&bp);
}

static void test_namemap(void) {
    cxf_alloc *al = cxf_alloc_heap();
    cxf_arena a;
    cxf_namemap m;
    uint32_t id = 999;

    cxf_arena_init(&a, al, 4096);
    cxf_namemap_init(&m);

    CHECK(cxf_namemap_add(&m, &a, CXF_STR_LIT("quote"), 3));
    CHECK(cxf_namemap_add(&m, &a, CXF_STR_LIT("merge"), 1));
    CHECK(cxf_namemap_add(&m, &a, CXF_STR_LIT("train"), 2));
    CHECK(!cxf_namemap_add(&m, &a, CXF_STR_LIT("merge"), 7));  /* N1：重复注册是错误 */
    CHECK(cxf_namemap_count(&m) == 3);
    CHECK(cxf_namemap_check(&m));

    CHECK(cxf_namemap_find(&m, CXF_STR_LIT("merge"), &id) && id == 1);
    CHECK(cxf_namemap_find(&m, CXF_STR_LIT("quote"), &id) && id == 3);
    CHECK(!cxf_namemap_find(&m, CXF_STR_LIT("quot"), &id) && id == 3);  /* 未命中不改 id */

    /* 前缀枚举起点 */
    {
        uint32_t i = cxf_namemap_lower_bound(&m, CXF_STR_LIT("m"));

        CHECK(i == 0);   /* "m" 是 "merge" 的前缀 ⇒ 第一个 >= "m" 的就是 merge */
        CHECK(cxf_str_starts_with(m.ents[i].name, CXF_STR_LIT("m")));
    }

    /* 严格升序 */
    CHECK(cxf_str_order(m.ents[0].name, m.ents[1].name) < 0);
    CHECK(cxf_str_order(m.ents[1].name, m.ents[2].name) < 0);

    cxf_arena_release(&a);
}

static void test_algo(void) {
    uint32_t a[64], ref[64], i, j;
    uint32_t keys[8] = { 5, 1, 9, 3, 9, 2, 7, 0 };
    uint32_t top[8];

    for (i = 0; i < 64; i++) { a[i] = (i * 37u + 11u) % 97u; ref[i] = a[i]; }
    cxf_algo_sort_u32(a, 64);
    for (i = 0; i < 64; i++)   /* 参照实现：选择排序，慢但显然正确 */
        for (j = i + 1; j < 64; j++)
            if (ref[j] < ref[i]) { uint32_t t = ref[i]; ref[i] = ref[j]; ref[j] = t; }
    CHECK(memcmp(a, ref, sizeof(a)) == 0);

    { uint32_t s[5] = { 4, 2, 5, 1, 3 };
      cxf_algo_sort_u32(s, 5);
      CHECK(s[0] == 1 && s[4] == 5); }

    CHECK(cxf_algo_lower_bound_u32(a, 64, 0) == 0);
    CHECK(cxf_algo_lower_bound_u32(a, 64, 1000) == 64);
    CHECK(cxf_algo_lower_bound_u32(a, 64, a[10]) <= 10);

    /* topk：最大的 3 个，按 key 降序（9, 9, 7） */
    CHECK(cxf_algo_topk_u32(keys, 8, 3, top) == 3);
    CHECK(keys[top[0]] == 9 && keys[top[1]] == 9 && keys[top[2]] == 7);
    CHECK(cxf_algo_topk_u32(keys, 8, 0, top) == 0);
    CHECK(cxf_algo_topk_u32(keys, 8, 100, top) == 8);   /* k > n 收敛到 n */

    CHECK(cxf_algo_hash_str(CXF_STR_LIT("abc")) == cxf_algo_hash_bytes("abc", 3));
    CHECK(cxf_algo_hash_str(CXF_STR_LIT("abc")) != cxf_algo_hash_str(CXF_STR_LIT("abd")));
    CHECK(cxf_algo_hash_str(cxf_str_make(NULL, 0)) == 2166136261u);  /* 空串 = offset basis */
}

static void test_csr(void) {
    /* 0→1, 0→2, 1→2, 2→3 */
    cxf_csr_edge e[4] = { { 0, 1 }, { 0, 2 }, { 1, 2 }, { 2, 3 } };
    uint32_t mem[9];
    cxf_csr g;

    CHECK(cxf_csr_bytes(4, 4) == 9 * 4);
    CHECK(cxf_csr_from_edges(&g, mem, 4, e, 4) == CXF_OK);
    CHECK(g.n == 4 && g.m == 4);
    CHECK(cxf_csr_deg(&g, 0) == 2);
    CHECK(cxf_csr_at(&g, 0, 0) == 1 && cxf_csr_at(&g, 0, 1) == 2);  /* C3：输入序 */
    CHECK(cxf_csr_deg(&g, 3) == 0);
    CHECK(cxf_csr_check(&g));

    /* 反向图：2 应有两条入边（来自 0、1），0 没有 */
    {
        uint32_t mem2[9];
        cxf_csr r;

        CHECK(cxf_csr_from_edges_rev(&r, mem2, 4, e, 4) == CXF_OK);
        CHECK(cxf_csr_deg(&r, 2) == 2);
        CHECK(cxf_csr_deg(&r, 0) == 0);
        CHECK(cxf_csr_check(&r));
    }

    /* 越界边：装配期响亮失败，不静默丢弃 */
    {
        cxf_csr_edge bad[1] = { { 0, 9 } };
        uint32_t mem3[5];
        cxf_csr b;

        CHECK(cxf_csr_from_edges(&b, mem3, 3, bad, 1) == CXF_ERR_RANGE);
    }
}

static void test_graph_algo(void) {
    cxf_csr_edge e[4] = { { 0, 1 }, { 0, 2 }, { 1, 2 }, { 2, 3 } };
    uint32_t mem[9], indeg[4], order[4], queue[4];
    uint64_t vmem[1];
    cxf_bitmap visited;
    cxf_csr g;

    CHECK(cxf_csr_from_edges(&g, mem, 4, e, 4) == CXF_OK);

    CHECK(cxf_algo_toposort(&g, indeg, order) == CXF_OK);
    CHECK(order[0] == 0);
    CHECK(order[3] == 3);   /* 3 是唯一汇点，必在最后 */
    {
        uint32_t pos[4], i;

        for (i = 0; i < 4; i++) pos[order[i]] = i;
        CHECK(pos[0] < pos[1] && pos[1] < pos[2] && pos[2] < pos[3]);
    }

    cxf_bitmap_init(&visited, vmem, 4);
    CHECK(cxf_algo_reach(&g, 0, 3, &visited, queue));
    CHECK(cxf_algo_reach(&g, 0, 0, &visited, queue));   /* 自反 */
    CHECK(!cxf_algo_reach(&g, 3, 0, &visited, queue));  /* 反向不可达 */

    /* 成环：0↔1 ⇒ 拓扑失败，但互相可达 */
    {
        cxf_csr_edge c[2] = { { 0, 1 }, { 1, 0 } };
        uint32_t m2[5];
        cxf_csr cg;

        CHECK(cxf_csr_from_edges(&cg, m2, 2, c, 2) == CXF_OK);
        CHECK(cxf_algo_toposort(&cg, indeg, order) == CXF_ERR_DEP);

        cxf_bitmap_init(&visited, vmem, 4);
        CHECK(cxf_algo_reach(&cg, 0, 1, &visited, queue));
        CHECK(cxf_algo_reach(&cg, 1, 0, &visited, queue));
    }
}

static void test_pqueue(void) {
    cxf_pqueue_ent mem[8];
    cxf_pqueue q;
    uint32_t k, id, i;

    cxf_pqueue_init(&q, mem, 8);
    CHECK(cxf_pqueue_size(&q) == 0);
    CHECK(!cxf_pqueue_pop(&q, &k, &id));
    CHECK(!cxf_pqueue_peek(&q, &k, &id));

    /* P3：相等 key 按 id 升序 —— 没有这一条，弹出序会随内部移动历史漂移 */
    CHECK(cxf_pqueue_push(&q, 5, 2));
    CHECK(cxf_pqueue_push(&q, 5, 1));
    CHECK(cxf_pqueue_push(&q, 3, 9));
    CHECK(cxf_pqueue_check(&q));
    CHECK(cxf_pqueue_peek(&q, &k, &id) && k == 3 && id == 9);
    CHECK(cxf_pqueue_pop(&q, &k, &id) && k == 3 && id == 9);
    CHECK(cxf_pqueue_pop(&q, &k, &id) && k == 5 && id == 1);
    CHECK(cxf_pqueue_pop(&q, &k, &id) && k == 5 && id == 2);
    CHECK(cxf_pqueue_size(&q) == 0);

    /* 满：返回 false 且不覆盖任何元素 */
    for (i = 0; i < 8; i++) CHECK(cxf_pqueue_push(&q, i + 100, i));
    CHECK(!cxf_pqueue_push(&q, 1, 1));
    CHECK(cxf_pqueue_size(&q) == 8);

    /* update_at：变小上浮、变大的下沉 */
    cxf_pqueue_clear(&q);
    CHECK(cxf_pqueue_push(&q, 10, 0));
    CHECK(cxf_pqueue_push(&q, 20, 1));
    CHECK(cxf_pqueue_push(&q, 30, 2));
    cxf_pqueue_update_at(&q, 2, 5);
    CHECK(cxf_pqueue_check(&q));
    CHECK(cxf_pqueue_peek(&q, &k, &id) && k == 5 && id == 2);
    cxf_pqueue_update_at(&q, 0, 99);
    CHECK(cxf_pqueue_check(&q));
    CHECK(cxf_pqueue_peek(&q, &k, &id) && k == 10 && id == 0);

    /* make：自底向上建堆。乱序输入 + 相等 key，弹出必须是 (key,id) 全序：
     *   (1,7) (2,6) (3,3) (3,5) (4,1) (4,2) (9,0)
     * 相等 key 的两两之间 id 必须递增 —— P3 在"乱序建堆"后仍要成立。 */
    {
        cxf_pqueue_ent src[7];
        uint32_t want_key[7] = {1, 2, 3, 3, 4, 4, 9};
        uint32_t want_id[7]  = {7, 6, 3, 5, 1, 2, 0};

        src[0].key = 9; src[0].id = 0;
        src[1].key = 3; src[1].id = 5;
        src[2].key = 4; src[2].id = 2;
        src[3].key = 1; src[3].id = 7;
        src[4].key = 4; src[4].id = 1;
        src[5].key = 3; src[5].id = 3;
        src[6].key = 2; src[6].id = 6;

        cxf_pqueue_make(&q, src, 7);
        CHECK(cxf_pqueue_size(&q) == 7);
        CHECK(cxf_pqueue_check(&q));
        for (i = 0; i < 7; i++) {
            CHECK(cxf_pqueue_pop(&q, &k, &id));
            CHECK(k == want_key[i] && id == want_id[i]);
        }
        CHECK(cxf_pqueue_size(&q) == 0);
    }
}

static void test_vec(void) {
    cxf_alloc *al = cxf_alloc_heap();
    uint64_t before = al->stat.bytes_live;
    cxf_vec v = cxf_vec_make(al, sizeof(uint32_t));
    uint32_t i;

    CHECK(cxf_vec_check(&v));
    CHECK(cxf_vec_len(&v) == 0);
    CHECK(cxf_vec_cap(&v) == 0);

    /* 增长：十次 push 跨过一次扩容，内容按序保留 */
    for (i = 0; i < 10; i++) CHECK(cxf_vec_push(&v, &i) == CXF_OK);
    CHECK(cxf_vec_len(&v) == 10);
    CHECK(cxf_vec_cap(&v) >= 10);

    /* 再推到 200：多次扩容后前缀必须逐项不变 —— 扩容搬数据的唯一验收点 */
    for (i = 10; i < 200; i++) CHECK(cxf_vec_push(&v, &i) == CXF_OK);
    for (i = 0; i < 200; i++) {
        CHECK(*(const uint32_t *)cxf_vec_cat(&v, i) == i);
    }
    CHECK(cxf_vec_data(&v, uint32_t)[199] == 199);

    /* push_slot：槽位未初始化，调用方必须写全（这里写全再验） */
    {
        uint32_t *slot = (uint32_t *)cxf_vec_push_slot(&v);

        CHECK(slot != NULL);
        CHECK(cxf_vec_len(&v) == 201);
        *slot = 12345u;
        CHECK(cxf_vec_data(&v, uint32_t)[200] == 12345u);
    }

    /* clear 只丢长度，容量保留 —— "重置而不重建" */
    {
        uint32_t cap = cxf_vec_cap(&v);

        cxf_vec_clear(&v);
        CHECK(cxf_vec_len(&v) == 0);
        CHECK(cxf_vec_cap(&v) == cap);
        CHECK(cxf_vec_check(&v));
    }

    /* 宽元素：elem 不是 1 或 4 时，步长与对齐都要对 */
    {
        struct wide { uint32_t a; double b; } tmp;
        cxf_vec wv = cxf_vec_make(al, (uint32_t)sizeof(tmp));
        const struct wide *got;

        tmp.a = 7;
        tmp.b = 2.5;
        CHECK(cxf_vec_push(&wv, &tmp) == CXF_OK);
        tmp.a = 8;
        tmp.b = -1.0;
        CHECK(cxf_vec_push(&wv, &tmp) == CXF_OK);

        got = (const struct wide *)cxf_vec_cat(&wv, 0);
        CHECK(got != NULL && got->a == 7 && got->b == 2.5);
        got = (const struct wide *)cxf_vec_cat(&wv, 1);
        CHECK(got != NULL && got->a == 8 && got->b == -1.0);
        cxf_vec_free(&wv);
    }

    cxf_vec_free(&v);
    CHECK(al->stat.bytes_live == before);   /* 释放大小与申请时同一算式 */
    cxf_vec_free(&v);                       /* 幂等 */
    CHECK(cxf_vec_len(&v) == 0 && cxf_vec_cap(&v) == 0);
}

int main(void) {
    test_handle();
    test_table_class();
    test_alloc();
    test_arena();
    test_arena_frame_depth();
    test_table();
    test_table_grow();
    test_table_remove_during_iteration();
    test_table_clear();
    test_ring();
    test_str();
    test_sb();
    test_bitmap();
    test_buf();
    test_blob();
    test_namemap();
    test_algo();
    test_csr();
    test_graph_algo();
    test_pqueue();
    test_vec();

    if (g_fail == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_fail);
    return 1;
}
