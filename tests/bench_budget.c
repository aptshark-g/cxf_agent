/*
 * 上下文预算对比台（docs/agent.md §4.15）。
 *
 * 量一件事：**给多少字节的检索结果，任务所需的信息才进入上下文**。
 * 这不是性能基准，是"实现之间做数据比较"的台子 —— 同一批任务、同一套预算阶梯，
 * 只换检索实现，看拐点落在哪。胜出的实现才写进 CapDef，候选不上目录。
 *
 * ★ 它测的是三段里的**第二段**，不是全部。三段必须分开：
 *
 *     1. 找   retrieval   工具调用几次 / 几轮才找到     ← 本台**固定 1 次**，没测
 *     2. 装   context     塞进 prompt 的字节与噪声      ← **本台测这一段**
 *     3. 用   inference   模型能否答对 / 多贵           ← 没测（要真模型）
 *
 * 所以输出里的 recall 是**"信息到达率"**，不是"答对率"：判据是目标标记在不在
 * 上下文里，与模型理解无关。这一点必须钉住 ——
 *   预算 < b* 而失败 ⇒ 信息没进去（检索的锅）
 *   预算 ≥ b* 仍失败 ⇒ 信息进去了但没用上（腐烂 / 注意力 / 干扰的锅）
 * **b* 就是区分这两种失败的分母**；没有它，"模型变差"无法归因。
 *
 * 口径诚实：这里用**字节**不是 token（不引 tokenizer，零依赖）。同一语料同一任务
 * 比较的是比值，所以 85× 与"噪声 7→0"的结论与单位无关；但若要和"32K token 悬崖"
 * 那类数字对照，必须乘本语料自己的 bytes/token 比，不能直接比。
 *
 * 三个候选实现的差别只在**查询感知程度**：
 *   ls        列目录（零感知）    —— 只有名字，没有内容
 *   read_all  整文读入（零感知）  —— 把文件一段段塞满
 *   grep_hits 命中行（查询感知）  —— 只给包含目标的那一行
 *
 * 为什么不在 make test 里：它产出的是**数**（拐点），不是"过/不过"。
 * 与 bench_ds 同一类：测量，不是回归。
 *
 * 为什么语料自己造：基准数据不能借别处的状态（bench_ds 踩过：teardown 后读
 * 已 free 的数组，读到复用内存，输入根本不是随机的）。这里每次运行先重建语料，
 * 内容只由 (d,f,line) 决定，与仓库内容无关 —— 我改文档不会改变曲线。
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define NDIR    3
#define NFILE   4
#define NLINES  24
#define NTASK   8
#define MAXFILE 4096

#define CORPUS_ROOT "build/budget_corpus"

static char     g_body[NDIR][NFILE][MAXFILE];
static uint32_t g_len [NDIR][NFILE];
static char     g_path[NDIR][NFILE][96];

/* 每行要种哪个任务号；-1 = 不种。种在行首之后，长度固定，便于确定性断言。 */
static int g_plant[NDIR][NFILE][NLINES];

static uint32_t g_corpus_bytes;

/* ---- 语料 ---- */

