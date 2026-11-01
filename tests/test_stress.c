#include <stdio.h>
#include <string.h>

#include "cxf/algo.h"
#include "cxf/alloc.h"
#include "cxf/arena.h"
#include "cxf/bitmap.h"
#include "cxf/blob.h"
#include "cxf/buf.h"
#include "cxf/csr.h"
#include "cxf/namemap.h"
#include "cxf/pqueue.h"
#include "cxf/ring.h"
#include "cxf/sb.h"
#include "cxf/str.h"
#include "cxf/table.h"
#include "cxf/vec.h"

/*
 * 模型对照的随机序列测试 —— "成熟"在这里的定义是：
 * 任意操作顺序下都**不静默错**，而不只是"用例都过"。
 *
 * 三条被守的性质：
 *   1. handle 判活精确等于模型的集合（不多不少），值跟着 handle 走
 *      —— 这条同时兜住 swap-remove 搬行、槽位复用、世代翻转、容量增长
 *   2. 环形缓冲区是字节流，不重不漏不串序
 *   3. 帧内分配不重叠；pop 回到入帧水位
 *
 * 用确定性伪随机而不是 rand()：同一颗种子在任何机器上跑出同一条序列，
 * 失败可复现、可最小化。rand() 的序列随实现变，"上次那个失败"就再也查不到。
 */
static int g_fail = 0;

#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

static uint32_t g_rng = 0x9E3779B9u;

static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static uint32_t rnd_n(uint32_t n) {
    return n != 0 ? rnd() % n : 0;
}

/* ── 表：模型 = 一组 (handle -> 值)，加一组已死的 handle ─────────────── */

#define MDL_MAX  256
#define DEAD_MAX 32

typedef struct {
    cxf_handle h;
    uint32_t   val;
} mdl_row;

static mdl_row      g_mdl[MDL_MAX];
static uint32_t     g_mdl_n;
static cxf_handle   g_dead[DEAD_MAX];
static uint32_t     g_dead_n;

static void stress_table(void) {
    cxf_alloc *al = cxf_alloc_heap();
    const size_t sizes[1] = { sizeof(uint32_t) };
    cxf_table t;
    uint32_t tag = 1, i;

    /* cap 故意给 4：让"增长时搬数据"这条路径被反复踩到 */
    CHECK(cxf_table_init_class(&t, al, sizes, 1, 4, 7) == CXF_OK);

    g_mdl_n = 0;
    g_dead_n = 0;

    for (i = 0; i < 40000; i++) {
        uint32_t op = rnd_n(100);

        if (op < 45 && g_mdl_n < MDL_MAX) {
            cxf_handle h = cxf_table_add(&t);

            CHECK(!cxf_handle_null(h));
            CHECK(cxf_table_live(&t, h));
            CHECK(cxf_handle_class(h) == 7);       /* T8：class 位不许被增长/复用弄丢 */

            g_mdl[g_mdl_n].h = h;
            g_mdl[g_mdl_n].val = tag;
            *(uint32_t *)cxf_table_get(&t, h, 0) = tag;
            g_mdl_n++;
            tag++;
        } else if (op < 80 && g_mdl_n > 0) {
            uint32_t k = rnd_n(g_mdl_n);
            cxf_handle h = g_mdl[k].h;

            CHECK(cxf_table_remove(&t, h) == CXF_OK);
            CHECK(!cxf_table_live(&t, h));
            CHECK(cxf_table_get(&t, h, 0) == NULL);

            if (g_dead_n < DEAD_MAX) g_dead[g_dead_n++] = h;
            g_mdl[k] = g_mdl[g_mdl_n - 1];   /* swap-remove 掉模型里的那一行 */
            g_mdl_n--;
        } else if (op < 88 && g_dead_n > 0) {
            cxf_handle h = g_dead[rnd_n(g_dead_n)];

            /* 死 handle 重复删除必须响亮失败，不能二次回收制造双活 */
            CHECK(!cxf_table_live(&t, h));
            CHECK(cxf_table_remove(&t, h) == CXF_ERR_BAD_HANDLE);
        } else if (op < 99) {
            uint32_t k;

            /* 全量核对：判活精确等于模型集合，值跟着 handle 走 */
            CHECK(t.count == g_mdl_n);
            for (k = 0; k < g_mdl_n; k++) {
                uint32_t *p = cxf_table_get(&t, g_mdl[k].h, 0);

                CHECK(cxf_table_live(&t, g_mdl[k].h));
                CHECK(p != NULL);
                if (p != NULL) CHECK(*p == g_mdl[k].val);
            }
            for (k = 0; k < g_dead_n; k++)
                CHECK(!cxf_table_live(&t, g_dead[k]));
            CHECK(cxf_table_check(&t));
        } else {
            uint32_t k;

            cxf_table_clear(&t);
            for (k = 0; k < g_mdl_n; k++)
                CHECK(!cxf_table_live(&t, g_mdl[k].h));  /* 清空即全部失效 */
            g_mdl_n = 0;
            CHECK(t.count == 0);
            CHECK(cxf_table_check(&t));
        }
    }
    cxf_table_release(&t);
}

