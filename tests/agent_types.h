#ifndef AGENT_TYPES_H
#define AGENT_TYPES_H

#include <stdint.h>

#include "cxf/handle.h"
#include "cxf/type.h"

/*
 * agent 运行时的状态类型。
 *
 * 全部是标量或 handle —— 无裸指针、无数组（生成器会拒绝数组）。
 * 因此天然扁平、可序列化、可跨节点搬运（docs/agent.md §3）。
 *
 * 变长 args / result 不在这里：它们存 blob 池，行里只放 handle（§3.1）。
 * 理由已在内核审核里写明：blob 跨 tick 存活，而 arena 帧在 tick 结束整帧回收，
 * arena_state 只增不回收 —— 两个都不匹配 blob 的生命周期。
 *
 * 没有 id 字段：handle 本身（index + gen）就是唯一 id，见 handle.h H1/H2。
 */

typedef enum WorkItemStatus {
    WI_BLOCKED = 0,   /* 等子项汇聚，pending 未归零 */
    WI_READY   = 1,
    WI_RUNNING = 2,
    WI_DONE    = 3,
    WI_FAILED  = 4,
} WorkItemStatus;

typedef struct {
    uint32_t   cap_id;
    uint8_t    status;
    cxf_handle args;     /* blob 池 handle，可为 NULL（无入参） */
    cxf_handle result;   /* blob 池 handle，DONE 后有效 */
    cxf_handle parent;   /* fan-in 汇聚点；NULL 表示自己是最终节点 */
    uint32_t   pending;  /* 还有几个子项未完成；归零时转为 READY */
    uint32_t   origin_rank;
    uint32_t   owner_rank;
    uint8_t    trust_max; /* 请求声明的信任天花板（CapTrust）。必须随条目走到底：
                             降级链会在派发期换实现，天花板不跟着走就会被绕过 */
    uint8_t    tries;    /* 已执行轮数。链在注册期已验证无环，所以它只是观测 */
    /* 这条条目是第四轮闭环里的第几轮（docs/agent.md §4.8）。根条目 = 0，
       每派生一次 +1。**沿 lineage 继承**：不继承则两个能力互相 CALL 就是
       无界循环，上限形同虚设。判定在派生点（INJECT），不在执行点 ——
       否则能力就得知道"自己在第几步"，而它本该不知道图。
       与 parent 是两条不同的关系：parent 是同轮内的 fan-in（数据流），
       rounds 是跨轮的 lineage（控制流）。 */
    uint32_t   rounds;
    /* 这条条目是**本轮第几条动作**（1 基）。根条目 / 不在批次里 = 0。
     *
     * 为什么必须存，而不能由记录顺序推出来：`recycle_tick` 倒序遍历
     * （`agent.c` 里 swap-remove 的要求）⇒ 父节点 args 里的记录是**派出序的倒序**；
     * 而配额闸门（§4.17.8）下同一批子项落在不同 tick ⇒ 记录顺序变成**完成序**。
     * 三种顺序（派出 / 倒序派出 / 完成）都真实存在 ⇒ 顺序不承载身份。
     * ⇒ 判据：**凡是"靠顺序配对"的隐式契约，一旦有第二个并发源就不成立**，
     *   必须有一个字段把它变成显式数据。
     *
     * 读者有两个：`#fail <seq> <cap>` 那一行（模型据它判断"该从第几段重来"），
     * 以及失败位置直方图（`stat_fail_seq[]`，即段化收益实验的分子）。
     */
    uint8_t    seq;
    /* 让这条条目变成 FAILED 的那个 `cxf_result`（0 = `CXF_OK` = 没记过）。
     *
     * 为什么必须存下来：`status` 只说"没成"，而"没成"至少有四种**下一步动作
     * 完全相反**的处置 —— 参数/格式错（改参数）、我们的上限（换请求）、
     * 世界不配合（路径不存在 / 连不上，重发同样的参数字节没用）、装不下
     * （`FULL`，该扩容量）。这一位在失败那一刻就在手上，随后被丢掉；
     * 实测代价（v1.14.23，121 那台）：收据只有 `#fail <seq> <cap>` 时，
     * 模型为搞清"为什么"花了 **3 次调用**（grep → grep → diff）。
     *
     * 为什么不和 `status` 挨着放（我第一版就是那么写的）：会**移动既有列号**。
     * `test_types` 钉的那条契约是"新字段追加在末尾，既有列号不变"——
     * `seq` 当初也是这么加的。语义上要的是"同一张表"，不是"相邻两列"。
     *
     * 为什么要和 `status` 在同一张表里：两者**必须一起变** —— 一条 FAILED
     * 却没有原因的条目就是静默（而 `0` 恰好是"没有错误"，它会伪装成合理值）。
     * 所以写入口收敛成 `item_fail()` 一个函数，读入口只有回执那一行。
     *
     * 为什么是 `rc` 而不是一句人话：归因归模型（§4.18.15）。运行时能确定的
     * 只有"失败发生在哪一侧"，那就是 `rc`；把 `rc` 翻成散文就是运行时开始
     * 编故事，而故事一旦编错比不给更坏。
     */
    uint8_t    err;
} WorkItem;

/* 轨迹 / 对话消息。第一刀只用 role + tag 标量占位，文本进 blob 池。 */
typedef struct {
    cxf_handle agent;
    uint8_t    role;
    uint32_t   tag;
} Message;

CXF_TYPE(WorkItem);
CXF_TYPE(Message);

#endif
