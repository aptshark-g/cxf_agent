#include "cxf/roofline.h"
#include "cost_internal.h"
#include <math.h>

static bool positive(double x){return isfinite(x)&&x>0&&x<=1e30;}
static double maximum(double a,double b){return a>b?a:b;}
static void op(cxf_roofline_result *r,double flops,double bytes,double compute,double bandwidth,
               uint32_t repeats,bool attention){
    double c=1000*flops/compute*repeats,m=1000*bytes/bandwidth*repeats,t=maximum(c,m);
    r->compute_ms+=c;r->memory_ms+=m;r->dominant_flops+=flops*repeats;r->operand_bytes+=bytes*repeats;
    if(attention)r->attention_ms+=t;else r->linear_ms+=t;
}
cxf_result cxf_roofline_estimate(const cxf_roofline_spec *s,const cxf_roofline_device *d,
                                 cxf_cost_work w,cxf_roofline_result *out){
    if(!s||!d||!out)return CXF_ERR_RANGE;
    if(!s->layers||s->layers>256||!s->embedding||!s->query_heads||!s->kv_heads||
       s->kv_heads>s->query_heads||s->query_heads%s->kv_heads||!s->key_dim||!s->value_dim||
       !s->ffn||!s->vocabulary||!s->kv_scalar_bytes||!s->activation_scalar_bytes||
       !positive(s->output_weight_bytes)||!positive(s->embedding_row_bytes)||
       !w.batch_tokens||w.new_tokens>UINT32_MAX-w.prefix_tokens)return CXF_ERR_RANGE;
    for(int i=0;i<7;i++)if(!positive(s->weight_bytes[i]))return CXF_ERR_RANGE;
    uint64_t calls=((uint64_t)w.new_tokens+w.batch_tokens-1)/w.batch_tokens;
    if(calls>4096)return CXF_ERR_RANGE;
    cxf_roofline_result r={0};r.decode_calls=(uint32_t)calls;
    if(!w.new_tokens){*out=r;return CXF_OK;}
    double rates[]={d->linear_flops_s,d->attention_flops_s,d->bandwidth_bytes_s};
    for(int i=0;i<3;i++)if(!isfinite(rates[i])||rates[i]<0||rates[i]>1e30)return CXF_ERR_RANGE;
    for(int i=0;i<3;i++)if(!rates[i])return CXF_ERR_EMPTY;
    double E=s->embedding,Q=(double)s->query_heads*s->key_dim,K=(double)s->kv_heads*s->key_dim;
    double V=(double)s->kv_heads*s->value_dim,O=(double)s->query_heads*s->value_dim;
    double input[]={E,E,E,O,E,E,s->ffn},output[]={Q,K,V,E,s->ffn,s->ffn,E};
    uint32_t processed=0;
    while(processed<w.new_tokens){
        uint32_t n=w.new_tokens-processed;if(n>w.batch_tokens)n=w.batch_tokens;
        double context=(double)w.prefix_tokens+processed,pairs=n*context+(double)n*(n+1.0)/2;
        for(int i=0;i<7;i++){
            uint32_t last_n=(i>=4 && s->last_layer_ffn_tokens && s->last_layer_ffn_tokens<n)?s->last_layer_ffn_tokens:n;
            op(&r,2.0*n*input[i]*output[i],s->weight_bytes[i]+(double)s->activation_scalar_bytes*n*(input[i]+output[i]),
                d->linear_flops_s,d->bandwidth_bytes_s,s->layers-1,false);
            op(&r,2.0*last_n*input[i]*output[i],s->weight_bytes[i]+(double)s->activation_scalar_bytes*last_n*(input[i]+output[i]),
                d->linear_flops_s,d->bandwidth_bytes_s,1,false);
        }
        /* Fused attention traffic proxy: one K/V pass per chunk; no NxL score
         * materialization. Actual tiling/cache traffic requires profiling. */
        double f=2.0*s->query_heads*((double)s->key_dim+s->value_dim)*pairs;
        double b=s->kv_scalar_bytes*(context+n)*(K+V)+(double)s->activation_scalar_bytes*n*(Q+O);
        op(&r,f,b,d->attention_flops_s,d->bandwidth_bytes_s,s->layers,true);
        /* Exactly one logits position per native batch in current cxf decode. */
        op(&r,2.0*E*s->vocabulary,s->output_weight_bytes+s->activation_scalar_bytes*(E+s->vocabulary)+n*s->embedding_row_bytes,
            d->linear_flops_s,d->bandwidth_bytes_s,1,false);
        processed+=n;
    }
    r.ms=r.linear_ms+r.attention_ms;
    if(!isfinite(r.ms)||!isfinite(r.compute_ms)||!isfinite(r.memory_ms)||r.ms>1e100)return CXF_ERR_RANGE;
    *out=r;return CXF_OK;
}
cxf_result cxf_roofline_init(cxf_roofline_calibration *m,uint64_t profile){return m?cxf_cost_init(&m->fit,profile):CXF_ERR_RANGE;}
cxf_result cxf_roofline_predict(const cxf_roofline_calibration *m,uint64_t profile,const cxf_roofline_spec *s,
    const cxf_roofline_device *d,cxf_cost_work w,cxf_cost_prediction *out){
    if(!m||!out)return CXF_ERR_RANGE;
    if(!profile||m->fit.profile!=profile)return CXF_ERR_BAD_HANDLE;
    cxf_roofline_result r;cxf_result rc=cxf_roofline_estimate(s,d,w,&r);if(rc!=CXF_OK)return rc;
    double x[]={r.linear_ms,r.attention_ms,r.decode_calls};
    return cxf_cost_predict_vector(&m->fit,profile,w,x,out);
}
cxf_result cxf_roofline_observe(cxf_roofline_calibration *m,uint64_t profile,const cxf_roofline_spec *s,
    const cxf_roofline_device *d,cxf_cost_work w,double ms){
    if(!m)return CXF_ERR_RANGE;
    if(!profile||m->fit.profile!=profile)return CXF_ERR_BAD_HANDLE;
    cxf_roofline_result r;cxf_result rc=cxf_roofline_estimate(s,d,w,&r);if(rc!=CXF_OK)return rc;
    double x[]={r.linear_ms,r.attention_ms,r.decode_calls};
    return cxf_cost_observe_vector(&m->fit,profile,w,x,ms);
}

