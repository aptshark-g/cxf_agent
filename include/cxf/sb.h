#ifndef CXF_SB_H
#define CXF_SB_H

#include "arena.h"
#include "base.h"
#include "str.h"

/*
 * 帧内字符串 builder：写进调用方给的缓冲，自己不分配。
 *
 * 为什么是两段式 counting：arena 只前进不回收 —— 写超了只能换一块更大的**并复制**，
 * 旧块白费到帧结束。于是"先数长度、再按需精确分配一次"比"长了再扩"合算。
 * 第一遍传 cxf_sb_measure() 只累加长度；第二遍拿 arena 上的精确缓冲，**跑同一份
 * 构建代码**，不搞第二套 API —— 两套 API 必然漂移，而漂移处正是长度算错的地方。
 *
 * 为什么单遍也能用：所有写函数是 snprintf 语义 —— 只写到 cap 为止（截断），
 * 但逻辑长度照常累加。于是截断可被发现（cxf_sb_overflow），而不是静默少写；
 * 写超之后 sb->len 正是要申请的字节数，直接拿去跑第二遍。这是"响亮失败"在写入侧
 * 的样子，也是本文件最主要的一条取舍。
 *
 * 不持有 arena：提交时显式传入，与宪法"分配走显式传入的 allocator"一致；
 * sb 因此只是游标（16 字节），生命周期不牵连任何容器，可以放在栈上随手传。
 *
 * 不变式：
 *   B1  used == min(len, cap)
 *   B2  p == NULL 即计数模式：只有 len 变化，任何字节都不写（cap 无意义）
 *   B3  len 单调不减，是"若空间足够将写入的总长"，与 cap 无关
 *   B4  越过 cap 的部分只计入 len，绝不写出缓冲区边界
 *
 * 不适用：不保证 NUL 结尾（那是边界产物，用 cxf_sb_commitz）；
 * 不做浮点格式化（浮点文本没有唯一表示，精度是调用方的决定，不该藏在这里）；
 * 不支持回退或覆写已写内容（要改就从上一个帧重来）。
 */
typedef struct cxf_sb {
    char     *p;    /* NULL = 计数模式，见 B2 */
    uint32_t  len;  /* 逻辑长度，见 B3 */
    uint32_t  cap;  /* 目标容量 */
} cxf_sb;

static inline cxf_sb cxf_sb_measure(void) {
    cxf_sb sb = { NULL, 0, 0 };
    return sb;
}

static inline cxf_sb cxf_sb_buf(char *p, uint32_t cap) {
    cxf_sb sb = { p, 0, cap };
    return sb;
}

static inline uint32_t cxf_sb_len(const cxf_sb *sb) {
    return sb->len;
}

static inline uint32_t cxf_sb_used(const cxf_sb *sb) {
    return sb->len < sb->cap ? sb->len : sb->cap;  /* B1 */
}

static inline bool cxf_sb_overflow(const cxf_sb *sb) {
    /* 计数模式没有"截断"这回事：它本来就不写，一切超界都是正常的。 */
    return sb->p != NULL && sb->len > sb->cap;
}

/* 已写内容的视图。可能是被截断的 —— 先问 cxf_sb_overflow。 */
static inline cxf_str cxf_sb_view(const cxf_sb *sb) {
    return cxf_str_make(sb->p, cxf_sb_used(sb));
}

void cxf_sb_put(cxf_sb *sb, const void *src, uint32_t n);
void cxf_sb_puts(cxf_sb *sb, cxf_str s);
void cxf_sb_putc(cxf_sb *sb, char c);
void cxf_sb_u32(cxf_sb *sb, uint32_t v);
void cxf_sb_u64(cxf_sb *sb, uint64_t v);

/*
 * 把已写内容复制到 arena 当前帧，返回精确大小的切片。
 * 计数模式下无可提交内容，返回 CXF_ERR_EMPTY；被截断返回 CXF_ERR_FULL
 * （此时 sb->len 就是要申请的字节数，重跑第二遍即可）。
 *
 * 热路径更省的做法是第二段直接把 arena 缓冲交给 cxf_sb_buf，跳过这次复制 ——
 * commit 是"单遍 + 事后精确化"的便利路径，不是默认路径。
 */
cxf_result cxf_sb_commit(const cxf_sb *sb, cxf_arena *a, cxf_str *out);

/*
 * 同上，但末尾补 '\0'，返回的切片**不含** NUL（p[len] == '\0'）。
 * 仅供边界使用：把本框架的字节交给 C API（printf / exec / 第三方库）。
 * 数据通路内部请用 cxf_sb_commit —— 零终止串不进数据通路。
 */
cxf_result cxf_sb_commitz(const cxf_sb *sb, cxf_arena *a, cxf_str *out);

#endif
