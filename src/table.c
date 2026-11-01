#include <string.h>

#include "cxf/table.h"

/* 契约与不变式见 table.h，这里只写机制上的"为什么"。 */

static const cxf_handle null_handle = { 0, 0 };

void cxf_table_release(cxf_table *t) {
    if (t->alloc == NULL) return;

    for (uint32_t i = 0; i < t->col_count; i++) {
        if (t->cols[i] != NULL)
            cxf_alloc_free(t->alloc, t->cols[i], (size_t)t->cap * t->col_size[i]);
    }
    if (t->sparse != NULL) cxf_alloc_free(t->alloc, t->sparse, (size_t)t->cap * 4);
    if (t->dense  != NULL) cxf_alloc_free(t->alloc, t->dense,  (size_t)t->cap * 4);
    if (t->gen    != NULL) cxf_alloc_free(t->alloc, t->gen,    (size_t)t->cap * 4);
    memset(t, 0, sizeof(*t));
}

/*
 * 增长到 cap。策略是"先全部申请成功，再统一替换"：
 * 中途任何一次失败都走 fail 分支释放新块，旧指针原封不动。
 * 这样调用方看到 CXF_ERR_NOMEM 时表仍是可用的旧状态，而不是半截的坏状态。
 */
static cxf_result table_reserve_to(cxf_table *t, uint32_t cap) {
    uint8_t  *ncols[CXF_TABLE_MAX_COLS] = { 0 };
    uint32_t *nsparse = NULL, *ndense = NULL, *ngen = NULL;

    if (cap <= t->cap) return CXF_OK;

    for (uint32_t i = 0; i < t->col_count; i++) {
        ncols[i] = cxf_alloc_alloc(t->alloc, (size_t)cap * t->col_size[i], CXF_MIN_ALIGN);
        if (ncols[i] == NULL) goto fail;
    }
    nsparse = cxf_alloc_alloc(t->alloc, (size_t)cap * 4, CXF_MIN_ALIGN);
    ndense  = cxf_alloc_alloc(t->alloc, (size_t)cap * 4, CXF_MIN_ALIGN);
    ngen    = cxf_alloc_alloc(t->alloc, (size_t)cap * 4, CXF_MIN_ALIGN);
    if (nsparse == NULL || ndense == NULL || ngen == NULL) goto fail;

    /* 列只需搬前 count 行；dense 同理。sparse 与 gen 按旧 cap 整段搬，
     * 因为槽位状态覆盖到 next_index <= cap。 */
    for (uint32_t i = 0; i < t->col_count; i++) {
        if (t->cols[i] != NULL)
            memcpy(ncols[i], t->cols[i], (size_t)t->count * t->col_size[i]);
    }
    if (t->sparse != NULL) memcpy(nsparse, t->sparse, (size_t)t->cap * 4);
    if (t->dense  != NULL) memcpy(ndense,  t->dense,  (size_t)t->count * 4);
    if (t->gen    != NULL) memcpy(ngen,    t->gen,    (size_t)t->cap * 4);

    /* 新槽位：sparse 置空链尾；gen 预置 class 位，计数器 0（偶数 ⇒ 死，等于"未用"）。
     * 预置而不是首次 add 时补，是为了让"gen 高位 == class_id"成为**无条件**不变式（T8），
     * 也省掉 add 路径上的一次判断。 */
    for (uint32_t j = t->cap; j < cap; j++) {
        nsparse[j] = CXF_NO_INDEX;
        ngen[j] = t->class_id << CXF_HANDLE_GEN_BITS;
    }

    for (uint32_t i = 0; i < t->col_count; i++) {
        if (t->cols[i] != NULL)
            cxf_alloc_free(t->alloc, t->cols[i], (size_t)t->cap * t->col_size[i]);
        t->cols[i] = ncols[i];
    }
    if (t->sparse != NULL) cxf_alloc_free(t->alloc, t->sparse, (size_t)t->cap * 4);
    if (t->dense  != NULL) cxf_alloc_free(t->alloc, t->dense,  (size_t)t->cap * 4);
    if (t->gen    != NULL) cxf_alloc_free(t->alloc, t->gen,    (size_t)t->cap * 4);

    t->sparse = nsparse;
    t->dense  = ndense;
    t->gen    = ngen;
    t->cap    = cap;
    return CXF_OK;

fail:
    for (uint32_t i = 0; i < t->col_count; i++)
        if (ncols[i] != NULL) cxf_alloc_free(t->alloc, ncols[i], (size_t)cap * t->col_size[i]);
    if (nsparse != NULL) cxf_alloc_free(t->alloc, nsparse, (size_t)cap * 4);
    if (ndense  != NULL) cxf_alloc_free(t->alloc, ndense,  (size_t)cap * 4);
    if (ngen    != NULL) cxf_alloc_free(t->alloc, ngen,    (size_t)cap * 4);
    return CXF_ERR_NOMEM;
}