static void ensure_dir(const char *p) {
    if (mkdir(p, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "mkdir %s: %s\n", p, strerror(errno));
}

static int plant_slot(uint32_t task, uint32_t *d, uint32_t *f, uint32_t *line) {
    /* 单射：k=0..7 落在互不相同的 (dir,file,line)。 */
    *d    = task % NDIR;
    *f    = (task / NDIR) % NFILE;
    *line = (task * 7u) % NLINES;

    for (uint32_t t = 0; t < task; t++) {
        uint32_t d2 = t % NDIR, f2 = (t / NDIR) % NFILE, l2 = (t * 7u) % NLINES;
        if (d2 == *d && f2 == *f && l2 == *line) return 0;   /* 撞了就说，别静默 */
    }
    return 1;
}

static void corpus_build(void) {
    memset(g_plant, 0xFF, sizeof(g_plant));   /* 全 -1 */
    g_corpus_bytes = 0;

    for (uint32_t k = 0; k < NTASK; k++) {
        uint32_t d, f, l;
        if (!plant_slot(k, &d, &f, &l)) {
            fprintf(stderr, "语料布置失败: task %u 与前面重位\n", k);
            return;
        }
        g_plant[d][f][l] = (int)k;
    }

    ensure_dir("build");
    ensure_dir(CORPUS_ROOT);

    for (uint32_t d = 0; d < NDIR; d++) {
        char dir[96];
        snprintf(dir, sizeof(dir), "%s/d%u", CORPUS_ROOT, d);
        ensure_dir(dir);

        for (uint32_t f = 0; f < NFILE; f++) {
            uint32_t n = 0;

            snprintf(g_path[d][f], sizeof(g_path[d][f]), "%s/f%u.txt", dir, f);

            for (uint32_t j = 0; j < NLINES; j++) {
                int n2 = snprintf(g_body[d][f] + n, MAXFILE - n,
                                  "d%u f%u l%02u:", d, f, j);
                if (n2 < 0 || (uint32_t)n2 >= MAXFILE - n) return;
                n += (uint32_t)n2;

                /* 24 个 'x' 填充：让"整段读入"的字节代价是真实的 */
                for (uint32_t p = 0; p < 24u; p++) {
                    if (n + 1u >= MAXFILE) return;
                    g_body[d][f][n++] = 'x';
                }
                if (g_plant[d][f][j] >= 0) {
                    n2 = snprintf(g_body[d][f] + n, MAXFILE - n,
                                  " TOK_%02d", (unsigned)g_plant[d][f][j]);
                    if (n2 < 0 || (uint32_t)n2 >= MAXFILE - n) return;
                    n += (uint32_t)n2;
                }
                if (n + 1u >= MAXFILE) return;
                g_body[d][f][n++] = '\n';
            }

            g_len[d][f] = n;
            g_corpus_bytes += n;

            FILE *fp = fopen(g_path[d][f], "wb");
            if (fp == NULL) {
                fprintf(stderr, "写不出来 %s\n", g_path[d][f]);
                return;
            }
            fwrite(g_body[d][f], 1u, n, fp);
            fclose(fp);
        }
    }
}

static bool read_from_disk(uint32_t d, uint32_t f, char *dst, uint32_t cap) {
    FILE *fp = fopen(g_path[d][f], "rb");

    if (fp == NULL) return false;
    (void)fread(dst, 1u, cap, fp);   /* 长度以 g_len 为准：语料是本进程刚写的 */
    fclose(fp);
    return true;
}

/* ---- 三个候选实现 ---- */

static bool has_substr(const char *s, uint32_t n, const char *needle) {
    uint32_t m = (uint32_t)strlen(needle);

    if (m == 0u || n < m) return false;
    for (uint32_t i = 0; i + m <= n; i++)
        if (memcmp(s + i, needle, m) == 0) return true;
    return false;
}

/*
 * 列目录：只给路径。这是"最省上下文"的极端，也是"最没用"的极端 ——
 * 名字里不含内容，所以任何预算都命中不了。留着它就是为了让另外两列的
 * "节省"有分母：省字节不是目的，**在预算内命中**才是。
 */
static uint32_t v_ls(uint32_t task, uint32_t budget, char *out, uint32_t out_cap) {
    uint32_t n = 0;

    (void)task;
    if (budget > out_cap) return 0xFFFFFFFFu;   /* 报告：预算超过缓冲区 */

    for (uint32_t d = 0; d < NDIR; d++)
        for (uint32_t f = 0; f < NFILE; f++) {
            int w = snprintf(out + n, budget - n, "d%u/f%u.txt\n", d, f);
            if (w < 0 || (uint32_t)w >= budget - n) return n;
            n += (uint32_t)w;
        }
    return n;
}

/* 整文读入：零查询感知，按固定顺序一段段塞满。 */
static uint32_t v_read_all(uint32_t task, uint32_t budget, char *out, uint32_t out_cap) {
    uint32_t n = 0;
    char tmp[MAXFILE];

    (void)task;
    if (budget > out_cap) return 0xFFFFFFFFu;

    for (uint32_t d = 0; d < NDIR; d++)
        for (uint32_t f = 0; f < NFILE; f++) {
            uint32_t take;

            if (!read_from_disk(d, f, tmp, MAXFILE)) return n;
            take = g_len[d][f];
            if (take > budget - n) take = budget - n;
            memcpy(out + n, tmp, take);
            n += take;
            if (n >= budget) return n;
        }
    return n;
}

/*
 * 命中行：查询感知。只输出包含目标 token 的那一行（带定位前缀）。
 * 这是"能力返回粒度决定上下文默认质量"的正面样本 ——
 * 同一份语料、同一个任务，它把预算需求从"整库"压到"一行"。
 */
static uint32_t v_grep_hits(uint32_t task, uint32_t budget, char *out, uint32_t out_cap) {
    char needle[16];
    char tmp[MAXFILE];
    uint32_t n = 0;

    if (budget > out_cap) return 0xFFFFFFFFu;
    snprintf(needle, sizeof(needle), "TOK_%02u", (unsigned)task);

    for (uint32_t d = 0; d < NDIR; d++)
        for (uint32_t f = 0; f < NFILE; f++) {
            uint32_t i = 0, lineno = 0;

            if (!read_from_disk(d, f, tmp, MAXFILE)) return n;

            while (i < g_len[d][f]) {
                uint32_t e = i;
                int w;

                while (e < g_len[d][f] && tmp[e] != '\n') e++;
                lineno++;
                if (has_substr(tmp + i, e - i, needle)) {
                    /* 定位前缀 + 命中行原样；行尾换行保留 */
                    w = snprintf(out + n, budget - n, "d%u/f%u:%u:", d, f, lineno);
                    if (w < 0 || (uint32_t)w >= budget - n) return n;
                    n += (uint32_t)w;

                    {
                        uint32_t span = e - i;
                        if (span > budget - n) span = budget - n;
                        memcpy(out + n, tmp + i, span);
                        n += span;
                    }
                    if (n < budget) out[n++] = '\n';
                    else return n;
                }
                i = e + 1u;
            }
        }
    return n;
}

typedef struct Variant {
    const char *name;
    const char *note;   /* 这一列想证明什么，写在这里而不是输出里现编 */
    uint32_t  (*run)(uint32_t task, uint32_t budget, char *out, uint32_t out_cap);
} Variant;

static const Variant VARIANTS[] = {
    { "ls",        "零感知：只有名字，没有内容",     v_ls        },
    { "read_all",  "零感知：整文段段塞满",           v_read_all  },
    { "grep_hits", "查询感知：只给命中行",           v_grep_hits },
};
#define NVARIANT ((uint32_t)(sizeof(VARIANTS) / sizeof(VARIANTS[0])))

#define NBUDGET 11
static const uint32_t BUDGETS[NBUDGET] = {
    48u, 96u, 192u, 384u, 768u, 1536u, 3072u, 6144u, 8192u, 10240u, 12288u
};
#define BUFCAP 16384u

/*
 * 数一数上下文里混进了多少个"别的任务的"标记。
 *
 * 这一列比字节数更要紧：Chroma 的实测是**语义邻近的无关内容比更短的上下文更差**，
 * 而这里每个 TOK_xx 都是同形同域的标记 —— 正是"看起来相关但不是这个任务"的那一类。
 * 于是三个实现除了"要多少字节"，还给出了"带进多少噪声"，两个数一起看才叫经济性。
 */
static uint32_t count_tokens(const char *s, uint32_t n) {
    uint32_t c = 0;

    for (uint32_t i = 0; i + 4u <= n; i++)
        if (memcmp(s + i, "TOK_", 4u) == 0) c++;
    return c;
}

/* ---- 测量 ---- */

int main(void) {
    char out[BUFCAP];
    FILE *csv;
    uint32_t best[NVARIANT][NTASK];   /* 拐点 b*：0 = 该实现拿不到这条任务 */
    uint32_t bnoise[NVARIANT][NTASK]; /* 在拐点处的噪声数 */
    uint32_t vmin[NVARIANT], vmax[NVARIANT], vhits[NVARIANT], vnoise[NVARIANT];

    corpus_build();
    if (g_corpus_bytes == 0u) {
        fprintf(stderr, "语料没建起来\n");
        return 1;
    }

    csv = fopen("build/budget.csv", "w");
    if (csv == NULL) { fprintf(stderr, "开不了 build/budget.csv\n"); return 1; }
    fprintf(csv, "variant,task,budget,bytes,recall,noise\n");

    for (uint32_t v = 0; v < NVARIANT; v++)
        for (uint32_t t = 0; t < NTASK; t++) { best[v][t] = 0u; bnoise[v][t] = 0u; }

    printf("corpus %u files, %u bytes\n\n", NDIR * NFILE, g_corpus_bytes);

    for (uint32_t v = 0; v < NVARIANT; v++) {
        for (uint32_t t = 0; t < NTASK; t++) {
            char needle[16];
            snprintf(needle, sizeof(needle), "TOK_%02u", (unsigned)t);

            for (uint32_t b = 0; b < NBUDGET; b++) {
                uint32_t n = VARIANTS[v].run(t, BUDGETS[b], out, BUFCAP);
                uint32_t total, noise;
                int hit;

                if (n == 0xFFFFFFFFu) { fprintf(stderr, "预算超缓冲\n"); return 1; }
                hit   = has_substr(out, n, needle) ? 1 : 0;
                total = count_tokens(out, n);
                noise = total - (uint32_t)hit;

                fprintf(csv, "%s,%u,%u,%u,%d,%u\n",
                        VARIANTS[v].name, t, BUDGETS[b], n, hit, noise);

                if (hit && best[v][t] == 0u) { best[v][t] = BUDGETS[b]; bnoise[v][t] = noise; }
            }
        }
    }
    fclose(csv);

    /* 每条任务的最小可行预算 = 拐点 b*（信息刚好进得去的那个预算）；取不到 = 该实现拿不到这条任务 */
    printf("%-11s %-7s %-7s %-6s %-6s %s\n",
           "variant", "b*min", "b*max", "召回", "噪声", "说明");
    for (uint32_t v = 0; v < NVARIANT; v++) {
        vmin[v] = 0xFFFFFFFFu;
        vmax[v] = 0u;
        vhits[v] = 0u;
        vnoise[v] = 0u;

        for (uint32_t t = 0; t < NTASK; t++) {
            if (best[v][t] == 0u) continue;
            vhits[v]++;
            if (best[v][t] < vmin[v]) vmin[v] = best[v][t];
            if (best[v][t] > vmax[v]) vmax[v] = best[v][t];
            if (bnoise[v][t] > vnoise[v]) vnoise[v] = bnoise[v][t];
        }
    }

    for (uint32_t v = 0; v < NVARIANT; v++) {
        char a[16], b[16], hits[16], noise[16];

        if (vhits[v] == 0u) {
            snprintf(a, sizeof(a), "-");
            snprintf(b, sizeof(b), "-");
            snprintf(noise, sizeof(noise), "-");
        } else {
            snprintf(a, sizeof(a), "%u", vmin[v]);
            snprintf(b, sizeof(b), "%u", vmax[v]);
            snprintf(noise, sizeof(noise), "%u", vnoise[v]);
        }
        snprintf(hits, sizeof(hits), "%u/%u", vhits[v], (unsigned)NTASK);

        printf("%-11s %-7s %-7s %-6s %-6s %s\n", VARIANTS[v].name, a, b, hits, noise,
               vhits[v] == 0u ? "任何预算都不到达 —— 只给名字不够" : VARIANTS[v].note);
    }

    if (vhits[2] != 0u && vhits[1] != 0u)
        printf("\n同一语料同一任务：查询感知把最坏预算 %u → %u（%.0f×），"
               "拐点噪声 %u → %u\n",
               vmax[1], vmax[2], (double)vmax[1] / (double)vmax[2],
               vnoise[1], vnoise[2]);

    printf("注意：召回 = 目标信息是否进入上下文，不是模型是否答对。\n");
    printf("逐点数据：build/budget.csv\n");
    return 0;
}
