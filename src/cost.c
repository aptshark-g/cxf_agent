#include "cxf/cost.h"
#include "cost_internal.h"
#include <math.h>
#include <string.h>

static cxf_result features(cxf_cost_work w, double x[3]) {
    if (!w.batch_tokens || w.new_tokens > UINT32_MAX - w.prefix_tokens) return CXF_ERR_RANGE;
    x[0] = ((uint64_t)w.new_tokens + w.batch_tokens - 1) / w.batch_tokens;
    x[1] = w.new_tokens / 512.0;
    x[2] = ((double)w.prefix_tokens * w.new_tokens +
            (double)w.new_tokens * (w.new_tokens ? w.new_tokens - 1 : 0) / 2.0) / 1048576.0;
    return CXF_OK;
}
static double dot(const double *a, const double *b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static double absolute(double x) { return x < 0 ? -x : x; }

cxf_result cxf_cost_init(cxf_cost_model *m, uint64_t profile) {
    if (!m || !profile) return CXF_ERR_RANGE;
    memset(m, 0, sizeof *m); m->profile = profile; return CXF_OK;
}
cxf_result cxf_cost_predict_vector(const cxf_cost_model *m, uint64_t profile,
                            cxf_cost_work w, const double x[3], cxf_cost_prediction *out) {
    if (!m || !out) return CXF_ERR_RANGE;
    if (!profile || m->profile != profile) return CXF_ERR_BAD_HANDLE;
    if (w.new_tokens && !m->count) return CXF_ERR_EMPTY;
    cxf_cost_prediction p = {0}; p.observations = m->count;
    p.ms = dot(m->coefficient, x); p.fit_mae_ms = w.new_tokens ? m->fit_mae_ms : 0;
    if (w.new_tokens) {
        uint32_t min_p=UINT32_MAX,max_p=0,min_n=UINT32_MAX,max_n=0;
        bool batch_seen=false;
        for (uint32_t i=0;i<m->count;i++) {
            cxf_cost_work t=m->samples[i].work;
            if (t.prefix_tokens<min_p) min_p=t.prefix_tokens;
            if (t.prefix_tokens>max_p) max_p=t.prefix_tokens;
            if (t.new_tokens<min_n) min_n=t.new_tokens;
            if (t.new_tokens>max_n) max_n=t.new_tokens;
            if (t.batch_tokens==w.batch_tokens) batch_seen=true;
        }
        p.extrapolated = !batch_seen || w.prefix_tokens<min_p || w.prefix_tokens>max_p ||
                         w.new_tokens<min_n || w.new_tokens>max_n;
    }
    *out=p; return CXF_OK;
}
cxf_result cxf_cost_observe_vector(cxf_cost_model *m, uint64_t profile, cxf_cost_work w, const double x[3], double ms) {
    if (!m) return CXF_ERR_RANGE;
    if (!profile || m->profile!=profile) return CXF_ERR_BAD_HANDLE;
    /* Keep numeric products bounded even for an accidentally wrong time unit. */
    if (!w.new_tokens || !isfinite(ms) || ms<0 || ms>1e12) return CXF_ERR_RANGE;
    cxf_cost_sample *s=&m->samples[m->next];
    memcpy(s->x,x,3*sizeof *x);s->ms=ms;s->work=w;
    m->next=(m->next+1)%CXF_COST_CAPACITY;
    if(m->count<CXF_COST_CAPACITY)m->count++;
    double a[3][3]={{0}},b[3]={0};
    for(uint32_t i=0;i<m->count;i++)for(int j=0;j<3;j++) {
        b[j]+=m->samples[i].x[j]*m->samples[i].ms;
        for(int k=0;k<3;k++)a[j][k]+=m->samples[i].x[j]*m->samples[i].x[k];
    }
    /* Nonnegative coordinate descent on three physical work proxies. Scaling
     * keeps the tiny normal system well behaved; zero/rank-deficient columns
     * are supported without inverting a matrix. Warm-start after each sample. */
    for(int pass=0;pass<128;pass++)for(int j=0;j<3;j++) {
        double residual=b[j];
        for(int k=0;k<3;k++)if(k!=j)residual-=a[j][k]*m->coefficient[k];
        m->coefficient[j]=a[j][j]>0 ? residual/a[j][j] : 0;
        if(m->coefficient[j]<0)m->coefficient[j]=0;
    }
    m->fit_mae_ms=0;
    for(uint32_t i=0;i<m->count;i++)
        m->fit_mae_ms+=absolute(dot(m->coefficient,m->samples[i].x)-m->samples[i].ms)/m->count;
    return CXF_OK;
}

cxf_result cxf_cost_predict(const cxf_cost_model *m,uint64_t profile,cxf_cost_work w,cxf_cost_prediction *out) {
    if(!m || !out)return CXF_ERR_RANGE;
    if(!profile || m->profile!=profile)return CXF_ERR_BAD_HANDLE;
    double x[3];cxf_result rc=features(w,x);if(rc!=CXF_OK)return rc;
    return cxf_cost_predict_vector(m,profile,w,x,out);
}
cxf_result cxf_cost_observe(cxf_cost_model *m,uint64_t profile,cxf_cost_work w,double ms) {
    if(!m)return CXF_ERR_RANGE;
    if(!profile || m->profile!=profile)return CXF_ERR_BAD_HANDLE;
    double x[3];cxf_result rc=features(w,x);if(rc!=CXF_OK)return rc;
    return cxf_cost_observe_vector(m,profile,w,x,ms);
}
