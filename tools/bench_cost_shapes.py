#!/usr/bin/env python3
"""Batch/context diagnosis and shape-aware estimator; explicit local paths only."""
import argparse
import hashlib
import json
import math
import re
from pathlib import Path
import statistics
import subprocess
from bench_roofline_estimator import hardware, capture, digest, COMMIT

ROOT=Path(__file__).resolve().parents[1]
MODES=['empirical_frozen','empirical_online','roof_raw','roof_frozen','roof_online','shape_raw','shape_frozen','shape_online']

def error_summary(rows,mode):
    errors=[r[mode+'_ms']-r['wall_ms'] for r in rows]
    return {'mode':mode,'samples':len(rows),'mae_ms':statistics.mean(abs(v) for v in errors),
            'signed_bias_ms':statistics.mean(errors),'max_abs_error_ms':max(abs(v) for v in errors),
            'wape_percent':100*sum(abs(v) for v in errors)/sum(r['wall_ms'] for r in rows)}

def summarize(rows):
    out=[]
    for batch in [512,128]:
        group=[r for r in rows if r.get('scenario')=='evaluation' and r['batch_tokens']==batch]
        for mode in MODES:out.append({'batch_tokens':batch,**error_summary(group,mode)})
    return out

def grouped(rows):
    out=[]
    for batch in [512,128]:
        for prefix in sorted({r["prefix_tokens"] for r in rows if r.get("scenario")=="evaluation"}):
            for n in sorted({r["new_tokens"] for r in rows if r.get("scenario")=="evaluation"}):
                g=[r for r in rows if r.get('scenario')=='evaluation' and r['batch_tokens']==batch and r['prefix_tokens']==prefix and r['new_tokens']==n]
                v=[r['wall_ms'] for r in g];median=statistics.median(v)
                out.append({'batch_tokens':batch,'prefix_tokens':prefix,'new_tokens':n,'samples':len(g),
                            'wall_median_ms':median,'wall_min_ms':min(v),'wall_max_ms':max(v),
                            'wall_mad_ms':statistics.median(abs(x-median) for x in v),
                            'errors':[error_summary(g,mode) for mode in MODES]})
    return out

