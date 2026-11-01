#ifndef CXF_NAMEMAP_H
#define CXF_NAMEMAP_H

#include "arena.h"
#include "base.h"
#include "str.h"

/*
 * 名字 → id 索引：**排序数组 + 二分**，不是哈希表。
 *
 * 这不是省事，是按访问模式选的：注册表是"装配期批量插入、运行期只读"。
 * 哈希表的三个机制（冲突探测、重哈希、装填因子）在只读场景里全是纯成本；
 * 而二分的每一跳地址都能提前算出 —— 连续数组，预取友好，正是"指针链"那类
 * 最贵形状的反面（见项目记忆里那条实测：随机指针链比顺序访问慢两个量级）。
 * 另外排序数组天生支持前缀枚举：lower_bound 之后顺序扫，不需要额外结构。
 *
 * 所以删除只有一种做法：**重建**。注册期规模是"几十到几百"，重建比增量维护便宜，
 * 也避免了墓碑与压缩这两种只在动态哈希里才需要的状态。
 *
 * 存储用 arena：注册期一次性的结构，生命周期与世界同级；
 * 扩容是"另申请一块更大的再复制"，旧块随 arena 一起消失（没有 free 这个动作）。
 *
 * 不变式：
 *   N1  ents[0..count-1] 按 name 字典序**严格递增**（无重复名字）
 *   N2  count <= cap；cap == 0 时 ents 可为 NULL
 *   N3  name 是视图：生命周期由调用方保证（通常是 rodata 字面量或 arena 内的字节）
 *   N4  id 的稠密性不由本结构保证 —— 名字索引与 id 分配是两件事（见 §4.6 的三个 id 空间）
 *
 * 复杂度：find O(log n)；add O(n)（二分 + 搬移），注册期专用；
 * 扩容 O(n) 且按 1.5 倍增长；check O(n)。
 *
 * 不适用：不支持删除（重建即可）；不是哈希表（没有 O(1) 期望查找，n 到万级才需要换）；
 * 不做字符串驻留（name 不复制，重复字节由调用方复用）。
 */
typedef struct cxf_namemap_ent {
    cxf_str  name;
    uint32_t id;
} cxf_namemap_ent;

typedef struct cxf_namemap {
    cxf_namemap_ent *ents;
    uint32_t         count;
    uint32_t         cap;
} cxf_namemap;

void cxf_namemap_init(cxf_namemap *m);

/* 插入一个新名字。名字已存在时返回 false 且不改动（重复注册是错误，不是覆盖）。
 * 失败（arena 取不到内存）同样返回 false —— 注册期的失败一律是"参数/资源错误"，
 * 不需要区分，调用方本来就要中止装配。 */
bool cxf_namemap_add(cxf_namemap *m, cxf_arena *a, cxf_str name, uint32_t id);

/* 查名字。命中写 *id 并返回 true；未命中不改 *id。 */
bool cxf_namemap_find(const cxf_namemap *m, cxf_str name, uint32_t *id);

/* 前缀枚举的起点：第一个 >= key 的下标（0..count）。要枚举前缀 p，
 * 从 cxf_namemap_lower_bound(m, p) 开始顺序扫到 starts_with 失败为止。 */
uint32_t cxf_namemap_lower_bound(const cxf_namemap *m, cxf_str key);

static inline uint32_t cxf_namemap_count(const cxf_namemap *m) {
    return m->count;
}

/* N1 的调试期断言。 */
#ifndef NDEBUG
bool cxf_namemap_check(const cxf_namemap *m);
#else
#define cxf_namemap_check(m) ((void)(m), true)
#endif

#endif
