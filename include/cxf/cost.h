#ifndef CXF_COST_H
#define CXF_COST_H
#include "base.h"
#ifdef __cplusplus
extern "C" {
#endif

#define CXF_COST_CAPACITY 64
#define CXF_COST_FEATURES 3

typedef struct cxf_cost_work {
    uint32_t prefix_tokens, new_tokens, batch_tokens;
} cxf_cost_work;
typedef struct cxf_cost_sample {
    double x[CXF_COST_FEATURES], ms;
    cxf_cost_work work;
} cxf_cost_sample;
/* Bounded numerical state, no heap or context text. Caller serializes access.
 * profile is caller-derived from hardware/model/backend/execution settings;
 * it is an identity tag, not a security hash or a throughput specification. */
typedef struct cxf_cost_model {
    uint64_t profile;
    uint32_t count, next;
    double coefficient[CXF_COST_FEATURES];
    double fit_mae_ms;
    cxf_cost_sample samples[CXF_COST_CAPACITY];
} cxf_cost_model;
typedef struct cxf_cost_prediction {
    double ms, fit_mae_ms; /* residual on the fitting window, NOT a confidence bound */
    uint32_t observations;
    bool extrapolated;
} cxf_cost_prediction;

cxf_result cxf_cost_init(cxf_cost_model *, uint64_t profile);
/* Zero work -> zero. Nonzero work without observations -> EMPTY.
 * Profile mismatch -> BAD_HANDLE. Invalid inputs leave output/state unchanged.
 * Work means a complete flush with this batch cap; not arbitrary fragmented calls. */
cxf_result cxf_cost_predict(const cxf_cost_model *, uint64_t profile,
                            cxf_cost_work, cxf_cost_prediction *);
/* Only successful completed service time (ms), excluding queue and model load.
 * Invalid/NaN/infinite/negative observations do not change model state. */
cxf_result cxf_cost_observe(cxf_cost_model *, uint64_t profile, cxf_cost_work, double ms);
#ifdef __cplusplus
}
#endif
#endif