def historical_e15():
    path=ROOT/'docs/evidence/roofline-diagnosis-20261030.json'
    rows=[r for r in json.loads(path.read_text())['rows'] if r.get('kind')=='cost_sample']
    coefficient=[0.0]*3;window=[];slot=0;predictions=[]
    for r in rows:
        x=[r['corrected_linear_ms'],r['corrected_attention_ms'],(r['new_tokens']+r['batch_tokens']-1)//r['batch_tokens']]
        if r['scenario']=='evaluation':predictions.append(dict(r,replayed_roof_online_ms=sum(a*b for a,b in zip(coefficient,x))))
        if len(window)<64:window.append((x,r['wall_ms']))
        else:window[slot]=(x,r['wall_ms'])
        slot=(slot+1)%64
        a=[[sum(v[0][i]*v[0][j] for v in window) for j in range(3)] for i in range(3)]
        b=[sum(v[0][i]*v[1] for v in window) for i in range(3)]
        for _ in range(128):
            for j in range(3):
                coefficient[j]=max(0,(b[j]-sum(a[j][k]*coefficient[k] for k in range(3) if k!=j))/a[j][j]) if a[j][j] else 0
    cells=[]
    for prefix in [0,1024,4096]:
        for n in [16,64,256,768]:
            g=[r for r in predictions if r['prefix_tokens']==prefix and r['new_tokens']==n]
            cells.append({'prefix_tokens':prefix,'new_tokens':n,
                          'errors':[error_summary(g,m) for m in ['corrected_calibrated','online','replayed_roof_online']]})
    return {'source':'docs/evidence/roofline-diagnosis-20261030.json','source_sha256':digest(path),
            'scope':'post-hoc numeric replay only; no new inference or proof of future accuracy',
            'summary':[error_summary(predictions,m) for m in ['corrected_calibrated','online','replayed_roof_online']],
            'cells':cells}

def finalize(args,profile,hashes,fingerprint,tag,recovery):
    log=(args.out/'engine.log').read_text();offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',log)
    assert len(offloads)==2 and all(a==b and int(a)>0 for a,b in offloads)
    assert 'sparse=1' not in log
    rows=[json.loads(line) for line in (args.out/'raw.jsonl').read_text().splitlines()]
    assert len([r for r in rows if r.get('kind')=='cost_sample'])==60*args.repeats
    for r in rows:
        if r.get('scenario')=='evaluation':
            for m in MODES:assert math.isclose(r[m+'_abs_error_ms'],abs(r[m+'_ms']-r['wall_ms']),abs_tol=3e-6)
    assert len([r for r in rows if r.get('kind')=='closed' and r['passed']])==2
    assert sum(r['fresh_shapes'] for r in rows if r['kind']=='correctness' and r['passed'])==60
    evidence={'date':'2026-10-30','scope':'Batch/context residual diagnosis; fair frozen/online comparisons and numeric shape profile',**profile,
              'profile_sha256':fingerprint,'profile_tag':tag,'postprocessor_sha256':digest(Path(__file__)),'postprocessing_recovery':recovery,'rows':rows,'prediction_summary':summarize(rows),'cell_summary':grouped(rows),
              'historical_e15':historical_e15(),'sources_sha256':hashes,'origin_directory':str(args.out.relative_to(ROOT)),'verified_full_gpu_offload':True,
              'method':{'repeats':args.repeats,'per_configuration_calibration_samples':9*args.repeats,'per_configuration_evaluation_samples':21*args.repeats,
                        'width_knots':[8,16,19,20,128,512],'context_knots':[0,2048,6144],'probe_depths':[1,16],'probe_repeats':7,'probe_warmups':2,
                        'calibration_prefixes':[0,2048,6144],'calibration_new_tokens':[32,128,512],'evaluation_prefixes':sorted({r['prefix_tokens'] for r in rows if r.get('scenario')=='evaluation'}),'evaluation_new_tokens':sorted({r['new_tokens'] for r in rows if r.get('scenario')=='evaluation'}),
                        'online_predicts_before_observe':True,'all_online_models_receive_same_past_labels':True,'frozen_receives_calibration_only':True,'microprobes_precede_inference_labels':True,
                        'model_load_prefix_setup_shape_warmup_excluded':True,'configuration_order':[512,128],'route_classification':'inferred from pinned source; not GPU counters'},
              'references':{'vidur':'https://www.microsoft.com/en-us/research/wp-content/uploads/2024/05/vidur_mlsys24.pdf','flashattention2':'https://arxiv.org/abs/2307.08691','roofline_regression':'https://research.ibm.com/publications/predicting-llm-inference-latency-a-roofline-driven-ml-method'},
              'limits':['One machine/model/backend; no inference/scheduler change','Shape profile uses extra microprobe work; synthetic inputs, fixed Q8 matrix family and one KV layout',
                        'Kernel route labels inferred, GPU timing/DRAM/frequency/temperature unmeasured','Online consumes past evaluation labels; frozen comparison has different information',
                        'No held-out tuning or run selection; old evidence retained; group ranges/MAD are descriptive, not confidence bounds']}
    (args.out/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    for r in evidence['prediction_summary']:print(r['batch_tokens'],r['mode'],f"MAE={r['mae_ms']:.3f} ms WAPE={r['wape_percent']:.2f}%",flush=True)
    print('records',len(rows),flush=True)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938')
    ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True)
    ap.add_argument('--new-grid',action='store_true');ap.add_argument('--finalize-existing',action='store_true');ap.add_argument('--out',type=Path,required=True);ap.add_argument('--repeats',type=int,default=7)
    args=ap.parse_args();args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT) or not 1<=args.repeats<=30:ap.error('project-local fresh out and repeats1..30 required')
    if args.finalize_existing:
        if (args.out/'evidence.json').exists():ap.error('evidence already finalized; do not overwrite')
        saved=json.loads((args.out/'manifest.json').read_text())
        finalize(args,saved['profile'],saved['sources_sha256'],saved['profile_sha256'],saved['profile_sha256'][:16],
                 'Recovered metadata after missing re import; original native run completed, raw samples unchanged; no inference rerun')
        return
    if (args.out/'manifest.json').exists():ap.error('fresh out required')
    commit=capture(['git','-C',str(args.sdk),'rev-parse','HEAD']).strip()
    if commit!=COMMIT or commit[:9] not in capture([str(args.runtime/'llama-server'),'--version']):ap.error('matching b10938 required')
    machine=hardware()
    if machine['os']!='Darwin' or not machine.get('cpu'):ap.error('measured runner requires macOS/Metal hardware access')
    args.out.mkdir(parents=True,exist_ok=True)
    sources=sorted((ROOT/'src').glob('*.c'))+[ROOT/'tests/bench_llama_cost_shapes.c']
    headers=[ROOT/n for n in ['include/cxf/cost.h','include/cxf/roofline.h','include/cxf/llama.h','src/cost_internal.h','src/llama_internal.h','tests/roofline_probe.h','tests/roofline_graph_probe.h','tests/roofline_shape_probe.h']]
    hashes={str(p.relative_to(ROOT)):digest(p) for p in sources+headers+[Path(__file__),ROOT/'tools/bench_roofline_estimator.py']}
    config={'context_tokens':8192,'batch_token_limits':[512,128],'cpu_threads':4,'sessions':2,'kv_type':'f16','backend':'Metal','evaluation_set':'v2-new-shapes' if args.new_grid else 'v1-shapes'}
    libs=['libllama.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']
    profile={'estimator_schema':'roofline-shape-v2-time-interpolation','hardware':machine,'model':args.model.name,'model_sha256':digest(args.model),'llama_commit':commit,
             'runtime_libraries_sha256':{n:digest(args.runtime/n) for n in libs if (args.runtime/n).exists()},'configuration':config}
    fingerprint=hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest();tag=fingerprint[:16]
    command=['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Wpedantic','-I'+str(ROOT/'include'),'-I'+str(args.sdk/'include'),'-I'+str(args.sdk/'ggml/include')]
    if args.new_grid:command += ['-DCXF_SHAPES_V2']
    command += [str(p) for p in sources]+['-L'+str(args.runtime),'-Wl,-rpath,'+str(args.runtime),'-lllama','-lggml','-lggml-base','-lm','-o',str(args.out/'bench_shapes')]
    (args.out/'compile.txt').write_text(capture(command))
    (args.out/'manifest.json').write_text(json.dumps({'profile':profile,'profile_sha256':fingerprint,'sources_sha256':hashes,'compile':command},indent=2)+'\n')
    print('Running batch/context diagnosis:',args.out.name,flush=True)
    with (args.out/'raw.jsonl').open('w') as out,(args.out/'engine.log').open('w') as log:
        p=subprocess.run([str(args.out/'bench_shapes'),str(args.model),str(args.runtime),str(args.repeats),tag],stdout=out,stderr=log)
    if p.returncode:raise SystemExit('native run failed; private logs retained')
    finalize(args,profile,hashes,fingerprint,tag,None)

if __name__=='__main__':main()
