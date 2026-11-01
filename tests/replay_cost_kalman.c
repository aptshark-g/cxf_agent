#define _POSIX_C_SOURCE 200809L
#include "cxf/cost_kalman.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
static uint64_t tick(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
int main(void){
    unsigned reset,batch,index,base_id,prefix,n;
    double base,actual,constant_r,scaled_r,q,p0,alpha,ewma=0;
    cxf_cost_kalman fixed={0},scaled={0};
    printf("{\"kind\":\"filter_metadata\",\"state_bytes\":%zu}\n",sizeof fixed);
    while(scanf("%u %u %u %u %u %u %lf %lf %lf %lf %lf %lf %lf",&reset,&batch,&index,&base_id,&prefix,&n,&base,&actual,&constant_r,&scaled_r,&q,&p0,&alpha)==13){
        uint64_t tag=((uint64_t)batch<<32)|(base_id+1);cxf_cost_work work={prefix,n,batch};
        if(reset){assert(cxf_cost_kalman_init(&fixed,tag,q,p0)==CXF_OK);assert(cxf_cost_kalman_init(&scaled,tag,q,p0)==CXF_OK);ewma=0;}
        cxf_cost_kalman_prediction a,b;
        assert(cxf_cost_kalman_predict(&fixed,tag,work,base,constant_r,&a)==CXF_OK);
        assert(cxf_cost_kalman_predict(&scaled,tag,work,base,scaled_r,&b)==CXF_OK);
        double smoothed=base+ewma;if(smoothed<0)smoothed=0;
        assert(fabs(smoothed-a.ms)<1e-7);
        printf("{\"kind\":\"filter_prediction\",\"source_record_index\":%u,\"batch_tokens\":%u,\"base_id\":%u,\"observations_before\":%" PRIu64 ",\"base_ms\":%.12f,\"actual_ms\":%.12f,\"ewma_ms\":%.12f,\"fixed_ms\":%.12f,\"scaled_ms\":%.12f,\"fixed_variance_ms2\":%.12f,\"scaled_variance_ms2\":%.12f,\"fixed_gain\":%.12f,\"scaled_gain\":%.12f,\"fixed_innovation_ms\":%.12f,\"scaled_innovation_ms\":%.12f}\n",
            index,batch,base_id,a.observations,base,actual,smoothed,a.ms,b.ms,a.variance_ms2,b.variance_ms2,a.gain,b.gain,actual-base-a.bias_ms,actual-base-b.bias_ms);
        /* The actual label is used only after every prediction above. */
        ewma+=alpha*(actual-base-ewma);
        assert(cxf_cost_kalman_observe(&fixed,tag,work,base,constant_r,actual)==CXF_OK);
        assert(cxf_cost_kalman_observe(&scaled,tag,work,base,scaled_r,actual)==CXF_OK);
    }
    assert(feof(stdin));volatile double checksum=0;
    cxf_cost_work work={2048,64,64};assert(cxf_cost_kalman_init(&fixed,7,.01,.1)==CXF_OK);
    for(int rep=0;rep<10;rep++){
        uint64_t start=tick();for(int i=0;i<50000;i++){cxf_cost_kalman_prediction x;assert(cxf_cost_kalman_predict(&fixed,7,work,100,1,&x)==CXF_OK);checksum+=x.ms;}
        double predict_us=(tick()-start)/50000000.0;
        start=tick();for(int i=0;i<10000;i++)assert(cxf_cost_kalman_observe(&fixed,7,work,100,1,100+(i%3))==CXF_OK);
        printf("{\"kind\":\"filter_overhead\",\"rep\":%d,\"predict_us\":%.9f,\"observe_us\":%.9f}\n",rep,predict_us,(tick()-start)/10000000.0);
    }
    assert(checksum>0);return 0;
}
