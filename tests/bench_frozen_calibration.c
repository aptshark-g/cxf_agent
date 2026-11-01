#define _POSIX_C_SOURCE 200809L
#include "cxf/llama.h"
#include "cxf/cost.h"
#include "cxf/cost_residual.h"
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
static cxf_roofline_shape shape,clipped_shape;
static uint32_t batch_limit;
static uint64_t tags[2],empirical_tag;
static cxf_roofline_calibration legacy[2],clipped,shape_online;
static cxf_cost_residual residual[2];
static cxf_cost_model empirical_online;
static uint64_t tagged(uint64_t h,const void *data,size_t size){const unsigned char *p=data;for(size_t i=0;i<size;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h?h:1;}
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

static const char *modes[]={"raw","legacy_clipped_sparse","legacy_sparse","legacy_diverse","residual_sparse","residual_diverse","empirical_online","shape_online"};
static void record(const char *scenario,const char *coverage,const char *stage,int rep,cxf_cost_work w,Trial t,const cxf_roofline_result *raw,const cxf_roofline_result *clip,bool extra,bool clip_extra,const double *prediction){
    uint32_t vec=0,dense=0,done=0;
    while(done<w.new_tokens){uint32_t n=w.new_tokens-done;if(n>w.batch_tokens)n=w.batch_tokens;if(n<20)vec++;else dense++;done+=n;}
    printf("{\"kind\":\"cost_sample\",\"scenario\":\"%s\",\"coverage\":\"%s\",\"anchor_stage\":\"%s\",\"rep\":%d,\"prefix_tokens\":%u,\"new_tokens\":%u,\"batch_tokens\":%u,\"chunk_calls\":%u,\"fa_vec_calls\":%u,\"fa_dense_calls\":%u,\"wall_ms\":%.9f,\"shape_linear_ms\":%.9f,\"shape_attention_ms\":%.9f,\"raw_ms\":%.9f,\"clip_linear_ms\":%.9f,\"clip_attention_ms\":%.9f,\"shape_extrapolated\":%s,\"clip_extrapolated\":%s,\"correct\":true",
        scenario,coverage,stage,rep,w.prefix_tokens,w.new_tokens,w.batch_tokens,vec+dense,vec,dense,t.ms,raw->linear_ms,raw->attention_ms,raw->ms,clip->linear_ms,clip->attention_ms,extra?"true":"false",clip_extra?"true":"false");
    if(prediction)for(int k=0;k<8;k++)if(k)printf(",\"%s_ms\":%.9f,\"%s_abs_error_ms\":%.9f",modes[k],prediction[k],modes[k],absd(prediction[k]-t.ms));
    if(prediction)printf(",\"raw_abs_error_ms\":%.9f",absd(raw->ms-t.ms));
    puts("}");fflush(stdout);
}
static void anchors(cxf_llama *e,const int32_t *tokens,const char *stage){
    const uint32_t ps[]={0,2048,6144},ns[]={32,17,512};
    for(int i=0;i<3;i++){
        cxf_llama_session root;ok(cxf_llama_session_new(e,&root));if(ps[i]){append(e,root,tokens,ps[i]);fill(e,root);}
        int32_t expected=trial(e,root,tokens,ps[i],ns[i]).token;cxf_cost_work w={ps[i],ns[i],batch_limit};
        cxf_roofline_result raw,clip;bool extra,ce;ok(cxf_roofline_shape_estimate(&spec,&shape,w,&raw,&extra));ok(cxf_roofline_shape_estimate(&spec,&clipped_shape,w,&clip,&ce));
        for(int rep=0;rep<3;rep++){Trial t=trial(e,root,tokens,ps[i],ns[i]);assert(t.token==expected);record("anchor","none",stage,rep,w,t,&raw,&clip,extra,ce,NULL);}
        ok(cxf_llama_session_close(e,root));
    }
}
static void calibrate(cxf_llama *e,const int32_t *tokens,int repeats,int *oracles){
    const uint32_t ps[]={0,2048,6144},sparse[]={32,128,512},diverse[3][3]={{8,96,448},{17,96,448},{8,17,448}};
    for(int pi=0;pi<3;pi++){
        uint32_t p=ps[pi],sizes[6];for(int i=0;i<3;i++){sizes[2*i]=sparse[i];sizes[2*i+1]=diverse[pi][i];}
        cxf_llama_session root;ok(cxf_llama_session_new(e,&root));if(p){append(e,root,tokens,p);fill(e,root);}
        int32_t expected[6];for(int i=0;i<6;i++)expected[i]=trial(e,root,tokens,p,sizes[i]).token;
        for(int rep=0;rep<repeats;rep++)for(int j=0;j<6;j++){
            int i=(j+rep)%6,coverage=i%2;uint32_t n=sizes[i];cxf_cost_work w={p,n,batch_limit};
            cxf_roofline_result raw,clip;bool extra,ce;ok(cxf_roofline_shape_estimate(&spec,&shape,w,&raw,&extra));ok(cxf_roofline_shape_estimate(&spec,&clipped_shape,w,&clip,&ce));
            Trial t=trial(e,root,tokens,p,n);assert(t.token==expected[i]);record("calibration",coverage?"diverse":"sparse","none",rep,w,t,&raw,&clip,extra,ce,NULL);
            ok(cxf_roofline_shape_observe(&legacy[coverage],tags[coverage],&spec,&shape,w,t.ms));
            ok(cxf_cost_residual_observe(&residual[coverage],tags[coverage],w,raw.ms,raw.attention_ms,t.ms));
            if(!coverage){ok(cxf_roofline_shape_observe(&clipped,tags[0],&spec,&clipped_shape,w,t.ms));ok(cxf_cost_observe(&empirical_online,empirical_tag,w,t.ms));}
        }
        ok(cxf_llama_session_close(e,root));
        for(int i=0;i<6;i++){
            cxf_llama_session ref;ok(cxf_llama_session_new(e,&ref));append(e,ref,tokens,p+sizes[i]);fill(e,ref);assert(query(e,ref)==expected[i]);ok(cxf_llama_session_close(e,ref));(*oracles)++;
        }
    }
}
static void evaluate(cxf_llama *e,const int32_t *tokens,int repeats,int *oracles){
    const uint32_t ps[]={1280,3328,5376},ns[]={10,15,21,72,176,352,672};
    for(int pi=0;pi<3;pi++){
        uint32_t p=ps[pi];cxf_llama_session root;ok(cxf_llama_session_new(e,&root));append(e,root,tokens,p);fill(e,root);
        int32_t expected[7];for(int i=0;i<7;i++)expected[i]=trial(e,root,tokens,p,ns[i]).token;
        for(int rep=0;rep<repeats;rep++)for(int j=0;j<7;j++){
            int i=(j+rep)%7;uint32_t n=ns[i];cxf_cost_work w={p,n,batch_limit};cxf_cost_prediction q;double prediction[8]={0};
            cxf_roofline_result raw,clip;bool extra,ce;ok(cxf_roofline_shape_estimate(&spec,&shape,w,&raw,&extra));ok(cxf_roofline_shape_estimate(&spec,&clipped_shape,w,&clip,&ce));
            assert(fabs(raw.ms-clip.ms)<1e-8);prediction[0]=raw.ms;
            ok(cxf_roofline_shape_predict(&clipped,tags[0],&spec,&clipped_shape,w,&q));prediction[1]=q.ms;
            for(int k=0;k<2;k++){
                ok(cxf_roofline_shape_predict(&legacy[k],tags[k],&spec,&shape,w,&q));prediction[2+k]=q.ms;
                ok(cxf_cost_residual_predict(&residual[k],tags[k],w,raw.ms,raw.attention_ms,&q));prediction[4+k]=q.ms;
            }
            ok(cxf_cost_predict(&empirical_online,empirical_tag,w,&q));prediction[6]=q.ms;
            ok(cxf_roofline_shape_predict(&shape_online,tags[0],&spec,&shape,w,&q));prediction[7]=q.ms;
            Trial t=trial(e,root,tokens,p,n);assert(t.token==expected[i]);record("evaluation","none","none",rep,w,t,&raw,&clip,extra,ce,prediction);
            ok(cxf_cost_observe(&empirical_online,empirical_tag,w,t.ms));ok(cxf_roofline_shape_observe(&shape_online,tags[0],&spec,&shape,w,t.ms));
        }
        ok(cxf_llama_session_close(e,root));
        for(int i=0;i<7;i++){
            cxf_llama_session ref;ok(cxf_llama_session_new(e,&ref));append(e,ref,tokens,p+ns[i]);fill(e,ref);assert(query(e,ref)==expected[i]);ok(cxf_llama_session_close(e,ref));(*oracles)++;
        }
    }
}
static void fits(void){
    printf("{\"kind\":\"fitted_models\",\"batch_tokens\":%u,\"legacy_clipped_sparse\":[%.12f,%.12f,%.12f],\"legacy_sparse\":[%.12f,%.12f,%.12f],\"legacy_diverse\":[%.12f,%.12f,%.12f],\"residual_sparse\":[%.12f,%.12f,%.12f],\"residual_diverse\":[%.12f,%.12f,%.12f]}\n",batch_limit,
        clipped.fit.coefficient[0],clipped.fit.coefficient[1],clipped.fit.coefficient[2],legacy[0].fit.coefficient[0],legacy[0].fit.coefficient[1],legacy[0].fit.coefficient[2],legacy[1].fit.coefficient[0],legacy[1].fit.coefficient[1],legacy[1].fit.coefficient[2],residual[0].coefficient[0],residual[0].coefficient[1],residual[0].coefficient[2],residual[1].coefficient[0],residual[1].coefficient[1],residual[1].coefficient[2]);
}
static void overhead(void){
    cxf_cost_residual copy=residual[1];cxf_cost_work w={2048,96,batch_limit};cxf_roofline_result raw;bool extra;ok(cxf_roofline_shape_estimate(&spec,&shape,w,&raw,&extra));volatile double checksum=0;
    for(int rep=0;rep<10;rep++){
        uint64_t start=tick();for(int i=0;i<5000;i++){cxf_roofline_result r;cxf_cost_prediction q;ok(cxf_roofline_shape_estimate(&spec,&shape,w,&r,&extra));ok(cxf_cost_residual_predict(&copy,tags[1],w,r.ms,r.attention_ms,&q));checksum+=q.ms;}
        double predict_us=(tick()-start)/5000000.0;
        start=tick();for(int i=0;i<200;i++)ok(cxf_cost_residual_observe(&copy,tags[1],w,raw.ms,raw.attention_ms,50+(i%3)));
        printf("{\"kind\":\"residual_overhead\",\"batch_tokens\":%u,\"rep\":%d,\"predict_us\":%.6f,\"observe_us\":%.6f}\n",batch_limit,rep,predict_us,(tick()-start)/200000.0);
    }
    assert(checksum>0);
}
int main(int argc,char **argv){
    assert(argc==5);int repeats=atoi(argv[3]);assert(repeats>0&&repeats<=30);uint64_t profile=strtoull(argv[4],NULL,16);assert(profile);
    cxf_llama_params p=cxf_llama_default_params();p.model_path=argv[1];p.backend_path=argv[2];p.context_tokens=8192;p.max_sessions=2;p.batch_tokens=256;p.threads=4;cxf_llama *e;
    ok(cxf_llama_open(&p,&e));spec=read_spec(argv[1],cxf_llama_native_model(e));spec.last_layer_ffn_tokens=1;
    (void)hardware_probe;device=graph_hardware_probe();shape=build_shape_table(device.bandwidth_bytes_s);clipped_shape=shape;clipped_shape.context_count=3;
    printf("{\"kind\":\"metadata\",\"profile_tag\":\"%016" PRIx64 "\",\"residual_bytes\":%zu,\"table_bytes\":%zu}\n",profile,sizeof(cxf_cost_residual),sizeof shape);
    const char *text="The following is a stable benchmark prefix. alpha beta gamma delta.\n";int32_t pattern[128],tokens[8192];uint32_t length;ok(cxf_llama_tokenize(e,text,(uint32_t)strlen(text),false,pattern,128,&length));
    for(int i=0;i<8192;i++)tokens[i]=pattern[i%length];
    const uint32_t budgets[]={256,64};
    for(int config=0;config<2;config++){
        batch_limit=budgets[config];if(config){p.batch_tokens=batch_limit;p.prefill_min_tokens=batch_limit<128?batch_limit:128;ok(cxf_llama_open(&p,&e));}
        empirical_tag=tagged(profile,&batch_limit,sizeof batch_limit);tags[0]=tagged(empirical_tag,&shape,sizeof shape);tags[1]=tags[0]^UINT64_C(0x9e3779b97f4a7c15);assert(tags[1]);
        for(int k=0;k<2;k++){ok(cxf_roofline_init(&legacy[k],tags[k]));ok(cxf_cost_residual_init(&residual[k],tags[k]));}ok(cxf_roofline_init(&clipped,tags[0]));ok(cxf_cost_init(&empirical_online,empirical_tag));
        anchors(e,tokens,"before_calibration");int oracles=0;calibrate(e,tokens,repeats,&oracles);shape_online=legacy[0];fits();anchors(e,tokens,"after_calibration");evaluate(e,tokens,repeats,&oracles);anchors(e,tokens,"after_evaluation");overhead();
        printf("{\"kind\":\"correctness\",\"batch_tokens\":%u,\"fresh_shapes\":%d,\"passed\":true}\n",batch_limit,oracles);
        cxf_llama_stats stats;cxf_llama_get_stats(e,&stats);assert(!stats.sessions&&!stats.stored_tokens);cxf_llama_close(e);
        printf("{\"kind\":\"closed\",\"batch_tokens\":%u,\"sessions\":0,\"stored_tokens\":0,\"passed\":true}\n",batch_limit);
    }
    return 0;
}
