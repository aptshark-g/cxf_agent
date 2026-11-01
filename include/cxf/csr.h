#ifndef CXF_CSR_H
#define CXF_CSR_H

#include "base.h"

/*
 * 只读图：CSR（压缩稀疏行）= off[n+1] + adj[m]。
 * 节点 i 的出边就是区间 adj[off[i] .. off[i+1]) —— 没有 next 指针。
 *
 * 为什么必须是 CSR 而不是邻接链表：链表的遍历地址是**运行时数据**
 * （"下一条在哪"要等上一次 load），正是实测里最贵的形状（指针链比顺序访问慢两个量级）。
 * 换成区间后，每条边的位置可以提前算出来 ⇒ 预取与向量化都成立。
 * 顺带消掉一类注册期 bug：链的节点编址一旦算错就变成自环，遍历挂死；
 * 区间没有"链"，算错只会是越界，能被 check 抓住。
 *
 * 生命周期：**视图**。内存在调用方（通常是 arena 帧或世界级 arena），
 * 本结构只切分它。建好后只读 —— 要改图请重建（增删边会改变所有后续偏移）。
 *
 * 不变式：
 *   C1  off[0] == 0，off[n] == m，且 off 单调不减
 *   C2  adj[0..m-1] 全部 < n（建图时校验，越界边直接拒绝）
 *   C3  同一节点的出边按**输入顺序**排列（确定性：同样的边集在任何构建顺序下
 *       只取决于输入序，这样拓扑序与渲染都可复现）
 *
 * 复杂度：建图 O(n + m)，**零临时数组**（两遍计数 + 逆序填 + 左移，见 csr.c）；
 * 查询 O(1) 取区间；遍历 O(deg)。
 *
 * 不适用：不支持增删边（只读）；不存边权（要权就在 adj 旁边再开一列，别改这里）；
 * 不做去重（重边会被保留 —— 拓扑排序对重边无所谓，但计数类算法要注意）。
 */
typedef struct cxf_csr_edge {
    uint32_t from;
    uint32_t to;
} cxf_csr_edge;

typedef struct cxf_csr {
    uint32_t *off;   /* n + 1 项 */
    uint32_t *adj;   /* m 项 */
    uint32_t  n;
    uint32_t  m;
} cxf_csr;

/* 容纳 n 节点 m 边所需的字节数（off 在前、adj 在后）。 */
static inline uint32_t cxf_csr_bytes(uint32_t n, uint32_t m) {
    return (n + 1u) * 4u + m * 4u;
}

/* 切分内存并把 off 清零（空图，m == 0）。mem 必须 4 字节对齐。 */
void cxf_csr_init(cxf_csr *g, void *mem, uint32_t n, uint32_t m);

/*
 * 由边表建图。mem 至少 cxf_csr_bytes(n, m) 字节。
 * 边端点 >= n 时返回 CXF_ERR_RANGE（装配期响亮失败，不静默丢弃）。
 */
cxf_result cxf_csr_from_edges(cxf_csr *g, void *mem, uint32_t n,
                              const cxf_csr_edge *edges, uint32_t m);

/* 反向图：把每条边 (u,v) 当作 (v,u) 建。查"谁指向我"用它，不另造结构。 */
cxf_result cxf_csr_from_edges_rev(cxf_csr *g, void *mem, uint32_t n,
                                  const cxf_csr_edge *edges, uint32_t m);

static inline uint32_t cxf_csr_deg(const cxf_csr *g, uint32_t i) {
    return g->off[i + 1u] - g->off[i];
}

/* 节点 i 的第 k 条出边终点。前置：k < cxf_csr_deg(g, i)。 */
static inline uint32_t cxf_csr_at(const cxf_csr *g, uint32_t i, uint32_t k) {
    return g->adj[g->off[i] + k];
}

#ifndef NDEBUG
bool cxf_csr_check(const cxf_csr *g);
#else
#define cxf_csr_check(g) ((void)(g), true)
#endif

#endif
