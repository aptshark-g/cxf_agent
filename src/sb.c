#include <string.h>

#include "cxf/sb.h"

/* 契约与不变式见 sb.h，这里只写实现的取舍。 */

void cxf_sb_put(cxf_sb *sb, const void *src, uint32_t n) {
    uint32_t used;

    if (n == 0) return;
    CXF_ASSERT(sb->len <= 0xFFFFFFFFu - n);  /* 逻辑长度不许回绕 */

    used = cxf_sb_used(sb);                  /* B1：先取，再推进 len */
    if (sb->p != NULL && used < sb->cap) {
        uint32_t room = sb->cap - used;
        memcpy(sb->p + used, src, n < room ? n : room);  /* B4：只写不越界 */
    }
    sb->len += n;                            /* B3：截断不影响逻辑长度 */
}

void cxf_sb_puts(cxf_sb *sb, cxf_str s) {
    cxf_sb_put(sb, s.p, s.len);  /* n == 0 时不碰 s.p，空切片安全 */
}

void cxf_sb_putc(cxf_sb *sb, char c) {
    cxf_sb_put(sb, &c, 1);
}

/*
 * 数字低位先出，所以先倒着写进栈上小缓冲，再按正序吐给 put。
 * 20 字节够 u64 满值；不引 printf —— 格式化库会拖进 locale 与浮点。
 */
void cxf_sb_u64(cxf_sb *sb, uint64_t v) {
    char     tmp[20];
    uint32_t n = 0, i;

    do {
        tmp[n++] = (char)('0' + (char)(v % 10u));
        v /= 10u;
    } while (v != 0);

    for (i = n; i-- > 0;) cxf_sb_putc(sb, tmp[i]);
}

void cxf_sb_u32(cxf_sb *sb, uint32_t v) {
    cxf_sb_u64(sb, (uint64_t)v);
}

static cxf_result commit_impl(const cxf_sb *sb, cxf_arena *a, cxf_str *out, bool z) {
    uint32_t need;
    char    *dst;

    CXF_ASSERT(sb != NULL && a != NULL && out != NULL);

    *out = cxf_str_make(NULL, 0);
    if (sb->p == NULL) return CXF_ERR_EMPTY;   /* 计数模式：没有内容可提交 */
    if (sb->len > sb->cap) return CXF_ERR_FULL; /* 截断过：len 就是要申请的字节数 */
    if (sb->len == 0) return CXF_OK;

    need = sb->len + (z ? 1u : 0u);
    dst = cxf_arena_alloc(a, need, 1);
    if (dst == NULL) return CXF_ERR_NOMEM;

    memcpy(dst, sb->p, sb->len);
    if (z) dst[sb->len] = '\0';
    *out = cxf_str_make(dst, sb->len);  /* 切片不含 NUL，长度语义保持纯净 */
    return CXF_OK;
}

cxf_result cxf_sb_commit(const cxf_sb *sb, cxf_arena *a, cxf_str *out) {
    return commit_impl(sb, a, out, false);
}

cxf_result cxf_sb_commitz(const cxf_sb *sb, cxf_arena *a, cxf_str *out) {
    return commit_impl(sb, a, out, true);
}
