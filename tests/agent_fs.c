/*
 * fs.grep 的实现：目录里第一个真实能力。
 *
 * 这个文件只做一件事 —— 把"找工作区里哪些行提到了某个东西"变成
 * 一次 tick 内的一个 WorkItem。它不做排序、不做摘要、不做重排：
 * 那些是 §7.3 索引层的事，而索引层现在还零代码。先有一个真的能读盘
 * 的能力，比先有一套漂亮的检索架构重要 —— 否则所有"命中率/预算/噪声"
 * 的讨论都在对着 mock 算出来的数字说话。
 *
 * 与 mock 那一族的分界线不在复杂度，在**状态**：`cap_mock_price` 是
 * 入参的纯函数，永远成功、永远不产生副作用；这里的失败是真实的
 * （目录不在 / 装不下 / 行太长），而"重跑是否安全"第一次成为一个
 * 需要用代码回答的问题 —— 答案是"安全"，因为它只读且顺序固定。
 */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _DEFAULT_SOURCE 1
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cxf/service.h"

#include "agent_args.h"
#include "agent_fs.h"

/* ---- 输出汇：只是"往 out 里追加，装不下就记一笔" ----
 *
 * `cap` 与 `len` 是一对**必须一起变**的值（"装得下吗"这个判断同时读它们）。
 * 所以只有这一份实现，`fs.grep` 与 `fs.diff` 都用它：两份实现对溢出判定的
 * 唯一差别会是 `<` 与 `<=`，而那个差别只会在恰好填满时表现为"少写一个字节"。
 */
typedef struct OutBuf {
    char     *out;
    uint32_t  cap;
    uint32_t  len;
    bool      full;      /* 容量不足 —— 与"被上限截断"是两件事 */
} OutBuf;

static bool buf_put(OutBuf *b, const char *p, uint32_t n) {
    /* len <= cap 恒成立，所以减法安全；n == 0 不会走到 memcpy */
    if (n > b->cap - b->len) { b->full = true; return false; }
    if (n != 0u) memcpy(b->out + b->len, p, n);
    b->len += n;
    return true;
}

/* grep 的汇：追加能力来自 OutBuf，自己只多带"数命中"和"查什么" */
typedef struct Sink {
    OutBuf      b;
    uint32_t    hits;       /* 已写出的命中行 */
    uint32_t    total;      /* 命中总数（含因上限/容量没写出的） */
    uint32_t    max_hits;
    uint32_t    max_depth;
    const char *pat;
    uint32_t    pat_len;
} Sink;

static bool sink_put(Sink *s, const char *p, uint32_t n) {
    return buf_put(&s->b, p, n);
}

/* 有界子串查找。args 来自 blob，不保证 NUL 结尾，所以不能用 strstr。
 * 也不做正则 / 大小写折叠：这不是功能缺失，是**接口窄**的选择 ——
 * 一个能力该有几个自由度，决定了模型能用错几个自由度（§4.15.1）。 */
static bool has_bytes(const char *s, uint32_t n, const char *pat, uint32_t m) {
    if (m == 0u || n < m) return false;
    for (uint32_t i = 0; i + m <= n; i++)
        if (memcmp(s + i, pat, m) == 0) return true;
    return false;
}

/*
 * 把一个字节串的末尾退到**最后一个完整的 UTF-8 字符边界**上。
 *
 * 为什么需要它：`fgets` 在 FS_GREP_LINE_MAX-1 处**按字节**硬切，切口会落在
 * 多字节序列中间（实测：`STATUS.md:352` 那行 553 字节，511 处切开，切口正是
 * 一个 3 字节序列的引导字节 `0xe5`，后续两个字节没了）。于是写进上下文的是一串
 * **非法字节** —— 它进请求体 JSON，宽宏大量的服务端照收、严格一点的直接 400，
 * 而能力层报告的是成功（`rc=OK`、无任何标记）。
 *
 * 这是"有界可以，静默不行"的第三个实例，且比前两个毒：坏的**不是数量，是字节本身**。
 * 超长行只给首段是 §4.15.2 写下的设计（可接受），但切口必须落在字符边界上。
 *
 * 不猜、不修坏字节：引导字节非法时按长度 1 放过（垃圾进垃圾出，但绝不把合法的
 * 序列切坏）。病态输入（连续 4+ 个续字节找不到引导字节）原样返回 —— 宁可保留
 * 原样，也不因为"修不了"把整行吃掉。
 */
static uint32_t utf8_clip(const char *s, uint32_t n) {
    uint32_t k = 0u;
    unsigned char lead;
    uint32_t need;

    if (n == 0u) return 0u;
    while (k < 3u && k + 1u < n &&
           ((unsigned char)s[n - 1u - k] & 0xC0u) == 0x80u) k++;

    lead = (unsigned char)s[n - 1u - k];
    need = (lead & 0x80u) == 0u ? 1u :
           (lead & 0xE0u) == 0xC0u ? 2u :
           (lead & 0xF0u) == 0xE0u ? 3u :
           (lead & 0xF8u) == 0xF0u ? 4u : 1u;

    return (k + 1u >= need) ? n : (n - 1u - k);
}

/*
 * 扫一个文件。
 *
 * 行号规则（超长行的处理是这一节唯一容易写错的地方）：`fgets` 会在
 * FS_GREP_LINE_MAX-1 处硬切，切出来的余料**不是新的一行**。若照普通
 * 循环处理，一份 minified 文件会让后面所有行号整体漂移 —— 而
 * `<path>:<line>` 是索引项的锚点（§7.3 判据：索引项必须能回到代码验证），
 * 锚点漂了，整个索引就不可信。
 *
 * 所以：余料整段跳过、不参与匹配、不增行号。代价是超长行只匹配首段，
 * 这是可接受的边界（超长行的首段就是它的"行首"）—— 但首段也要被
 * `utf8_clip` 收到字符边界上，否则切口会把一个汉字劈成半个。
 */
static void grep_file(Sink *s, const char *full, const char *rel) {
    FILE *f;
    char line[FS_GREP_LINE_MAX];
    uint32_t lineno = 0u;
    bool skipping = false;

    f = fopen(full, "rb");
    if (f == NULL) return;

    while (fgets(line, (int)sizeof line, f) != NULL) {
        size_t n = strlen(line);
        size_t m = n;
        bool cont;

        while (m > 0u && (line[m - 1u] == '\n' || line[m - 1u] == '\r')) m--;
        m = utf8_clip(line, (uint32_t)m);   /* 切口必须落在字符边界上 */

        /* 没读到换行且缓冲写满 ⇒ 该行还有下文 */
        cont = (n == sizeof line - 1u) && (n == 0u || line[n - 1u] != '\n');

        if (skipping) { skipping = cont; continue; }

        lineno++;
        skipping = cont;

        if (!has_bytes(line, (uint32_t)m, s->pat, s->pat_len)) continue;

        s->total++;
        if (s->hits >= s->max_hits) continue;   /* 超上限：只计数，不写出 */

        {
            char head[FS_GREP_PATH_MAX + 24u];
            int hn = snprintf(head, sizeof head, "%s:%u:", rel, lineno);

            if (hn < 0 || (size_t)hn >= sizeof head) continue;
            if (!sink_put(s, head, (uint32_t)hn)) break;
            if (!sink_put(s, line, (uint32_t)m)) break;
            if (!sink_put(s, "\n", 1u)) break;
        }
        s->hits++;
    }

    fclose(f);
}

/*
 * 递归遍历。三条确定性 / 安全约束都在这里：
 *
 *   1. `scandir + alphasort` 而不是 `readdir` 顺序 —— 同一次运行内
 *      readdir 的顺序都可能变，照抄它等于让同一棵树产出不同字节。
 *   2. `lstat` 而不是 `stat`，且只认 REG / DIR —— 符号链接一律不跟。
 *      跟链接会让"遍历一棵树"变成"遍历一张可能有环的图"，而环在
 *      文件系统里是合法状态（`a -> b -> a`）。这是防环第一道，
 *      max_depth 是第二道。
 *   3. 跳过以 '.' 开头的名字 —— 包括 `.git`。这不是疏忽：搜索工作区
 *      不该把版本库内部对象当源码喂给模型（那里每个文件都是 zlib 压缩
 *      的二进制，命中率极高、信息量极低）。
 */
