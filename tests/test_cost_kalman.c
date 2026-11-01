#include "cxf/cost_kalman.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void){
    cxf_cost_kalman m,copy;cxf_cost_work w={0,16,64};cxf_cost_kalman_prediction p={0},before;
    assert(cxf_cost_kalman_init(&m,7,1,4)==CXF_OK);
    copy=m;assert(cxf_cost_kalman_predict(&m,7,w,100,9,&p)==CXF_OK);
    assert(p.ms==100 && p.variance_ms2==14 && fabs(p.gain-5.0/14)<1e-14);
    assert(cxf_cost_kalman_predict(&m,7,w,100,9,&p)==CXF_OK && !memcmp(&m,&copy,sizeof m));
    assert(cxf_cost_kalman_observe(&m,7,w,100,9,114)==CXF_OK);
    assert(fabs(m.bias_ms-5)<1e-14 && fabs(m.posterior_variance_ms2-45.0/14)<1e-14 && m.observations==1);
    before=p;assert(cxf_cost_kalman_predict(&m,8,w,100,9,&p)==CXF_ERR_BAD_HANDLE && !memcmp(&p,&before,sizeof p));
    copy=m;assert(cxf_cost_kalman_observe(&m,7,w,100,0,114)==CXF_ERR_RANGE && !memcmp(&m,&copy,sizeof m));
    assert(cxf_cost_kalman_observe(&m,7,w,100,9,NAN)==CXF_ERR_RANGE && !memcmp(&m,&copy,sizeof m));
    assert(cxf_cost_kalman_observe(&m,7,(cxf_cost_work){0,0,64},0,9,0)==CXF_ERR_RANGE);
    assert(cxf_cost_kalman_predict(&m,7,(cxf_cost_work){0,0,64},0,9,&p)==CXF_OK && !p.ms && !p.variance_ms2);
    assert(cxf_cost_kalman_predict(&m,7,(cxf_cost_work){UINT32_MAX,1,64},100,9,&p)==CXF_ERR_RANGE);
    double alpha=(sqrt(.01*.01+4*.01)-.01)/2,bias=0;
    assert(cxf_cost_kalman_init(&m,7,.01,alpha)==CXF_OK);
    for(int i=0;i<100;i++){
        double actual=100+(i<20?0:10)+(i%3-1);
        assert(cxf_cost_kalman_predict(&m,7,w,100,1,&p)==CXF_OK);
        assert(fabs(p.ms-(100+bias))<1e-12 && fabs(p.gain-alpha)<1e-12);
        bias+=alpha*(actual-100-bias);assert(cxf_cost_kalman_observe(&m,7,w,100,1,actual)==CXF_OK);
    }
    /* A single spike harms the following clean prediction: no universal MAE win. */
    assert(cxf_cost_kalman_init(&m,7,.01,alpha)==CXF_OK);
    assert(cxf_cost_kalman_observe(&m,7,w,100,1,200)==CXF_OK);
    assert(cxf_cost_kalman_predict(&m,7,w,100,1,&p)==CXF_OK && p.ms>109);
    assert(cxf_cost_kalman_init(&m,7,0,1)==CXF_OK);
    for(int i=0;i<9;i++)assert(cxf_cost_kalman_observe(&m,7,w,0,1,10)==CXF_OK);
    assert(fabs(m.posterior_variance_ms2-.1)<1e-12 && fabs(m.bias_ms-9)<1e-12);
    copy=m;assert(cxf_cost_kalman_init(&m,7,-1,1)==CXF_ERR_RANGE && !memcmp(&m,&copy,sizeof m));
    assert(cxf_cost_kalman_init(&m,7,0,1)==CXF_OK);
    assert(cxf_cost_kalman_observe(&m,7,w,100,1,0)==CXF_OK);
    assert(cxf_cost_kalman_predict(&m,7,w,0,1,&p)==CXF_OK && !p.ms);
    printf("PASS Kalman: analytic gain/Joseph, read-only predict, EWMA equivalence, step, spike propagation, zero-Q, clamp, profile/range and atomicity; state=%zu bytes\n",sizeof m);
    return 0;
}
