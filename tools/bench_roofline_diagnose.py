#!/usr/bin/env python3
"""Compare local hardware Roofline and empirical prefill estimates; explicit paths, no network/config.

Outputs private compile/engine logs under a fresh build directory. Public evidence
contains only safe configuration, hardware/model identity, numbers and source hashes.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import re
import statistics
import subprocess

ROOT=Path(__file__).resolve().parents[1]
COMMIT='f1e44dcc11d8802d107bd7331a3d3fd3e6f57b93'

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def capture(args):
    p=subprocess.run(args,capture_output=True,text=True)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return p.stdout+p.stderr
def hardware():
    values={'os':platform.system(),'os_release':platform.release(),'architecture':platform.machine()}
    if platform.system()=='Darwin':
        for key,field in [('machdep.cpu.brand_string','cpu'),('hw.memsize','memory_bytes'),('hw.logicalcpu','logical_cpus'),('hw.model','model_identifier')]:
            p=subprocess.run(['/usr/sbin/sysctl','-n',key],capture_output=True,text=True)
            value=p.stdout.strip() if p.returncode==0 else None
            values[field]=int(value) if value and field in ('memory_bytes','logical_cpus') else value
    return values
def summarize(rows):
    evaluation=[r for r in rows if r.get('scenario')=='evaluation']
    result=[]
    for mode in ['fixed','frozen','online','legacy','math_only','bandwidth_only','linear_only','attention_only','rate_only','corrected','corrected_calibrated']:
        for group in ['all','inside_frozen_domain','outside_frozen_domain']:
            selected=[r for r in evaluation if group=='all' or r['frozen_extrapolated']==(group=='outside_frozen_domain')]
            if not selected:continue
            errors=[r[mode+'_ms']-r['wall_ms'] for r in selected]
            result.append({'mode':mode,'group':group,'samples':len(selected),
                           'mae_ms':statistics.mean(abs(x) for x in errors),
                           'median_abs_error_ms':statistics.median(abs(x) for x in errors),
                           'max_abs_error_ms':max(abs(x) for x in errors),
                           'mean_signed_error_ms':statistics.mean(errors),
                           'mape_percent':100*statistics.mean(abs(x)/r['wall_ms'] for x,r in zip(errors,selected)),
                           'wape_percent':100*sum(abs(x) for x in errors)/sum(r['wall_ms'] for r in selected)})
    return result
def audit_e14():
    path=ROOT/'docs/evidence/roofline-estimator-20261030.json'
    old=json.loads(path.read_text());rows=old['rows']
    spec=next(r for r in rows if r['kind']=='roofline_spec')
    dev=next(r for r in rows if r['kind']=='hardware_profile')
    E=spec['embedding'];F=spec['ffn'];Q=spec['query_heads']*spec['key_dim'];O=spec['query_heads']*spec['value_dim']
    K=spec['kv_heads']*spec['key_dim'];V=spec['kv_heads']*spec['value_dim'];L=spec['layers']
    matrices=list(zip([E,E,E,O,E,E,F],[Q,K,V,E,F,F,E]))
    checked=0
    for r in rows:
        if r.get('kind')!='cost_sample':continue
        remain=r['new_tokens'];context=r['prefix_tokens'];flops=0;traffic=0;ms=0
        while remain:
            n=min(remain,r['batch_tokens'])
            dense_f=2*n*sum(k*v for k,v in matrices)*L
            dense_b=(sum(spec['weight_bytes'])+spec['activation_scalar_bytes']*n*sum(k+v for k,v in matrices))*L
            attn_f=2*spec['query_heads']*(spec['key_dim']+spec['value_dim'])*(n*context+n*(n+1)/2)*L
            attn_b=(spec['kv_scalar_bytes']*(context+n)*(K+V)+spec['activation_scalar_bytes']*n*(Q+O))*L
            head_f=2*E*spec['vocabulary'];head_b=spec['output_weight_bytes']+spec['activation_scalar_bytes']*(E+spec['vocabulary'])+n*spec['embedding_row_bytes']
            flops+=dense_f+attn_f+head_f;traffic+=dense_b+attn_b+head_b
            for (k,v),weight in zip(matrices,spec['weight_bytes']):
                ms+=L*max(2*n*k*v/dev['linear_flops_s'],(weight+spec['activation_scalar_bytes']*n*(k+v))/dev['bandwidth_bytes_s'])*1000
            ms+=max(attn_f/dev['attention_flops_s'],attn_b/dev['bandwidth_bytes_s'])*1000
            ms+=max(head_f/dev['linear_flops_s'],head_b/dev['bandwidth_bytes_s'])*1000
            remain-=n;context+=n
        assert flops==r['dominant_flops'] and traffic==r['operand_bytes']
        assert math.isclose(ms,r['roofline_ms'],abs_tol=1e-6)
        checked+=1
    assert checked==147
    return {'source':'docs/evidence/roofline-estimator-20261030.json','source_sha256':digest(path),'rows_recomputed':checked,
            'flops_bytes_and_roofline_match':True,'remaining_semantic_error':'last-layer FFN output-row pruning omitted; rates were synchronized isolated-op wall rates and F16 GEMM was not FlashAttention'}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938')
    ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True)
    ap.add_argument('--probes-only',action='store_true');ap.add_argument('--out',type=Path,required=True);ap.add_argument('--repeats',type=int,default=7)
    args=ap.parse_args()
    args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT):ap.error('out must be inside the project')
    if not 1<=args.repeats<=30:ap.error('repeats must be 1..30')
    if platform.system()!='Darwin':ap.error('first measured runner requires macOS/Metal; C estimator is platform-independent')
    commit=capture(['git','-C',str(args.sdk),'rev-parse','HEAD']).strip()
    version=capture([str(args.runtime/'llama-server'),'--version'])
    if commit!=COMMIT or commit[:9] not in version:ap.error('matching b10938 source/runtime required')
    args.out.mkdir(parents=True,exist_ok=True)
    if (args.out/'manifest.json').exists():ap.error('fresh --out required')
    sources=sorted((ROOT/'src').glob('*.c'))+[ROOT/'tests/bench_llama_roofline_diagnose.c']
    source_hashes={str(p.relative_to(ROOT)):digest(p) for p in sources+[ROOT/'include/cxf/cost.h',ROOT/'include/cxf/llama.h',ROOT/'include/cxf/roofline.h',ROOT/'src/cost_internal.h',ROOT/'tests/roofline_probe.h',ROOT/'tests/roofline_graph_probe.h',ROOT/'src/llama_internal.h',Path(__file__)]}
    config={'context_tokens':8192,'batch_tokens':512,'cpu_threads':4,'sessions':2,'kv_type':'f16','backend':'Metal' if platform.system()=='Darwin' else 'not_yet_validated'}
    machine=hardware()
    if not machine.get('cpu') or not machine.get('memory_bytes'):ap.error('hardware metadata unavailable; run with local hardware access')
    libraries=['libllama.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']
    profile={'estimator_schema':'roofline-v1-diagnostic-chain16-fa-finalffn','hardware':machine,'model':args.model.name,'model_sha256':digest(args.model),'llama_commit':commit,
             'runtime_libraries_sha256':{name:digest(args.runtime/name) for name in libraries if (args.runtime/name).exists()},'configuration':config}
    profile_hash=hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest()
    profile_tag=profile_hash[:16] if int(profile_hash[:16],16) else '0000000000000001'
    command=['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Wpedantic','-I'+str(ROOT/'include'),'-I'+str(args.sdk/'include'),'-I'+str(args.sdk/'ggml/include')]
    command += [str(p) for p in sources]+['-L'+str(args.runtime),'-Wl,-rpath,'+str(args.runtime),'-lllama','-lggml','-lggml-base','-lm','-o',str(args.out/'bench_cost')]
    (args.out/'compile.txt').write_text(capture(command))
    manifest={'profile':profile,'profile_sha256':profile_hash,'profile_tag':profile_tag,'sources_sha256':source_hashes,'compile':command,'repeats':args.repeats,'probes_only':args.probes_only}
    (args.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Running calibration/evaluation:',args.out.name,flush=True)
    with (args.out/'raw.jsonl').open('w') as output,(args.out/'engine.log').open('w') as log:
        p=subprocess.run([str(args.out/'bench_cost'),str(args.model),str(args.runtime),str(args.repeats),profile_tag]+(['probes_only'] if args.probes_only else []),stdout=output,stderr=log)
    if p.returncode:raise SystemExit('native measurement failed; inspect local engine.log')
    engine_log=(args.out/'engine.log').read_text()
    offload=re.search(r'offloaded (\d+)/(\d+) layers to GPU',engine_log)
    if not offload or offload[1]!=offload[2] or int(offload[1])==0:raise SystemExit('full GPU offload not verified; no publishable result')
    rows=[json.loads(line) for line in (args.out/'raw.jsonl').read_text().splitlines() if line.strip()]
    for r in rows:
        if r.get('scenario')=='evaluation':
            for mode in ['fixed','frozen','online','legacy','math_only','bandwidth_only','linear_only','attention_only','rate_only','corrected','corrected_calibrated']:assert abs(r[mode+'_abs_error_ms']-abs(r[mode+'_ms']-r['wall_ms']))<3e-6
    if args.probes_only:
        print('Probe run complete:',len(rows),'records',flush=True)
        return
    assert 'sparse=1' not in engine_log, 'unexpected sparse attention kernel'
    result=summarize(rows)
    model_structure={}
    for name in ['n_layer','n_head','n_head_kv','n_embd_head_k','n_embd_head_v']:
        match=re.search(r'\b'+name+r'\s*=\s*(\d+)',engine_log)
        if match:model_structure[name]=int(match[1])
    if all(k in model_structure for k in ['n_layer','n_head_kv','n_embd_head_k','n_embd_head_v']):
        model_structure['f16_kv_payload_bytes_per_token']=model_structure['n_layer']*model_structure['n_head_kv']*(model_structure['n_embd_head_k']+model_structure['n_embd_head_v'])*2
    evidence={'date':'2026-10-30','scope':'Roofline diagnosis: dependent-graph slope, actual FlashAttention, final-layer FFN pruning; same-job ablations, no scheduling change',
              **profile,'profile_sha256':profile_hash,'profile_tag':profile_tag,'model_structure':model_structure,
              'audit_e14':audit_e14(),'verified_full_gpu_offload':True,'rows':rows,'prediction_summary':result,
              'sources_sha256':source_hashes,'origin_directory':str(args.out.relative_to(ROOT)),
              'method':{'hardware_probes':'Legacy single-op Q8/F16 GEMM and 64MiB copy; dependent graphs depth1/16 for Q8 and F16 alternating transpose sizes, GQA FlashAttention prefix2048/new512 and 64MiB copy; 2 warmup + 7 samples each; rates from median difference slope, not physical peak', 'roofline_traffic':'one KV read pass per decode chunk proxy, not measured DRAM traffic; norms/rope/activation omitted', 'calibration_samples':9*args.repeats,'evaluation_samples':12*args.repeats,'shape_warmup_excluded':True,
                        'frozen_model_uses_no_evaluation_labels':True,'roofline_calibration_uses_no_evaluation_labels':True,'final_layer_ffn_tokens':1,'primitive_slope_frozen_before_evaluation':True,'online_prediction_precedes_current_observation':True,
                        'fixed_rate_uses_all_calibration_samples':True,'model_load_and_prefix_setup_excluded':True,
                        'calibration_prefixes':[0,2048,6144],'calibration_new_tokens':[32,128,512],
                        'evaluation_prefixes':[0,1024,4096],'evaluation_new_tokens':[16,64,256,768]},
              'limits':['One machine/model/backend; primitive effective rates are proxies, not hardware peak values or pure GPU timestamps',
                        'No cold-start, queue, KV-file restore, agent ETA or arbitrary-program complexity estimate',
                        'fit_mae is a fitting residual, not a confidence interval; range-based extrapolation is only a warning',
                        'Online consumes past evaluation observations after prediction; frozen consumes calibration only',
                        'Prediction quality improvement is not measured model-inference speedup; estimator overhead reported separately']}
    (args.out/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    for r in result:
        if r['group']=='all':print(r['mode'],f"MAE={r['mae_ms']:.3f} ms WAPE={r['wape_percent']:.2f}%",flush=True)
    print('records:',len(rows),'evidence written locally; review before publication')
if __name__=='__main__':main()
