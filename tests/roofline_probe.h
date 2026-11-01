#ifndef CXF_TEST_ROOFLINE_PROBE_H
#define CXF_TEST_ROOFLINE_PROBE_H
#include "cxf/roofline.h"
#include "ggml.h"
#include "ggml-backend.h"
#include "ggml-alloc.h"
#include "gguf.h"
#include <math.h>

/* Test-only GGUF adapter and same-backend primitive probes. No model weights
 * are loaded by this metadata context; the real inference engine owns them. */
static cxf_roofline_spec read_spec(const char *path,const struct llama_model *model){
    struct ggml_context *ctx=NULL;
    struct gguf_context *g=gguf_init_from_file(path,(struct gguf_init_params){true,&ctx});assert(g&&ctx);
    cxf_roofline_spec s={0};s.layers=llama_model_n_layer(model);s.embedding=llama_model_n_embd(model);
    s.query_heads=llama_model_n_head(model);s.kv_heads=llama_model_n_head_kv(model);
    const char *names[]={"attn_q","attn_k","attn_v","attn_output","ffn_gate","ffn_up","ffn_down"};
    uint32_t ins[7]={0},outs[7]={0};
    for(uint32_t l=0;l<s.layers;l++)for(int i=0;i<7;i++){
        char name[96];snprintf(name,sizeof name,"blk.%u.%s.weight",l,names[i]);
        struct ggml_tensor *t=ggml_get_tensor(ctx,name);assert(t&&t->type==GGML_TYPE_Q8_0 && t->ne[2]==1&&t->ne[3]==1);
        assert(t->ne[0]>0&&t->ne[0]<=UINT32_MAX&&t->ne[1]>0&&t->ne[1]<=UINT32_MAX);
        if(!l){ins[i]=(uint32_t)t->ne[0];outs[i]=(uint32_t)t->ne[1];s.weight_bytes[i]=(double)ggml_nbytes(t);}
        else assert(ins[i]==(uint32_t)t->ne[0]&&outs[i]==(uint32_t)t->ne[1]&&s.weight_bytes[i]==(double)ggml_nbytes(t));
    }
    assert(outs[1]%s.kv_heads==0&&outs[2]%s.kv_heads==0);
    s.key_dim=outs[1]/s.kv_heads;s.value_dim=outs[2]/s.kv_heads;s.ffn=outs[4];
    uint32_t expected_in[]={s.embedding,s.embedding,s.embedding,s.query_heads*s.value_dim,s.embedding,s.embedding,s.ffn};
    uint32_t expected_out[]={s.query_heads*s.key_dim,s.kv_heads*s.key_dim,s.kv_heads*s.value_dim,s.embedding,s.ffn,s.ffn,s.embedding};
    for(int i=0;i<7;i++)assert(ins[i]==expected_in[i]&&outs[i]==expected_out[i]);
    struct ggml_tensor *emb=ggml_get_tensor(ctx,"token_embd.weight"),*out=ggml_get_tensor(ctx,"output.weight");
    assert(emb&&emb->ne[0]==s.embedding);if(!out)out=emb;
    assert(out->type==GGML_TYPE_Q8_0&&out->ne[0]==s.embedding&&out->ne[1]==emb->ne[1]);
    s.vocabulary=(uint32_t)emb->ne[1];s.output_weight_bytes=(double)ggml_nbytes(out);
    s.embedding_row_bytes=(double)ggml_nbytes(emb)/s.vocabulary;s.kv_scalar_bytes=2;s.activation_scalar_bytes=4;
    printf("{\"kind\":\"roofline_spec\",\"layers\":%u,\"embedding\":%u,\"query_heads\":%u,\"kv_heads\":%u,\"key_dim\":%u,\"value_dim\":%u,\"ffn\":%u,\"vocabulary\":%u,\"weight_bytes\":[",
        s.layers,s.embedding,s.query_heads,s.kv_heads,s.key_dim,s.value_dim,s.ffn,s.vocabulary);
    for(int i=0;i<7;i++)printf("%s%.0f",i?",":"",s.weight_bytes[i]);
    printf("],\"output_weight_bytes\":%.0f,\"embedding_row_bytes\":%.6f,\"kv_scalar_bytes\":2,\"activation_scalar_bytes\":4}\n",s.output_weight_bytes,s.embedding_row_bytes);
    gguf_free(g);ggml_free(ctx);return s;
}
static int compare_double(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
static double probe_primitive(ggml_backend_t backend,const char *mode,enum ggml_type type,bool copy){
    struct ggml_context *ctx=ggml_init((struct ggml_init_params){2*1024*1024,NULL,true});assert(ctx);
    const int64_t K=copy?0:(type==GGML_TYPE_Q8_0?1024:128),N=copy?0:(type==GGML_TYPE_Q8_0?3072:4096),M=512;
    const int64_t elements=64*1024*1024/4;
    struct ggml_tensor *a=copy?ggml_new_tensor_1d(ctx,GGML_TYPE_F32,elements):ggml_new_tensor_2d(ctx,type,K,N);
    struct ggml_tensor *b=copy?NULL:ggml_new_tensor_2d(ctx,GGML_TYPE_F32,K,M);
    struct ggml_tensor *out=copy?ggml_dup(ctx,a):ggml_mul_mat(ctx,a,b);
    struct ggml_cgraph *graph=ggml_new_graph_custom(ctx,128,false);ggml_build_forward_expand(graph,out);
    ggml_backend_buffer_t buffer=ggml_backend_alloc_ctx_tensors(ctx,backend);assert(buffer);
    if(copy){
        float *values=malloc((size_t)elements*sizeof *values);assert(values);uint32_t seed=7919;
        for(int64_t i=0;i<elements;i++){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;values[i]=(seed>>8)*(1.0f/16777216);}
        ggml_backend_tensor_set(a,values,0,ggml_nbytes(a));free(values);
    }else{
        float *values=malloc((size_t)(K*N)*sizeof *values);void *packed=malloc(ggml_nbytes(a));assert(values&&packed);
        for(int64_t i=0;i<K*N;i++)values[i]=0.125f;
        assert(ggml_quantize_chunk(type,values,packed,0,N,K,NULL)==ggml_nbytes(a));
        ggml_backend_tensor_set(a,packed,0,ggml_nbytes(a));free(packed);free(values);
        values=malloc((size_t)(K*M)*sizeof *values);assert(values);
        for(int64_t i=0;i<K*M;i++)values[i]=0.25f;
        ggml_backend_tensor_set(b,values,0,ggml_nbytes(b));free(values);
    }
    double times[7],flops=copy?0:2.0*K*N*M,bytes=copy?2.0*ggml_nbytes(a):0;
    for(int rep=-2;rep<7;rep++){
        uint64_t start=tick();assert(ggml_backend_graph_compute(backend,graph)==GGML_STATUS_SUCCESS);ggml_backend_synchronize(backend);
        double ms=(tick()-start)/1e6;if(rep>=0){times[rep]=ms;
            printf("{\"kind\":\"hardware_probe\",\"mode\":\"%s\",\"rep\":%d,\"wall_ms\":%.6f,\"flops\":%.0f,\"traffic_bytes\":%.0f,\"correct\":true}\n",mode,rep,ms,flops,bytes);}
    }
    float actual=0;ggml_backend_tensor_get(out,&actual,0,sizeof actual);
    if(copy){float input;ggml_backend_tensor_get(a,&input,0,sizeof input);assert(actual==input);}
    else assert(fabs(actual-(float)(K/32.0))/(K/32.0)<0.01);
    qsort(times,7,sizeof *times,compare_double);double rate=(copy?bytes:flops)/(times[3]/1000.0);
    ggml_backend_buffer_free(buffer);ggml_free(ctx);return rate;
}
static cxf_roofline_device hardware_probe(void){
    ggml_backend_dev_t dev=ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);assert(dev);
    ggml_backend_t backend=ggml_backend_dev_init(dev,NULL);assert(backend);
    cxf_roofline_device d;
    d.linear_flops_s=probe_primitive(backend,"q8_gemm",GGML_TYPE_Q8_0,false);
    d.attention_flops_s=probe_primitive(backend,"f16_gemm",GGML_TYPE_F16,false);
    d.bandwidth_bytes_s=probe_primitive(backend,"copy_64mib",GGML_TYPE_F32,true);
    printf("{\"kind\":\"hardware_profile\",\"linear_flops_s\":%.6f,\"attention_flops_s\":%.6f,\"bandwidth_bytes_s\":%.6f,\"same_backend\":true}\n",d.linear_flops_s,d.attention_flops_s,d.bandwidth_bytes_s);
    ggml_backend_free(backend);return d;
}
#endif