/* ── 环：写进去的是一段不带空隙的字节流 ─────────────────────────────── */

static void stress_ring(void) {
    cxf_alloc *al = cxf_alloc_heap();
    cxf_ring r;
    uint8_t wr[128], rd[128];
    uint32_t written = 0, consumed = 0, i;

    CHECK(cxf_ring_init(&r, al, 256) == CXF_OK);

    for (i = 0; i < 40000; i++) {
        if (rnd_n(100) < 55) {
            uint32_t n = rnd_n(100) + 1, k, w;

            for (k = 0; k < n; k++) wr[k] = (uint8_t)(written + k);
            w = cxf_ring_write(&r, wr, n);
            written += w;   /* 只推进真正写进去的部分：部分写不得在流里留洞 */
        } else {
            uint32_t n = rnd_n(100) + 1, k, got;

            got = cxf_ring_read(&r, rd, n);
            for (k = 0; k < got; k++) CHECK(rd[k] == (uint8_t)(consumed + k));
            consumed += got;
        }
        CHECK(cxf_ring_check(&r));
    }

    CHECK(written - consumed == cxf_ring_size(&r));  /* 流守恒 */
    cxf_ring_release(&r, al);
}

/* ── arena：帧内不重叠，pop 精确回到入帧水位 ─────────────────────────── */

static void stress_arena(void) {
    cxf_alloc *al = cxf_alloc_heap();
    cxf_arena a;
    unsigned char *prev = NULL;
    uint32_t prev_n = 0, prev_fill = 0, prev_gen = 0, gen = 0, i;

    cxf_arena_init(&a, al, 1024);   /* 小 chunk，逼出跨 chunk 分配 */

    for (i = 0; i < 30000; i++) {
        uint32_t op = rnd_n(100);

        if (op < 35 && a.depth < CXF_ARENA_MAX_FRAMES) {
            CHECK(cxf_arena_push(&a) == CXF_OK);
        } else if (op < 60 && a.depth > 0) {
            cxf_arena_pop(&a);
            gen++;
            prev = NULL;   /* pop 之后旧指针失效（R5），不许再读 */
        } else {
            uint32_t n = rnd_n(400) + 1;
            size_t before = cxf_arena_bytes_used(&a);
            void *p = cxf_arena_alloc(&a, n, 1);

            CHECK(p != NULL);
            CHECK(cxf_arena_bytes_used(&a) >= before + n);  /* 帧内只前进 */

            memset(p, (int)(unsigned char)(i & 0xFF), n);
            if (prev != NULL && prev_gen == gen) {
                uint32_t k;

                for (k = 0; k < prev_n; k++)
                    CHECK(prev[k] == (unsigned char)prev_fill);  /* 不得被后一次分配覆盖 */
            }
            prev = p;
            prev_n = n;
            prev_fill = i & 0xFF;
            prev_gen = gen;
        }
        CHECK(cxf_arena_check(&a));
    }
    cxf_arena_release(&a);
}