static void walk(Sink *s, const char *dir, const char *rel, uint32_t depth) {
    struct dirent **list = NULL;
    int n = scandir(dir, &list, NULL, alphasort);

    if (n < 0) return;

    for (int i = 0; i < n && !s->b.full; i++) {
        const char *name = list[i]->d_name;
        char full[FS_GREP_PATH_MAX + FS_GREP_PATH_MAX];
        char child[FS_GREP_PATH_MAX];
        struct stat st;
        int fn, cn;

        if (name[0] == '.') continue;

        fn = snprintf(full, sizeof full, "%s/%s", dir, name);
        cn = rel[0] != '\0'
           ? snprintf(child, sizeof child, "%s/%s", rel, name)
           : snprintf(child, sizeof child, "%s", name);
        if (fn < 0 || (size_t)fn >= sizeof full) continue;
        if (cn < 0 || (size_t)cn >= sizeof child) continue;
        if (lstat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth < s->max_depth) walk(s, full, child, depth + 1u);
        } else if (S_ISREG(st.st_mode)) {
            grep_file(s, full, child);
        }
    }

    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
}

cxf_result fs_grep_exec(const void *ctx, const char *args, uint32_t args_len,
                        char *out, uint32_t out_cap, uint32_t *out_len) {
    const FsCfg *cfg = (const FsCfg *)ctx;
    struct stat st;
    Sink s;
    char tail[64];
    int tn;

    if (out_len != NULL) *out_len = 0u;
    if (out == NULL || out_cap == 0u) return CXF_ERR_FULL;
    if (cfg == NULL || cfg->root == NULL) return CXF_ERR_RANGE;
    if (args == NULL || args_len == 0u) return CXF_ERR_FORMAT;

    /* root 打不开是**响亮失败**，不是空结果。空结果会被上层读成
     * "工作区里没有这个东西"，那是一个假否定 —— 比报错难查得多。 */
    if (lstat(cfg->root, &st) != 0 || !S_ISDIR(st.st_mode)) return CXF_ERR_IO;

    s = (Sink){
        { out, out_cap, 0u, false },
        0u, 0u,
        cfg->max_hits  != 0u ? cfg->max_hits  : FS_GREP_MAX_HITS,
        cfg->max_depth != 0u ? cfg->max_depth : FS_GREP_MAX_DEPTH,
        args, args_len
    };

    walk(&s, cfg->root, "", 0u);

    /* 装不下：返回 FULL 让执行层翻倍重跑。这条路径安全的前提是本实现
     * 幂等（只读 + 顺序固定），而幂等的证明在测试里（同一语料两次
     * 不同起始容量 ⇒ 逐字节相同）。 */
    if (s.b.full) return CXF_ERR_FULL;

    /* 被 max_hits 截断：**这不是错误**，所以不能借 FULL 报 ——
     * 借了会让执行层为一个语义边界白烧 5 次翻倍重跑。
     * 界写进输出，模型与测试都能看见。 */
    if (s.total > s.hits) {
        tn = snprintf(tail, sizeof tail, "#truncated %u/%u\n", s.hits, s.total);
        if (tn > 0 && (size_t)tn < sizeof tail) {
            if (!sink_put(&s, tail, (uint32_t)tn)) return CXF_ERR_FULL;
        }
    }

    if (out_len != NULL) *out_len = s.b.len;
    return CXF_OK;
}

/* ---- fs.write_new ---- */

/*
 * args 切成 path / content：第一个 '\n' 之前是路径，之后是内容。
 *
 * 为什么用换行而不是空格分隔：路径里的空格比内容里的换行常见得多，而
 * "内容可以有多行"正是这个能力存在的理由（它要写文件）。选分隔符的标准
 * 不是好看，是**哪一种字符在两侧的出现频率更低**。
 *
 * 没有 '\n' 时内容为空 —— 这是确定的解读（建空文件），不是猜。反过来
 * "没有分隔符就报错"会把"建空文件"变成无法表达的事，而线格式那个"去掉
 * 一个尾随换行"的归一化（§4.11）恰好让它变得常见。
 */
/* 方言 `path\ncontent` 的切分在 `agent_args.h`（五个能力共用，含 http.post）。
 * 放头里是因为它是**定义**不是逻辑：唯一的分歧点只在"没有换行时 body 指向哪"，
 * 而那正是最容易被抄错、且抄错不会报错的地方。 */

/*
 * 路径形状。**这不是安全边界** —— 越界由下面的 realpath 判据管，这里只判
 * "这个能力不接受什么形状"，让错误尽早落在 RANGE 而不是等一个奇怪的 IO。
 *
 * 查 NUL 是唯一一处"不查就会静默"的地方：args 是一段**字节**，不是 C 串。
 * 若路径里混进 '\0'，`strlen` 会在那里停住，我们就会拿着一个"合法但更短"的
 * 路径去做边界判定、再按同一个更短的路径去 open —— 判的和写的虽然是同一个，
 * 但它已经不是调用方给的那一个了。宁可拒。
 */
static bool path_shape_ok(const char *p, uint32_t n) {
    if (n == 0u || n > FS_WRITE_PATH_MAX) return false;
    if (p[0] == '/') return false;   /* 只收相对路径：绝对路径没有"根之内"可言 */
    if (p[n - 1u] == '/') return false;   /* 以 '/' 结尾 = 想建目录，不是这个能力的事 */
    for (uint32_t i = 0; i < n; i++)
        if ((unsigned char)p[i] == 0u) return false;
    return true;
}

/* `p` 是否落在 `root` 之内。判据是**组件边界**，不是字符串前缀：
 * `/tmp/work2` 的前缀是 `/tmp/work`，但它不在 `/tmp/work` 之内。 */
static bool within(const char *p, const char *root, size_t rl) {
    if (rl == 1u && root[0] == '/') return p[0] == '/';
    if (strncmp(p, root, rl) != 0) return false;
    return p[rl] == '\0' || p[rl] == '/';
}

/*
 * 把 payload 里的相对路径解析成"根之内的绝对路径"。**L2 参数准入只有这一份实现。**
 *
 * 为什么必须抽出共用：`fs.diff` 与 `fs.write_new` / `fs.write_apply` 判的是
 * 同一条边界。两份实现对"符号链接 + `..` + 大小写不敏感文件系统"的处理迟早
 * 不同步，而不同步的表现是**边界只存在于其中一份里** —— 另一半成了越权通道，
 * 且代码看起来完全正常。
 *
 * 返回：
 *   RANGE  路径形状不合契约 / 父目录解出来在根外 / 末段是 '.' '..'
 *   IO     根打不开 / 父目录不存在（**这个族不建目录**，那是另一个自由度）
 *
 * 只做解析，不碰目标本身：目标存不存在、是不是普通文件，由调用方按自己的
 * 语义决定（`write_new` 要 EEXIST⇒DONE，`diff` 要 absent⇒可新建）。
 */
