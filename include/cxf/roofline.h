#ifndef CXF_ROOFLINE_H
#define CXF_ROOFLINE_H
#include "cost.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Uniform dense decoder/GQA only. Weight sizes include quantization metadata.
 * Seven per-layer matrices: Q,K,V,O,gate,up,down; not whole-model file bytes. */
typedef struct cxf_roofline_spec {
    uint32_t layers, embedding, query_heads, kv_heads, key_dim, value_dim, ffn, vocabulary;
    double weight_bytes[7], output_weight_bytes, embedding_row_bytes;
    uint32_t kv_scalar_bytes, activation_scalar_bytes;
    /* 0: legacy full-token FFN; otherwise cap final-layer FFN positions per
     * chunk (native last-logit Qwen3 uses 1 after upstream get_rows pruning).
     * Source-compatible zero initialization; callers must recompile. */
    uint32_t last_layer_ffn_tokens;
} cxf_roofline_spec;
/* Effective, same-backend microprobe rates, NOT hardware peak specifications.
 * Caller keys hardware/model/rates/config into a calibration profile. */
typedef struct cxf_roofline_device {
    double linear_flops_s, attention_flops_s, bandwidth_bytes_s;
} cxf_roofline_device;
typedef struct cxf_roofline_result {
    double ms, linear_ms, attention_ms, compute_ms, memory_ms;
    double dominant_flops, operand_bytes; /* traffic proxy, NOT measured DRAM */
    uint32_t decode_calls;
} cxf_roofline_result;
typedef struct cxf_roofline_calibration { cxf_cost_model fit; } cxf_roofline_calibration;

/* Computes sum of per-op rooflines, not max over whole-model totals. Zero work
 * -> zero; zero/missing rates -> EMPTY; invalid spec/rates/ranges -> RANGE.
 * At most 4096 decode chunks; no heap and invalid input preserves output. */
cxf_result cxf_roofline_estimate(const cxf_roofline_spec *,const cxf_roofline_device *,
                                cxf_cost_work,cxf_roofline_result *);
cxf_result cxf_roofline_init(cxf_roofline_calibration *,uint64_t profile);
cxf_result cxf_roofline_predict(const cxf_roofline_calibration *,uint64_t profile,
    const cxf_roofline_spec *,const cxf_roofline_device *,cxf_cost_work,cxf_cost_prediction *);
cxf_result cxf_roofline_observe(cxf_roofline_calibration *,uint64_t profile,
    const cxf_roofline_spec *,const cxf_roofline_device *,cxf_cost_work,double ms);

#define CXF_ROOFLINE_WIDTHS 12
#define CXF_ROOFLINE_CONTEXTS 8
/* Same-backend numeric probes, frozen before inference labels. Per-layer FA
 * times include kernel work but exclude the measured per-graph intercept.
 * Sorted knots; interpolate normalized linear service time (width/rate) and
 * FA time only within pinned Metal width regimes <=8,
 * 9..19, >=20. This partition is profile-specific, not universal GPU policy. */
typedef struct cxf_roofline_shape {
    uint32_t width_count, context_count;
    uint32_t widths[CXF_ROOFLINE_WIDTHS], contexts[CXF_ROOFLINE_CONTEXTS];
    double linear_flops_s[CXF_ROOFLINE_WIDTHS];
    double attention_ms[CXF_ROOFLINE_WIDTHS][CXF_ROOFLINE_CONTEXTS];
    double bandwidth_bytes_s;
} cxf_roofline_shape;
cxf_result cxf_roofline_shape_estimate(const cxf_roofline_spec *,const cxf_roofline_shape *,
    cxf_cost_work,cxf_roofline_result *,bool *extrapolated);
cxf_result cxf_roofline_shape_predict(const cxf_roofline_calibration *,uint64_t profile,
    const cxf_roofline_spec *,const cxf_roofline_shape *,cxf_cost_work,cxf_cost_prediction *);
cxf_result cxf_roofline_shape_observe(cxf_roofline_calibration *,uint64_t profile,
    const cxf_roofline_spec *,const cxf_roofline_shape *,cxf_cost_work,double ms);
#ifdef __cplusplus
}
#endif
#endif