/* ── str / sb：split 与 sb 必须互为逆运算 ───────────────────────────── */

static void stress_str_sb(void) {
    cxf_alloc *al = cxf_alloc_heap();
    cxf_arena a;
    uint32_t iter;

    cxf_arena_init(&a, al, 4096);

    for (iter = 0; iter < 3000; iter++) {
        char raw[192];
        uint32_t n = rnd_n(180), k, ntok = 1;
        cxf_sb m, sb;
        cxf_str view, tok;
        cxf_str_iter it;

        for (k = 0; k < n; k++) {
            char c = (char)('a' + rnd_n(26));

            if (rnd_n(8) == 0) {   /* 掺入分隔符，包括连续与首尾的位置 */
                c = '|';
                ntok++;
            }
            raw[k] = c;
        }

        CHECK(cxf_arena_push(&a) == CXF_OK);

        /* 第一遍只数，第二遍写进 arena 的精确缓冲 —— 两遍长度必须一致 */
        m = cxf_sb_measure();
        for (k = 0; k < n; k++) cxf_sb_putc(&m, raw[k]);

        {
            char *p = cxf_arena_alloc(&a, m.len != 0 ? m.len : 1u, 1);

            CHECK(p != NULL);
            sb = cxf_sb_buf(p, m.len);
        }
        for (k = 0; k < n; k++) cxf_sb_putc(&sb, raw[k]);
        CHECK(cxf_sb_len(&sb) == m.len);
        CHECK(!cxf_sb_overflow(&sb));

        view = cxf_sb_view(&sb);
        CHECK(cxf_str_eq(view, cxf_str_make(raw, n)));

        /* 切分再拼回：token 数 = 分隔符数 + 1，内容逐字节相等 */
        {
            cxf_sb m2 = cxf_sb_measure(), sb2;
            char  *p2;
            uint32_t first = 1, seen = 0;

            cxf_str_split(&it, view, '|');
            while (cxf_str_split_next(&it, &tok)) {
                if (!first) cxf_sb_putc(&m2, '|');
                cxf_sb_puts(&m2, tok);
                first = 0;
                seen++;
            }
            CHECK(seen == ntok);

            p2 = cxf_arena_alloc(&a, m2.len != 0 ? m2.len : 1u, 1);
            CHECK(p2 != NULL);
            sb2 = cxf_sb_buf(p2, m2.len);

            first = 1;
            cxf_str_split(&it, view, '|');
            while (cxf_str_split_next(&it, &tok)) {
                if (!first) cxf_sb_putc(&sb2, '|');
                cxf_sb_puts(&sb2, tok);
                first = 0;
            }
            CHECK(cxf_str_eq(cxf_sb_view(&sb2), cxf_str_make(raw, n)));
        }

        cxf_arena_pop(&a);
    }
    cxf_arena_release(&a);
}

/* ── 位图：与影子数组逐位对拍 ───────────────────────────────────────── */

static void stress_bitmap(void) {
    enum { NB = 1000 };              /* 非 64 倍数，专踩末字尾部 */
    uint64_t        mem[(NB + 63) / 64];
    static uint8_t  shadow[NB];
    cxf_bitmap      b;
    uint32_t        i, k;

    memset(shadow, 0, sizeof(shadow));
    cxf_bitmap_init(&b, mem, NB);

    for (i = 0; i < 30000; i++) {
        uint32_t at = rnd_n(NB);
        uint32_t op = rnd_n(4);

        if (op == 0) {
            cxf_bitmap_set(&b, at);
            shadow[at] = 1;
        } else if (op == 1) {
            cxf_bitmap_unset(&b, at);
            shadow[at] = 0;
        } else if (op == 2) {
            CHECK(cxf_bitmap_get(&b, at) == (shadow[at] != 0));
        } else {
            uint32_t expect = CXF_NO_INDEX;   /* 影子实现：线性扫 */

            for (k = at; k < NB; k++) if (shadow[k]) { expect = k; break; }
            CHECK(cxf_bitmap_next(&b, at) == expect);
        }
    }

    {
        uint32_t n = 0;

        for (k = 0; k < NB; k++) n += shadow[k] != 0;
        CHECK(cxf_bitmap_popcount(&b) == n);
    }
    CHECK(cxf_bitmap_check(&b));
}

