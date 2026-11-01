#include "cxf/roofline.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static double absd(double x){return x<0?-x:x;}
int main(void){
    cxf_roofline_spec s={.layers=1,.embedding=2,.query_heads=1,.kv_heads=1,.key_dim=2,.value_dim=2,.ffn=2,.vocabulary=2,
        .weight_bytes={8,8,8,8,8,8,8},.output_weight_bytes=8,.embedding_row_bytes=4,.kv_scalar_bytes=2,.activation_scalar_bytes=4};
    cxf_roofline_device d={1000,1000,1000};cxf_roofline_result r={0},before;
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){0,1,1},&r)==CXF_OK);
    assert(r.dominant_flops==72 && r.operand_bytes==220 && r.ms==220 && r.decode_calls==1);
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){0,2,2},&r)==CXF_OK);
    assert(r.dominant_flops==144 && r.operand_bytes==360 && r.ms==360 && r.decode_calls==1);
    s.last_layer_ffn_tokens=1;
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){0,2,2},&r)==CXF_OK);
    assert(r.dominant_flops==120 && r.operand_bytes==312 && r.ms==312);
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){0,2,1},&r)==CXF_OK && r.ms==448);
    s.layers=2;
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){0,2,2},&r)==CXF_OK);
    assert(r.dominant_flops==256 && r.operand_bytes==640 && r.ms==640);
    s.layers=1;s.last_layer_ffn_tokens=0;
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){0,2,1},&r)==CXF_OK && r.ms==448 && r.decode_calls==2);
    before=r;cxf_roofline_device invalid=d;invalid.bandwidth_bytes_s=0;
    assert(cxf_roofline_estimate(&s,&invalid,(cxf_cost_work){0,1,1},&r)==CXF_ERR_EMPTY && !memcmp(&r,&before,sizeof r));
    invalid.linear_flops_s=NAN;assert(cxf_roofline_estimate(&s,&invalid,(cxf_cost_work){0,1,1},&r)==CXF_ERR_RANGE);
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){UINT32_MAX,1,1},&r)==CXF_ERR_RANGE);
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){0,4097,1},&r)==CXF_ERR_RANGE);
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){0,0,1},&r)==CXF_OK && r.ms==0);
    cxf_roofline_calibration m,copy;assert(cxf_roofline_init(&m,99)==CXF_OK);cxf_cost_prediction prediction;
    assert(cxf_roofline_predict(&m,99,&s,&d,(cxf_cost_work){1,1,2},&prediction)==CXF_ERR_EMPTY);
    copy=m;assert(cxf_roofline_observe(&m,99,&s,&d,(cxf_cost_work){0,1,1},NAN)==CXF_ERR_RANGE && !memcmp(&m,&copy,sizeof m));
    assert(cxf_roofline_observe(&m,98,&s,&d,(cxf_cost_work){0,1,1},200)==CXF_ERR_BAD_HANDLE);
    for(int rep=0;rep<4;rep++)for(uint32_t p=0;p<=8;p+=4)for(uint32_t n=1;n<=4;n*=2){
        cxf_cost_work w={p,n,2};assert(cxf_roofline_estimate(&s,&d,w,&r)==CXF_OK);
        assert(cxf_roofline_observe(&m,99,&s,&d,w,1.2*r.linear_ms+0.7*r.attention_ms+3*r.decode_calls)==CXF_OK);
    }
    cxf_cost_work unseen={2,3,2};assert(cxf_roofline_estimate(&s,&d,unseen,&r)==CXF_OK);
    assert(cxf_roofline_predict(&m,99,&s,&d,unseen,&prediction)==CXF_OK);
    double expected=1.2*r.linear_ms+0.7*r.attention_ms+3*r.decode_calls;
    assert(absd(prediction.ms-expected)/expected<0.02);
    s.query_heads=2; /* GQA: Q/O widen while K/V stay unchanged. */
    assert(cxf_roofline_estimate(&s,&d,(cxf_cost_work){0,1,1},&r)==CXF_OK);
    assert(r.dominant_flops==96 && r.operand_bytes==252);
    s.query_heads=1;s.kv_heads=2;assert(cxf_roofline_estimate(&s,&d,unseen,&r)==CXF_ERR_RANGE);
    s.kv_heads=1;
    cxf_roofline_shape table={.width_count=2,.context_count=2,.widths={20,40},.contexts={0,100},
        .linear_flops_s={1000,2000},.attention_ms={{2,12},{4,24}},.bandwidth_bytes_s=1e30};bool extra=true;
    assert(cxf_roofline_shape_estimate(&s,&table,(cxf_cost_work){50,30,30},&r,&extra)==CXF_OK && !extra);
    /* Both endpoints have identical width/rate, so per-matrix time is flat.
     * Inverse-rate interpolation would incorrectly give 1266 ms here. */
    assert(absd(r.linear_ms-1125.3333333333333)<1e-8 && absd(r.attention_ms-10.5)<1e-8);
    cxf_roofline_result first,second;
    assert(cxf_roofline_shape_estimate(&s,&table,(cxf_cost_work){0,30,30},&first,&extra)==CXF_OK);
    assert(cxf_roofline_shape_estimate(&s,&table,(cxf_cost_work){30,30,30},&second,&extra)==CXF_OK);
    assert(cxf_roofline_shape_estimate(&s,&table,(cxf_cost_work){0,60,30},&r,&extra)==CXF_OK && r.decode_calls==2);
    assert(absd(r.ms-first.ms-second.ms)<1e-8);
    table.widths[0]=19;table.widths[1]=20;
    assert(cxf_roofline_shape_estimate(&s,&table,(cxf_cost_work){0,19,19},&r,&extra)==CXF_OK && !extra && absd(r.attention_ms-2)<1e-8);
    assert(cxf_roofline_shape_estimate(&s,&table,(cxf_cost_work){0,20,20},&r,&extra)==CXF_OK && !extra && absd(r.attention_ms-4)<1e-8);
    assert(cxf_roofline_shape_estimate(&s,&table,(cxf_cost_work){0,16,16},&r,&extra)==CXF_OK && extra && absd(r.attention_ms-2)<1e-8);
    before=r;extra=true;table.widths[1]=19;
    assert(cxf_roofline_shape_estimate(&s,&table,(cxf_cost_work){0,20,20},&r,&extra)==CXF_ERR_RANGE && extra && !memcmp(&r,&before,sizeof r));
    table.widths[1]=20;table.attention_ms[0][0]=NAN;
    assert(cxf_roofline_shape_estimate(&s,&table,(cxf_cost_work){0,20,20},&r,&extra)==CXF_ERR_RANGE);
    table.attention_ms[0][0]=2;
    cxf_roofline_calibration shaped;assert(cxf_roofline_init(&shaped,991)==CXF_OK);
    assert(cxf_roofline_shape_predict(&shaped,991,&s,&table,(cxf_cost_work){0,16,16},&prediction)==CXF_ERR_EMPTY);
    copy=shaped;assert(cxf_roofline_shape_observe(&shaped,991,&s,&table,(cxf_cost_work){0,16,16},NAN)==CXF_ERR_RANGE && !memcmp(&shaped,&copy,sizeof copy));
    assert(cxf_roofline_shape_observe(&shaped,991,&s,&table,(cxf_cost_work){0,16,16},10)==CXF_OK);
    assert(cxf_roofline_shape_predict(&shaped,991,&s,&table,(cxf_cost_work){0,16,16},&prediction)==CXF_OK && prediction.extrapolated);
    assert(cxf_roofline_shape_predict(&shaped,992,&s,&table,(cxf_cost_work){0,16,16},&prediction)==CXF_ERR_BAD_HANDLE);
    printf("PASS shape profile: hand interpolation, 19/20 boundary, advancing chunk context, extrapolation, profile and failure atomicity; table=%zu bytes\n",sizeof table);
    printf("PASS roofline: manual FLOPs/bytes, per-chunk final logits, last-layer FFN pruning, GQA validation, unknown rates, failure atomicity, profile and unseen calibrated shape; state=%zu bytes\n",sizeof m);
    return 0;
}