static cxf_result resolve_in_root(const char *root, const char *path, uint32_t plen,
                                  char *full, size_t full_cap) {
    char root_real[PATH_MAX];
    char dir_real[PATH_MAX];
    char cand[PATH_MAX + FS_WRITE_PATH_MAX];
    char parent[FS_WRITE_PATH_MAX + 1u];
    char leaf[FS_WRITE_PATH_MAX + 1u];
    uint32_t sepp, leaf_len;
    int tn;

    if (root == NULL) return CXF_ERR_RANGE;
    if (!path_shape_ok(path, plen)) return CXF_ERR_RANGE;

    /*
     * 父目录 / 末段：只在**最后一个** '/' 处切。
     * 逐段自己解 '..' 会让 realpath 有第二份实现 —— 而 realpath 是 `../` 与
     * 符号链接这两种越界走法的**共同终点**，绕过它就等于绕过边界。
     */
    sepp = plen;
    for (uint32_t i = plen; i-- > 0;)
        if (path[i] == '/') { sepp = i; break; }

    leaf_len = plen - (sepp < plen ? sepp + 1u : 0u);
    if (leaf_len == 0u || leaf_len > FS_WRITE_PATH_MAX) return CXF_ERR_RANGE;
    memcpy(leaf, path + (sepp < plen ? sepp + 1u : 0u), leaf_len);
    leaf[leaf_len] = '\0';

    /* 末段是 '.' / '..' 时 open 会打到父目录或更上层 —— 那不是"这个路径" */
    if ((leaf_len == 1u && leaf[0] == '.') ||
        (leaf_len == 2u && leaf[0] == '.' && leaf[1] == '.'))
        return CXF_ERR_RANGE;

    /* 没有 '/' 时 `sepp == plen`：路径只有一段，父目录就是根本身。
     * （这一格最初写成 `sepp > 0` 就拷贝，于是 "a.txt" 被当成父目录
     *   ⇒ realpath 失败 ⇒ 一切都变成 IO。边界条件与"有没有斜杠"是同一件事。） */
    if (sepp > 0u && sepp < plen) {
        if (sepp > FS_WRITE_PATH_MAX) return CXF_ERR_RANGE;
        memcpy(parent, path, sepp);
        parent[sepp] = '\0';
    } else {
        parent[0] = '\0';
    }

    /* 根打不开 = 部署事实坏了：响亮失败，不是"少看/少写一点" */
    if (realpath(root, root_real) == NULL) return CXF_ERR_IO;

    tn = parent[0] == '\0'
       ? snprintf(cand, sizeof cand, "%s", root_real)
       : snprintf(cand, sizeof cand, "%s/%s", root_real, parent);
    if (tn < 0 || (size_t)tn >= sizeof cand) return CXF_ERR_RANGE;

    /* 父目录必须**存在**：这一族能力不建目录（那是另一个自由度，另一刀） */
    if (realpath(cand, dir_real) == NULL) return CXF_ERR_IO;

    /* ★ L2 参数准入的唯一一行谓词 */
    if (!within(dir_real, root_real, strlen(root_real))) return CXF_ERR_RANGE;

    tn = snprintf(full, full_cap, "%s/%s", dir_real, leaf);
    if (tn < 0 || (size_t)tn >= full_cap) return CXF_ERR_RANGE;
    return CXF_OK;
}

cxf_result fs_write_new_exec(const void *ctx, const char *args, uint32_t args_len,
                             char *out, uint32_t out_cap, uint32_t *out_len) {
    const FsRootCfg *cfg = (const FsRootCfg *)ctx;
    char full[PATH_MAX + FS_WRITE_PATH_MAX];
    const char *path, *body;
    uint32_t plen = 0u, blen = 0u, nl;
    cxf_result rc;
    struct stat st;
    int fd = -1, tn;

    if (out_len != NULL) *out_len = 0u;
    if (out == NULL || out_cap == 0u) return CXF_ERR_FULL;
    if (cfg == NULL || cfg->root == NULL) return CXF_ERR_RANGE;
    if (args == NULL || args_len == 0u) return CXF_ERR_FORMAT;

    split_args(args, args_len, &path, &plen, &body, &blen);
    if (plen == 0u || plen == args_len) return CXF_ERR_FORMAT;

    /*
     * ★ **先判容量，再动世界。**
     *
     * 收据长度有确定上界（`FS_WRITE_OUT_MAX`），所以"报得出吗"这件事在碰文件
     * 系统之前就有答案。反过来的顺序（先写盘、再发现装不下）会让一次**成功**
     * 被一次容量不足撤销：要么 unlink 掉刚写的文件（把成功变成没发生），
     * 要么返回 FULL 但文件已经在那儿（重跑时会看到 `exists`，于是"新建"这个
     * 事实永远说不出口）。
     *
     * 这是写能力与读能力最本质的一处不同：读能力的 FULL 只是"再多给我点地方"，
     * 写能力的 FULL 若在副作用之后才判，就变成了"要么白做、要么说谎"。
     */
    if (out_cap < FS_WRITE_OUT_MAX) return CXF_ERR_FULL;

    /* 边界与解析只有一份实现（resolve_in_root）；这里只关心"目标本身怎么了" */
    rc = resolve_in_root(cfg->root, path, plen, full, sizeof full);
    if (rc != CXF_OK) return rc;

    /*
     * ★ 这一段是"可逆"和"幂等"的实施本体。
     *
     * `O_EXCL` 有两重作用：让"这路径是本次创建的"成为**原子事实**（不需要
     * 先 stat 再 open，那之间有窗口），以及在目标是符号链接时**拒绝跟随**
     * （POSIX 保证，包括悬空链接）—— 于是"写到我以为的文件里"这件事在
     * 这一层不可能发生。
     */
    fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno != EEXIST) return CXF_ERR_IO;

        /*
         * 已经有人在这儿放了东西。**这不是失败** —— 它是"这一步之前已经
         * 做完了"。这正是 A1 要表达的那条语义（`EEXIST ⇒ DONE`），而它
         * 在这里不需要框架任何配合：返回 CXF_OK 就够了（§4.18.6）。
         *
         * 但只有**普通文件**才算"做完了"。目录 / 符号链接 / 设备节点都不是
         * 这个能力会创建的东西，报成功会让模型以为文件在那儿 —— 而"以为
         * 在那儿"比"没写"更贵：它会把后续步骤全部建立在一个不存在的前提上。
         *
         * 不比较内容：判"内容是否相同"要读盘，而读进来之后要回答的问题是
         * "该不该覆盖" —— 那是 `fs.write` + dry-run 的事，不塞进这里。
         */
        if (lstat(full, &st) != 0) return CXF_ERR_IO;
        if (!S_ISREG(st.st_mode)) return CXF_ERR_RANGE;

        tn = snprintf(out, out_cap, "exists %lld\n", (long long)st.st_size);
        if (tn < 0 || (uint32_t)tn >= out_cap) return CXF_ERR_FULL;
        if (out_len != NULL) *out_len = (uint32_t)tn;
        return CXF_OK;
    }

    for (uint32_t off = 0u; off < blen;) {
        ssize_t wr = write(fd, body + off, (size_t)(blen - off));

        if (wr < 0) {
            if (errno == EINTR) continue;
            goto fail;
        }
        if (wr == 0) goto fail;     /* write 返回 0 不是"写完"，是"没写进去" */
        off += (uint32_t)wr;
    }

    /* close 也会失败（配额 / NFS）—— 它不报，就等于把一次半写当成功 */
    if (close(fd) != 0) { fd = -1; goto fail; }
    fd = -1;

    tn = snprintf(out, out_cap, "new %u\n", blen);
    /* 不可达：上面已经判过 out_cap >= FS_WRITE_OUT_MAX。留着是因为 snprintf
     * 的返回值必须被读一次 —— 而这里**不能** goto fail：文件已经写完，
     * unlink 等于把一次成功撤销掉。 */
    if (tn < 0 || (uint32_t)tn >= out_cap) return CXF_ERR_FULL;
    nl = (uint32_t)tn;
    if (out_len != NULL) *out_len = nl;
    return CXF_OK;

fail:
    /*
     * 写不全就把刚建的文件删掉：**路径存在 ⟺ 内容完整**。
     *
     * 这是"可逆"真正的实施 —— 不是"允许重跑"，而是"失败不留痕迹"。留下的
     * 半截文件是不可逆的：逆操作（unlink）虽然还在，但没有任何信息说明它是
     * 半截，于是重跑会看到 `exists` 并把它当成"已经做完了"。
     *
     * unlink 也可能失败（目录被并发删掉 / 权限变了），那时会残留一个半截
     * 文件。这仍然**响亮**（返回 IO），只是那一层不归这部分代码管（L5）。
     */
    if (fd >= 0) close(fd);
    unlink(full);
    return CXF_ERR_IO;
}

/* ==== fs.diff ==== */

/*
 * 为什么这一节必须做行对齐（LCS），不能逐行比较：
 *
 * 逐行比较在**插入一行**之后会让后面每一行都变成"既删又加"—— 200 行的文件
 * 只改了一行，输出却是 400 行。那不是差异，那是把两份文件都打印一遍。
 * 而这份输出的读者是模型，它要回答的问题是"我这一改动了什么"。
 *
 * 代价是 O(oln×nln) 的表。界由 FS_DIFF_MAX_LINES 压住 —— 那个界不是性能
 * 保护，是**注意力保护**：256 行还改不清楚的文件，该分块改，不该一次塞进上下文。
 */

/* 一行在缓冲区里的位置。**只记偏移不复制**：新旧内容都已各自在内存里。 */
typedef struct DLine {
    uint32_t off;
    uint32_t len;
} DLine;

