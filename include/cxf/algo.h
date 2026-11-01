#ifndef CXF_ALGO_H
#define CXF_ALGO_H

#include "base.h"
#include "bitmap.h"
#include "csr.h"
#include "str.h"

/*
 * 算法库：只收**给定形状上的通用变换** —— 连续数组、字节串。
 *
 * 边界（防止它变成"成员函数回收站"）：
 *   swap-remove 属于 table（它依赖 sparse/dense 的对应关系），不在这里；
 *   二分的名字查找属于 namemap（它依赖条目布局），不在这里。
 *   只有"输入是一段连续内存、输出是同一段内存的某种次序/位置"才进来。
 *
 * 全局约束：**无分配、无函数指针、无 world**。因此不接受比较器 ——
 * 比较器会把内联变成间接跳转，正是热路径上最贵的那类（见项目记忆的指针实测）。
 * 需要按别的键排序时，先构造键数组再排下标，而不是传一个回调进来。
 *
 * 不适用：不保证稳定（heapsort 天然不稳定；要稳定就用下标数组 + 原始位置作为次级键）；
 * 不做浮点（浮点的顺序依赖 NaN/±0 的语义，调用方自己定）。
 */

/* 原地升序。n < 16 走插入排序，否则 heapsort —— 两者都不分配。
 * 复杂度：O(n log n) 最坏；小数组 O(n²) 但常数极小。 */
void     cxf_algo_sort_u32(uint32_t *a, uint32_t n);

/* 第一个 >= key 的下标（0..n）。数组必须已升序。 */
uint32_t cxf_algo_lower_bound_u32(const uint32_t *a, uint32_t n, uint32_t key);

/*
 * 取最大的 k 个元素的下标，按 key **降序**写入 out（out 至少 k 项）。
 * 返回实际写入数（min(k, n)）。O(n log k)，用 out 自己当堆，不额外分配。
 * 相等 key 的相对次序不保证 —— 需要确定性时把下标作为次级键（调用方自己加）。
 */
uint32_t cxf_algo_topk_u32(const uint32_t *keys, uint32_t n, uint32_t k, uint32_t *out);

/* FNV-1a 32。用途：名字/字节串的快速判别（哈希表、去重、缓存键）。
 * 非加密 —— 不要拿它做防碰撞的安全边界。 */
uint32_t cxf_algo_hash_bytes(const void *p, uint32_t n);
uint32_t cxf_algo_hash_str(cxf_str s);

/*
 * Kahn 拓扑排序。order 输出 n 项；indeg 是调用方给的 n 项 u32 暂存。
 * 有环返回 CXF_ERR_DEP（此时 order 只有前若干项是已定序的，内容不可用）。
 *
 * 确定性：初始入度为零的节点按**升序**入队，队列是 FIFO ⇒ 同一张图永远给同一条序列。
 * 这不是洁癖：装配期顺序如果随哈希/内存布局漂移，日志与断言就都不可复现。
 *
 * 为什么 indeg 要调用方给：本库零分配。调用方通常在同一个 arena 帧里给
 * n * 4 字节，与建图的内存一起回收。
 */
cxf_result cxf_algo_toposort(const cxf_csr *g, uint32_t *indeg, uint32_t *order);

/*
 * src 能否到达 dst（含 src == dst）。visited 至少 g->n 位，queue 至少 g->n 项。
 * **会清空 visited**（复用同一块位图时不必自己清）。
 *
 * 用 BFS 而不是 DFS：BFS 的队列是顺序写，DFS 的显式栈也是顺序写，
 * 但 BFS 在"找一条最短路"时顺便给出层数；图规模小的时候差别在于可读性。
 * 两者都不递归 —— 递归深度受图规模影响，是最难查的崩溃来源之一。
 */
bool cxf_algo_reach(const cxf_csr *g, uint32_t src, uint32_t dst,
                    cxf_bitmap *visited, uint32_t *queue);

#endif
