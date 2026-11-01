#ifndef CXF_TEST_ROOFLINE_GRAPH_PROBE_H
#define CXF_TEST_ROOFLINE_GRAPH_PROBE_H
/* Test-only dependent graphs. Distinct weights / KVs per stage; no parallel
 * independent matmuls, no per-node sync, no model-inference timing labels. */
typedef struct GraphProbe { double median_ms, flops, bytes, min_ms, max_ms; } GraphProbe;
static void set_constant(struct ggml_tensor *t,float value){
    size_t count=(size_t)ggml_nelements(t);float *v=malloc(count*sizeof *v);assert(v);
    for(size_t i=0;i<count;i++)v[i]=value;
    if(t->type==GGML_TYPE_F32)ggml_backend_tensor_set(t,v,0,ggml_nbytes(t));
    else{
        void *packed=malloc(ggml_nbytes(t));assert(packed);
        assert(ggml_quantize_chunk(t->type,v,packed,0,ggml_nrows(t),t->ne[0],NULL)==ggml_nbytes(t));
        ggml_backend_tensor_set(t,packed,0,ggml_nbytes(t));free(packed);
    }
    free(v);
}
static GraphProbe graph_probe(ggml_backend_t backend,const char *mode,int depth){
    bool copy=!strcmp(mode,"copy_chain"),fa=!strcmp(mode,"fa_chain"),q8=!strcmp(mode,"q8_chain");
    assert(depth==1||depth==16);
    struct ggml_context *ctx=ggml_init((struct ggml_init_params){4*1024*1024,NULL,true});assert(ctx);
    struct ggml_tensor *weights[16]={0},*keys[16]={0},*values[16]={0},*mask=NULL,*initial;
    const int64_t n=512,prefix=2048,span=prefix+n;
    if(copy)initial=ggml_new_tensor_1d(ctx,GGML_TYPE_F32,64*1024*1024/4);
    else if(fa)initial=ggml_new_tensor_4d(ctx,GGML_TYPE_F32,128,16,n,1);
    else initial=ggml_new_tensor_2d(ctx,GGML_TYPE_F32,q8?1024:128,n);
    struct ggml_tensor *current=fa?ggml_permute(ctx,initial,0,2,1,3):initial;
    if(fa)mask=ggml_new_tensor_2d(ctx,GGML_TYPE_F16,span,n);
    double flops=0,traffic=0;
    for(int i=0;i<depth;i++){
        if(copy){traffic+=2.0*ggml_nbytes(current);current=ggml_dup(ctx,current);}
        else if(fa){
            keys[i]=ggml_new_tensor_4d(ctx,GGML_TYPE_F16,128,8,span,1);
            values[i]=ggml_new_tensor_4d(ctx,GGML_TYPE_F16,128,8,span,1);
            current=ggml_flash_attn_ext(ctx,current,ggml_permute(ctx,keys[i],0,2,1,3),ggml_permute(ctx,values[i],0,2,1,3),mask,1.0f/sqrtf(128.0f),0,0);
            ggml_prec_set_acc(current,GGML_PREC_F32);
            ggml_flash_attn_ext_set_n_kv_max(current,0); /* dense llama path; nonzero selects sparse kernels */
            current=ggml_permute(ctx,current,0,2,1,3);
            flops+=2.0*16*(128+128)*(n*prefix+n*(n+1.0)/2);
        }else{
            int64_t k=current->ne[0],out=q8?(i%2?1024:3072):(i%2?128:4096);
            weights[i]=ggml_new_tensor_2d(ctx,q8?GGML_TYPE_Q8_0:GGML_TYPE_F16,k,out);
            current=ggml_mul_mat(ctx,weights[i],current);flops+=2.0*k*out*n;
        }
    }
    struct ggml_cgraph *graph=ggml_new_graph_custom(ctx,256,false);ggml_build_forward_expand(graph,current);
    ggml_backend_buffer_t buffer=ggml_backend_alloc_ctx_tensors(ctx,backend);assert(buffer);
    if(copy){
        size_t count=(size_t)ggml_nelements(initial);float *v=malloc(count*sizeof *v);assert(v);uint32_t seed=7919;
        for(size_t i=0;i<count;i++){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;v[i]=(seed>>8)*(1.0f/16777216);}
        ggml_backend_tensor_set(initial,v,0,ggml_nbytes(initial));free(v);
    }else set_constant(initial,0.25f);
    if(fa){
        size_t count=(size_t)(span*n);float *v=malloc(count*sizeof *v);ggml_fp16_t *packed=malloc(count*sizeof *packed);assert(v&&packed);
        for(int64_t i=0;i<n;i++)for(int64_t j=0;j<span;j++)v[i*span+j]=j<=prefix+i?0:-INFINITY;
        ggml_fp32_to_fp16_row(v,packed,(int64_t)count);ggml_backend_tensor_set(mask,packed,0,ggml_nbytes(mask));free(packed);free(v);
        for(int i=0;i<depth;i++){set_constant(keys[i],0.0f);set_constant(values[i],0.25f);}
    }else if(!copy)for(int i=0;i<depth;i++)set_constant(weights[i],1.0f/(float)weights[i]->ne[0]);
    double times[7];
    for(int rep=-2;rep<7;rep++){
        uint64_t start=tick();assert(ggml_backend_graph_compute(backend,graph)==GGML_STATUS_SUCCESS);ggml_backend_synchronize(backend);
        double ms=(tick()-start)/1e6;
        if(rep>=0){times[rep]=ms;printf("{\"kind\":\"graph_probe\",\"mode\":\"%s\",\"depth\":%d,\"rep\":%d,\"wall_ms\":%.6f,\"flops\":%.0f,\"traffic_bytes\":%.0f,\"correct\":true}\n",mode,depth,rep,ms,flops,traffic);}
    }
    float first,last;ggml_backend_tensor_get(current,&first,0,sizeof first);ggml_backend_tensor_get(current,&last,ggml_nbytes(current)-sizeof last,sizeof last);
    if(copy){float a,b;ggml_backend_tensor_get(initial,&a,0,sizeof a);ggml_backend_tensor_get(initial,&b,ggml_nbytes(initial)-sizeof b,sizeof b);assert(first==a&&last==b);}
    else assert(isfinite(first)&&isfinite(last)&&fabsf(first-0.25f)<0.02f&&fabsf(last-0.25f)<0.02f);
    qsort(times,7,sizeof *times,compare_double);
    GraphProbe r={times[3],flops,traffic,times[0],times[6]};ggml_backend_buffer_free(buffer);ggml_free(ctx);return r;
}
static cxf_roofline_device graph_hardware_probe(void){
    ggml_backend_dev_t dev=ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);assert(dev);
    ggml_backend_t backend=ggml_backend_dev_init(dev,NULL);assert(backend);
    const char *modes[]={"q8_chain","f16_chain","fa_chain","copy_chain"};double rates[4];
    for(int i=0;i<4;i++){
        GraphProbe a=graph_probe(backend,modes[i],1),b=graph_probe(backend,modes[i],16);
        double delta_ms=b.median_ms-a.median_ms;assert(delta_ms>0 && b.min_ms>a.max_ms);
        double per_op=delta_ms/15,intercept=a.median_ms-per_op;
        rates[i]=(i==3?b.bytes-a.bytes:b.flops-a.flops)/(delta_ms/1000);
        printf("{\"kind\":\"graph_rate\",\"mode\":\"%s\",\"depth1_ms\":%.6f,\"depth16_ms\":%.6f,\"per_op_ms\":%.9f,\"graph_intercept_ms\":%.9f,\"effective_rate\":%.6f}\n",modes[i],a.median_ms,b.median_ms,per_op,intercept,rates[i]);
    }
    cxf_roofline_device d={rates[0],rates[2],rates[3]};ggml_backend_free(backend);
    printf("{\"kind\":\"graph_profile\",\"linear_flops_s\":%.6f,\"attention_flops_s\":%.6f,\"bandwidth_bytes_s\":%.6f,\"attention_primitive\":\"FLASH_ATTN_EXT\",\"prefix_tokens\":2048,\"new_tokens\":512,\"depths\":[1,16]}\n",d.linear_flops_s,d.attention_flops_s,d.bandwidth_bytes_s);
    backend=ggml_backend_dev_init(dev,NULL);assert(backend);
    cxf_roofline_device warm;
    warm.linear_flops_s=probe_primitive(backend,"warm_q8_gemm",GGML_TYPE_Q8_0,false);
    warm.attention_flops_s=probe_primitive(backend,"warm_f16_gemm",GGML_TYPE_F16,false);
    warm.bandwidth_bytes_s=probe_primitive(backend,"warm_copy_64mib",GGML_TYPE_F32,true);
    printf("{\"kind\":\"warm_hardware_profile\",\"linear_flops_s\":%.6f,\"attention_flops_s\":%.6f,\"bandwidth_bytes_s\":%.6f}\n",warm.linear_flops_s,warm.attention_flops_s,warm.bandwidth_bytes_s);
    ggml_backend_free(backend);return d;
}
#endif
