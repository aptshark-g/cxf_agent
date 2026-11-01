#ifndef CXF_BASE_H
#define CXF_BASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* 内核公共定义。
 *
 * 注释约定：头文件只写"契约"，不写"复述"。每条契约必须满足其一：
 *   1. 能在 debug 检查函数里被断言（cxf_*_check）
 *   2. 能在测试里被观测
 *   3. 是调用方必须知道、但无法从签名推出的代价或边界
 * 复述代码行为的注释一律不写 —— 它们会腐烂，腐烂的注释比没有更糟。
 */

#define CXF_CACHE_LINE   64
#define CXF_MIN_ALIGN    16
#define CXF_NO_INDEX     UINT32_MAX

#define CXF_ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((size_t)(a) - 1))
#define CXF_IS_POW2(x)       (((x) & ((x) - 1)) == 0)
#define CXF_ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))
#define CXF_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define CXF_MAX(a, b)        ((a) > (b) ? (a) : (b))

/*
 * 前置条件违反 = 程序员错误，release 下整个消失，不付运行时成本。
 * 可恢复的资源不足 = 返回值，不在这里。
 * 这条分界线是全局约定：看到 CXF_ASSERT 就知道调用方本可以保证；
 * 看到 cxf_result 就知道调用方必须处理。
 */
#ifndef NDEBUG
#include <assert.h>
#define CXF_ASSERT(c) assert(c)
#else
#define CXF_ASSERT(c) ((void)0)
#endif

typedef enum cxf_result {
    CXF_OK = 0,
    CXF_ERR_NOMEM,       /* 底层 allocator 返回 NULL，表/arena 状态不变 */
    CXF_ERR_FULL,        /* 容量上限，不是 OOM：重试或扩容后可能成功 */
    CXF_ERR_EMPTY,
    CXF_ERR_BAD_HANDLE,  /* handle 失效、不属于本表、或 gen 不匹配 */
    CXF_ERR_RANGE,       /* 参数越界，例如帧栈深度用尽 */
    CXF_ERR_DEP,         /* 依赖缺失或成环，装配期专用 */
    CXF_ERR_FORMAT,      /* 字节/文本格式不合法，解析专用。
                          * 与 RANGE 分开的理由：宽松解析器会把错误藏起来
                          * （" 12"/"12a" 都成功），而协议解析必须能拒绝，
                          * 且"根本不是数字"与"数字太大"对调用方是两种处置。 */
    CXF_ERR_IO,          /* 传输失败：连不上 / 超时 / 对端先关闭。
                          * 放在公共结果空间而不是某一层自己的错误码里，理由是
                          * **能力 ABI 的返回类型就是 cxf_result** —— 若传输层
                          * 另立一套，每个 handler 都得翻译一次，而翻译表会漂移。
                          * 与 DEP 分开：DEP 是"装配期缺依赖"，IO 是"运行期对端不在"，
                          * 前者该在启动时失败，后者该在重试策略里处理。 */
} cxf_result;

const char *cxf_result_str(cxf_result r);

#endif
