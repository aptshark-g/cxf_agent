#ifndef CXF_VEC_H
#define CXF_VEC_H

#include "alloc.h"
#include "base.h"

/*
 * 定宽元素的可增长连续数组：显式 allocator + 倍增。**增长策略的唯一实现**。
 *
 * 为什么需要它：本仓库曾有六份手写倍增（buf / blob 条目 / service 表 / type 表 /
 * world 模块表 / world 依赖边），每份各自实现对空指针、溢出、复制起点、释放大小的
 * 判断。六份就是六组边界，漏掉任一处都是静默错误。收成一处的收益不是省代码，
 * 是**只剩一组边界要验**。`cxf_buf` 是它 elem == 1 的退化情形，因此 buf 的增长也走这里。
 *
 * 为什么"容量"必须和"指针"放在同一处：两者分开存放，释放时就会各自取到不同的值。
 * 落地前 world.c 的真实缺陷正是如此 —— 用 cap 申请、用 count 释放，于是
 * `bytes_live` 每次 start 漂移一点（`make test` 里 world 的记账断言会直接抓住）。
 * 结构上把 p / cap / len 绑成一个对象，这类错误就没有表达方式。
 *
 * 为什么元素宽度是运行期参数而不是模板：C 没有模板，void* 泛型化的代价是
 * 每次访问都要乘一次 elem，且编译器无法向量化。所以热路径用 `cxf_vec_data(v, T)`
 * 宏拿到 `T *`（零成本），只有长/容量这类边界操作走带 elem 的函数。
 *
 * 不变式：
 *   V1  p == NULL 当且仅当 cap == 0；len <= cap；elem > 0
 *   V2  p 指向 cap * elem 字节，[0, len * elem) 内的字节有效
 *   V3  增长后 cap >= need 且至少翻倍，且 cap * elem 不溢出 uint32
 *   V4  失败时对象完全不变（先申请成功再替换，与 buf / table 同一条纪律）
 *
 * 复杂度：push 摊还 O(1)；reserve O(len * elem) 仅在增长时；clear / free O(1)。
 * 代价：最多浪费一倍容量。要精确大小在最后复制一次。
 *
 * 不适用：不做按下标插入/删除（memmove 是另一个语义，等有调用方再加；
 * 无序删除用 table 的 swap-remove）；不做多列 SoA（table 的列容量是另一种形状，
 * 它一次长一组列，不是单个数组）；不格式化（那是 sb）。
 */
typedef struct cxf_vec {
    cxf_alloc *alloc;
    void      *p;
    uint32_t   len;    /* 已用元素数 */
    uint32_t   cap;    /* 容量，元素数 */
    uint32_t   elem;   /* 元素宽度（字节），> 0 */
} cxf_vec;

static inline cxf_vec cxf_vec_make(cxf_alloc *a, uint32_t elem) {
    cxf_vec v;

    CXF_ASSERT(a != NULL && elem > 0);
    v.alloc = a;
    v.p = NULL;
    v.len = 0;
    v.cap = 0;
    v.elem = elem;
    return v;
}

static inline uint32_t cxf_vec_len(const cxf_vec *v) { return v->len; }
static inline uint32_t cxf_vec_cap(const cxf_vec *v) { return v->cap; }

/* 热路径取基址：`cxf_vec_data(&v, cxf_item)[i]`，零成本、无乘除。
 * 它不做边界检查 —— 边界是调用方的循环条件（i < len），与数组同一套纪律。 */
#define cxf_vec_data(v, T) ((T *)((v)->p))

/* 一次访问：越界返回 NULL（debug 下断言）。低频路径用这个，高频用 cxf_vec_data。 */
static inline void *cxf_vec_at(cxf_vec *v, uint32_t i) {
    CXF_ASSERT(i < v->len);
    return i < v->len ? (void *)((uint8_t *)v->p + (size_t)i * v->elem) : NULL;
}

static inline const void *cxf_vec_cat(const cxf_vec *v, uint32_t i) {
    CXF_ASSERT(i < v->len);
    return i < v->len ? (const void *)((const uint8_t *)v->p + (size_t)i * v->elem) : NULL;
}

/*
 * 增长内核：保证 *cap >= need（倍增），复制 len * elem 字节，释放旧块。
 * 成功时写回 *p / *cap；失败时两者都不动（V4）。
 *
 * 这个原型刻意接收散装字段而不是 cxf_vec*：注册期的数组（service / type /
 * world）把它们当作结构体的独立成员，绑不成一个 cxf_vec。**边界逻辑是一份，
 * 状态布局各随其主** —— 强行统一布局反而是在为一个内部细节改公共接口。
 */
cxf_result cxf_vec_grow(cxf_alloc *a, void **p, uint32_t *cap,
                        uint32_t len, uint32_t need, uint32_t elem);

/* 保证 cap >= need（V4：失败不改变状态）。 */
cxf_result cxf_vec_reserve(cxf_vec *v, uint32_t need);

/* 追加一个元素（复制 elem 字节）。失败返回 NOMEM / RANGE 且内容不变。
 * **e 不得指向 v 自己的 p** —— 增长会释放旧块，e 随即悬空（要改写自身用 memmove 语义）。 */
cxf_result cxf_vec_push(cxf_vec *v, const void *e);

/* 要一个未初始化的新槽位并 len++：构造期"先占位再填字段"用它。
 * 失败返回 NULL。槽位内容是上一任残留，调用方必须写全 —— 否则读到的是垃圾。 */
void *cxf_vec_push_slot(cxf_vec *v);

/* len = 0，容量保留 —— "重置而不重建"。 */
void cxf_vec_clear(cxf_vec *v);

/* 释放并清零。重复调用安全（p 已为 NULL）。 */
void cxf_vec_free(cxf_vec *v);

#ifndef NDEBUG
bool cxf_vec_check(const cxf_vec *v);
#else
#define cxf_vec_check(v) ((void)(v), true)
#endif

#endif
