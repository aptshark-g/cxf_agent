#ifndef AGENT_ARGS_H
#define AGENT_ARGS_H

#include <stdint.h>

/*
 * 写族能力共用的方言切分：`path\nbody`。
 *
 * **这是"方言"而不是"算法"** —— 它只有一个决定：第一个 `\n` 之前是路径，
 * 之后**全部**是内容（内容里可以再有 `\n`，那些不切）。
 *
 * 为什么放到头里而不是各写一份：`fs.write_new` / `fs.diff` / `fs.write_apply` /
 * `fs.append` / `http.post` 五个能力共用它。两份实现之间的唯一差别会是
 * "找不到 `\n` 时 body 指向哪里"这种边界 —— 而那个差别不会在正常输入下暴露，
 * 只会在"模型漏了换行"时表现为一个读到越界指针的 body。
 *
 * 用 `static inline` 而不是抽成 .c：这段只有 12 行，且它是**定义**不是逻辑。
 * 编译期复制两份机器码是零代价的；多一个编译单元和一次跨模块调用不是。
 */

/*
 * 切 `args[0..len)` 为 path 与 body。
 *
 * 没有 `\n` 时：path = 全部，body = 空（指向末尾，长度 0）—— 这是"有路径没内容"，
 * 这里只负责切分。写入族会拒绝缺分隔符；清空文件必须显式提供 `path\n`。
 * `http.post` 可按自己的契约接受空请求体。
 */
static inline void split_args(const char *args, uint32_t len,
                              const char **path, uint32_t *plen,
                              const char **body, uint32_t *blen) {
    uint32_t i;

    for (i = 0; i < len; i++)
        if (args[i] == '\n') break;

    *path = args;
    *plen = i;
    if (i < len) { *body = args + i + 1u; *blen = len - (i + 1u); }
    else         { *body = args + len;    *blen = 0u; }
}

#endif