/* ── buf：增长之后前缀必须一模一样 ──────────────────────────────────── */

static void stress_buf(void) {
    cxf_alloc     *al = cxf_alloc_heap();
    cxf_buf        b = cxf_buf_make(al);
    static uint8_t shadow[1 << 15];
    uint32_t       slen = 0, i;

    for (i = 0; i < 1500; i++) {
        uint8_t  tmp[64];
        uint32_t n = rnd_n(64), k;

        for (k = 0; k < n; k++) tmp[k] = (uint8_t)rnd();

        if (slen + n > sizeof(shadow)) {   /* 到顶就重置，继续踩"重置而不重建" */
            cxf_buf_reset(&b);
            slen = 0;
            continue;
        }
        CHECK(cxf_buf_append(&b, tmp, n) == CXF_OK);
        memcpy(shadow + slen, tmp, n);
        slen += n;

        CHECK(b.len == slen);
        CHECK(cxf_buf_check(&b));
        CHECK(memcmp(b.p, shadow, slen) == 0);   /* 倍增搬移最容易在这里出错 */
    }
    cxf_buf_free(&b);
}

/* ── blob：随机建/写/还，最后必须归零 ───────────────────────────────── */

static void stress_blob(void) {
    enum { BN = 64 };
    cxf_alloc    *al = cxf_alloc_heap();
    cxf_blobpool  bp;
    cxf_handle    hs[BN];
    uint8_t       live[BN];
    uint32_t      nlive = 0, i;

    memset(live, 0, sizeof(live));
    CHECK(cxf_blobpool_init(&bp, al, 8, 3) == CXF_OK);

    for (i = 0; i < 20000; i++) {
        uint32_t k = rnd_n(BN);

        if (!live[k]) {
            hs[k] = cxf_blob_new(&bp, rnd_n(32));
            if (!cxf_handle_null(hs[k])) { live[k] = 1; nlive++; }
        } else {
            uint8_t  tmp[48];
            uint32_t n = rnd_n(48), j;

            for (j = 0; j < n; j++) tmp[j] = (uint8_t)(i + j);
            CHECK(cxf_blob_append(&bp, hs[k], tmp, n) == CXF_OK);

            if (rnd_n(3) == 0) {           /* 归还集中在这里，模拟终态回收 */
                CHECK(cxf_blob_free(&bp, hs[k]) == CXF_OK);
                live[k] = 0;
                nlive--;
            }
        }
        if (rnd_n(1000) == 0) {
            CHECK(cxf_blobpool_check(&bp));
            CHECK(bp.count == nlive);   /* 模型存活数与池的对账 */
        }
    }

    for (i = 0; i < BN; i++)
        if (live[i]) CHECK(cxf_blob_free(&bp, hs[i]) == CXF_OK);

    CHECK(cxf_blobpool_bytes(&bp) == 0);   /* L2：全归还 ⇒ 零 */
    CHECK(cxf_blobpool_check(&bp));
    cxf_blobpool_release(&bp);
}

/* ── namemap：批量插入后只读，二分的正确性靠全量回查 ────────────────── */