/*
 * 切行。`cap` 只是**写入**上限，所以超出时仍然返回真实行数 ——
 * 低报的行数比没有更坏：它会让调用方得出"规模在界内"的结论。
 *
 * 规则：'\n' 是**分隔符**而不是行内容。于是"末尾少一个换行"表现成
 * "所有行都相同、字节数不同"，那恰好是 #eol 要说的那件事。
 */
static uint32_t split_lines(const char *s, uint32_t n, DLine *ln, uint32_t cap) {
    uint32_t k = 0u, start = 0u;

    for (uint32_t i = 0u; i < n; i++) {
        if (s[i] != '\n') continue;
        if (k < cap) { ln[k].off = start; ln[k].len = i - start; }
        k++;
        start = i + 1u;
    }
    if (start < n) {                    /* 末行没有以换行结尾 */
        if (k < cap) { ln[k].off = start; ln[k].len = n - start; }
        k++;
    }
    return k;
}

static bool line_eq(const char *a, const DLine *x, const char *b, const DLine *y) {
    return x->len == y->len
        && (x->len == 0u || memcmp(a + x->off, b + y->off, x->len) == 0);
}

/* 编辑脚本的一格。`idx` 是旧行号（EQ / DEL）或新行号（INS）。 */
typedef struct DOp {
    uint8_t  k;
    uint32_t idx;
} DOp;

enum { DO_EQ = 0u, DO_DEL = 1u, DO_INS = 2u };

#define DIFF_OPS_MAX (2u * FS_DIFF_MAX_LINES)

/*
 * LCS 表按**后缀**分配：tab[i*nln+j] = LCS(old[i..), new[j..))，不含第 0 行/列。
 *
 * 那两行两列在数学上恒为 0，为它们各留一格会让 256×256 变成 257×257 = 66049，
 * 而"单元上限"与"行数上限"就会变成两个**看似独立、实则可以互相矛盾**的数。
 * 现在它只是一个恒等式：表大小 = oln×nln ≤ MAX_LINES²，所以 .h 里
 * FS_DIFF_MAX_CELLS 就是 MAX_LINES 的平方，不是一个可以单独调的参数。
 */
static uint32_t lcs_at(const uint16_t *tab, uint32_t oln, uint32_t nln,
                       uint32_t i, uint32_t j) {
    return (i < oln && j < nln) ? tab[(size_t)i * nln + j] : 0u;
}

/* 从 LCS 表回溯出编辑脚本。**从前往后**走，于是 ops 天然按行序排好 ——
 * 输出是流式的，若反向回溯就得先把脚本倒排一遍，那一步没有语义。 */
static void diff_ops_build(const char *op, const DLine *oldl, uint32_t oln,
                           const char *np, const DLine *newl, uint32_t nln,
                           uint16_t *tab, DOp *ops, uint32_t *nops) {
    uint32_t i, j, k;

    for (i = oln; i-- > 0u;)
        for (j = nln; j-- > 0u;) {
            if (line_eq(op, &oldl[i], np, &newl[j]))
                tab[(size_t)i * nln + j] =
                    (uint16_t)(1u + lcs_at(tab, oln, nln, i + 1u, j + 1u));
            else {
                uint32_t d = lcs_at(tab, oln, nln, i + 1u, j);
                uint32_t r = lcs_at(tab, oln, nln, i, j + 1u);
                tab[(size_t)i * nln + j] = (uint16_t)(d >= r ? d : r);
            }
        }

    i = 0u; j = 0u; k = 0u;
    while (i < oln && j < nln) {
        if (line_eq(op, &oldl[i], np, &newl[j])) {
            ops[k++] = (DOp){ DO_EQ, i };
            i++; j++;
        } else if (lcs_at(tab, oln, nln, i + 1u, j) >= lcs_at(tab, oln, nln, i, j + 1u)) {
            ops[k++] = (DOp){ DO_DEL, i };
            i++;
        } else {
            ops[k++] = (DOp){ DO_INS, j };
            j++;
        }
    }
    while (i < oln) { ops[k++] = (DOp){ DO_DEL, i }; i++; }
    while (j < nln) { ops[k++] = (DOp){ DO_INS, j }; j++; }
    *nops = k;
}

/* 变更段：编辑脚本里一串连续的非 EQ。删与增在回溯里天然相邻，不必再分。 */
typedef struct DRun {
    uint32_t s, e;    /* ops 区间 [s, e) */
} DRun;

/* 块：相邻的两段若"上下文窗口相接"就并成一块（否则会出现两个块共用同一批
 * 上下文行，读起来像重复）*/
typedef struct DHunk {
    uint32_t first, last;   /* runs 下标，闭区间 */
} DHunk;

typedef struct DDiff {
    const char     *op;
    const DLine    *oldl;
    uint32_t        oln;
    const char     *np;
    const DLine    *newl;
    uint32_t        nln;
    const DOp      *ops;
    uint32_t        nops;
    const uint32_t *opos;    /* opos[k] = ops[0..k) 消耗的旧行数 */
    const uint32_t *npos;
    uint32_t        oldb, newb;
} DDiff;

/*
 * 正式输出：头 + 块 + 可有可无的标记。
 *
 * 三种"没写全"必须分开报，与 §3 第 20 条同一条理由：
 *   #eol        没有差异，但字节不同（行尾）—— 不是错误，是结论
 *   #omitted k  块数超上限 —— 语义边界，扩容没用
 *   CXF_ERR_FULL 容量不足 —— 语义是"再给一倍"
 * 把后两者混成一种，会让执行层为一个语义边界白烧 5 次翻倍重跑。
 */
static void diff_emit(OutBuf *b, const DDiff *d, uint32_t *nhunks) {
    DRun  runs[DIFF_OPS_MAX];
    DHunk hk[FS_DIFF_HUNK_MAX];
    uint32_t nruns = 0u, nh = 0u, i = 0u;
    char  head[96], hhead[48], tail[48];
    int   tn;

    /* 1. 收段 */
    while (i < d->nops) {
        if (d->ops[i].k == DO_EQ) { i++; continue; }
        {
            uint32_t s = i;

            while (i < d->nops && d->ops[i].k != DO_EQ) i++;
            runs[nruns++] = (DRun){ s, i };
        }
    }

    tn = snprintf(head, sizeof head, "diff %u %u +%u -%u\n",
                  d->oldb, d->newb, d->nln, d->oln);
    if (tn < 0 || (size_t)tn >= sizeof head) { b->full = true; return; }
    if (!buf_put(b, head, (uint32_t)tn)) return;

    /* 行全部对齐、字节却不同 ⇒ 差别只可能在行尾（末尾换行 / 空行）。
     * 这时**不输出空块**：一个没有内容的 @@ 头是噪声，而 #eol 本身已是结论。 */
    if (nruns == 0u) { buf_put(b, "#eol\n", 5u); return; }

    /* 2. 分组。计数与落块分成两步，因为 #omitted 的数字要在所有块数完之后
     *    才知道 —— 而输出是流式的，不能回头改。 */
    for (uint32_t r = 0u; r < nruns;) {
        uint32_t last = r;

        while (last + 1u < nruns
               && runs[last + 1u].s - runs[last].e <= 2u * FS_DIFF_CTX)
            last++;
        if (nh < FS_DIFF_HUNK_MAX) hk[nh] = (DHunk){ r, last };
        nh++;
        r = last + 1u;
    }
    *nhunks = nh;

    /* 3. 出块 */
    for (uint32_t h = 0u; h < nh && h < FS_DIFF_HUNK_MAX; h++) {
        uint32_t os = runs[hk[h].first].s;
        uint32_t oe = runs[hk[h].last].e;
        uint32_t back = 0u, fwd = 0u;

        while (os > 0u && d->ops[os - 1u].k == DO_EQ && back < FS_DIFF_CTX) { os--; back++; }
        while (oe < d->nops && d->ops[oe].k == DO_EQ && fwd < FS_DIFF_CTX) { oe++; fwd++; }

        tn = snprintf(hhead, sizeof hhead, "@@ -%u +%u @@\n",
                      d->opos[os] + 1u, d->npos[os] + 1u);
        if (tn < 0 || (size_t)tn >= sizeof hhead) { b->full = true; return; }
        if (!buf_put(b, hhead, (uint32_t)tn)) return;

        for (uint32_t k = os; k < oe; k++) {
            const DOp   *o   = &d->ops[k];
            const char  *src = o->k == DO_INS ? d->np : d->op;
            const DLine *L   = o->k == DO_INS ? &d->newl[o->idx] : &d->oldl[o->idx];
            char pre = o->k == DO_EQ ? ' ' : (o->k == DO_DEL ? '-' : '+');

            if (!buf_put(b, &pre, 1u)) return;
            if (!buf_put(b, src + L->off, L->len)) return;
            if (!buf_put(b, "\n", 1u)) return;
        }
    }

    if (nh > FS_DIFF_HUNK_MAX) {
        tn = snprintf(tail, sizeof tail, "#omitted %u\n", nh - FS_DIFF_HUNK_MAX);
        if (tn < 0 || (size_t)tn >= sizeof tail) { b->full = true; return; }
        buf_put(b, tail, (uint32_t)tn);
    }
}

