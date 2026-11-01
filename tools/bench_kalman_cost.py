#!/usr/bin/env python3
"""Fresh native timings + prequential C Kalman replay; public evidence is path-free."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics as st
import subprocess
from bench_roofline_estimator import hardware, capture, digest, COMMIT
from analyze_frozen_calibration import nnls

ROOT = Path(__file__).resolve().parents[1]
BASES = ['raw', 'residual_diverse', 'shape_online']
MODES = ['base', 'ewma', 'fixed', 'scaled']


def noise_profiles(rows):
    result = []
    for batch in [256, 64]:
        train = [r for r in rows if r.get('scenario') == 'calibration' and r['batch_tokens'] == batch and r['coverage'] == 'diverse']
        groups = []
        for p, n in sorted({(r['prefix_tokens'], r['new_tokens']) for r in train}):
            values = [r['wall_ms'] for r in train if r['prefix_tokens'] == p and r['new_tokens'] == n]
            assert len(values) == 7
            groups.append(dict(prefix_tokens=p, new_tokens=n, samples=7, mean_ms=st.mean(values), variance_ms2=st.variance(values)))
        constant = max(.25, st.median(g['variance_ms2'] for g in groups))
        coef = nnls([[1, (g['mean_ms']/100)**2] for g in groups], [g['variance_ms2'] for g in groups])
        alpha = (math.sqrt(.01**2+4*.01)-.01)/2
        result.append(dict(batch_tokens=batch, groups=groups, constant_r_ms2=constant, scaled_r_coefficient=coef,
                           q_ms2=.01*constant, initial_p_ms2=alpha*constant, alpha=alpha))
    return result


def summary(trace):
    out = []
    for batch in [256, 64]:
        for base_id, base in enumerate(BASES):
            group = [r for r in trace if r['kind'] == 'filter_prediction' and r['batch_tokens'] == batch and r['base_id'] == base_id]
            for mode in MODES:
                e = [r[mode+'_ms']-r['actual_ms'] for r in group]
                out.append(dict(batch_tokens=batch, base=base, mode=mode, samples=len(e), mae_ms=st.mean(abs(x) for x in e),
                                rmse_ms=math.sqrt(st.mean(x*x for x in e)), max_abs_error_ms=max(abs(x) for x in e), mean_signed_error_ms=st.mean(e),
                                wape_percent=100*sum(abs(x) for x in e)/sum(r['actual_ms'] for r in group),
                                first14_mae_ms=st.mean(abs(x) for x in e[:14])))
    return out


def diagnostics(trace):
    out = []
    for batch in [256, 64]:
        for base_id, base in enumerate(BASES):
            group = [r for r in trace if r['kind'] == 'filter_prediction' and r['batch_tokens'] == batch and r['base_id'] == base_id]
            for mode in ['fixed', 'scaled']:
                z = [r[mode+'_innovation_ms']/math.sqrt(r[mode+'_variance_ms2']) for r in group]
                mean = st.mean(z); v = st.mean((x-mean)**2 for x in z)
                lag = sum((a-mean)*(b-mean) for a,b in zip(z[:-1],z[1:]))/sum((x-mean)**2 for x in z) if v else 0
                out.append(dict(batch_tokens=batch, base=base, mode=mode, samples=len(z), normalized_mean=mean, normalized_sd=math.sqrt(v),
                                skew=st.mean((x-mean)**3 for x in z)/v**1.5 if v else 0,
                                excess_kurtosis=st.mean((x-mean)**4 for x in z)/v**2-3 if v else 0,
                                lag1=lag, interval_coverage_percent=100*sum(abs(x)<=1.96 for x in z)/len(z)))
    return out


def check_replay(data):
    """Independent Python Joseph replay: calibration only noise; predict before label."""
    profiles = noise_profiles(data['rows'])
    assert profiles == data['noise_profiles']
    for noise in profiles:
        batch = noise['batch_tokens']
        for base_id, base in enumerate(BASES):
            source = [(i,r) for i,r in enumerate(data['rows']) if r.get('scenario') == 'evaluation' and r['batch_tokens'] == batch]
            replay = [r for r in data['filter_rows'] if r['kind']=='filter_prediction' and r['batch_tokens']==batch and r['base_id']==base_id]
            assert len(source)==len(replay)==147
            bias=[0.,0.]; post=[noise['initial_p_ms2']]*2; ewma=0.
            for count, ((index,r), t) in enumerate(zip(source,replay)):
                assert t['source_record_index']==index and t['observations_before']==count
                value=r[base+'_ms']; actual=r['wall_ms']
                assert abs(t['base_ms']-value)<2e-9 and abs(t['actual_ms']-actual)<2e-9
                assert abs(t['ewma_ms']-max(0,value+ewma))<2e-8 and abs(t['fixed_ms']-t['ewma_ms'])<2e-8
                for j,mode in enumerate(['fixed','scaled']):
                    rr = noise['constant_r_ms2'] if j==0 else max(.25, noise['scaled_r_coefficient'][0]+noise['scaled_r_coefficient'][1]*(value/100)**2)
                    prior=post[j]+noise['q_ms2']; variance=prior+rr; gain=prior/variance
                    innovation=actual-value-bias[j]
                    expected={'ms':max(0,value+bias[j]),'variance_ms2':variance,'gain':gain,'innovation_ms':innovation}
                    for key,v in expected.items(): assert abs(t[mode+'_'+key]-v)<max(2e-8,abs(v)*1e-9),(mode,key)
                    bias[j]+=gain*innovation;post[j]=(1-gain)**2*prior+gain*gain*rr
                ewma+=noise['alpha']*(actual-value-ewma)
                for mode in MODES: assert abs(t[mode+'_abs_error_ms']-abs(t[mode+'_ms']-actual))<2e-9
    assert summary(data['filter_rows'])==data['prediction_summary']
    assert diagnostics(data['filter_rows'])==data['diagnostics']


def finalize(args,saved):
    rows=[json.loads(s) for s in (args.out/'raw.jsonl').read_text().splitlines()]
    log=(args.out/'engine.log').read_text();offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',log)
    assert len(rows)==1162 and len(offloads)==2 and all(a==b and int(a)>0 for a,b in offloads) and 'sparse=1' not in log
    assert sum(r['fresh_shapes'] for r in rows if r['kind']=='correctness' and r['passed'])==78
    assert sum(r.get('scenario')=='calibration' for r in rows)==252
    assert sum(r.get('scenario')=='evaluation' for r in rows)==294
    noise=noise_profiles(rows); stream=[]
    for profile in noise:
        batch=profile['batch_tokens'];coef=profile['scaled_r_coefficient']
        for base_id, base in enumerate(BASES):
            group=[(i,r) for i,r in enumerate(rows) if r.get('scenario')=='evaluation' and r['batch_tokens']==batch]
            for count,(i,r) in enumerate(group):
                value=r[base+'_ms'];rr=max(.25,coef[0]+coef[1]*(value/100)**2)
                fields=[int(count==0),batch,i,base_id,r['prefix_tokens'],r['new_tokens'],value,r['wall_ms'],profile['constant_r_ms2'],rr,profile['q_ms2'],profile['initial_p_ms2'],profile['alpha']]
                stream.append(' '.join(str(v) for v in fields))
    p=subprocess.run([str(args.out/'replay_kalman')],input='\n'.join(stream)+'\n',capture_output=True,text=True,check=True)
    (args.out/'filter-raw.jsonl').write_text(p.stdout)
    trace=[json.loads(s) for s in p.stdout.splitlines()]
    for t in trace:
        if t['kind']=='filter_prediction':
            source=rows[t['source_record_index']]
            t.update(prefix_tokens=source['prefix_tokens'],new_tokens=source['new_tokens'])
            for mode in MODES:t[mode+'_abs_error_ms']=abs(t[mode+'_ms']-t['actual_ms'])
    e=dict(date='2026-10-31',scope='Fresh native timings, optional scalar Kalman residual: prequential C replay, no scheduler or model speed change',
           **saved['profile'],profile_sha256=saved['profile_sha256'],profile_tag=saved['profile_sha256'][:16],sources_sha256=saved['sources_sha256'],
           rows=rows,filter_rows=trace,noise_profiles=noise,prediction_summary=summary(trace),diagnostics=diagnostics(trace),
           origin_directory=str(args.out.relative_to(ROOT)),raw_jsonl_sha256=digest(args.out/'raw.jsonl'),filter_raw_jsonl_sha256=digest(args.out/'filter-raw.jsonl'),
           engine_log_sha256=digest(args.out/'engine.log'),verified_full_gpu_offload=True,
           method=dict(repeats=7,bases=BASES,modes=MODES,evaluation_prefixes=[1792,3840,5504],evaluation_new_tokens=[11,18,26,88,208,400,688],
                       calibration='Same E19 sparse/diverse interleaved grid, 63 labels per coverage/cap; all anchors excluded',
                       noise='Diverse training group sample variance only; R=max(.25,median variance), NNLS scaled R=max(.25,c0+c1*(base/100)^2), Q=.01*R, P0=alpha*R',
                       initialization='Bias0; no evaluation filter warmup; alpha=(sqrt(.01^2+4*.01)-.01)/2',
                       order='Within each native base and cap: predict every layer, then observe unfiltered-base residual; no future/current label leakage',
                       layer='Numeric C offline replay of fresh measured labels; 882 predictions reference 294 original native jobs, not extra inference jobs',
                       time='Warm native wall time; microtable/load/warmup/oracle excluded; filter CPU overhead measured separately',
                       diagnostics='Innovation / sqrt(Pprior+R); nominal +-1.96 coverage descriptive, not guaranteed Gaussian or confidence interval'),
           limits=['Single M5/Metal/Qwen0.6B run; no Windows/CUDA or scheduler verification',
                   'Constant Q/R steady-state Kalman equals EWMA; not an independent accuracy gain',
                   'Changing task-dependent bias can contaminate following tasks; Gaussian and independent noise unproven',
                   'Q advances per successful observation, not wall-clock time; scaled variance can be misspecified',
                   'No evaluation tuning or best-run selection; no claim of faster model kernels'],
           references={'filter':'https://www.mathworks.com/help/fusion/ug/introduction-to-estimation-filters.html','contract':'docs/design/cost-estimator-v1.md'})
    check_replay(e)
    (args.out/'evidence.json').write_text(json.dumps(e,indent=2)+'\n')
    for r in e['prediction_summary']: print(r['batch_tokens'],r['base'],r['mode'],f"MAE={r['mae_ms']:.3f} RMSE={r['rmse_ms']:.3f} max={r['max_abs_error_ms']:.3f}",flush=True)
    print('native',len(rows),'filter records',len(trace),'oracle fresh shapes',78,flush=True)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938');ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True);ap.add_argument('--finalize-existing',action='store_true')
    args=ap.parse_args();args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT):ap.error('project-local out required')
    if args.finalize_existing:
        if (args.out/'evidence.json').exists():ap.error('evidence already exists')
        finalize(args,json.loads((args.out/'manifest.json').read_text()));return
    if (args.out/'manifest.json').exists():ap.error('fresh out required')
    commit=capture(['git','-C',str(args.sdk),'rev-parse','HEAD']).strip()
    if commit!=COMMIT or commit[:9] not in capture([str(args.runtime/'llama-server'),'--version']):ap.error('matching pinned runtime required')
    machine=hardware()
    if machine['os']!='Darwin' or not machine.get('cpu'):ap.error('local Metal access required')
    sources=sorted((ROOT/'src').glob('*.c'))+[ROOT/'tests/bench_kalman_cost_native.c']
    extras=[ROOT/n for n in ['include/cxf/cost.h','include/cxf/cost_kalman.h','include/cxf/cost_residual.h','include/cxf/roofline.h','include/cxf/llama.h','src/cost_internal.h','src/llama_internal.h','tests/roofline_probe.h','tests/roofline_graph_probe.h','tests/roofline_shape_probe.h','tests/replay_cost_kalman.c','tools/analyze_frozen_calibration.py','tools/bench_roofline_estimator.py']]
    hashes={str(p.relative_to(ROOT)):digest(p) for p in sources+extras+[Path(__file__)]}
    libs=['libllama.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']
    profile=dict(estimator_schema='scalar-kalman-residual-v1-frozen-noise',hardware=machine,model=args.model.name,model_sha256=digest(args.model),llama_commit=commit,
                 runtime_libraries_sha256={n:digest(args.runtime/n) for n in libs},configuration=dict(context_tokens=8192,batch_token_limits=[256,64],cpu_threads=4,sessions=2,kv_type='f16',backend='Metal',prefill_min_tokens_per_cap={'256':128,'64':64}))
    fingerprint=hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest();args.out.mkdir(parents=True,exist_ok=True)
    command=['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Wpedantic','-DCXF_FROZEN_STUDY','-I'+str(ROOT/'include'),'-I'+str(args.sdk/'include'),'-I'+str(args.sdk/'ggml/include')]
    command += [str(p) for p in sources]+['-L'+str(args.runtime),'-Wl,-rpath,'+str(args.runtime),'-lllama','-lggml','-lggml-base','-lm','-o',str(args.out/'bench_kalman')]
    (args.out/'compile.txt').write_text(capture(command))
    replay=['cc','-std=c11','-O2','-Wall','-Wextra','-Wpedantic','-I'+str(ROOT/'include'),str(ROOT/'src/cost_kalman.c'),str(ROOT/'tests/replay_cost_kalman.c'),'-lm','-o',str(args.out/'replay_kalman')]
    (args.out/'compile-replay.txt').write_text(capture(replay))
    saved=dict(profile=profile,profile_sha256=fingerprint,sources_sha256=hashes,compile=command,replay_compile=replay)
    (args.out/'manifest.json').write_text(json.dumps(saved,indent=2)+'\n')
    print('Measuring new Kalman grid:',args.out.name,flush=True)
    with (args.out/'raw.jsonl').open('w') as out,(args.out/'engine.log').open('w') as log:
        p=subprocess.run([str(args.out/'bench_kalman'),str(args.model),str(args.runtime),'7',fingerprint[:16]],stdout=out,stderr=log)
    if p.returncode:raise SystemExit('Native failed; artifacts retained')
    finalize(args,saved)


if __name__=='__main__':main()
