#include <string.h>

#include "cxf/str.h"

/* 契约与不变式见 str.h，这里只写实现的取舍。 */

cxf_str cxf_str_from_cstr(const char *z) {
    if (z == NULL) return cxf_str_make(NULL, 0);
    return cxf_str_make(z, (uint32_t)strlen(z));
}

bool cxf_str_eq(cxf_str a, cxf_str b) {
    if (a.len != b.len) return false;
    if (a.len == 0) return true;  /* 长度为零时不碰指针，S2 */
    return memcmp(a.p, b.p, a.len) == 0;
}

int cxf_str_order(cxf_str a, cxf_str b) {
    uint32_t n = a.len < b.len ? a.len : b.len;
    int c = n != 0 ? memcmp(a.p, b.p, n) : 0;

    if (c != 0) return c < 0 ? -1 : 1;
    /* 公共前缀相等时短的小 —— 与字典序一致，且让"前缀 < 全文"成为一条不变式。 */
    if (a.len == b.len) return 0;
    return a.len < b.len ? -1 : 1;
}

bool cxf_str_starts_with(cxf_str s, cxf_str prefix) {
    if (prefix.len > s.len) return false;
    if (prefix.len == 0) return true;
    return memcmp(s.p, prefix.p, prefix.len) == 0;
}

bool cxf_str_ends_with(cxf_str s, cxf_str suffix) {
    if (suffix.len > s.len) return false;
    if (suffix.len == 0) return true;
    return memcmp(s.p + (s.len - suffix.len), suffix.p, suffix.len) == 0;
}

/*
 * 朴素搜索，但用 memchr 跳过"首字节就不匹配"的位置：
 * glibc 的 memchr 走 SIMD，实际比手写循环快一个量级，
 * 且不改变最坏界（仍然是朴素 O(n·m)）。
 */
uint32_t cxf_str_find(cxf_str hay, cxf_str needle) {
    uint32_t last, i;

    if (needle.len == 0) return 0;
    if (needle.len > hay.len) return CXF_NO_INDEX;

    last = hay.len - needle.len;
    for (i = 0; i <= last; i++) {
        const char *hit = memchr(hay.p + i, needle.p[0], (size_t)(last - i) + 1u);
        if (hit == NULL) return CXF_NO_INDEX;
        i = (uint32_t)(hit - hay.p);
        if (memcmp(hit, needle.p, needle.len) == 0) return i;
    }
    return CXF_NO_INDEX;
}

uint32_t cxf_str_find_char(cxf_str hay, char c) {
    const char *hit;

    if (hay.len == 0) return CXF_NO_INDEX;
    hit = memchr(hay.p, c, hay.len);
    if (hit == NULL) return CXF_NO_INDEX;
    return (uint32_t)(hit - hay.p);
}

cxf_str cxf_str_sub(cxf_str s, uint32_t off, uint32_t len) {
    if (off >= s.len) return cxf_str_make(NULL, 0);
    s.p += off;
    s.len -= off;
    if (len < s.len) s.len = len;
    return s;
}

static bool is_space_ascii(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

cxf_str cxf_str_trim(cxf_str s) {
    uint32_t b = 0, e = s.len;

    while (b < e && is_space_ascii(s.p[b])) b++;
    while (e > b && is_space_ascii(s.p[e - 1])) e--;
    if (b == e) return cxf_str_make(s.p + b, 0);
    return cxf_str_make(s.p + b, e - b);
}

void cxf_str_split(cxf_str_iter *it, cxf_str s, char sep) {
    it->rest = s;
    it->sep = sep;
    it->done = false;
}

bool cxf_str_split_next(cxf_str_iter *it, cxf_str *out) {
    uint32_t i;

    if (it->done) return false;

    for (i = 0; i < it->rest.len; i++) {
        if (it->rest.p[i] == it->sep) {
            *out = cxf_str_make(it->rest.p, i);
            it->rest.p += i + 1;
            it->rest.len -= i + 1;
            return true;
        }
    }
    /* 没有更多分隔符：剩余部分就是最后一个 token，下一轮耗尽。 */
    *out = it->rest;
    it->done = true;
    return true;
}

cxf_result cxf_str_parse_u32(cxf_str s, uint32_t *out) {
    uint64_t v = 0;
    uint32_t i;

    if (s.p == NULL || s.len == 0) return CXF_ERR_FORMAT;

    for (i = 0; i < s.len; i++) {
        uint32_t d = (uint32_t)(unsigned char)s.p[i] - (uint32_t)'0';
        if (d > 9u) return CXF_ERR_FORMAT;   /* 含符号、空白、任何非数字 */
        v = v * 10u + d;
        if (v > 0xFFFFFFFFull) return CXF_ERR_RANGE;
    }
    *out = (uint32_t)v;
    return CXF_OK;
}

#ifndef NDEBUG
bool cxf_str_check(cxf_str s) {
    if (s.len > 0 && s.p == NULL) return false;  /* S1 */
    return true;                                  /* S2/S4 由各函数的空判定保证 */
}
#endif
