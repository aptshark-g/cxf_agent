#ifndef CXF_COST_KALMAN_H
#define CXF_COST_KALMAN_H
#include "cost.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Scalar random-walk residual bias. Q is ms^2 per accepted observation;
 * R is supplied per call. A covariance is a model assumption, not a proven
 * Gaussian confidence interval. Caller keys profile to the unfiltered base. */
typedef struct cxf_cost_kalman {
    uint64_t profile, observations;
    double bias_ms, posterior_variance_ms2, process_variance_ms2;
} cxf_cost_kalman;
typedef struct cxf_cost_kalman_prediction {
    double ms, bias_ms, variance_ms2, gain;
    uint64_t observations;
} cxf_cost_kalman_prediction;
cxf_result cxf_cost_kalman_init(cxf_cost_kalman *,uint64_t profile,double q_ms2,double initial_p_ms2);
/* Read-only. Repeated predictions do not advance Q. Zero work returns zero. */
cxf_result cxf_cost_kalman_predict(const cxf_cost_kalman *,uint64_t profile,cxf_cost_work,
    double unfiltered_base_ms,double r_ms2,cxf_cost_kalman_prediction *);
/* Successful nonzero work only, using the SAME UNFILTERED base used before
 * execution. Joseph covariance update; invalid input preserves the state. */
cxf_result cxf_cost_kalman_observe(cxf_cost_kalman *,uint64_t profile,cxf_cost_work,
    double unfiltered_base_ms,double r_ms2,double actual_ms);
#ifdef __cplusplus
}
#endif
#endif
