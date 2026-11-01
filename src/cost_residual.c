#include "cxf/cost_residual.h"
#include <math.h>
#include <string.h>

static bool numeric(cxf_cost_work w,double raw,double attention,double x[3]){
    if(!w.batch_tokens||w.new_tokens>UINT32_MAX-w.prefix_tokens||
       !isfinite(raw)||raw<0||raw>1e12||!isfinite(attention)||attention<0||attention>raw)return false;
    x[0]=((uint64_t)w.new_tokens+w.batch_tokens-1)/w.batch_tokens;
    x[1]=w.new_tokens/512.0;x[2]=attention/100.0;
    return w.new_tokens || (!raw&&!attention);
}
cxf_result cxf_cost_residual_init(cxf_cost_residual *m,uint64_t profile){
    if(!m||!profile)return CXF_ERR_RANGE;
    memset(m,0,sizeof *m);m->profile=profile;return CXF_OK;
}
cxf_result cxf_cost_residual_predict(const cxf_cost_residual *m,uint64_t profile,cxf_cost_work w,
    double raw,double attention,cxf_cost_prediction *out){
    if(!m||!out)return CXF_ERR_RANGE;
    if(!profile||m->profile!=profile)return CXF_ERR_BAD_HANDLE;
    double x[3];if(!numeric(w,raw,attention,x))return CXF_ERR_RANGE;
    if(w.new_tokens&&!m->count)return CXF_ERR_EMPTY;
    cxf_cost_prediction q={0};q.observations=m->count;
    if(w.new_tokens){
        q.ms=raw;for(int j=0;j<3;j++)q.ms+=m->coefficient[j]*x[j];
        if(!isfinite(q.ms))return CXF_ERR_RANGE;
        if(q.ms<0)q.ms=0;
        uint32_t min_p=UINT32_MAX,max_p=0,min_n=UINT32_MAX,max_n=0;bool batch_seen=false;
        for(uint32_t i=0;i<m->count;i++){
            cxf_cost_work t=m->samples[i].work;
            if(t.prefix_tokens<min_p)min_p=t.prefix_tokens;if(t.prefix_tokens>max_p)max_p=t.prefix_tokens;
            if(t.new_tokens<min_n)min_n=t.new_tokens;if(t.new_tokens>max_n)max_n=t.new_tokens;
            if(t.batch_tokens==w.batch_tokens)batch_seen=true;
        }
        q.extrapolated=!batch_seen||w.prefix_tokens<min_p||w.prefix_tokens>max_p||w.new_tokens<min_n||w.new_tokens>max_n;
        q.fit_mae_ms=m->fit_mae_ms; /* descriptive, not a confidence interval */
    }
    *out=q;return CXF_OK;
}
cxf_result cxf_cost_residual_observe(cxf_cost_residual *m,uint64_t profile,cxf_cost_work w,
    double raw,double attention,double actual){
    if(!m)return CXF_ERR_RANGE;
    if(!profile||m->profile!=profile)return CXF_ERR_BAD_HANDLE;
    double x[3];if(!numeric(w,raw,attention,x)||!w.new_tokens||!isfinite(actual)||actual<0||actual>1e12)return CXF_ERR_RANGE;
    cxf_cost_residual next=*m;cxf_cost_residual_sample *s=&next.samples[next.next];
    memcpy(s->x,x,sizeof x);s->residual_ms=actual-raw;s->weight=1.0/(actual>20?actual:20);s->work=w;
    next.next=(next.next+1)%CXF_COST_CAPACITY;if(next.count<CXF_COST_CAPACITY)next.count++;
    double scale[3]={0},total=0;
    for(uint32_t i=0;i<next.count;i++){
        s=&next.samples[i];total+=s->weight;
        for(int j=0;j<3;j++)scale[j]+=s->weight*s->x[j]*s->x[j];
    }
    for(int j=0;j<3;j++){scale[j]=sqrt(scale[j]/total);if(!scale[j])scale[j]=1;}
    double a[3][3]={{0}},b[3]={0};
    for(uint32_t i=0;i<next.count;i++){
        s=&next.samples[i];double z[3];for(int j=0;j<3;j++)z[j]=s->x[j]/scale[j];
        for(int j=0;j<3;j++){
            b[j]+=s->weight*z[j]*s->residual_ms;
            for(int k=0;k<3;k++)a[j][k]+=s->weight*z[j]*z[k];
        }
    }
    for(int j=0;j<3;j++)a[j][j]+=0.01*total;
    /* Normalized ridge is positive definite, including zero/collinear columns.
     * Cholesky solves three variables directly, no iteration budget. */
    double l[3][3]={{0}},y[3],c[3];
    for(int j=0;j<3;j++)for(int k=0;k<=j;k++){
        double v=a[j][k];for(int h=0;h<k;h++)v-=l[j][h]*l[k][h];
        if(j==k){if(!(v>0)||!isfinite(v))return CXF_ERR_RANGE;l[j][k]=sqrt(v);}
        else l[j][k]=v/l[k][k];
    }
    for(int j=0;j<3;j++){double v=b[j];for(int k=0;k<j;k++)v-=l[j][k]*y[k];y[j]=v/l[j][j];}
    for(int j=2;j>=0;j--){double v=y[j];for(int k=j+1;k<3;k++)v-=l[k][j]*c[k];c[j]=v/l[j][j];next.coefficient[j]=c[j]/scale[j];if(!isfinite(next.coefficient[j]))return CXF_ERR_RANGE;}
    next.fit_mae_ms=0;
    for(uint32_t i=0;i<next.count;i++){
        s=&next.samples[i];double error=-s->residual_ms;
        for(int j=0;j<3;j++)error+=next.coefficient[j]*s->x[j];
        next.fit_mae_ms+=fabs(error)/next.count;
    }
    if(!isfinite(next.fit_mae_ms))return CXF_ERR_RANGE;
    *m=next;return CXF_OK;
}
