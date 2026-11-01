#ifndef CXF_BUF_H
#define CXF_BUF_H

#include "alloc.h"
#include "base.h"
#include "str.h"

/*
 * 增长型字节缓冲：显式 allocator + 倍增预留。本文件是"变长数据怎么长"的**唯一**实现。
 *
 * 为什么要有它：agent 层此前手写了三份 args 增长（各自翻倍、各自判失败、各自 memcpy），
 * 而 §4.8 的 action / args 一落码就会有第四份。三份意味着三处边界（空指针、溢出、
 * 复制起点），任何一处漏掉都是静默截断。收成一处的收益不是省代码，是**只剩一组边界要验**。
 *
 * 为什么不用 arena：arena 只前进不回收，无法"扩容并丢弃旧块"以外的任何策略；
 * 跨 tick 存活的数据必须能独立释放。生命周期决定容器，见 §2.1。
 *
 * 不变式：
 *   B1  p == NULL 当且仅当 cap == 0；len <= cap
 *   B2  非 NULL 的 p 常驻到 release / 下一次增长，内容在 [0, len) 内有效
 *   B3  增长后 cap 至少是 need，且至少翻倍（防抖），但绝不超过 UINT32_MAX
 *   B4  reserve / append 失败时 buf 完全不变（先申请成功再替换，与 table 同一条纪律）
 *
 * 复杂度：append 摊还 O(1)（倍增），reserve O(len) 仅在增长时；free O(1)。
 * 代价：最多浪费一倍容量 + len 的复制；要精确大小就自己在最后复制一次。
 *
 * 不适用：不负责格式化（那是 sb；两者用"先 measure 再写"组合）；
 * 不是 blob 池（没有 handle、没有所有权交接，见 blob.h）；不做越界检查依赖 ASan。
 */
typedef struct cxf_buf {
    cxf_alloc *alloc;
    uint8_t   *p;
    uint32_t   len;
    uint32_t   cap;
} cxf_buf;

static inline cxf_buf cxf_buf_make(cxf_alloc *a) {
    cxf_buf b;
    b.alloc = a;
    b.p = NULL;
    b.len = 0;
    b.cap = 0;
    return b;
}

static inline cxf_str cxf_buf_view(const cxf_buf *b) {
    return cxf_str_make((const char *)b->p, b->len);
}

/* 保证 cap >= need。失败返回 NOMEM（B4：状态不变）。 */
cxf_result cxf_buf_reserve(cxf_buf *b, uint32_t need);

/* 追加，必要时增长。失败返回 NOMEM 且内容不变。
 * **src 不得指向 b 自己的 p** —— 增长会把旧块释放掉，src 随即悬空。
 * 改写自身用 cxf_buf_assign（它走 memmove）。 */
cxf_result cxf_buf_append(cxf_buf *b, const void *src, uint32_t n);
cxf_result cxf_buf_append_byte(cxf_buf *b, uint8_t c);
cxf_result cxf_buf_append_str(cxf_buf *b, cxf_str s);

/* 改写内容（等价于 len = 0 再 append），保留容量。 */
cxf_result cxf_buf_assign(cxf_buf *b, const void *src, uint32_t n);

/* len = 0，容量保留 —— "重置而不重建"。 */
void       cxf_buf_reset(cxf_buf *b);

/* 释放并清零。重复调用安全（p 已为 NULL）。 */
void       cxf_buf_free(cxf_buf *b);

#ifndef NDEBUG
bool cxf_buf_check(const cxf_buf *b);
#else
#define cxf_buf_check(b) ((void)(b), true)
#endif

#endif