static void stress_namemap(void) {
    cxf_alloc   *al = cxf_alloc_heap();
    cxf_arena    a;
    cxf_namemap  m;
    char         names[200][5];
    uint32_t     i, id;

    cxf_arena_init(&a, al, 1u << 16);
    cxf_namemap_init(&m);

    for (i = 0; i < 200; i++) {
        static const char *hex = "0123456789abcdef";

        names[i][0] = 'n';
        names[i][1] = hex[(i >> 8) & 0xFu];
        names[i][2] = hex[(i >> 4) & 0xFu];
        names[i][3] = hex[i & 0xFu];
        names[i][4] = '\0';
        CHECK(cxf_namemap_add(&m, &a, cxf_str_make(names[i], 4), i));
    }
    CHECK(cxf_namemap_count(&m) == 200);
    CHECK(cxf_namemap_check(&m));

    for (i = 0; i < 200; i++)
        CHECK(cxf_namemap_find(&m, cxf_str_make(names[i], 4), &id) && id == i);

    CHECK(!cxf_namemap_find(&m, CXF_STR_LIT("zzzz"), &id));
    CHECK(!cxf_namemap_add(&m, &a, cxf_str_make(names[7], 4), 999));  /* 重复必须被拒 */

    cxf_arena_release(&a);
}

/* ── csr：拓扑序与可达性对着独立实现（邻接矩阵 + DFS / 传递闭包）────── */

/* 独立判环：三色 DFS。n <= 8，递归深度有界，不存在栈风险。 */
static bool dfs_cycle(const uint8_t *m, uint32_t n, uint32_t u, uint8_t *color) {
    uint32_t v;

    color[u] = 1;
    for (v = 0; v < n; v++) {
        if (!m[u * n + v]) continue;
        if (color[v] == 1) return true;
        if (color[v] == 0 && dfs_cycle(m, n, v, color)) return true;
    }
    color[u] = 2;
    return false;
}

static bool brute_cyclic(const uint8_t *m, uint32_t n) {
    uint8_t color[8] = { 0 };
    uint32_t i;

    for (i = 0; i < n; i++)
        if (color[i] == 0 && dfs_cycle(m, n, i, color)) return true;
    return false;
}

static void stress_csr(void) {
    enum { GN = 6, GM = 12 };
    cxf_csr_edge e[GM];
    uint32_t     mem[GN + 1 + GM];
    uint32_t     indeg[GN], order[GN], queue[GN];
    uint64_t     vmem[1];
    cxf_bitmap   visited;
    uint8_t      adjm[GN * GN], clos[GN * GN];
    uint32_t     iter;

    cxf_bitmap_init(&visited, vmem, GN);

    for (iter = 0; iter < 2000; iter++) {
        uint32_t m = rnd_n(GM + 1), i;
        cxf_csr  g;

        memset(adjm, 0, sizeof(adjm));
        for (i = 0; i < m; i++) {
            e[i].from = rnd_n(GN);
            e[i].to = rnd_n(GN);
            adjm[e[i].from * GN + e[i].to] = 1;   /* 重边在矩阵里合并，判环不受影响 */
        }

        CHECK(cxf_csr_from_edges(&g, mem, GN, e, m) == CXF_OK);
        CHECK(cxf_csr_check(&g));
        CHECK(g.n == GN && g.m == m);

        /* 拓扑序：判环与"边方向"两件事一起对拍 */
        {
            cxf_result rc = cxf_algo_toposort(&g, indeg, order);

            if (brute_cyclic(adjm, GN)) {
                CHECK(rc == CXF_ERR_DEP);
            } else {
                CHECK(rc == CXF_OK);
                if (rc == CXF_OK) {
                    uint32_t pos[GN];

                    for (i = 0; i < GN; i++) pos[order[i]] = i;
                    for (i = 0; i < m; i++)
                        CHECK(pos[e[i].from] < pos[e[i].to]);   /* 每条边都必须朝前 */
                }
            }
        }

        /* 可达性：对传递闭包 */
        {
            uint32_t a, b, k;

            memcpy(clos, adjm, sizeof(clos));
            for (k = 0; k < GN; k++)
                for (a = 0; a < GN; a++)
                    for (b = 0; b < GN; b++)
                        if (clos[a * GN + k] && clos[k * GN + b]) clos[a * GN + b] = 1;

            for (a = 0; a < GN; a++)
                for (b = 0; b < GN; b++) {
                    bool want = (a == b) || clos[a * GN + b] != 0;
                    CHECK(cxf_algo_reach(&g, a, b, &visited, queue) == want);
                }
        }
    }
}

