#ifndef SOA_TYPES_H
#define SOA_TYPES_H

#include <stdint.h>
#include <stddef.h>

#include "cxf/type.h"

/* 供 gen_registry.py 扫描的类型头文件。结构体保持日常写法，不做任何妥协。 */

typedef struct { float x, y, z; } Vec3;

typedef struct {
    Vec3     pos;
    float    vx, vy;
    uint32_t hp;
    void    *owner;
} Particle;

/* 自引用：指针是递归的终止符，展平必须停在 next 上。 */
typedef struct Node Node;
struct Node {
    int32_t v;
    Node   *next;
};

/* 叶子数超过列上限，用来验证是报错而不是截断。 */
typedef struct {
    uint32_t f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,  f8,
             f9,  f10, f11, f12, f13, f14, f15, f16;
} Wide;

/* 标记。展开成真 typedef，写错名字会在编译期报错。 */
CXF_TYPE(Vec3);
CXF_TYPE(Particle);
CXF_TYPE(Node);
CXF_TYPE(Wide);

#endif
