#!/usr/bin/env python3
"""Frozen calibration factorial study; explicit local model, immutable old evidence."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import subprocess
from bench_roofline_estimator import hardware, capture, digest, COMMIT
from bench_cost_shapes import error_summary
from analyze_frozen_calibration import audit, features, fit, nnls

ROOT=Path(__file__).resolve().parents[1]
MODES=['raw','legacy_clipped_sparse','legacy_sparse','legacy_diverse','weighted_sparse','weighted_diverse',
       'residual_sparse','residual_diverse','empirical_online','shape_online']

def weighted_controls(rows):
    coefficients=[];predictions=[]
    for batch in [256,64]:
        for coverage in ['sparse','diverse']:
            train=[r for r in rows if r.get('scenario')=='calibration' and r['batch_tokens']==batch and r['coverage']==coverage]
            coef=nnls([features(r) for r in train],[r['wall_ms'] for r in train],[1/max(r['wall_ms'],20) for r in train])
            coefficients.append({'batch_tokens':batch,'coverage':coverage,'coefficient':coef,'training_rows':len(train)})
        cs=coefficients[-2:]
        for i,r in enumerate(rows):
            if r.get('scenario')!='evaluation' or r['batch_tokens']!=batch:continue
            record={'source_record_index':i}
            for c in cs:
                key='weighted_'+c['coverage'];p=sum(a*b for a,b in zip(c['coefficient'],features(r)))
                record[key+'_ms']=p;record[key+'_abs_error_ms']=abs(p-r['wall_ms'])
            predictions.append(record)
    return coefficients,predictions

def evaluations(data):
    controls={r['source_record_index']:r for r in data['offline_predictions']}
    return [dict(r,**{k:v for k,v in controls[i].items() if k!='source_record_index'})
            for i,r in enumerate(data['rows']) if r.get('scenario')=='evaluation']

def summarize(data):
    rows=evaluations(data)
    return [{'batch_tokens':b,**error_summary([r for r in rows if r['batch_tokens']==b],m)} for b in [256,64] for m in MODES]

def grouped(data):
    rows=evaluations(data);result=[]
    for b in [256,64]:
        for p in [1280,3328,5376]:
            for n in [10,15,21,72,176,352,672]:
                g=[r for r in rows if r['batch_tokens']==b and r['prefix_tokens']==p and r['new_tokens']==n]
                v=[r['wall_ms'] for r in g];median=statistics.median(v)
                result.append({'batch_tokens':b,'prefix_tokens':p,'new_tokens':n,'samples':len(g),'wall_median_ms':median,
                               'wall_min_ms':min(v),'wall_max_ms':max(v),'wall_mad_ms':statistics.median(abs(x-median) for x in v),
                               'errors':[error_summary(g,m) for m in MODES]})
    return result

def anchors(rows):
    out=[]
    for b in [256,64]:
        for p,n in [(0,32),(2048,17),(6144,512)]:
            for stage in ['before_calibration','after_calibration','after_evaluation']:
                v=[r['wall_ms'] for r in rows if r.get('scenario')=='anchor' and r['batch_tokens']==b and r['prefix_tokens']==p and r['new_tokens']==n and r['anchor_stage']==stage]
                assert len(v)==3
                out.append({'batch_tokens':b,'prefix_tokens':p,'new_tokens':n,'stage':stage,'samples':3,
                            'median_ms':statistics.median(v),'min_ms':min(v),'max_ms':max(v)})
    return out

def finalize(args,saved,recovery=None):
    log=(args.out/'engine.log').read_text();offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',log)
    assert len(offloads)==2 and all(a==b and int(a)>0 for a,b in offloads) and 'sparse=1' not in log
    rows=[json.loads(line) for line in (args.out/'raw.jsonl').read_text().splitlines()]
    assert len([r for r in rows if r.get('scenario')=='calibration'])==36*args.repeats
    assert len([r for r in rows if r.get('scenario')=='evaluation'])==42*args.repeats
    assert sum(r['fresh_shapes'] for r in rows if r['kind']=='correctness' and r['passed'])==78
    assert len([r for r in rows if r['kind']=='closed' and r['passed']])==2
    coefs,controls=weighted_controls(rows)
    e={'date':'2026-10-30','scope':'Frozen calibration: table coverage, equal-label diverse coverage, weighted objective and residual basis; new batch/context validation',
       **saved['profile'],'profile_sha256':saved['profile_sha256'],'profile_tag':saved['profile_sha256'][:16],
       'sources_sha256':saved['sources_sha256'],'postprocessor_sha256':digest(Path(__file__)),'postprocessing_recovery':recovery,
       'rows':rows,'offline_coefficients':coefs,'offline_predictions':controls,'anchor_summary':anchors(rows),
       'development_e17':{'source':'docs/evidence/cost-shapes-v2-20261030.json','source_sha256':digest(ROOT/'docs/evidence/cost-shapes-v2-20261030.json'),
                          'scope':'Development only; exact NNLS and candidate checks on previously observed E17','audit':audit()},
       'origin_directory':str(args.out.relative_to(ROOT)),'raw_jsonl_sha256':digest(args.out/'raw.jsonl'),'engine_log_sha256':digest(args.out/'engine.log'),'verified_full_gpu_offload':True,
       'method':{'repeats':args.repeats,'configuration_order':[256,64],'calibration_per_coverage_per_configuration':63,'evaluation_per_configuration':147,
                 'sparse_grid':{'prefixes':[0,2048,6144],'new_tokens':[32,128,512]},
                 'diverse_points':[[0,8],[0,96],[0,448],[2048,17],[2048,96],[2048,448],[6144,8],[6144,17],[6144,448]],
                 'evaluation_prefixes':[1280,3328,5376],'evaluation_new_tokens':[10,15,21,72,176,352,672],
                 'width_knots':[8,16,19,20,128,512],'context_knots':[0,2048,6144,7168],'clipped_context_knots':[0,2048,6144],
                 'sparse_diverse_interleaved_per_prefix':True,'all_frozen_models_use_training_only':True,'online_initialization':'sparse only, then predict before current observation',
                 'weighted_nnls':'Offline training-only control, same original 3 features; source_record_index references original raw row, no added inference samples',
                 'residual':'raw + ridge correction [calls,new_tokens/512,raw_attention_ms/100]; weight=1/max(actual_ms,20), RMS scaling, lambda=.01, beta prior0, clamp output>=0',
                 'anchor_excluded_from_all_fitting':True,'anchors_repeats':3,'anchors_stages':['before_calibration','after_calibration','after_evaluation'],
                 'model_load_prefix_warmup_oracles_excluded':True,'no_evaluation_tuning_or_rerun_selection':True},
       'references':{'ridge':'https://scikit-learn.org/stable/modules/generated/sklearn.linear_model.Ridge.html','prior_context':'docs/design/cost-estimator-v1.md'},
       'limits':['Single machine/model/backend; changing objective and residual basis is an optional estimator, not faster inference',
                 'Two new batch caps and new context/token grid; no universal accuracy claim',
                 'Anchor ranges describe phase effects; GPU frequency/temperature, actual DRAM and pure GPU time unmeasured',
                 'Dividing coverage/control effects does not make them additive; all predictions share actual jobs',
                 'Residual model uses 3 coefficients like old model, but a different basis/prior/objective; beta is not physical attribution']}
    e['prediction_summary']=summarize(e);e['cell_summary']=grouped(e)
    (args.out/'evidence.json').write_text(json.dumps(e,indent=2)+'\n')
    for r in e['prediction_summary']:print(r['batch_tokens'],r['mode'],f"MAE={r['mae_ms']:.3f} ms WAPE={r['wape_percent']:.2f}%",flush=True)
    print('native records',len(rows),'offline controls',len(controls),flush=True)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938');ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True);ap.add_argument('--repeats',type=int,default=7);ap.add_argument('--finalize-existing',action='store_true')
    args=ap.parse_args();args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT) or args.repeats!=7:ap.error('project-local fresh out and frozen repeats7 required')
    if args.finalize_existing:
        if (args.out/'evidence.json').exists():ap.error('evidence exists; do not overwrite')
        finalize(args,json.loads((args.out/'manifest.json').read_text()),'Recovered metadata from original completed native records; no inference rerun');return
    if (args.out/'manifest.json').exists():ap.error('fresh out required')
    commit=capture(['git','-C',str(args.sdk),'rev-parse','HEAD']).strip()
    if commit!=COMMIT or commit[:9] not in capture([str(args.runtime/'llama-server'),'--version']):ap.error('matching b10938 required')
    machine=hardware()
    if machine['os']!='Darwin' or not machine.get('cpu'):ap.error('Metal and local hardware access required')
    sources=sorted((ROOT/'src').glob('*.c'))+[ROOT/'tests/bench_frozen_calibration.c']
    additional=[ROOT/n for n in ['include/cxf/cost.h','include/cxf/cost_residual.h','include/cxf/roofline.h','include/cxf/llama.h','src/cost_internal.h','src/llama_internal.h','tests/roofline_probe.h','tests/roofline_graph_probe.h','tests/roofline_shape_probe.h','tools/analyze_frozen_calibration.py','tools/bench_cost_shapes.py','tools/bench_roofline_estimator.py']]
    hashes={str(p.relative_to(ROOT)):digest(p) for p in sources+additional+[Path(__file__)]}
    config={'context_tokens':8192,'batch_token_limits':[256,64],'cpu_threads':4,'sessions':2,'kv_type':'f16','backend':'Metal','prefill_min_tokens_per_cap':{'256':128,'64':64}}
    libs=['libllama.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']
    profile={'estimator_schema':'residual-ridge-v1-equal-coverage-interleaved','hardware':machine,'model':args.model.name,'model_sha256':digest(args.model),'llama_commit':commit,
             'runtime_libraries_sha256':{n:digest(args.runtime/n) for n in libs if (args.runtime/n).exists()},'configuration':config}
    fingerprint=hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest();args.out.mkdir(parents=True,exist_ok=True)
    command=['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Wpedantic','-DCXF_FROZEN_STUDY','-I'+str(ROOT/'include'),'-I'+str(args.sdk/'include'),'-I'+str(args.sdk/'ggml/include')]
    command += [str(p) for p in sources]+['-L'+str(args.runtime),'-Wl,-rpath,'+str(args.runtime),'-lllama','-lggml','-lggml-base','-lm','-o',str(args.out/'bench_frozen')]
    (args.out/'compile.txt').write_text(capture(command))
    saved={'profile':profile,'profile_sha256':fingerprint,'sources_sha256':hashes,'compile':command};(args.out/'manifest.json').write_text(json.dumps(saved,indent=2)+'\n')
    print('Running frozen calibration study:',args.out.name,flush=True)
    with (args.out/'raw.jsonl').open('w') as out,(args.out/'engine.log').open('w') as log:
        p=subprocess.run([str(args.out/'bench_frozen'),str(args.model),str(args.runtime),'7',fingerprint[:16]],stdout=out,stderr=log)
    if p.returncode:raise SystemExit('native run failed; original private artifacts retained')
    finalize(args,saved)

if __name__=='__main__':main()
