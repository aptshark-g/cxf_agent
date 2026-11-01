#include <stdio.h>

#include "cxf/service.h"

#include "services.h"

/*
 * 服务的实现与注册。CXF_SERVICE 展开成外部链接的常量，所以它只能写在
 * 实现文件里 —— 写在头文件里被多个 TU 包含会重复定义符号。
 */

static int32_t add_impl(int32_t a, int32_t b) { return a + b; }
static int32_t mul_impl(int32_t a, int32_t b) { return a * b; }

/* API 表保持 static：对外可见的只有下面那一层注册项。 */
static const MathApi math_api_v1 = { add_impl, mul_impl };

CXF_SERVICE(math_v1, "math", 1, &math_api_v1);
CXF_SERVICE(math_v0, "math", 0, &math_api_v1);   /* 同一张表，两个版本号 */

static uint32_t g_lines = 0;

static void write_impl(const char *msg) {
    g_lines++;
    (void)msg;   /* 测试里不真的输出，只计数 */
}

static uint32_t lines_impl(void) { return g_lines; }

static const LogApi log_api_v1 = { write_impl, lines_impl };

CXF_SERVICE(log_v1, "log", 1, &log_api_v1);
