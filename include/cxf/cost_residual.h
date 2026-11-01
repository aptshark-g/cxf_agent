#ifndef CXF_COST_RESIDUAL_H
#define CXF_COST_RESIDUAL_H
#include "cost.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cxf_cost_residual_sample {
    double x[3], residual_ms, weight;
    cxf_cost_work work;
} cxf_cost_residual_sample;
/* Optional signed residual ridge, prior=raw, lambda=.01, weight=1/max(ms,20).
 * Same three features for every profile: calls, tokens/512, attention_ms/100.
 * Fixed numeric state, no heap/context; caller serializes all access. */
typedef struct cxf_cost_residual {
    uint64_t profile;
    uint32_t count,next;
    double coefficient[3];
    double fit_mae_ms;
    cxf_cost_residual_sample samples[CXF_COST_CAPACITY];
} cxf_cost_residual;
cxf_result cxf_cost_residual_init(cxf_cost_residual *,uint64_t profile);
cxf_result cxf_cost_residual_predict(const cxf_cost_residual *,uint64_t profile,
    cxf_cost_work,double raw_ms,double raw_attention_ms,cxf_cost_prediction *);
/* Successful full work only. Invalid numeric input leaves state unchanged. */
cxf_result cxf_cost_residual_observe(cxf_cost_residual *,uint64_t profile,
    cxf_cost_work,double raw_ms,double raw_attention_ms,double actual_ms);
#ifdef __cplusplus
}
#endif
#endif
