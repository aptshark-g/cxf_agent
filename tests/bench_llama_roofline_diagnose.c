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
static cxf_roofline_spec roof_spec;
static cxf_roofline_device roof_device,graph_device;
static cxf_roofline_spec math_spec;
static cxf_roofline_calibration roof_fit;
static uint64_t roof_profile;
static uint64_t rate_profile(uint64_t base,const cxf_roofline_device *d){
    /* Local numeric identity; base already keys model/spec/backend/config.
     * Include actual rate bits so separately calibrated rates cannot be reused. */
    const unsigned char *bytes=(const unsigned char *)d;uint64_t hash=base;
    for(size_t i=0;i<sizeof *d;i++){hash^=bytes[i];hash*=UINT64_C(1099511628211);}
    return hash?hash:1;
}

typedef struct Trial { double ms;int32_t token; } Trial;
static Trial trial(cxf_llama *e,cxf_llama_session root,const int32_t *tokens,uint32_t prefix,uint32_t n){
    cxf_llama_session h;ok(cxf_llama_fork(e,root,view(e,root).revision,prefix,&h));append(e,h,tokens+prefix,n);
    cxf_llama_stats a,b;cxf_llama_get_stats(e,&a);uint32_t done;uint64_t start=tick();
    ok(cxf_llama_prefill(e,h,view(e,h).revision,UINT32_MAX,true,&done));
    double elapsed=(tick()-start)/1e6;cxf_llama_get_stats(e,&b);
    assert(done==n && b.evaluated_tokens-a.evaluated_tokens==n && b.decode_calls-a.decode_calls==(n+511)/512);
    int32_t actual[8192];ok(cxf_llama_read(e,h,0,actual,prefix+n));assert(!memcmp(actual,tokens,(prefix+n)*sizeof *tokens));
    Trial result={elapsed,query(e,h)};ok(cxf_llama_session_close(e,h));return result;
}
static void phase(cxf_llama *e,const int32_t *tokens,int repeats,bool calibration,
                  uint64_t profile,cxf_cost_model *online,const cxf_cost_model *frozen,
                  double rate,double *sum_ms,uint64_t *sum_tokens,int *oracles){
    const uint32_t train_p[]={0,2048,6144},test_p[]={0,1024,4096};
    const uint32_t train_n[]={32,128,512},test_n[]={16,64,256,768};
    const uint32_t *prefixes=calibration?train_p:test_p,*sizes=calibration?train_n:test_n;int count=calibration?3:4;
    for(int pi=0;pi<3;pi++){
        uint32_t prefix=prefixes[pi];cxf_llama_session root;ok(cxf_llama_session_new(e,&root));
        if(prefix){append(e,root,tokens,prefix);fill(e,root);}
        int32_t expected[4];
        /* Warm each shape; never use these timings/labels for model fitting. */
        for(int i=0;i<count;i++)expected[i]=trial(e,root,tokens,prefix,sizes[i]).token;
        for(int rep=0;rep<repeats;rep++)for(int j=0;j<count;j++){
            int i=(j+rep)%count;uint32_t n=sizes[i];cxf_cost_work work={prefix,n,512};
            cxf_cost_prediction f={0},o={0},roof_cal={0};
            cxf_roofline_result roof_raw,math_only,rate_only,corrected,bandwidth_only,linear_only,attention_only;
            cxf_roofline_device partial=roof_device;partial.bandwidth_bytes_s=graph_device.bandwidth_bytes_s;
            ok(cxf_roofline_estimate(&roof_spec,&partial,work,&bandwidth_only));
            partial=roof_device;partial.linear_flops_s=graph_device.linear_flops_s;
            ok(cxf_roofline_estimate(&roof_spec,&partial,work,&linear_only));
            partial=roof_device;partial.attention_flops_s=graph_device.attention_flops_s;
            ok(cxf_roofline_estimate(&roof_spec,&partial,work,&attention_only));
            ok(cxf_roofline_estimate(&roof_spec,&roof_device,work,&roof_raw));
            ok(cxf_roofline_estimate(&math_spec,&roof_device,work,&math_only));
            ok(cxf_roofline_estimate(&roof_spec,&graph_device,work,&rate_only));
            ok(cxf_roofline_estimate(&math_spec,&graph_device,work,&corrected));
            if(!calibration)ok(cxf_roofline_predict(&roof_fit,roof_profile,&math_spec,&graph_device,work,&roof_cal));
            if(!calibration){ok(cxf_cost_predict(frozen,profile,work,&f));ok(cxf_cost_predict(online,profile,work,&o));}
            Trial t=trial(e,root,tokens,prefix,n);assert(t.token==expected[i]);
            printf("{\"kind\":\"cost_sample\",\"scenario\":\"%s\",\"rep\":%d,\"prefix_tokens\":%u,\"new_tokens\":%u,\"batch_tokens\":512,\"wall_ms\":%.6f,\"correct\":true",
                   calibration?"calibration":"evaluation",rep,prefix,n,t.ms);
            if(!calibration)printf(",\"fixed_ms\":%.6f,\"frozen_ms\":%.6f,\"online_ms\":%.6f,\"fixed_abs_error_ms\":%.6f,\"frozen_abs_error_ms\":%.6f,\"online_abs_error_ms\":%.6f,\"frozen_extrapolated\":%s,\"online_extrapolated\":%s,\"online_observations\":%u",
                rate*n,f.ms,o.ms,absd(rate*n-t.ms),absd(f.ms-t.ms),absd(o.ms-t.ms),f.extrapolated?"true":"false",o.extrapolated?"true":"false",o.observations);
            printf(",\"legacy_ms\":%.6f,\"legacy_linear_ms\":%.6f,\"legacy_attention_ms\":%.6f,\"math_only_ms\":%.6f,\"rate_only_ms\":%.6f,\"corrected_ms\":%.6f,\"corrected_linear_ms\":%.6f,\"corrected_attention_ms\":%.6f,\"corrected_compute_ms\":%.6f,\"corrected_memory_ms\":%.6f,\"dominant_flops\":%.0f,\"operand_bytes\":%.0f",roof_raw.ms,roof_raw.linear_ms,roof_raw.attention_ms,math_only.ms,rate_only.ms,corrected.ms,corrected.linear_ms,corrected.attention_ms,corrected.compute_ms,corrected.memory_ms,corrected.dominant_flops,corrected.operand_bytes);
            printf(",\"bandwidth_only_ms\":%.6f,\"linear_only_ms\":%.6f,\"attention_only_ms\":%.6f,\"legacy_compute_ms\":%.6f,\"legacy_memory_ms\":%.6f",bandwidth_only.ms,linear_only.ms,attention_only.ms,roof_raw.compute_ms,roof_raw.memory_ms);
            if(!calibration)printf(",\"bandwidth_only_abs_error_ms\":%.6f,\"linear_only_abs_error_ms\":%.6f,\"attention_only_abs_error_ms\":%.6f",absd(bandwidth_only.ms-t.ms),absd(linear_only.ms-t.ms),absd(attention_only.ms-t.ms));
            if(!calibration)printf(",\"corrected_calibrated_ms\":%.6f,\"legacy_abs_error_ms\":%.6f,\"math_only_abs_error_ms\":%.6f,\"rate_only_abs_error_ms\":%.6f,\"corrected_abs_error_ms\":%.6f,\"corrected_calibrated_abs_error_ms\":%.6f",roof_cal.ms,absd(roof_raw.ms-t.ms),absd(math_only.ms-t.ms),absd(rate_only.ms-t.ms),absd(corrected.ms-t.ms),absd(roof_cal.ms-t.ms));
            puts("}");fflush(stdout);
            /* Every evaluation prediction precedes this row's observation. */
            ok(cxf_cost_observe(online,profile,work,t.ms));
            if(calibration){*sum_ms+=t.ms;*sum_tokens+=n;ok(cxf_roofline_observe(&roof_fit,roof_profile,&math_spec,&graph_device,work,t.ms));}
        }
        ok(cxf_llama_session_close(e,root));
        /* Free the old prefix before fresh-KV checks, so 8K remains sufficient. */
        for(int i=0;i<count;i++){
            cxf_llama_session ref;ok(cxf_llama_session_new(e,&ref));append(e,ref,tokens,prefix+sizes[i]);fill(e,ref);
            assert(query(e,ref)==expected[i]);ok(cxf_llama_session_close(e,ref));(*oracles)++;
        }
    }
}
int main(int argc,char **argv){
    assert(argc==5||argc==6);int repeats=atoi(argv[3]);assert(repeats>0 && repeats<=30);
    uint64_t profile=strtoull(argv[4],NULL,16);assert(profile);
    cxf_llama_params p=cxf_llama_default_params();p.model_path=argv[1];p.backend_path=argv[2];
    p.context_tokens=8192;p.max_sessions=2;p.batch_tokens=512;p.threads=4;
    uint64_t start=tick();cxf_llama *e;ok(cxf_llama_open(&p,&e));double open_ms=(tick()-start)/1e6;
    const struct llama_model *model=cxf_llama_native_model(e);
    printf("{\"kind\":\"metadata\",\"profile_tag\":\"%016" PRIx64 "\",\"open_ms\":%.6f,\"state_bytes\":%zu,\"layers\":%d,\"embedding\":%d,\"heads\":%d,\"kv_heads\":%d}\n",
        profile,open_ms,sizeof(cxf_cost_model),llama_model_n_layer(model),llama_model_n_embd(model),llama_model_n_head(model),llama_model_n_head_kv(model));
    roof_spec=read_spec(argv[1],model);roof_device=hardware_probe();graph_device=graph_hardware_probe();
    if(argc==6){assert(!strcmp(argv[5],"probes_only"));cxf_llama_close(e);return 0;}
    math_spec=roof_spec;math_spec.last_layer_ffn_tokens=1;
    roof_profile=rate_profile(profile,&graph_device);ok(cxf_roofline_init(&roof_fit,roof_profile));
    const char *text="The following is a stable benchmark prefix. alpha beta gamma delta.\n";
    int32_t pattern[128],tokens[8192];uint32_t length;ok(cxf_llama_tokenize(e,text,(uint32_t)strlen(text),false,pattern,128,&length));assert(length);
    for(int i=0;i<8192;i++)tokens[i]=pattern[i%length];
    cxf_cost_model online,frozen;ok(cxf_cost_init(&online,profile));double total=0;uint64_t count=0;int oracles=0;
    phase(e,tokens,repeats,true,profile,&online,NULL,0,&total,&count,&oracles);frozen=online;
    printf("{\"kind\":\"calibration_model\",\"samples\":%u,\"fixed_ms_per_token\":%.9f,\"coefficient\":[%.9f,%.9f,%.9f],\"fit_mae_ms\":%.6f}\n",
        frozen.count,total/count,frozen.coefficient[0],frozen.coefficient[1],frozen.coefficient[2],frozen.fit_mae_ms);
    printf("{\"kind\":\"roofline_calibration\",\"profile_tag\":\"%016" PRIx64 "\",\"coefficient\":[%.9f,%.9f,%.9f],\"samples\":%u}\n",roof_profile,roof_fit.fit.coefficient[0],roof_fit.fit.coefficient[1],roof_fit.fit.coefficient[2],roof_fit.fit.count);
    phase(e,tokens,repeats,false,profile,&online,&frozen,total/count,&total,&count,&oracles);
    cxf_roofline_calibration bench_fit=roof_fit;
    volatile double checksum=0;const int queries=5000,updates=200;cxf_cost_work w={2048,128,512};
    for(int rep=0;rep<10;rep++){
        start=tick();for(int i=0;i<queries;i++){cxf_cost_prediction q;ok(cxf_cost_predict(&online,profile,w,&q));checksum+=q.ms;}
        double predict_us=(tick()-start)/(1000.0*queries);
        start=tick();for(int i=0;i<updates;i++)ok(cxf_cost_observe(&online,profile,w,50.0+(i%3)));
        double update_us=(tick()-start)/(1000.0*updates);
        start=tick();for(int i=0;i<queries;i++){cxf_roofline_result rr;ok(cxf_roofline_estimate(&math_spec,&graph_device,w,&rr));checksum+=rr.ms;}
        double roof_us=(tick()-start)/(1000.0*queries);
        start=tick();for(int i=0;i<queries;i++){cxf_cost_prediction q;ok(cxf_roofline_predict(&bench_fit,roof_profile,&math_spec,&graph_device,w,&q));checksum+=q.ms;}
        double calibrated_us=(tick()-start)/(1000.0*queries);
        start=tick();for(int i=0;i<updates;i++)ok(cxf_roofline_observe(&bench_fit,roof_profile,&math_spec,&graph_device,w,50.0+(i%3)));
        double calibrated_observe_us=(tick()-start)/(1000.0*updates);
        printf("{\"kind\":\"roofline_overhead\",\"rep\":%d,\"estimate_us\":%.6f,\"calibrated_predict_us\":%.6f,\"calibrated_observe_us\":%.6f,\"query_loops\":%d,\"observe_loops\":%d}\n",rep,roof_us,calibrated_us,calibrated_observe_us,queries,updates);
        printf("{\"kind\":\"estimator_overhead\",\"rep\":%d,\"predict_us\":%.6f,\"observe_us\":%.6f,\"query_loops\":%d,\"observe_loops\":%d}\n",rep,predict_us,update_us,queries,updates);
    }
    assert(checksum>0);printf("{\"kind\":\"correctness\",\"passed\":true,\"fresh_shapes\":%d,\"cases\":[\"fresh_KV_greedy\",\"canonical_token_equality\",\"exact_new_token_and_batch_counts\",\"predict_before_observe\"]}\n",oracles);
    cxf_llama_stats s;cxf_llama_get_stats(e,&s);assert(!s.sessions && !s.stored_tokens);
    cxf_llama_close(e);puts("{\"kind\":\"closed\",\"sessions\":0,\"stored_tokens\":0,\"passed\":true}");return 0;
}