/*
 * handler。args = `path\ncontent`，与 `fs.write_new` 同一个方言。
 *
 * 它**不写盘**，所以"副作用原子性"对它空转 —— 这正是它能先于
 * `fs.write_apply` 落地的真实理由（组合风险归部署侧，不在这一层）。
 *
 * 三种错分开报（§3 第 20 条）：
 *   FORMAT  没有路径 —— 重试没用
 *   RANGE   路径形状不合契约 / 越界 / 目标是目录或符号链接 —— 设计边界
 *   IO      父目录不存在 / 打不开 / 读失败 —— 环境事实
 *   FULL    输出装不下 —— 语义是"再给一倍"，而本实现是纯函数，重跑安全
 */
cxf_result fs_diff_exec(const void *ctx, const char *args, uint32_t args_len,
                        char *out, uint32_t out_cap, uint32_t *out_len) {
    const FsRootCfg *cfg = (const FsRootCfg *)ctx;
    char        full[PATH_MAX + FS_WRITE_PATH_MAX];
    char        head[64];
    DLine       oldl[FS_DIFF_MAX_LINES], newl[FS_DIFF_MAX_LINES];
    DOp         ops[DIFF_OPS_MAX];
    uint32_t    opos[DIFF_OPS_MAX + 1u], npos[DIFF_OPS_MAX + 1u];
    uint16_t   *tab = NULL;
    char       *obuf = NULL;
    OutBuf      b;
    DDiff       d;
    const char *path, *body;
    uint32_t    plen = 0u, blen = 0u, oln = 0u, nln = 0u, nops = 0u;
    uint32_t    oldb = 0u, nh = 0u;
    cxf_result  rc;
    struct stat st;
    int         fd = -1, tn;

    if (out_len != NULL) *out_len = 0u;
    if (out == NULL || out_cap == 0u) return CXF_ERR_FULL;
    if (cfg == NULL || cfg->root == NULL) return CXF_ERR_RANGE;
    if (args == NULL || args_len == 0u) return CXF_ERR_FORMAT;

    split_args(args, args_len, &path, &plen, &body, &blen);
    if (plen == 0u) return CXF_ERR_FORMAT;

    rc = resolve_in_root(cfg->root, path, plen, full, sizeof full);
    if (rc != CXF_OK) return rc;

    b = (OutBuf){ out, out_cap, 0u, false };
    nln = split_lines(body, blen, newl, FS_DIFF_MAX_LINES);

    if (lstat(full, &st) != 0) {
        if (errno != ENOENT) return CXF_ERR_IO;

        /* 不存在不是错误，是 dry-run 的答案："应用会新建这么多"。
         * 与 `absent` 分开命名而不是报 `diff 0 <n>`，因为这两种结局对
         * 调用方的意思完全不同：一个要新建、一个要覆盖。 */
        tn = snprintf(head, sizeof head, "absent %u %u\n", blen, nln);
        if (tn < 0 || (size_t)tn >= sizeof head) return CXF_ERR_RANGE;
        if (!buf_put(&b, head, (uint32_t)tn)) return CXF_ERR_FULL;
        if (out_len != NULL) *out_len = b.len;
        return CXF_OK;
    }
    if (!S_ISREG(st.st_mode)) return CXF_ERR_RANGE;   /* 目录 / 链接 / 设备 */

    fd = open(full, O_RDONLY);
    if (fd < 0) return CXF_ERR_IO;

    /* 多读一个字节就能区分"恰好到上限"与"比上限大"，而不必信 st.st_size
     * （读之前它可能已经变了）。 */
    obuf = (char *)malloc((size_t)FS_DIFF_MAX_BYTES + 1u);
    if (obuf == NULL) { close(fd); return CXF_ERR_NOMEM; }

    for (;;) {
        ssize_t r;

        if (oldb > FS_DIFF_MAX_BYTES) break;
        r = read(fd, obuf + oldb, (size_t)(FS_DIFF_MAX_BYTES + 1u - oldb));
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd); free(obuf);
            return CXF_ERR_IO;
        }
        if (r == 0) break;
        oldb += (uint32_t)r;
    }
    close(fd);

    /*
     * 规模超界。**只报能对得上的数**：
     *   新侧行数已知（body 就在手里），旧侧行数未知（没读完就不可能知道），
     *   所以旧侧写 `-?` 而不是 `-0` —— 零是一个会让人得出错误结论的谎
     *   （"旧文件是空的"与"旧文件太大"是相反的判断）。
     */
    if (oldb > FS_DIFF_MAX_BYTES) {
        uint32_t sz = (uint32_t)st.st_size;

        tn = snprintf(head, sizeof head, "diff %u %u +%u -?\n", sz, blen, nln);
        if (tn < 0 || (size_t)tn >= sizeof head) { free(obuf); return CXF_ERR_RANGE; }
        if (!buf_put(&b, head, (uint32_t)tn)) { free(obuf); return CXF_ERR_FULL; }
        tn = snprintf(head, sizeof head, "#coarse bytes %u/%u\n",
                      sz, FS_DIFF_MAX_BYTES);
        if (tn < 0 || (size_t)tn >= sizeof head) { free(obuf); return CXF_ERR_RANGE; }
        if (!buf_put(&b, head, (uint32_t)tn)) { free(obuf); return CXF_ERR_FULL; }
        if (out_len != NULL) *out_len = b.len;
        free(obuf);
        return CXF_OK;
    }

    oln = split_lines(obuf, oldb, oldl, FS_DIFF_MAX_LINES);
    if (oln > FS_DIFF_MAX_LINES || nln > FS_DIFF_MAX_LINES) {
        /*
         * 头里的计数是**两个文件的事实**，不是"我们比了多少" —— 所以照实写
         * 300，不截成 256。截成上限就是低报：300 行与 256 行对调用方是
         * 两种不同的处置（分几块问），而"我们只看了 256"这件事由下一行的
         * 分母说清楚。这是"低报的计数器比没有更坏"在本节的第二处应用
         * （第一处是 `-?` 而不是 `-0`）。
         */
        tn = snprintf(head, sizeof head, "diff %u %u +%u -%u\n", oldb, blen, nln, oln);
        if (tn < 0 || (size_t)tn >= sizeof head) { free(obuf); return CXF_ERR_RANGE; }
        if (!buf_put(&b, head, (uint32_t)tn)) { free(obuf); return CXF_ERR_FULL; }
        tn = snprintf(head, sizeof head, "#coarse lines %u %u/%u\n",
                      oln, nln, FS_DIFF_MAX_LINES);
        if (tn < 0 || (size_t)tn >= sizeof head) { free(obuf); return CXF_ERR_RANGE; }
        if (!buf_put(&b, head, (uint32_t)tn)) { free(obuf); return CXF_ERR_FULL; }
        if (out_len != NULL) *out_len = b.len;
        free(obuf);
        return CXF_OK;
    }

    /* 逐字节相同：这是最短也最常被走到的路径，所以放在做任何对齐之前 */
    if (oldb == blen && (blen == 0u || memcmp(obuf, body, blen) == 0)) {
        free(obuf);
        if (!buf_put(&b, "same\n", 5u)) return CXF_ERR_FULL;
        if (out_len != NULL) *out_len = b.len;
        return CXF_OK;
    }

    if (oln != 0u && nln != 0u) {
        size_t cells = (size_t)oln * nln;

        tab = (uint16_t *)malloc(cells * sizeof *tab);
        if (tab == NULL) { free(obuf); return CXF_ERR_NOMEM; }
    }

    diff_ops_build(obuf, oldl, oln, body, newl, nln, tab, ops, &nops);
    free(tab);

    opos[0] = 0u;
    npos[0] = 0u;
    for (uint32_t k = 0u; k < nops; k++) {
        opos[k + 1u] = opos[k] + (ops[k].k != DO_INS ? 1u : 0u);
        npos[k + 1u] = npos[k] + (ops[k].k != DO_DEL ? 1u : 0u);
    }

    d = (DDiff){ obuf, oldl, oln, body, newl, nln, ops, nops, opos, npos, oldb, blen };
    diff_emit(&b, &d, &nh);
    free(obuf);

    if (b.full) return CXF_ERR_FULL;
    if (out_len != NULL) *out_len = b.len;
    return CXF_OK;
}