cxf_result cxf_table_init(cxf_table *t, cxf_alloc *alloc,
                          const size_t *col_sizes, uint32_t col_count,
                          uint32_t cap) {
    return cxf_table_init_class(t, alloc, col_sizes, col_count, cap,
                                CXF_HANDLE_CLASS_NONE);
}

cxf_result cxf_table_init_class(cxf_table *t, cxf_alloc *alloc,
                                const size_t *col_sizes, uint32_t col_count,
                                uint32_t cap, uint32_t class_id) {
    CXF_ASSERT(col_count > 0 && col_count <= CXF_TABLE_MAX_COLS);
    CXF_ASSERT(col_sizes != NULL);
    CXF_ASSERT(class_id <= CXF_HANDLE_CLASS_MAX);

    memset(t, 0, sizeof(*t));
    t->alloc = alloc;
    t->col_count = col_count;
    t->free_head = CXF_NO_INDEX;
    t->class_id = class_id & CXF_HANDLE_CLASS_MAX;
    for (uint32_t i = 0; i < col_count; i++) {
        CXF_ASSERT(col_sizes[i] > 0);
        t->col_size[i] = col_sizes[i];
    }
    return table_reserve_to(t, cap);
}

cxf_result cxf_table_reserve(cxf_table *t, uint32_t cap) {
    return table_reserve_to(t, cap);
}

/*
 * 取一个槽位：优先复用 free 链表，其次用从未分配过的 next_index。
 * count < cap 时必有其一可用（存活 + 空闲 + 未使用 == cap），
 * 所以"取不到"只可能是容量增长失败。
 * gen 自增使奇偶翻转：0->1 首次存活，或偶数（已回收）->奇数（再存活）。
 */
cxf_handle cxf_table_add(cxf_table *t) {
    uint32_t index, row;

    if (t->count >= t->cap) {
        uint32_t grow = t->cap != 0 ? t->cap * 2 : 8;
        if (table_reserve_to(t, grow) != CXF_OK) return null_handle;
    }

    if (t->free_head != CXF_NO_INDEX) {
        index = t->free_head;
        t->free_head = t->sparse[index];
    } else {
        index = t->next_index++;
    }

    t->gen[index] = cxf_handle_gen_next(t->gen[index]);  /* 只翻低 24 位，class 位不动 */
    row = t->count++;
    t->sparse[index] = row;
    t->dense[row] = index;
    return cxf_handle_make(index, t->gen[index]);
}

/*
 * 一次比较搞定三重判定：下标在界内、gen 匹配、gen 为奇数（存活）。
 * gen 匹配已经蕴含"不是旧 handle"，奇偶位再挡住理论上可能的边界情况。
 */
bool cxf_table_live(const cxf_table *t, cxf_handle h) {
    return h.index < t->cap && (h.gen & 1) == 1 && t->gen[h.index] == h.gen;
}

uint32_t cxf_table_row(const cxf_table *t, cxf_handle h) {
    CXF_ASSERT(cxf_table_live(t, h));
    return t->sparse[h.index];
}

/*
 * swap-remove：把末行搬到被删行的位置，再收缩 count。
 * 搬行后必须同步 sparse —— 这正是 handle 在行号变化时依然有效的原因。
 * 之后 gen 自增翻成偶数，旧 handle 从此失效。
 */
