#include "cxf/cost_kalman.h"
#include <math.h>
#include <string.h>

static bool variance(double x,bool zero){return isfinite(x)&&x<=(zero?1e26:1e24)&&(zero?x>=0:x>=1e-12);}
static bool duration(double x){return isfinite(x)&&x>=0&&x<=1e12;}
cxf_result cxf_cost_kalman_init(cxf_cost_kalman *m,uint64_t profile,double q,double p){
    if(!m||!profile||!variance(q,true)||q>1e24||!variance(p,true)||p>1e24)return CXF_ERR_RANGE;
    cxf_cost_kalman next;memset(&next,0,sizeof next);next.profile=profile;next.process_variance_ms2=q;next.posterior_variance_ms2=p;*m=next;return CXF_OK;
}
cxf_result cxf_cost_kalman_predict(const cxf_cost_kalman *m,uint64_t profile,cxf_cost_work w,
    double base,double r,cxf_cost_kalman_prediction *out){
    if(!m||!out)return CXF_ERR_RANGE;
    if(!profile||m->profile!=profile)return CXF_ERR_BAD_HANDLE;
    if(!w.batch_tokens||w.new_tokens>UINT32_MAX-w.prefix_tokens||!duration(base)||!variance(r,false)||
       !variance(m->posterior_variance_ms2,true)||!variance(m->process_variance_ms2,true)||
       !isfinite(m->bias_ms)||fabs(m->bias_ms)>1e12||(!w.new_tokens&&base))return CXF_ERR_RANGE;
    cxf_cost_kalman_prediction p={0};p.observations=m->observations;
    if(w.new_tokens){
        double predicted=m->posterior_variance_ms2+m->process_variance_ms2;
        p.variance_ms2=predicted+r;p.gain=predicted/p.variance_ms2;p.bias_ms=m->bias_ms;
        p.ms=base+m->bias_ms;if(p.ms<0)p.ms=0;
        if(!isfinite(p.variance_ms2)||!isfinite(p.ms))return CXF_ERR_RANGE;
    }
    *out=p;return CXF_OK;
}
cxf_result cxf_cost_kalman_observe(cxf_cost_kalman *m,uint64_t profile,cxf_cost_work w,
    double base,double r,double actual){
    if(!m)return CXF_ERR_RANGE;
    if(!profile||m->profile!=profile)return CXF_ERR_BAD_HANDLE;
    if(!w.new_tokens||!duration(actual)||m->observations==UINT64_MAX)return CXF_ERR_RANGE;
    cxf_cost_kalman_prediction p;cxf_result rc=cxf_cost_kalman_predict(m,profile,w,base,r,&p);if(rc!=CXF_OK)return rc;
    cxf_cost_kalman next=*m;double prior=m->posterior_variance_ms2+m->process_variance_ms2;
    next.bias_ms+=p.gain*(actual-base-m->bias_ms);
    next.posterior_variance_ms2=(1-p.gain)*(1-p.gain)*prior+p.gain*p.gain*r;
    if(!isfinite(next.bias_ms)||!variance(next.posterior_variance_ms2,true))return CXF_ERR_RANGE;
    next.observations++;*m=next;return CXF_OK;
}