/* ==== fs.write_apply ==== */

/*
 * 内容是否已经等于 body。
 *
 * **不需要把文件读进内存**：要比较多少字节是已知的（blen），所以按块比就行，
 * 零分配。这也是这条实现不需要 FS_DIFF_MAX_BYTES 那类上限的原因 —— 它的
 * 工作量由 payload 决定，不由文件决定。
 *
 * 返回 1 相同 / 0 不同 / -1 读失败。调用方已判 `size == blen`（那是快速否决），
 * 这里再读一个字节确认文件**真的到此为止** —— 否则 lstat 与 read 之间长出来的
 * 那截会被漏看，而我们报的 `same` 就成了假的。
 */
static int content_eq(const char *full, const char *body, uint32_t blen) {
    char buf[4096];
    uint32_t off = 0u;
    int fd = open(full, O_RDONLY);

    if (fd < 0) return -1;

    while (off < blen) {
        uint32_t want = blen - off;
        ssize_t r;

        if (want > (uint32_t)sizeof buf) want = (uint32_t)sizeof buf;
        r = read(fd, buf, (size_t)want);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (r == 0) { close(fd); return 0; }        /* 短读 ⇒ 不同 */
        if (memcmp(buf, body + off, (size_t)r) != 0) { close(fd); return 0; }
        off += (uint32_t)r;
    }

    {
        char extra;
        ssize_t r;

        do { r = read(fd, &extra, 1u); } while (r < 0 && errno == EINTR);
        close(fd);
        if (r < 0) return -1;
        return r == 0 ? 1 : 0;      /* 还有余料 ⇒ 不同 */
    }
}

/*
 * 用"同目录临时文件 + rename"替换目标。`mode` 是替换后应有的权限位。
 *
 * ★ 为什么不是 `open(O_TRUNC)`：`O_TRUNC` 在写之前就毁了旧内容，于是
 *   `write` 失败会留下**半截新文件 + 旧内容已不存在**。而 `write_new`
 *   在同样的情况下可以 `unlink` —— 这里不行，unlink 只会让文件消失，
 *   不会把旧内容变回来。所以失败必须停在**旧文件完好**那一侧，而这只有
 *   在"新内容写进另一个名字、最后一步才换名"时成立。
 *
 * ★ 不做 fsync。rename 的原子性保证"看到旧的"或"看到新的"，不保证崩溃后
 *   新的还在（那是持久化语义，另一个议题）。所以边界要写清：**崩溃后可能
 *   退回旧内容，但不会得到半截内容。**
 *
 * 临时文件名以 '.' 开头，于是 `fs.grep` 的"跳过点文件"顺带让中间态
 * 不会出现在检索结果里。`mkstemp` 负责唯一性 —— 这比"自己拼 pid + 计数器"
 * 少一个可变全局，而可变全局在这个项目里是明确禁止的。
 */
static cxf_result write_replace(const char *full, const char *body, uint32_t blen,
                                mode_t mode) {
    char   tmpl[PATH_MAX + FS_WRITE_PATH_MAX + 16u];
    const char *slash = strrchr(full, '/');
    size_t dl;
    int    fd, bad = 0;
    uint32_t off;

    if (slash == NULL) return CXF_ERR_RANGE;      /* resolve_in_root 永远给绝对路径 */
    dl = (size_t)(slash - full);
    if (dl + sizeof "/.cxf.XXXXXX" > sizeof tmpl) return CXF_ERR_RANGE;
    memcpy(tmpl, full, dl);
    memcpy(tmpl + dl, "/.cxf.XXXXXX", sizeof "/.cxf.XXXXXX");

    fd = mkstemp(tmpl);
    if (fd < 0) return CXF_ERR_IO;

    /* mkstemp 给 0600。覆盖时要保留旧文件的权限位 —— 否则一次覆盖会把
     * 0755 的脚本悄悄变成不可执行，而这件事不会出现在任何收据里。 */
    if (fchmod(fd, mode) != 0) { close(fd); unlink(tmpl); return CXF_ERR_IO; }

    for (off = 0u; off < blen;) {
        ssize_t wr = write(fd, body + off, (size_t)(blen - off));

        if (wr < 0) { if (errno == EINTR) continue; bad = 1; break; }
        if (wr == 0) { bad = 1; break; }
        off += (uint32_t)wr;
    }

    /* close 也会失败（配额 / NFS）。它不报就等于把一次半写当成成功 */
    if (!bad && close(fd) != 0) { fd = -1; bad = 2; }
    if (bad == 1) close(fd);

    if (!bad && rename(tmpl, full) != 0) bad = 3;

    if (bad) {
        unlink(tmpl);           /* 半截的新内容必须删掉，旧文件一格没动 */
        return CXF_ERR_IO;
    }
    return CXF_OK;
}

cxf_result fs_write_apply_exec(const void *ctx, const char *args, uint32_t args_len,
                               char *out, uint32_t out_cap, uint32_t *out_len) {
    const FsRootCfg *cfg = (const FsRootCfg *)ctx;
    char        full[PATH_MAX + FS_WRITE_PATH_MAX];
    const char *path, *body;
    uint32_t    plen = 0u, blen = 0u;
    cxf_result  rc;
    struct stat st;
    mode_t      mode = 0644;
    int         tn;

    if (out_len != NULL) *out_len = 0u;
    if (out == NULL || out_cap == 0u) return CXF_ERR_FULL;
    if (cfg == NULL || cfg->root == NULL) return CXF_ERR_RANGE;
    if (args == NULL || args_len == 0u) return CXF_ERR_FORMAT;

    split_args(args, args_len, &path, &plen, &body, &blen);
    /* Missing content is not consent to erase. Explicit path + newline still
     * represents an intentionally empty file. The local 0.6B trial hit this. */
    if (plen == 0u || plen == args_len) return CXF_ERR_FORMAT;

    /* 与 write_new 同一条：**先判容量，再动世界**。这里若不判，覆盖已经发生、
     * 旧内容已经毁掉，然后才发现报不出收据 —— 那就只剩"撤销一次成功"或
     * "说一次谎"两个选择（§4.18.6 第 1 条）。 */
    if (out_cap < FS_APPLY_OUT_MAX) return CXF_ERR_FULL;

    rc = resolve_in_root(cfg->root, path, plen, full, sizeof full);
    if (rc != CXF_OK) return rc;

    if (lstat(full, &st) == 0) {
        if (!S_ISREG(st.st_mode)) return CXF_ERR_RANGE;   /* 目录 / 链接 / 设备 */
        mode = st.st_mode & 07777;

        /*
         * 内容幂等：一样就**真的不动**。
         *
         * 便宜否决在前（大小不等 ⇒ 不可能相同），所以这一格对大文件也只用
         * 一次 lstat；真的可能相同才读，而读的量由 payload 决定。
         *
         * 收益不是省一次写：`same` 让"我改过了"这件事有据可查，也让模型
         * 重复调用同一个动作时不会得到第二个 `wrote`（那会被读成"改了两次"）。
         */
        if (st.st_size >= 0 && (uint64_t)st.st_size == (uint64_t)blen) {
            int eq = content_eq(full, body, blen);

            if (eq < 0) return CXF_ERR_IO;
            if (eq == 1) {
                tn = snprintf(out, out_cap, "same %u\n", blen);
                if (tn < 0 || (uint32_t)tn >= out_cap) return CXF_ERR_FULL;
                if (out_len != NULL) *out_len = (uint32_t)tn;
                return CXF_OK;
            }
        }
    } else if (errno != ENOENT) {
        return CXF_ERR_IO;
    }

    rc = write_replace(full, body, blen, mode);
    if (rc != CXF_OK) return rc;

    /* 收据里不区分"新建"与"覆盖"：这个能力说的是"使它等于这些字节"，
     * 之前是什么由 `fs.diff` 回答（它已经答过）。 */
    tn = snprintf(out, out_cap, "wrote %u\n", blen);
    if (tn < 0 || (uint32_t)tn >= out_cap) return CXF_ERR_FULL;   /* 不可达：上面判过 */
    if (out_len != NULL) *out_len = (uint32_t)tn;
    return CXF_OK;
}

