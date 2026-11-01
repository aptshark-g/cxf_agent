#define _POSIX_C_SOURCE 200809L
#include "cxf/llama.h"
#include "cxf/cost.h"
#include "cxf/roofline.h"
#include "../src/llama_internal.h"
#include "llama.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t tick(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static void ok(cxf_result rc){if(rc!=CXF_OK){fprintf(stderr,"cost benchmark rc=%d\n",rc);abort();}}
static cxf_llama_view view(cxf_llama *e,cxf_llama_session h){cxf_llama_view v;ok(cxf_llama_session_view(e,h,&v));return v;}
static void append(cxf_llama *e,cxf_llama_session h,const int32_t *t,uint32_t n){ok(cxf_llama_append(e,h,view(e,h).revision,t,n));}
static void fill(cxf_llama *e,cxf_llama_session h){uint32_t n;ok(cxf_llama_prefill(e,h,view(e,h).revision,UINT32_MAX,true,&n));}
static double absd(double x){return x<0?-x:x;}
static int32_t query(cxf_llama *e,cxf_llama_session h){int32_t t;ok(cxf_llama_greedy(e,h,view(e,h).revision,&t));return t;}

#include "roofline_probe.h"
#include "roofline_graph_probe.h"
#include "roofline_shape_probe.h"
static cxf_roofline_spec spec;
static cxf_roofline_device device;
static cxf_roofline_shape shape;
static uint32_t batch_limit;
static uint64_t empirical_tag,roof_tag,shape_tag;
static cxf_cost_model empirical_online,empirical_frozen;
static cxf_roofline_calibration roof_online,roof_frozen,shape_online,shape_frozen;
static uint64_t tagged(uint64_t h,const void *data,size_t size){
    const unsigned char *p=data;for(size_t i=0;i<size;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h?h:1;
}
typedef struct Trial {double ms;int32_t token;} Trial;
static Trial trial(cxf_llama *e,cxf_llama_session root,const int32_t *tokens,uint32_t prefix,uint32_t n){
    cxf_llama_session h;ok(cxf_llama_fork(e,root,view(e,root).revision,prefix,&h));append(e,h,tokens+prefix,n);
    cxf_llama_stats a,b;cxf_llama_get_stats(e,&a);uint32_t done;uint64_t start=tick();
    ok(cxf_llama_prefill(e,h,view(e,h).revision,UINT32_MAX,true,&done));
    double ms=(tick()-start)/1e6;cxf_llama_get_stats(e,&b);
    assert(done==n && b.evaluated_tokens-a.evaluated_tokens==n && b.decode_calls-a.decode_calls==(n+batch_limit-1)/batch_limit);
    int32_t actual[8192];ok(cxf_llama_read(e,h,0,actual,prefix+n));assert(!memcmp(actual,tokens,(prefix+n)*sizeof *tokens));
    Trial result={ms,query(e,h)};ok(cxf_llama_session_close(e,h));return result;
}
static void phase(cxf_llama *e,const int32_t *tokens,int repeats,bool calibration,int *oracles){
    const uint32_t train_p[]={0,2048,6144},train_n[]={32,128,512};
#ifdef CXF_SHAPES_V2
    const uint32_t test_p[]={512,3072,5120},test_n[]={12,18,24,48,192,384,640};
#else
    const uint32_t test_p[]={0,1024,4096},test_n[]={8,16,19,20,64,256,768};
#endif
    const uint32_t *prefixes=calibration?train_p:test_p,*sizes=calibration?train_n:test_n;int count=calibration?3:7;
    const char *modes[]={"empirical_frozen","empirical_online","roof_raw","roof_frozen","roof_online","shape_raw","shape_frozen","shape_online"};
    for(int pi=0;pi<3;pi++){
        uint32_t prefix=prefixes[pi];cxf_llama_session root;ok(cxf_llama_session_new(e,&root));
        if(prefix){append(e,root,tokens,prefix);fill(e,root);}
        int32_t expected[7];for(int i=0;i<count;i++)expected[i]=trial(e,root,tokens,prefix,sizes[i]).token;
        for(int rep=0;rep<repeats;rep++)for(int j=0;j<count;j++){
            int i=(j+rep)%count;uint32_t n=sizes[i];cxf_cost_work w={prefix,n,batch_limit};
            cxf_roofline_result base,shaped;bool extra;
            ok(cxf_roofline_estimate(&spec,&device,w,&base));ok(cxf_roofline_shape_estimate(&spec,&shape,w,&shaped,&extra));
            double predicted[8]={0,0,base.ms,0,0,shaped.ms,0,0};
            if(!calibration){
                cxf_cost_prediction q;
                ok(cxf_cost_predict(&empirical_frozen,empirical_tag,w,&q));predicted[0]=q.ms;
                ok(cxf_cost_predict(&empirical_online,empirical_tag,w,&q));predicted[1]=q.ms;
                ok(cxf_roofline_predict(&roof_frozen,roof_tag,&spec,&device,w,&q));predicted[3]=q.ms;
                ok(cxf_roofline_predict(&roof_online,roof_tag,&spec,&device,w,&q));predicted[4]=q.ms;
                ok(cxf_roofline_shape_predict(&shape_frozen,shape_tag,&spec,&shape,w,&q));predicted[6]=q.ms;
                ok(cxf_roofline_shape_predict(&shape_online,shape_tag,&spec,&shape,w,&q));predicted[7]=q.ms;
            }
            Trial t=trial(e,root,tokens,prefix,n);assert(t.token==expected[i]);
            uint32_t vec=0,dense=0,used=0;
            while(used<n){uint32_t chunk=n-used;if(chunk>batch_limit)chunk=batch_limit;if(chunk<20)vec++;else dense++;used+=chunk;}
            printf("{\"kind\":\"cost_sample\",\"scenario\":\"%s\",\"rep\":%d,\"prefix_tokens\":%u,\"new_tokens\":%u,\"batch_tokens\":%u,\"chunk_calls\":%u,\"fa_vec_calls\":%u,\"fa_dense_calls\":%u,\"max_chunk_tokens\":%u,\"tail_tokens\":%u,\"wall_ms\":%.6f,\"correct\":true,\"shape_extrapolated\":%s,\"roof_linear_ms\":%.6f,\"roof_attention_ms\":%.6f,\"shape_linear_ms\":%.6f,\"shape_attention_ms\":%.6f",
                calibration?"calibration":"evaluation",rep,prefix,n,batch_limit,vec+dense,vec,dense,n<batch_limit?n:batch_limit,(n-1)%batch_limit+1,t.ms,extra?"true":"false",base.linear_ms,base.attention_ms,shaped.linear_ms,shaped.attention_ms);
            if(!calibration)for(int k=0;k<8;k++)printf(",\"%s_ms\":%.6f,\"%s_abs_error_ms\":%.6f",modes[k],predicted[k],modes[k],absd(predicted[k]-t.ms));
            puts("}");fflush(stdout);
            ok(cxf_cost_observe(&empirical_online,empirical_tag,w,t.ms));
            ok(cxf_roofline_observe(&roof_online,roof_tag,&spec,&device,w,t.ms));
            ok(cxf_roofline_shape_observe(&shape_online,shape_tag,&spec,&shape,w,t.ms));
        }
        ok(cxf_llama_session_close(e,root));
        for(int i=0;i<count;i++){
            cxf_llama_session ref;ok(cxf_llama_session_new(e,&ref));append(e,ref,tokens,prefix+sizes[i]);fill(e,ref);
            assert(query(e,ref)==expected[i]);ok(cxf_llama_session_close(e,ref));(*oracles)++;
        }
    }
}
static void overhead(void){
    cxf_roofline_calibration copy=shape_online;volatile double checksum=0;cxf_cost_work w={2048,64,batch_limit};
    for(int rep=0;rep<10;rep++){
        uint64_t start=tick();for(int i=0;i<5000;i++){cxf_cost_prediction p;ok(cxf_roofline_shape_predict(&copy,shape_tag,&spec,&shape,w,&p));checksum+=p.ms;}
        double predict_us=(tick()-start)/5000000.0;
        start=tick();for(int i=0;i<200;i++)ok(cxf_roofline_shape_observe(&copy,shape_tag,&spec,&shape,w,50+(i%3)));
        double observe_us=(tick()-start)/200000.0;
        printf("{\"kind\":\"shape_overhead\",\"batch_tokens\":%u,\"rep\":%d,\"predict_us\":%.6f,\"observe_us\":%.6f}\n",batch_limit,rep,predict_us,observe_us);
    }
    assert(checksum>0);
}
int main(int argc,char **argv){
    assert(argc==5);int repeats=atoi(argv[3]);assert(repeats>0&&repeats<=30);uint64_t profile=strtoull(argv[4],NULL,16);assert(profile);
    cxf_llama *e=NULL;cxf_llama_params p=cxf_llama_default_params();p.model_path=argv[1];p.backend_path=argv[2];p.context_tokens=8192;p.max_sessions=2;p.batch_tokens=512;p.threads=4;
    ok(cxf_llama_open(&p,&e));spec=read_spec(argv[1],cxf_llama_native_model(e));spec.last_layer_ffn_tokens=1;
    (void)hardware_probe();device=graph_hardware_probe();shape=build_shape_table(device.bandwidth_bytes_s);
    printf("{\"kind\":\"metadata\",\"profile_tag\":\"%016" PRIx64 "\",\"state_bytes\":%zu,\"table_bytes\":%zu}\n",profile,sizeof(cxf_roofline_calibration),sizeof shape);
    const char *text="The following is a stable benchmark prefix. alpha beta gamma delta.\n";
    int32_t pattern[128],tokens[8192];uint32_t length;ok(cxf_llama_tokenize(e,text,(uint32_t)strlen(text),false,pattern,128,&length));assert(length);
    for(int i=0;i<8192;i++)tokens[i]=pattern[i%length];
    const uint32_t budgets[]={512,128};
    for(int config=0;config<2;config++){
        batch_limit=budgets[config];if(config){p.batch_tokens=batch_limit;ok(cxf_llama_open(&p,&e));}
        empirical_tag=tagged(profile,&batch_limit,sizeof batch_limit);roof_tag=tagged(empirical_tag,&device,sizeof device);shape_tag=tagged(roof_tag,&shape,sizeof shape);
        ok(cxf_cost_init(&empirical_online,empirical_tag));ok(cxf_roofline_init(&roof_online,roof_tag));ok(cxf_roofline_init(&shape_online,shape_tag));
        printf("{\"kind\":\"configuration\",\"batch_tokens\":%u,\"empirical_tag\":\"%016" PRIx64 "\",\"roof_tag\":\"%016" PRIx64 "\",\"shape_tag\":\"%016" PRIx64 "\"}\n",batch_limit,empirical_tag,roof_tag,shape_tag);
        int oracles=0;phase(e,tokens,repeats,true,&oracles);empirical_frozen=empirical_online;roof_frozen=roof_online;shape_frozen=shape_online;
        printf("{\"kind\":\"fitted_models\",\"batch_tokens\":%u,\"empirical\":[%.9f,%.9f,%.9f],\"roof\":[%.9f,%.9f,%.9f],\"shape\":[%.9f,%.9f,%.9f]}\n",batch_limit,empirical_frozen.coefficient[0],empirical_frozen.coefficient[1],empirical_frozen.coefficient[2],roof_frozen.fit.coefficient[0],roof_frozen.fit.coefficient[1],roof_frozen.fit.coefficient[2],shape_frozen.fit.coefficient[0],shape_frozen.fit.coefficient[1],shape_frozen.fit.coefficient[2]);
        phase(e,tokens,repeats,false,&oracles);overhead();
        printf("{\"kind\":\"correctness\",\"batch_tokens\":%u,\"fresh_shapes\":%d,\"passed\":true}\n",batch_limit,oracles);
        cxf_llama_stats stats;cxf_llama_get_stats(e,&stats);assert(!stats.sessions&&!stats.stored_tokens);cxf_llama_close(e);e=NULL;
        printf("{\"kind\":\"closed\",\"batch_tokens\":%u,\"passed\":true,\"sessions\":0,\"stored_tokens\":0}\n",batch_limit);
    }
    return 0;
}
