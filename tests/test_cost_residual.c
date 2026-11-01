#include "cxf/cost_residual.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void){
    cxf_cost_residual m,copy;cxf_cost_prediction p={0};cxf_cost_work w={0,512,512};
    assert(cxf_cost_residual_init(&m,7)==CXF_OK);
    assert(cxf_cost_residual_predict(&m,7,w,50,0,&p)==CXF_ERR_EMPTY);
    assert(cxf_cost_residual_predict(&m,8,w,50,0,&p)==CXF_ERR_BAD_HANDLE);
    assert(cxf_cost_residual_predict(&m,7,(cxf_cost_work){0,0,512},0,0,&p)==CXF_OK && !p.ms);
    copy=m;
    assert(cxf_cost_residual_observe(&m,7,w,50,51,60)==CXF_ERR_RANGE && !memcmp(&m,&copy,sizeof m));
    assert(cxf_cost_residual_observe(&m,7,w,50,0,NAN)==CXF_ERR_RANGE && !memcmp(&m,&copy,sizeof m));
    assert(cxf_cost_residual_observe(&m,7,(cxf_cost_work){UINT32_MAX,1,1},50,0,60)==CXF_ERR_RANGE);
    assert(cxf_cost_residual_observe(&m,7,w,50,0,60)==CXF_OK);
    assert(cxf_cost_residual_predict(&m,7,w,50,0,&p)==CXF_OK);
    /* Two identical normalized columns: exact ridge beta = 10/(2+.01). */
    assert(fabs(p.ms-(50+20/2.01))<1e-8);
    assert(fabs(m.coefficient[0]-10/2.01)<1e-8 && m.coefficient[2]==0);
    for(int i=0;i<80;i++)assert(cxf_cost_residual_observe(&m,7,w,50,0,40)==CXF_OK);
    assert(m.count==CXF_COST_CAPACITY && cxf_cost_residual_predict(&m,7,w,50,0,&p)==CXF_OK);
    assert(fabs(p.ms-(50-20/2.01))<1e-8 && m.coefficient[0]<0);
    assert(cxf_cost_residual_predict(&m,7,w,0,0,&p)==CXF_OK && p.ms==0); /* nonnegative output clamp */
    assert(cxf_cost_residual_predict(&m,7,(cxf_cost_work){1,512,512},50,0,&p)==CXF_OK && p.extrapolated);
    cxf_cost_prediction before=p;
    assert(cxf_cost_residual_predict(&m,7,w,INFINITY,0,&p)==CXF_ERR_RANGE && !memcmp(&p,&before,sizeof p));
    copy=m;assert(cxf_cost_residual_observe(&m,7,w,50,0,-1)==CXF_ERR_RANGE && !memcmp(&m,&copy,sizeof m));
    printf("PASS residual: analytic ridge/collinearity, zero column, signed correction, bounded window/drift, profiles, zero work, clamp and failure atomicity; state=%zu bytes\n",sizeof m);
    return 0;
}