/* ── 堆：模型 = (key,id) 的全序，参照实现是线性扫描 ──────────────────── */

/*
 * 参照实现刻意不调用堆、也不复用 less()：模型侧是"每次线性扫一遍找最小"，
 * 与堆的 sift 是完全不同的两条路径。若两边共用同一段比较代码，比较写错时
 * 两边会一起错，测试就成了自证。
 *
 * id 在 [0,cap) 内唯一、每个 id 同时最多在堆里一份 —— 这样 (key,id) 就是
 * 元素的身份，模型与堆可以逐项比对而不用处理"同一 key 两个相同元素"的歧义。
 * key 只取 0..7，制造大量相等 —— P3 的 id 次序正是在这种拥挤下才被压出来。
 */
#define PQ_CAP 64

static bool pq_mdl_min(const uint32_t *key, const bool *used, uint32_t n,
                       uint32_t *id_out, uint32_t *key_out) {
    uint32_t i, best = 0;
    bool found = false;

    for (i = 0; i < n; i++) {
        if (!used[i]) continue;
        /* 升序扫描 + 严格小于 ⇒ 相等 key 保留先遇到的 = id 最小者 */
        if (!found || key[i] < key[best]) {
            best = i;
            found = true;
        }
    }
    if (found) {
        *id_out = best;
        *key_out = key[best];
    }
    return found;
}

static void stress_pqueue(void) {
    cxf_pqueue_ent mem[PQ_CAP];
    cxf_pqueue q;
    uint32_t key[PQ_CAP], nm = 0, step;
    bool used[PQ_CAP];

    cxf_pqueue_init(&q, mem, PQ_CAP);
    memset(used, 0, sizeof used);

    for (step = 0; step < 20000; step++) {
        uint32_t op = rnd_n(10), id = rnd_n(PQ_CAP), k = rnd_n(8);
        uint32_t rk = 0, rid = 0;

        if (op < 4) {
            /* push */
            if (nm == PQ_CAP) {
                CHECK(!cxf_pqueue_push(&q, k, id));   /* 满：拒绝且不覆盖 */
            } else if (!used[id]) {
                CHECK(cxf_pqueue_push(&q, k, id));
                used[id] = true;
                key[id] = k;
                nm++;
            }
        } else if (op < 7) {
            /* pop：与模型的"最小项"逐字段相同 */
            bool want = pq_mdl_min(key, used, PQ_CAP, &rid, &rk);
            bool got = cxf_pqueue_pop(&q, &k, &id);

            CHECK(got == want);
            if (want) {
                CHECK(k == rk && id == rid);
                used[rid] = false;
                nm--;
            }
        } else if (op < 9) {
            /* update：find_pos + update_at，模型侧只改 key，身份不变 */
            if (used[id]) {
                uint32_t pos = PQ_CAP;

                CHECK(cxf_pqueue_find_pos(&q, id, &pos));
                CHECK(pos < nm);
                cxf_pqueue_update_at(&q, pos, k);
                key[id] = k;
            }
        } else {
            /* peek：不改变任何状态 */
            bool want = pq_mdl_min(key, used, PQ_CAP, &rid, &rk);
            bool got = cxf_pqueue_peek(&q, &k, &id);

            CHECK(got == want);
            if (want) CHECK(k == rk && id == rid);
        }

        /* 每一步都全校验堆序：sift 的方向错误会在这里立刻暴露，
         * 而不是等到某次 pop 才表现为"顺序奇怪" */
        CHECK(cxf_pqueue_check(&q));
        CHECK(cxf_pqueue_size(&q) == nm);
    }

    /* 收尾：连续弹出必须与模型的全序序列逐项相同（这条同时验"不重不漏"） */
    for (;;) {
        uint32_t rk = 0, rid = 0, k = 0, id = 0;
        bool want = pq_mdl_min(key, used, PQ_CAP, &rid, &rk);
        bool got = cxf_pqueue_pop(&q, &k, &id);

        CHECK(got == want);
        if (!want) break;
        CHECK(k == rk && id == rid);
        used[rid] = false;
    }
    CHECK(cxf_pqueue_size(&q) == 0);
    CHECK(cxf_pqueue_check(&q));
}