/*
 * 文件末尾的 blen 字节是否**恰好**等于 body。
 *
 * 与 `content_eq` 长得像，但比的东西不同：那个问"整个文件是否等于 body"
 * （所以读完还要再读一个字节确认文件到此为止），这个问"最后 blen 字节是否
 * 等于 body"（起点由 `size - blen` 给出，末尾自然对齐，不需要确认 EOF）。
 * 抄错的表现是：文件比 body 长时永远判"相同"，于是追加一次都不生效。
 *
 * 调用方已保证 `size >= blen`（便宜的否决在前）。
 * 返回：1 相同 / 0 不同（含"文件比 lstat 说的短"，即被并发截短）/ -1 读失败
 */
static int tail_eq(const char *full, const char *body, uint32_t blen, off_t size) {
    char buf[4096];
    uint32_t off = 0u;
    int fd = open(full, O_RDONLY);

    if (fd < 0) return -1;
    if (lseek(fd, size - (off_t)blen, SEEK_SET) == (off_t)-1) { close(fd); return -1; }

    while (off < blen) {
        uint32_t want = blen - off;
        ssize_t r;

        if (want > (uint32_t)sizeof buf) want = (uint32_t)sizeof buf;
        r = read(fd, buf, (size_t)want);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (r == 0) { close(fd); return 0; }        /* 短读 ⇒ 不同 */
        if (memcmp(buf, body + off, (size_t)r) != 0) { close(fd); return 0; }
        off += (uint32_t)r;
    }

    close(fd);
    return 1;
}

cxf_result fs_append_exec(const void *ctx, const char *args, uint32_t args_len,
                          char *out, uint32_t out_cap, uint32_t *out_len) {
    const FsRootCfg *cfg = (const FsRootCfg *)ctx;
    char        full[PATH_MAX + FS_WRITE_PATH_MAX];
    const char *path, *body;
    uint32_t    plen = 0u, blen = 0u, done = 0u;
    cxf_result  rc;
    struct stat st;
    int         tn, fd;

    if (out_len != NULL) *out_len = 0u;
    if (out == NULL || out_cap == 0u) return CXF_ERR_FULL;
    if (cfg == NULL || cfg->root == NULL) return CXF_ERR_RANGE;
    if (args == NULL || args_len == 0u) return CXF_ERR_FORMAT;

    split_args(args, args_len, &path, &plen, &body, &blen);
    if (plen == 0u || plen == args_len) return CXF_ERR_FORMAT;

    /* 先判容量，再动世界（与 write_new / apply 同一条）：追加完才发现收据
     * 装不下，就只剩"说谎"或"多写一次"两个选择。 */
    if (out_cap < FS_APPEND_OUT_MAX) return CXF_ERR_FULL;

    rc = resolve_in_root(cfg->root, path, plen, full, sizeof full);
    if (rc != CXF_OK) return rc;

    if (lstat(full, &st) != 0) {
        if (errno != ENOENT) return CXF_ERR_IO;

        /* 不存在是**事实**不是失败（与 fs.diff 的 absent 同一口径）。这个能力
         * 不带 O_CREAT，见头文件里"为什么不创建"的两条理由。 */
        tn = snprintf(out, out_cap, "absent\n");
        if (tn < 0 || (uint32_t)tn >= out_cap) return CXF_ERR_FULL;
        if (out_len != NULL) *out_len = (uint32_t)tn;
        return CXF_OK;
    }
    if (!S_ISREG(st.st_mode)) return CXF_ERR_RANGE;   /* 目录 / 链接 / 设备 */

    /*
     * 尾部分两步：**便宜的否决在前**。
     *   1. 追加 0 字节 ≡ 什么都不做 ⇒ `same`。不报 `wrote 0`：那会让重跑看起来
     *      "写了两次"，而事实是连写通道都没开。
     *   2. 长度不够 ⇒ 不可能是尾部（一次 lstat 就够，不必读盘）。
     *   3. 真的可能相同才读，读的量由 payload 决定上限。
     */
    if (blen == 0u) {
        tn = snprintf(out, out_cap, "same %lld\n", (long long)st.st_size);
        if (tn < 0 || (uint32_t)tn >= out_cap) return CXF_ERR_FULL;
        if (out_len != NULL) *out_len = (uint32_t)tn;
        return CXF_OK;
    }
    if ((uint64_t)blen <= (uint64_t)st.st_size) {
        int eq = tail_eq(full, body, blen, st.st_size);

        if (eq < 0) return CXF_ERR_IO;
        if (eq == 1) {
            /* ★ 本能力自证"我没做"的唯一凭据在此。模型重发的第二次落到这里，
             *   于是"我追加过了"有据可查，而文件一个字节没长。 */
            tn = snprintf(out, out_cap, "same %lld\n", (long long)st.st_size);
            if (tn < 0 || (uint32_t)tn >= out_cap) return CXF_ERR_FULL;
            if (out_len != NULL) *out_len = (uint32_t)tn;
            return CXF_OK;
        }
    }

    /*
     * 真的要写。**不带 O_CREAT**（不存在上面已经报 absent 走了），
     * `O_APPEND` 让"定位到文件末尾"由内核在 write 内完成 —— 并发追加不会
     * 互相覆盖（每次 write 都落在当时的 EOF）。但"比较 + 追加"整体不是一步，
     * 那件事要 flock，见头文件的边界 1。
     */
    fd = open(full, O_WRONLY | O_APPEND);
    if (fd < 0) return CXF_ERR_IO;

    while (done < blen) {
        ssize_t w = write(fd, body + done, (size_t)(blen - done));

        if (w < 0) { if (errno == EINTR) continue; close(fd); return CXF_ERR_IO; }
        if (w == 0u) { close(fd); return CXF_ERR_IO; }
        done += (uint32_t)w;
    }
    if (close(fd) != 0) return CXF_ERR_IO;

    /* total 从 lstat 的 size 推：**不用再 stat 一次**。若这期间别人也追加了，
     * 这个数会低报 —— 所以它报的是"我写完时认为的总长"，不是"权威总长"。 */
    tn = snprintf(out, out_cap, "wrote %u @%lld\n", blen,
                  (long long)st.st_size + (long long)blen);
    if (tn < 0 || (uint32_t)tn >= out_cap) return CXF_ERR_FULL;   /* 不可达：上面判过 */
    if (out_len != NULL) *out_len = (uint32_t)tn;
    return CXF_OK;
}

/* ---- 契约与实现描述符 ---- */

const IfaceDef fs_iface_hits = {
    IFACENAME_HITS, "一条命中记录：<path>:<line>:<content>", IFACE_INT
};

const CapDef fs_capdef_grep = {
    CAPNAME_FS,
    "在受控根目录下按子串检索行",
    "pattern（子串、按字节比较；路径不在 payload 里）",
    "<path>:<line>:<content> 每行一条；截断时尾行 #truncated <shown>/<total>",
    { IFACENAME_TASK, NULL }, IFACENAME_HITS
};

/*
 * 实现描述符作为**模板**存在，`ctx` 留空。
 *
 * 为什么它不进 bundle：ctx 是 `FsCfg`，root 是部署事实（这个进程能看哪片盘），
 * 而 bundle 是编译期常量表。把 root 塞进编译期常量等于把部署配置編进源码；
 * 退一步做成 `static FsCfg` + setter 更糟 —— 那就违反了"能力实现不许读
 * 文件级全局"（§3 第 17 条），且同一个进程里再也放不下第二片根。
 *
 * 正确形状是 §4.10 定的那一条：**契约随插件来（bundle），ctx 随部署来
 * （运行时注册）**。使用方拷一份模板、填 ctx、`cxf_service_register`。
 */
