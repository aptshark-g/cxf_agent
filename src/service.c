#include <string.h>

#include "cxf/service.h"
#include "cxf/vec.h"
#include "cxf/world.h"

/* 装配期字符串查找，线性扫描。低频路径，不做哈希 —— 见 service.h 的三阶段纪律。 */

static bool service_taken(const cxf_world *w, const char *name, uint32_t ver) {
    for (uint32_t i = 0; i < w->service_count; i++)
        if (w->services[i].api_version == ver &&
            strcmp(w->services[i].name, name) == 0)
            return true;
    return false;
}

static cxf_result service_reserve(cxf_world *w, uint32_t need) {
    /* 增长只有一份实现（cxf_vec_grow）：倍增、溢出、复制、释放大小都不在这里判。
     * 指针与容量一起写回，因此不可能出现"用 cap 申请、用 count 释放"。 */
    void *p = w->services;
    uint32_t cap = w->service_cap;
    cxf_result rc = cxf_vec_grow(w->alloc, &p, &cap, w->service_count, need,
                                 (uint32_t)sizeof(cxf_service_def));

    if (rc != CXF_OK) return rc;
    w->services = (cxf_service_def *)p;
    w->service_cap = cap;
    return CXF_OK;
}

cxf_result cxf_service_register(cxf_world *w, const char *name,
                                uint32_t api_version, const void *api) {
    cxf_result r;

    if (service_taken(w, name, api_version)) return CXF_ERR_RANGE;   /* S1 */

    r = service_reserve(w, w->service_count + 1);
    if (r != CXF_OK) return r;

    w->services[w->service_count].name = name;
    w->services[w->service_count].api_version = api_version;
    w->services[w->service_count].api = api;
    w->service_count++;
    return CXF_OK;
}

cxf_result cxf_service_register_bundle(cxf_world *w,
                                       const cxf_service_bundle *b) {
    cxf_result r;

    CXF_ASSERT(w != NULL && b != NULL && b->entries != NULL);

    /* 先整体校验再提交（S3）：服务表只增不减，失败不能留下半张表。 */
    for (uint32_t i = 0; i < b->count; i++) {
        const cxf_service_def *e = b->entries[i];
        if (e == NULL || e->name == NULL || e->api == NULL)
            return CXF_ERR_RANGE;
        if (service_taken(w, e->name, e->api_version)) return CXF_ERR_RANGE;  /* S1 */
        for (uint32_t j = 0; j < i; j++)
            if (b->entries[j]->api_version == e->api_version &&
                strcmp(b->entries[j]->name, e->name) == 0)
                return CXF_ERR_RANGE;                            /* S1 */
    }

    r = service_reserve(w, w->service_count + b->count);
    if (r != CXF_OK) return r;

    for (uint32_t i = 0; i < b->count; i++) {
        w->services[w->service_count] = *b->entries[i];
        w->service_count++;
    }
    return CXF_OK;
}

const void *cxf_service_find(const cxf_world *w, const char *name,
                             uint32_t api_version) {
    CXF_ASSERT(w != NULL && name != NULL);

    /* 与 type 表同款槽位：让"调用期零查找"可观测，而不是一条注释。 */
    ((cxf_world *)w)->service_find_count++;

    for (uint32_t i = 0; i < w->service_count; i++) {
        if (strcmp(w->services[i].name, name) == 0 &&
            w->services[i].api_version == api_version)   /* S2 */
            return w->services[i].api;
    }
    return NULL;
}