static int regime(uint32_t n){return n<=8?0:n<20?1:2;}
static double mix(double a,double b,double weight){return a+(b-a)*weight;}
cxf_result cxf_roofline_shape_estimate(const cxf_roofline_spec *s,const cxf_roofline_shape *t,
    cxf_cost_work w,cxf_roofline_result *out,bool *extrapolated){
    if(!s||!t||!out||!extrapolated||!w.batch_tokens||w.new_tokens>UINT32_MAX-w.prefix_tokens||
       !t->width_count||t->width_count>CXF_ROOFLINE_WIDTHS||!t->context_count||t->context_count>CXF_ROOFLINE_CONTEXTS||
       !positive(t->bandwidth_bytes_s)||((uint64_t)w.new_tokens+w.batch_tokens-1)/w.batch_tokens>4096)return CXF_ERR_RANGE;
    for(uint32_t i=0;i<t->width_count;i++){
        if(!t->widths[i]||(i&&t->widths[i]<=t->widths[i-1])||!positive(t->linear_flops_s[i]))return CXF_ERR_RANGE;
        for(uint32_t j=0;j<t->context_count;j++)if(!positive(t->attention_ms[i][j]))return CXF_ERR_RANGE;
    }
    for(uint32_t j=1;j<t->context_count;j++)if(t->contexts[j]<=t->contexts[j-1])return CXF_ERR_RANGE;
    cxf_roofline_device d={t->linear_flops_s[0],1,t->bandwidth_bytes_s};cxf_roofline_result r;
    cxf_result rc=cxf_roofline_estimate(s,&d,(cxf_cost_work){w.prefix_tokens,0,w.batch_tokens},&r);if(rc!=CXF_OK)return rc;
    bool extra=false;uint32_t done=0;
    while(done<w.new_tokens){
        uint32_t n=w.new_tokens-done;if(n>w.batch_tokens)n=w.batch_tokens;
        uint32_t p=w.prefix_tokens+done,lo=0,hi=0;bool found=false;
        for(uint32_t i=0;i<t->width_count;i++)if(regime(t->widths[i])==regime(n)){
            if(!found){lo=hi=i;found=true;}
            if(t->widths[i]<=n)lo=i;
            hi=i;if(t->widths[i]>=n)break;
        }
        if(!found){
            extra=true;uint64_t best=UINT64_MAX;
            for(uint32_t i=0;i<t->width_count;i++){uint64_t dist=t->widths[i]>n?t->widths[i]-n:n-t->widths[i];if(dist<best){best=dist;lo=hi=i;}}
        }
        if(n<t->widths[lo]||n>t->widths[hi])extra=true;
        double nw=lo==hi?0:((double)n-t->widths[lo])/(t->widths[hi]-t->widths[lo]);
        if(nw<0)nw=0;if(nw>1)nw=1;
        uint32_t pl=0,ph=0;
        for(uint32_t j=0;j<t->context_count;j++){if(t->contexts[j]<=p)pl=j;ph=j;if(t->contexts[j]>=p)break;}
        if(p<t->contexts[pl]||p>t->contexts[ph])extra=true;
        double pw=pl==ph?0:((double)p-t->contexts[pl])/(t->contexts[ph]-t->contexts[pl]);
        if(pw<0)pw=0;if(pw>1)pw=1;
        double attn=mix(mix(t->attention_ms[lo][pl],t->attention_ms[lo][ph],pw),
                        mix(t->attention_ms[hi][pl],t->attention_ms[hi][ph],pw),nw);
        /* Fixed matrix family: FLOPs scale with query width. Interpolate its
         * service time (width/rate), not inverse rate then multiply by width:
         * the latter creates artificial quadratic curvature between knots. */
        d.linear_flops_s=n/mix(t->widths[lo]/t->linear_flops_s[lo],t->widths[hi]/t->linear_flops_s[hi],nw);
        double f=2.0*s->query_heads*((double)s->key_dim+s->value_dim)*((double)n*p+(double)n*(n+1.0)/2);
        d.attention_flops_s=1000.0*f/attn;
        cxf_roofline_result part;rc=cxf_roofline_estimate(s,&d,(cxf_cost_work){p,n,n},&part);if(rc!=CXF_OK)return rc;
        r.ms+=part.ms;r.linear_ms+=part.linear_ms;r.attention_ms+=part.attention_ms;
        r.compute_ms+=part.compute_ms;r.memory_ms+=part.memory_ms;r.dominant_flops+=part.dominant_flops;r.operand_bytes+=part.operand_bytes;r.decode_calls++;
        done+=n;
    }
    if(!isfinite(r.ms)||r.ms>1e100)return CXF_ERR_RANGE;
    *out=r;*extrapolated=extra;return CXF_OK;
}
cxf_result cxf_roofline_shape_predict(const cxf_roofline_calibration *m,uint64_t profile,
    const cxf_roofline_spec *s,const cxf_roofline_shape *t,cxf_cost_work w,cxf_cost_prediction *out){
    if(!m||!out)return CXF_ERR_RANGE;
    if(!profile||m->fit.profile!=profile)return CXF_ERR_BAD_HANDLE;
    cxf_roofline_result r;bool extra;cxf_result rc=cxf_roofline_shape_estimate(s,t,w,&r,&extra);if(rc!=CXF_OK)return rc;
    double x[]={r.linear_ms,r.attention_ms,r.decode_calls};cxf_cost_prediction q;
    rc=cxf_cost_predict_vector(&m->fit,profile,w,x,&q);if(rc!=CXF_OK)return rc;
    q.extrapolated=q.extrapolated||extra;*out=q;return CXF_OK;
}
cxf_result cxf_roofline_shape_observe(cxf_roofline_calibration *m,uint64_t profile,
    const cxf_roofline_spec *s,const cxf_roofline_shape *t,cxf_cost_work w,double ms){
    if(!m)return CXF_ERR_RANGE;
    if(!profile||m->fit.profile!=profile)return CXF_ERR_BAD_HANDLE;
    cxf_roofline_result r;bool extra;cxf_result rc=cxf_roofline_shape_estimate(s,t,w,&r,&extra);if(rc!=CXF_OK)return rc;
    double x[]={r.linear_ms,r.attention_ms,r.decode_calls};return cxf_cost_observe_vector(&m->fit,profile,w,x,ms);
}
