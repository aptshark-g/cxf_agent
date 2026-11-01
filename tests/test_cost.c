#include "cxf/cost.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static double absd(double x){return x<0?-x:x;}
static double known(cxf_cost_work w) {
    double pairs=(double)w.prefix_tokens*w.new_tokens+(double)w.new_tokens*(w.new_tokens-1)/2;
    return 2.0*((w.new_tokens+w.batch_tokens-1)/w.batch_tokens)+80.0*w.new_tokens/512.0+3.0*pairs/1048576;
}
int main(void) {
    cxf_cost_model m,before;cxf_cost_prediction p={0},sentinel={123,456,789,true};
    cxf_cost_work w={1024,128,512};
    assert(cxf_cost_init(&m,17)==CXF_OK);
    p=sentinel;assert(cxf_cost_predict(&m,17,w,&p)==CXF_ERR_EMPTY);assert(!memcmp(&p,&sentinel,sizeof p));
    assert(cxf_cost_predict(&m,18,w,&p)==CXF_ERR_BAD_HANDLE);
    before=m;assert(cxf_cost_observe(&m,17,w,NAN)==CXF_ERR_RANGE);assert(!memcmp(&m,&before,sizeof m));
    assert(cxf_cost_observe(&m,17,w,INFINITY)==CXF_ERR_RANGE);assert(!memcmp(&m,&before,sizeof m));
    assert(cxf_cost_observe(&m,17,w,-1)==CXF_ERR_RANGE);assert(!memcmp(&m,&before,sizeof m));
    assert(cxf_cost_observe(&m,18,w,10)==CXF_ERR_BAD_HANDLE);assert(!memcmp(&m,&before,sizeof m));
    assert(cxf_cost_predict(&m,17,(cxf_cost_work){UINT32_MAX,1,512},&p)==CXF_ERR_RANGE);
    assert(cxf_cost_predict(&m,17,(cxf_cost_work){0,1,0},&p)==CXF_ERR_RANGE);
    assert(cxf_cost_predict(&m,17,(cxf_cost_work){100,0,512},&p)==CXF_OK && p.ms==0);
    for(int r=0;r<4;r++)for(uint32_t prefix=0;prefix<=6144;prefix+=2048)
        for(uint32_t n=32;n<=512;n*=4) {
            cxf_cost_work t={prefix,n,512};assert(cxf_cost_observe(&m,17,t,known(t))==CXF_OK);
        }
    for(uint32_t prefix=1024;prefix<=5120;prefix+=2048)for(uint32_t n=64;n<=256;n*=4) {
        cxf_cost_work t={prefix,n,512};assert(cxf_cost_predict(&m,17,t,&p)==CXF_OK);
        assert(absd(p.ms-known(t))/known(t)<0.02 && !p.extrapolated);
    }
    assert(cxf_cost_predict(&m,17,(cxf_cost_work){8192,768,512},&p)==CXF_OK && p.extrapolated);
    /* Entirely replace the window after a speed regime change; no stale sum. */
    for(int i=0;i<CXF_COST_CAPACITY;i++)assert(cxf_cost_observe(&m,17,w,200)==CXF_OK);
    assert(m.count==CXF_COST_CAPACITY);
    assert(cxf_cost_predict(&m,17,w,&p)==CXF_OK && absd(p.ms-200)<0.001);
    assert(cxf_cost_observe(&m,17,w,0)==CXF_OK);
    printf("PASS cost: unknown profile, invalid-state atomicity, zero work, unseen shapes, extrapolation, bounded window and drift correction; state=%zu bytes\n",sizeof m);
    return 0;
}
