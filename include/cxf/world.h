#ifndef CXF_WORLD_H
#define CXF_WORLD_H

#include "base.h"
#include "arena.h"
#include "module.h"
#include "service.h"
#include "type.h"
#include "vec.h"

/*
 * world = 一切的所有者。没有全局单例，测试可各自建一个互不影响。
 *
 * 不变式（全部在 cxf_world_check 中断言）：
 *   W1  started 为真时，order 覆盖所有 phase != NONE 的已加载模块
 *   W2  order 按 phase 升序分段，段内为 HARD 依赖的拓扑序
 *   W3  arena_state 与 arena_frame 分离，帧的 push/pop 永不触及模块状态
 *   W4  phase_begin 单调不减，且 phase_begin[0] == 0、phase_begin[COUNT] == order.len
 *   W5  types 中每个条目 name / type 非空且名字唯一（type 的其它约束见 K1-K3）
 *
 * 两个 arena 分离是必须的，不是洁癖：
 * 模块状态与帧临时内存若共用一个 arena，一帧结束时的 pop 会回退到帧起点，
 * 把状态一起回收掉。分成两个之后，状态只增不回收（指针稳定，可运行时加载），
 * 帧内存整帧回收（这就是"热路径零分配"的落地）。
 *
 * 阶段计时是"槽位"不是"统计"：内核只把每个 phase 的耗时写进 phase_ns[]，
 * 不做聚合、不做日志、不上报 —— 那些是模块的事（日志与统计都在内核之外）。
 * 每帧 8 次读时钟的成本相对帧预算可以忽略，这是白盒性的核心设施，不关掉。
 *
 * 不适用：不做跨线程并行调度。并行要求读写冲突检测（谁写哪张表），
 * 而那需要模块声明完整的读写集 —— 现在没有，就不假装有。
 * 单线程确定性优先，需要并行时再引入读写集声明。
 */

struct cxf_world {
    cxf_alloc *alloc;
    cxf_arena  arena_state;   /* 模块状态，只增不回收，见 W3 */
    cxf_arena  arena_frame;   /* 每帧临时分配，tick 内 push/pop */

    cxf_module *modules;
    uint32_t    module_count;
    uint32_t    module_cap;

    /* 执行顺序，存模块 id，见 W2。
     * 用 cxf_vec 而不是 (指针, count, cap) 三个散装值：这里是曾经的第二个
     * "申请用 (n+1)*4、释放用 module_count*4" 的缺陷现场。把容量绑在指针上，
     * 释放时就不可能取到另一个数。len 即原 order_count。 */
    cxf_vec   order;
    uint32_t  phase_begin[CXF_PHASE_COUNT + 1];  /* 各 phase 在 order 中的区间 */

    cxf_service_def *services;
    uint32_t         service_count;
    uint32_t         service_cap;
    uint64_t         service_find_count;  /* 槽位：查找次数。调用期它不应再增长 */

    /* 类型注册表。存指针，不拥有类型（K3）—— 见 type.h 的注册期说明。 */
    cxf_type_entry *types;
    uint32_t        type_count;
    uint32_t        type_cap;
    uint64_t        type_find_count;  /* 槽位：查找次数。调用期它不应再增长 */

    uint64_t phase_ns[CXF_PHASE_COUNT];   /* 上一 tick 各阶段耗时，槽位 */
    uint64_t tick_count;
    bool     started;
};

cxf_result cxf_world_init(cxf_world *w, cxf_alloc *alloc, size_t arena_chunk);
void       cxf_world_release(cxf_world *w);

/*
 * 加载模块。解析 HARD 依赖：缺失提供者则失败，成功则给被依赖者 refcount +1。
 * 这是热插拔安全的第一道闸 —— 有人的依赖在，它就卸不掉（M3）。
 */
cxf_result cxf_module_load(cxf_world *w, const cxf_module_def *def, uint32_t *out_id);

/*
 * 卸载。refcount 非 0 时拒绝（M3）；SOFT 依赖方不受影响，只是从此退化。
 * state 不回收：arena 只增不减，槽位标记为未加载即可，
 * 因此卸载不会让其它模块的 state 指针失效。
 */
cxf_result cxf_module_unload(cxf_world *w, uint32_t id);

/*
 * 从帧 arena 分配，整帧结束时自动回收，调用方不需要也不应该 free。
 * 这是模块在 tick 里获取临时内存的唯一途径 —— 见 tick 的说明。
 */
void *cxf_world_alloc(cxf_world *w, size_t size, size_t align);

void          *cxf_module_state(cxf_world *w, uint32_t id);
const cxf_module *cxf_module_get(const cxf_world *w, uint32_t id);
const cxf_module *cxf_module_find(const cxf_world *w, const char *name);

/*
 * 解析依赖、拓扑排序、按序调用 init。之后才能 tick。
 * 依赖图有环时返回 CXF_ERR_RANGE，不进入半启动状态。
 */
cxf_result cxf_world_start(cxf_world *w);

/*
 * 跑一帧。开始时 push 帧、结束时 pop ——
 * 模块在 tick 里用 cxf_world_alloc 拿的临时内存整帧回收，
 * 这就是"热路径零分配"的落地：模块既不用 malloc，也不用 free。
 *
 * 每个 phase 的耗时写入 phase_ns[]（见文件头"槽位不是统计"）。
 */
void cxf_world_tick(cxf_world *w);

/* 逆序 deinit，收尾。之后可以再 start，也可以 release。 */
void cxf_world_stop(cxf_world *w);

uint64_t cxf_world_phase_ns(const cxf_world *w, cxf_phase p);

/* 校验 W1-W4 与 M1-M5。NDEBUG 下编译为常量 true。 */
#ifndef NDEBUG
bool cxf_world_check(const cxf_world *w);
#else
#define cxf_world_check(w) ((void)(w), true)
#endif

#endif