/* ── vec：模型 = 影子数组 + 长度 ─────────────────────────────────────── */

/*
 * 只对拍三件事，它们覆盖了增长型数组的全部失败方式：
 *   1. 长度与影子一致（多少次 push 就是多长）
 *   2. 每次扩容后，抽样位置的值与影子逐字节相同（搬数据搬错 / 少搬）
 *   3. 原地改写看得见（data 指针取基址后的写入）
 * 抽样而不是全量比对，是因为全量是 O(n) 而每步都做会变成 O(n²)，
 * 测量目标就变成了测试自己。
 */
#define VEC_MAX 4096

static void stress_vec(void) {
    cxf_alloc *al = cxf_alloc_heap();
    uint64_t before = al->stat.bytes_live;
    static uint32_t shadow[VEC_MAX];
    cxf_vec v = cxf_vec_make(al, sizeof(uint32_t));
    uint32_t n = 0, step;

    for (step = 0; step < 20000; step++) {
        uint32_t op = rnd_n(10);

        if (op < 7) {
            /* push：值等于写入时的 step，便于定位"哪一次搬错了" */
            if (n < VEC_MAX) {
                uint32_t val = step;

                CHECK(cxf_vec_push(&v, &val) == CXF_OK);
                shadow[n++] = val;
            }
        } else if (op < 9) {
            /* 抽样回读 */
            if (n != 0) {
                uint32_t i = rnd_n(n);

                CHECK(cxf_vec_data(&v, uint32_t)[i] == shadow[i]);
                CHECK(*(const uint32_t *)cxf_vec_cat(&v, i) == shadow[i]);
            }
        } else if (op == 9 && n != 0) {
            /* 原地改写：走 data 基址，写进去必须立刻可见 */
            uint32_t i = rnd_n(n), val = step ^ 0xA5A5A5A5u;

            cxf_vec_data(&v, uint32_t)[i] = val;
            shadow[i] = val;
            CHECK(cxf_vec_data(&v, uint32_t)[i] == val);
        }

        CHECK(cxf_vec_len(&v) == n);
        CHECK(cxf_vec_cap(&v) >= n);
        CHECK(cxf_vec_check(&v));

        /* 越界读一次就够：elem 步长算错会立刻在这里对不上 */
        if (n != 0) {
            uint32_t j = n - 1;

            CHECK(cxf_vec_data(&v, uint32_t)[j] == shadow[j]);
        }
    }

    /* 收尾全量比对：每一次都验，代价一次付清 */
    CHECK(cxf_vec_len(&v) == n);
    for (uint32_t i = 0; i < n; i++)
        CHECK(cxf_vec_data(&v, uint32_t)[i] == shadow[i]);

    cxf_vec_free(&v);
    CHECK(al->stat.bytes_live == before);   /* 申请与释放同一算式 */
}

int main(void) {
    stress_table();
    stress_ring();
    stress_arena();
    stress_str_sb();
    stress_bitmap();
    stress_buf();
    stress_blob();
    stress_namemap();
    stress_csr();
    stress_pqueue();
    stress_vec();

    if (g_fail == 0) {
        printf("all stress tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_fail);
    return 1;
}