const CapImpl fs_capimpl_grep = {
    CAPNAME_FS, "fs", CAP_TIER_SCRIPT, 20, CAP_TRUST_IO,
    fs_grep_exec, NULL, FS_GREP_OUT_HINT, NULL
};

CXF_SERVICE(iface_fs_hits, "iface/fs.hits", 1, &fs_iface_hits);
CXF_SERVICE(capdef_fs_grep, "cap/fs.grep", 1, &fs_capdef_grep);

/* ---- fs.write_new 的契约与实现模板 ---- */

const IfaceDef fs_iface_wrote = {
    IFACENAME_WROTE, "一条写盘收据：<new|exists> <字节数>", IFACE_INT
};

const CapDef fs_capdef_write_new = {
    CAPNAME_FS_WRITE,
    "在受控根目录下新建一个文件；路径已存在时什么都不做",
    "path\\ncontent（第一行是相对路径，其后全部字节是内容；不追加、不补换行）",
    "new <n> 本次创建了 n 字节；exists <n> 路径已存在（n 是原大小），没有写入",
    { IFACENAME_TASK, NULL }, IFACENAME_WROTE
};

/*
 * 实现模板，ctx 留空 —— 与 grep 同一条纪律：写根是**部署事实**，随部署来。
 * 但这里多加一条：trust 是 CAP_TRUST_WRITE（§3 第 24 条），于是它能不能被
 * 模型看见，由请求的天花板决定，不由"这份代码编了进来"决定。
 */
const CapImpl fs_capimpl_write_new = {
    CAPNAME_FS_WRITE, "fs", CAP_TIER_SCRIPT, 20, CAP_TRUST_WRITE,
    fs_write_new_exec, NULL, FS_WRITE_OUT_HINT, NULL
};

/*
 * ★ 上面三个**刻意没有** `CXF_SERVICE` 注解，所以它们不进 `.gen.c` 的 bundle。
 *
 * 两条理由，第二条是硬的：
 *
 * 1. `fs.wrote` 是一个新接口类型，而不变式 7 要求"INT 必须至少被一个能力
 *    产出" ⇒ 类型、契约、实现必须在**同一次 sync** 之前到齐。这与
 *    `cap/agent.step` / `iface/agent.action` 的处理完全一致。
 *
 * 2. bundle 是**编译期常量表**，all-or-nothing。`cap/fs.write_new` 一旦进了
 *    bundle，所有注册 fs bundle 的部署（今天有 3 处）都必须交出一个写实现，
 *    否则"每个能力恰好一个链头"（不变式 4）当场拒绝。
 *    而**只读部署不该被迫声明写根** —— 那会把"这个进程能写哪"从部署事实
 *    退化成"为了让装配通过而随手填的一个路径"。
 *
 * 代价是部署方要记得注册三件（iface / capdef / impl）。用 `--write DIR`
 * 那样一个开关表达"我要写能力"，三件就自动一起到齐 —— 见 agentctl.c。
 */

/* ---- fs.diff 的契约与实现模板 ---- */

const IfaceDef fs_iface_diffed = {
    IFACENAME_DIFFED, "一份 dry-run 结论：same / absent / diff 块", IFACE_INT
};

const CapDef fs_capdef_diff = {
    CAPNAME_FS_DIFF,
    "对比一个文件与给定内容的差异，不写盘",
    "path\\ncontent（与 fs.write_new 同一方言：第一行路径，其后全部是内容）",
    "same；absent <字节> <行>；diff <旧字节> <新字节> +<新行> -<旧行> 后跟 @@ 块；"
    "标记 #eol / #coarse / #omitted <k>",
    { IFACENAME_TASK, NULL }, IFACENAME_DIFFED
};

/*
 * ★ trust 是 **CAP_TRUST_IO**，而 `fs.write_new` 是 WRITE —— 这一格就是
 * "dry-run 是能力不是开关"要买到的东西。
 *
 * 做成开关的话，部署侧只剩一个事实"这个进程能不能写"；拆开之后有两个：
 * "能不能看"和"能不能覆盖"。只读部署挂这一件（`--diff`），照样能回答
 * "我要改的这个文件现在长什么样、我改完会变成什么样"，而一个字节的写权限
 * 都没有。把它也标成 WRITE 就等于把这两件事又绑回一起。
 */
const CapImpl fs_capimpl_diff = {
    CAPNAME_FS_DIFF, "fs", CAP_TIER_SCRIPT, 20, CAP_TRUST_IO,
    fs_diff_exec, NULL, FS_DIFF_OUT_HINT, NULL
};

/* 同样**不带 CXF_SERVICE**：`fs.diffed` 是新接口类型，与它的产出者必须在
 * 同一次 sync 里到达（不变式 7）；而且一旦进 bundle，"每个能力恰好一个链头"
 * 会迫使所有注册 fs bundle 的部署都交出一个 diff 实现 —— 与写能力同一条
 * 理由，理由的落点却不同：那次是"只读部署不该被迫声明写根"，这次是
 * "不想给模型看差异的部署不该被迫注册它"。 */

/* ---- fs.write_apply 的契约与实现模板 ---- */

const CapDef fs_capdef_apply = {
    CAPNAME_FS_APPLY,
    "使一个文件恰好等于给定内容；内容已相同则什么都不做",
    "path\\ncontent（与 fs.write_new / fs.diff 同一方言）",
    "wrote <n> 已写入 n 字节；same <n> 内容本就相同（未写盘、未改动 inode）",
    { IFACENAME_TASK, NULL }, IFACENAME_WROTE
};

/*
 * ★ trust 是 **CAP_TRUST_WRITE** —— 它是本目录里唯一一个"做错了没有退路"的
 *   能力（旧内容被毁且不可恢复）。所以它与 write_new 共用 WRITE 这一档，
 *   而不是另立一档：档位的含义是"能造成多大的不可逆后果"，不是"代码有几行"。
 *
 * ★ 契约与实现都**不进 bundle**（无 `CXF_SERVICE`）：`cap/fs.write_apply` 若进了
 *   bundle，所有注册 fs bundle 的部署都必须交出一个覆盖实现 —— 于是"允许建新
 *   文件、不允许改已有文件"这种**真实存在**的部署策略就表达不出来了。
 *   部署矩阵因此有三格：`--diff`（只看）/ `--create`（能建）/ `--write`（能改）。
 */
const CapImpl fs_capimpl_apply = {
    CAPNAME_FS_APPLY, "fs", CAP_TIER_SCRIPT, 20, CAP_TRUST_WRITE,
    fs_write_apply_exec, NULL, FS_APPLY_OUT_HINT, NULL
};

/* ---- fs.append 的契约与实现模板 ---- */

const CapDef fs_capdef_append = {
    CAPNAME_FS_APPEND,
    "在文件末尾追加一段内容；尾部已是这段内容则什么都不做",
    "path\\ncontent（与 fs.write_new / fs.diff / fs.write_apply 同一方言）",
    "wrote <b> @<total> 已追加 b 字节、文件现为 total；"
    "same <total> 尾部已是这段内容（未写盘）；absent 文件不存在（未创建）",
    { IFACENAME_TASK, NULL }, IFACENAME_WROTE
};

/*
 * ★ trust 也是 **CAP_TRUST_WRITE**，但理由与 apply 不同：apply 的危险是
 *   "旧内容被毁"，append 的危险是**重复生效**（多写一份，且写进去的东西
 *   与正确内容混在一起后分不出哪份是多的）。两者都不可逆，所以同档。
 *
 * ★ cost = 10 比 apply 的 20 低：一次 append 是一次 open + 若干次 write，
 *   而 apply 要 mkstemp + fchmod + write + rename + 失败清理。这个数只用于
 *   路由与投机政策（§4.7），不影响正确性。
 *
 * ★ 同样**不进 bundle**（无 `CXF_SERVICE`）：append 是否放行是独立的部署
 *   决策 —— "允许改已有文件但不允许往日志里追"没有意义，但"允许往日志里追
 *   但不允许覆盖任何已有内容"是真实存在的（审计日志追加器）—— 后者只有把
 *   两者分开才能表达。
 */
const CapImpl fs_capimpl_append = {
    CAPNAME_FS_APPEND, "fs", CAP_TIER_SCRIPT, 10, CAP_TRUST_WRITE,
    fs_append_exec, NULL, FS_APPEND_OUT_HINT, NULL
};
