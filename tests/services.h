#ifndef SERVICES_H
#define SERVICES_H

#include <stdint.h>

/*
 * 服务的 API 形状。只有形状放在头文件，实现与注册项都在 services.c 里，
 * 因此 API 表本身可以是 static —— 外部拿不到它的符号。
 */

typedef struct MathApi {
    int32_t (*add)(int32_t a, int32_t b);
    int32_t (*mul)(int32_t a, int32_t b);
} MathApi;

typedef struct LogApi {
    void (*write)(const char *msg);
    uint32_t (*lines)(void);
} LogApi;

#endif
