#ifndef CXF_STR_H
#define CXF_STR_H

#include "base.h"

/*
 * 非拥有切片视图：{ 指针, 长度 }。不要求、也不产生 NUL 结尾。
 *
 * 为什么不用 char*：C 把长度藏进数据里（strlen 找零），于是每一层 API 都得付出
 * 一次 O(n) 扫描，且表达不了"内嵌 NUL 的子串"。更根本的是 —— 长度从编译期已知的
 * **值**退化成了**运行时数据**：预取、内联、批量比较一起失效。
 * 切片把它变回值：调用方手里已有长度，函数只消费、不重新求。
 * 这与内核那条统一原则是同一条 —— 凡是本可以提前知道的，别让它退化成运行时数据。
 *
 * 于是本条纪律：**零终止串不进数据通路**。strlen 只允许在边界出现一次
 * （cxf_str_from_cstr），内部一律传切片。字面量用 CXF_STR_LIT，长度编译期算完。
 *
 * 为什么 len 是 uint32_t：与 table / blob 的 len·cap 同一宽度；4GiB 是设计内上限。
 *
 * 本文件不持有状态、不分配、不依赖 world —— 它是**值类型 + 纯函数**，
 * 与 handle.h 同类，因此不增加内核那五件事的职责。
 *
 * 不变式：
 *   S1  len == 0 时 p 可为 NULL；len > 0 时 p 必须非 NULL
 *   S2  空切片（含 p == NULL）对所有函数安全，不被解引用
 *   S3  返回值一律是入参的视图：生命周期与入参绑定，不复制、不接管
 *   S4  不写写入参，不假设 p 的对齐，不假设尾部有 NUL
 *
 * 复杂度：eq / starts_with / ends_with O(n) 走 memcmp（可向量化）；
 * find 为朴素搜索 O(n·m)，最坏退化明显 —— **不要拿长模式去搜不可信输入**。
 * 需要线性最坏界时换 Two-Way，别在这里加正则。
 *
 * 不适用：不是拥有型字符串（释放归容器，见 §2.1 的所有权交接）；
 * 不做编码转换与规范化 —— 字节就是字节（与网关"禁改字节"同一条原则）；
 * 不做大小写不敏感比较，那要先定 locale，而 locale 是全局状态。
 */
typedef struct cxf_str {
    const char *p;
    uint32_t    len;
} cxf_str;

/* 只接受数组（字面量或 char[]）。传指针会得到指针宽度减一，这是调用方错误。 */
#define CXF_STR_LIT(lit) ((cxf_str){ (lit), (uint32_t)(sizeof(lit) - 1u) })

static inline cxf_str cxf_str_make(const char *p, uint32_t len) {
    cxf_str s;
    s.p = p;      /* 空切片的 p 允许为 NULL，也允许指向缓冲末尾，S1 只约束 len > 0 */
    s.len = len;
    return s;
}

static inline bool cxf_str_empty(cxf_str s) {
    return s.len == 0;
}

/*
 * 边界函数：本框架唯一允许 strlen 的地方。
 * 只在"外部 C 字符串进入数据通路"处调用（argv / getenv / 第三方 API 返回）。
 * 进不来就算 —— 一旦进了内部，长度必须已经在手里。
 */
cxf_str cxf_str_from_cstr(const char *z);

/* 全等：长度相同且逐字节相同。 */
bool cxf_str_eq(cxf_str a, cxf_str b);

/* 字典序，归一化为 -1 / 0 / 1（不把 memcmp 的原始差值泄给调用方）。 */
int cxf_str_order(cxf_str a, cxf_str b);

bool cxf_str_starts_with(cxf_str s, cxf_str prefix);
bool cxf_str_ends_with(cxf_str s, cxf_str suffix);

/* 返回下标，找不到返回 CXF_NO_INDEX（不用 -1：留在 u32 空间，少一次符号扩展）。 */
uint32_t cxf_str_find(cxf_str hay, cxf_str needle);
uint32_t cxf_str_find_char(cxf_str hay, char c);

/* 越界自动收敛：off >= len 返回空切片，长度超出剩余部分按剩余算。 */
cxf_str cxf_str_sub(cxf_str s, uint32_t off, uint32_t len);

/* 去掉首尾 ASCII 空白（' ' \t \r \n \v \f）。不用 isspace：它跟 locale 走。 */
cxf_str cxf_str_trim(cxf_str s);

/*
 * 分隔符切分，零分配：迭代器只持有"剩余部分"。
 * 空字段保留（"a,,b," 是四个 token：a / "" / b / ""）—— 协议字段不能因为空
 * 就被吞掉，否则列位置会错位。空输入产生一个空 token，之后耗尽。
 */
typedef struct cxf_str_iter {
    cxf_str rest;
    char     sep;
    bool     done;
} cxf_str_iter;

void cxf_str_split(cxf_str_iter *it, cxf_str s, char sep);

/* 有下一个就写进 out 并返回 true；耗尽返回 false。 */
bool cxf_str_split_next(cxf_str_iter *it, cxf_str *out);

/*
 * 严格十进制无符号解析：
 *   空串 / 非数字 / 前导 '+' / 前导空白 / 尾随垃圾 → CXF_ERR_FORMAT
 *   超过 UINT32_MAX                              → CXF_ERR_RANGE
 * 失败时不写 *out（调用方不必先清零）。
 * 为什么严格：宽松解析会把错误藏起来，而协议字段的错必须当场拒绝。
 */
cxf_result cxf_str_parse_u32(cxf_str s, uint32_t *out);

/* S1/S2/S4 的调试期断言。NDEBUG 下消失。 */
#ifndef NDEBUG
bool cxf_str_check(cxf_str s);
#else
#define cxf_str_check(s) ((void)(s), true)
#endif

#endif
