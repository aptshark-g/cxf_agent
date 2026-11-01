#ifndef CXF_TABLE_H
#define CXF_TABLE_H

#include "base.h"
#include "alloc.h"
#include "handle.h"

/*
 * SoA 稀疏集：三张并行数组换三件事同时成立。
 *   dense  稠密连续行 —— 顺序遍历缓存友好，可向量化
 *   sparse 槽位到行的映射 —— handle 在 swap-remove 后依然稳定
 *   gen    世代 —— 奇数活偶数死，旧 handle 自动失效而不静默错指
 *
 * 这三件事单独看都能用更简单的结构做到，但同时做到只有稀疏集：
 * 裸数组遍历快但索引会失效；链表索引稳定但不连续；
 * 稳定数组（不 swap）浪费空间且遍历有空洞。
 *
 * 不变式（全部在 cxf_table_check 中断言）：
 *   T1  count <= cap
 *   T2  next_index <= cap；index >= next_index 的槽位从未被使用过
 *   T3  对任意 row < count：sparse[dense[row]] == row
 *   T4  对任意存活 index：sparse[index] < count
 *   T5  对任意空闲 index：sparse[index] 是 free 链表后继或 CXF_NO_INDEX
 *   T6  gen[index] 为奇数当且仅当该槽位存活
 *   T7  dense[0..count-1] 中的 index 两两不同
 *   T8  所有已投用槽位（i < next_index）的 gen 高位 == class_id；
 *       class_id <= CXF_HANDLE_CLASS_MAX（0 = 未标注，见 handle.h H4）
 *
 * 遍历中删除：倒序 for (uint32_t i = t->count; i-- > 0;) 是安全的。
 * swap-remove 只搬动末尾行，而倒序遍历时末尾行已经处理完毕。
 * 正序遍历中删除会漏掉被搬到当前位置的那一行 —— 这是唯一需要注意的坑。
 *
 * 复杂度：
 *   add / remove / get / live   O(1)
 *   容量增长                     O(count * col_count)，倍增摊还后 O(1)
 *   顺序遍历                     O(count)，缓存友好
 *
 * 代价：每行额外 12 字节（sparse + dense + gen），
 * 且 swap-remove 会打乱行顺序 —— 需要稳定顺序时用间接索引排序，别改 remove。
 *
 * 不适用：列数上限是编译期常量 CXF_TABLE_MAX_COLS，不是运行时可配置。
 * 这是有意的：列数组是值类型内联在结构体里，避免每次访问多一次间接加载。
 *
 * 上限取 16 而不是 8：soa.h 的自动布局会把结构体的每个叶子字段摊成一列，
 * 8 列对稍复杂的结构体不够。代价是 cxf_table 结构体本身变大，
 * 但它不常访问 —— 热路径访问的是列数据，不是这个结构体。
 * 超过 16 列请拆表，别调这个数。
 */
#define CXF_TABLE_MAX_COLS 16

typedef struct cxf_table {
    cxf_alloc *alloc;
    uint8_t   *cols[CXF_TABLE_MAX_COLS];  /* 每列独立分配，列内连续 */
    size_t     col_size[CXF_TABLE_MAX_COLS];
    uint32_t   col_count;

    uint32_t   count;       /* 存活行数，见 T1 */
    uint32_t   cap;         /* 槽位容量，也是行数上限 */
    uint32_t   next_index;  /* 从未使用过的下一个槽位，见 T2 */
    uint32_t   free_head;   /* 回收槽位链表头，用 sparse 存后继，见 T5 */

    /* 本表的型别：写进本表产出的每张 handle 的高 8 位，见 T8 / handle.h H3。
     * 装配期分配，运行期不变；0 = 未标注（不提供跨表保护）。*/
    uint32_t   class_id;

    uint32_t  *sparse;
    uint32_t  *dense;
    uint32_t  *gen;
} cxf_table;

/*
 * 建表。col_sizes 有 col_count 个，每个都必须 > 0。
 * cap 是初始槽位容量，不是硬上限 —— 满了会自动倍增。
 * 返回 CXF_ERR_NOMEM 时表未初始化，调用方不必清理。
 */
cxf_result cxf_table_init(cxf_table *t, cxf_alloc *alloc,
                          const size_t *col_sizes, uint32_t col_count,
                          uint32_t cap);

/*
 * 同上，但指定本表的 class（1..255；0 = 未标注，与 cxf_table_init 等价）。
 * class 写进本表产出的每张 handle 的高 8 位，于是"把 A 表的 handle 喂给 B 表"
 * 从概率拦截变成确定性拒绝（handle.h H3）。装配期决定、运行期不变。
 * 不破 cxf_table_init 签名是有意的：生成物不必为这一个字段重新生成。
 */
cxf_result cxf_table_init_class(cxf_table *t, cxf_alloc *alloc,
                                const size_t *col_sizes, uint32_t col_count,
                                uint32_t cap, uint32_t class_id);

/* 释放全部列与三张索引数组。之后 t 被清零，可安全再次 init。 */
void       cxf_table_release(cxf_table *t);

/*
 * 预留到至少 cap 个槽位。
 * 全部新内存分配成功后才替换旧指针，因此失败时表完全不变，不是部分增长状态。
 */
cxf_result cxf_table_reserve(cxf_table *t, uint32_t cap);

/*
 * 清空所有行，并把所有已用过槽位的 gen 推到偶数，使既有 handle 全部失效。
 * 内存不释放，容量保留 —— 这是"重置而不重建"的路径。
 */
void       cxf_table_clear(cxf_table *t);

/*
 * 新增一行，返回其 handle。列内容未初始化，调用方自行写入。
 * 失败（OOM）时返回 gen == 0 的无效 handle —— 用 cxf_handle_null 判定，
 * 不要用 cxf_table_live，因为失败时根本没产生槽位。
 */
cxf_handle cxf_table_add(cxf_table *t);

/*
 * 删除一行，swap-remove。返回 CXF_ERR_BAD_HANDLE 表示 handle 已失效或不属于本表。
 * 注意：此操作会让其它行的行号改变，但它们的 handle 不变（这正是 sparse 的作用）。
 * 正在遍历时请倒序，见文件头说明。
 */
cxf_result cxf_table_remove(cxf_table *t, cxf_handle h);

/* handle 是否仍指向本表的存活行。O(1)：一次精确比较。
 * gen 已含 class 位，所以跨表 handle 必然不等 —— 那个分支不必额外写出来。 */
bool       cxf_table_live(const cxf_table *t, cxf_handle h);

/* handle 解引用为稠密行号。前置：live 为真（debug 下断言）。用于批量访问。 */
uint32_t   cxf_table_row(const cxf_table *t, cxf_handle h);

/*
 * 取某列在某行的元素地址。handle 失效时返回 NULL，不断言 —— 
 * 因为失效在热插拔下是正常事件，不是程序员错误。
 */
void      *cxf_table_get(cxf_table *t, cxf_handle h, uint32_t col);

/* 取整列基址，用于顺序遍历。配合 cxf_table_row 做随机访问。 */
void      *cxf_table_col(cxf_table *t, uint32_t col);

/* 行号反查 handle。前置：row < count。遍历时用它在行与 handle 之间转换。 */
cxf_handle cxf_table_at(const cxf_table *t, uint32_t row);

/* 校验 T1-T7，测试与调试期调用。NDEBUG 下编译为常量 true。 */
#ifndef NDEBUG
bool cxf_table_check(const cxf_table *t);
#else
#define cxf_table_check(t) ((void)(t), true)
#endif

#endif
