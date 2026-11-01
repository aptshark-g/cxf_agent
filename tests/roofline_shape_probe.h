#ifndef CXF_TEST_ROOFLINE_SHAPE_PROBE_H
#define CXF_TEST_ROOFLINE_SHAPE_PROBE_H
static GraphProbe shape_probe(ggml_backend_t backend,const char *mode,int depth,int64_t n,int64_t prefix){
    bool copy=!strcmp(mode,"copy_chain"),fa=!strcmp(mode,"fa_chain"),q8=!strcmp(mode,"q8_chain");
    assert(depth==1||depth==16);
    struct ggml_context *ctx=ggml_init((struct ggml_init_params){4*1024*1024,NULL,true});assert(ctx);
    struct ggml_tensor *weights[16]={0},*keys[16]={0},*values[16]={0},*mask=NULL,*initial;
    const int64_t span=((prefix+n+255)/256)*256;
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
        if(rep>=0){times[rep]=ms;printf("{\"kind\":\"shape_probe\",\"new_tokens\":%u,\"prefix_tokens\":%u,\"mode\":\"%s\",\"depth\":%d,\"rep\":%d,\"wall_ms\":%.6f,\"flops\":%.0f,\"traffic_bytes\":%.0f,\"correct\":true}\n",(unsigned)n,(unsigned)prefix,mode,depth,rep,ms,flops,traffic);}
    }
    float first,last;ggml_backend_tensor_get(current,&first,0,sizeof first);ggml_backend_tensor_get(current,&last,ggml_nbytes(current)-sizeof last,sizeof last);
    if(copy){float a,b;ggml_backend_tensor_get(initial,&a,0,sizeof a);ggml_backend_tensor_get(initial,&b,ggml_nbytes(initial)-sizeof b,sizeof b);assert(first==a&&last==b);}
    else assert(isfinite(first)&&isfinite(last)&&fabsf(first-0.25f)<0.02f&&fabsf(last-0.25f)<0.02f);
    qsort(times,7,sizeof *times,compare_double);
    GraphProbe r={times[3],flops,traffic,times[0],times[6]};ggml_backend_buffer_free(buffer);ggml_free(ctx);return r;
}

static double shape_slope(ggml_backend_t b,const char *mode,uint32_t n,uint32_t p,bool time_only){
    GraphProbe one=shape_probe(b,mode,1,n,p),many=shape_probe(b,mode,16,n,p);
    double delta=many.median_ms-one.median_ms;assert(delta>0 && many.min_ms>one.max_ms);
    double ms=delta/15,rate=(many.flops-one.flops)/(delta/1000);
    printf("{\"kind\":\"shape_rate\",\"mode\":\"%s\",\"new_tokens\":%u,\"prefix_tokens\":%u,\"per_op_ms\":%.9f,\"effective_rate\":%.6f}\n",mode,n,p,ms,rate);
    return time_only?ms:rate;
}
static cxf_roofline_shape build_shape_table(double bandwidth){
    uint64_t start=tick();cxf_roofline_shape t={.width_count=6,.context_count=3,
        .widths={8,16,19,20,128,512},.contexts={0,2048,6144},.bandwidth_bytes_s=bandwidth};
#ifdef CXF_FROZEN_STUDY
    t.context_count=4;t.contexts[3]=7168;
#endif
    ggml_backend_t b=ggml_backend_dev_init(ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU),NULL);assert(b);
    for(uint32_t i=0;i<t.width_count;i++){
        t.linear_flops_s[i]=shape_slope(b,"q8_chain",t.widths[i],0,false);
        for(uint32_t j=0;j<t.context_count;j++)t.attention_ms[i][j]=shape_slope(b,"fa_chain",t.widths[i],t.contexts[j],true);
    }
    ggml_backend_free(b);
    printf("{\"kind\":\"shape_table\",\"widths\":[8,16,19,20,128,512],\"contexts\":[");
    for(uint32_t j=0;j<t.context_count;j++)printf("%s%u",j?",":"",t.contexts[j]);
    printf("],\"linear_flops_s\":[");
    for(uint32_t i=0;i<t.width_count;i++)printf("%s%.9f",i?",":"",t.linear_flops_s[i]);
    printf("],\"attention_ms\":[");
    for(uint32_t i=0;i<t.width_count;i++){printf("%s[",i?",":"");for(uint32_t j=0;j<t.context_count;j++)printf("%s%.9f",j?",":"",t.attention_ms[i][j]);printf("]");}
    printf("],\"bandwidth_bytes_s\":%.9f,\"state_bytes\":%zu,\"build_ms\":%.6f}\n",bandwidth,sizeof t,(tick()-start)/1e6);
    return t;
}
#endif