cxf_result cxf_table_remove(cxf_table *t, cxf_handle h) {
    uint32_t row, last;

    if (!cxf_table_live(t, h)) return CXF_ERR_BAD_HANDLE;

    row = t->sparse[h.index];
    last = t->count - 1;

    if (row != last) {
        uint32_t moved = t->dense[last];
        for (uint32_t i = 0; i < t->col_count; i++) {
            memcpy(t->cols[i] + (size_t)row * t->col_size[i],
                   t->cols[i] + (size_t)last * t->col_size[i],
                   t->col_size[i]);
        }
        t->dense[row] = moved;
        t->sparse[moved] = row;
    }

    t->count = last;
    t->gen[h.index] = cxf_handle_gen_next(t->gen[h.index]);
    t->sparse[h.index] = t->free_head;
    t->free_head = h.index;
    return CXF_OK;
}

void *cxf_table_get(cxf_table *t, cxf_handle h, uint32_t col) {
    CXF_ASSERT(col < t->col_count);
    if (!cxf_table_live(t, h)) return NULL;
    return t->cols[col] + (size_t)t->sparse[h.index] * t->col_size[col];
}

void *cxf_table_col(cxf_table *t, uint32_t col) {
    CXF_ASSERT(col < t->col_count);
    return t->cols[col];
}

cxf_handle cxf_table_at(const cxf_table *t, uint32_t row) {
    uint32_t index;
    CXF_ASSERT(row < t->count);
    index = t->dense[row];
    return cxf_handle_make(index, t->gen[index]);
}

/*
 * 清空但不释放。所有存活槽位的 gen 推到偶数，让既有 handle 全部失效 ——
 * 这与 remove 的行为保持一致：失效可被检测，而不是静默复用。
 */
void cxf_table_clear(cxf_table *t) {
    for (uint32_t i = 0; i < t->next_index; i++) {
        if ((t->gen[i] & 1) == 1) t->gen[i] = cxf_handle_gen_next(t->gen[i]);
        t->sparse[i] = CXF_NO_INDEX;
    }
    t->count = 0;
    t->next_index = 0;
    t->free_head = CXF_NO_INDEX;
}

#ifndef NDEBUG
/* 守住 table.h 里声明的 T1-T8。NDEBUG 下整个不编译。 */
bool cxf_table_check(const cxf_table *t) {
    uint32_t live = 0, free_n = 0;

    if (t == NULL) return false;
    if (t->count > t->cap) return false;                              /* T1 */
    if (t->next_index > t->cap) return false;                         /* T2 */
    if (t->col_count == 0 || t->col_count > CXF_TABLE_MAX_COLS) return false;
    if (t->class_id > CXF_HANDLE_CLASS_MAX) return false;              /* T8 */
    if (t->cap != 0 && (t->sparse == NULL || t->dense == NULL || t->gen == NULL))
        return false;

    for (uint32_t row = 0; row < t->count; row++) {
        uint32_t idx = t->dense[row];
        if (idx >= t->cap) return false;
        if (t->sparse[idx] != row) return false;                      /* T3 */
        if ((t->gen[idx] & 1) == 0) return false;                     /* T6 */
    }

    for (uint32_t i = 0; i < t->next_index; i++) {
        if ((t->gen[i] >> CXF_HANDLE_GEN_BITS) != t->class_id) return false; /* T8 */
        if ((t->gen[i] & 1) == 1) {
            if (t->sparse[i] >= t->count) return false;               /* T4 */
            live++;
        }
    }
    if (live != t->count) return false;

    /* free 链表：无环、只含已死槽位、长度与存活数互补（T5） */
    for (uint32_t i = t->free_head; i != CXF_NO_INDEX; i = t->sparse[i]) {
        if (i >= t->cap) return false;
        if ((t->gen[i] & 1) == 1) return false;
        if (++free_n > t->cap) return false;
    }
    if (t->next_index != live + free_n) return false;

    return true;
}
#endif
