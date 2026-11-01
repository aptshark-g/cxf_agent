#ifndef CXF_MODULE_H
#define CXF_MODULE_H

#include "base.h"
#include "arena.h"

/*
 * 模块 = 一个具名的"数据 + 行为"单元，可选地参与 tick。
 *
 * 颗粒度随意是硬需求：一个模块可以是整个网络栈，也可以只是一个哈希函数。
 * 因此最小契约为零 —— 四个"零值"撑起全部自由度：
 *   state_size == 0   无状态（纯函数模块，框架不分配状态）
 *   phase == NONE     不参与 tick（被动服务，被别人调用）
 *   tick == NULL      同上
 *   provides == 0     纯内部实现，不对外提供能力
 *
 * 不变式（全部在 cxf_world_check 中断言）：
 *   M1  同一 name 不会重复加载
 *   M2  HARD 依赖构成的图无环
 *   M3  refcount 为 0 的模块才允许卸载
 *   M4  state_size > 0 的已加载模块，其 state 非 NULL 且指针在 world 生命期内稳定
 *   M5  phase 要么 < CXF_PHASE_COUNT，要么 == CXF_PHASE_NONE
 *
 * 复杂度：
 *   加载        O(模块数 + 能力位数)，低频，慢一点无所谓
 *   依赖解析    O(V + E)
 *   tick        O(参与 tick 的模块数)，按 phase 顺序一趟遍历
 *
 * 不适用：不做热重载（卸载后重新加载同一 name 是允许的，但 ABI 兼容性由调用方保证），
 * 不做跨线程并行调度 —— 并行需要读写冲突检测，见 world.h 的说明。
 */

typedef struct cxf_world cxf_world;

typedef enum cxf_phase {
    CXF_PHASE_INPUT  = 0,
    CXF_PHASE_UPDATE = 1,
    CXF_PHASE_LATE   = 2,
    CXF_PHASE_LAST   = 3,
} cxf_phase;

#define CXF_PHASE_COUNT 4
#define CXF_PHASE_NONE  255

/*
 * 能力位最多 32 种。超出请用 service.h 的字符串服务表 —— 那边是装配期
 * 字符串查找，慢但无数量上限。两套机制并存是刻意的：位匹配用于高频、
 * 编译期已知的核心能力；字符串用于插件式、数量不定的扩展服务。
 */
typedef uint32_t cxf_caps;

typedef cxf_result (*cxf_fn_init)(cxf_world *w, void *state);
typedef void (*cxf_fn_tick)(cxf_world *w, void *state, cxf_arena *frame);
typedef void (*cxf_fn_deinit)(cxf_world *w, void *state);

/* 编译期常量，放只读段。它本身就是"白盒清单"。 */
typedef struct cxf_module_def {
    const char  *name;
    uint8_t      phase;
    uint32_t     state_size;
    cxf_caps     provides;
    cxf_caps     requires;       /* SOFT：缺失则退化，不报错 */
    cxf_caps     requires_hard;  /* HARD：缺失则加载失败，卸载时受 refcount 保护 */
    cxf_fn_init   init;
    cxf_fn_tick   tick;
    cxf_fn_deinit deinit;
} cxf_module_def;

/*
 * state 由 world 的 arena_state 分配，不是独立 malloc。
 * 这样状态之间连续（利于 dump 与缓存），且指针永不移动（M4）——
 * arena 只增不回收，chunk 一旦交出就不会被搬走。
 * 这也让"运行时加载模块"成为安全操作：新模块的状态分配不影响既有模块。
 */
typedef struct cxf_module {
    const cxf_module_def *def;
    void    *state;
    uint32_t id;
    uint32_t refcount;   /* 被多少模块 HARD 依赖，见 M3 */
    bool     loaded;     /* 已登记 */
    bool     inited;     /* 已调用 init，start 可重复调用以纳入新加载的模块 */
} cxf_module;

#endif
