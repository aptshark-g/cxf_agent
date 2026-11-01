#ifndef CXF_COST_INTERNAL_H
#define CXF_COST_INTERNAL_H
#include "cxf/cost.h"
/* Private numerical fit reuse; each caller uses a distinct model/profile. */
cxf_result cxf_cost_predict_vector(const cxf_cost_model *, uint64_t,
    cxf_cost_work, const double x[3], cxf_cost_prediction *);
cxf_result cxf_cost_observe_vector(cxf_cost_model *, uint64_t,
    cxf_cost_work, const double x[3], double ms);
#endif
