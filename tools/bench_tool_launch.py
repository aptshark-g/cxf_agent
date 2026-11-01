#!/usr/bin/env python3
"""First executable launch versus same-artifact rerun and an incremental-build task control."""
import argparse,json,math,re,statistics as st,subprocess,sys,zipfile
from pathlib import Path
from bench_roofline_estimator import ROOT,hardware,digest,COMMIT
from bench_tool_prepare import ALPHA
from bench_restore_timing import summaries
from bench_tool_taskcost import parts as base_parts,task_accounting

MODES=['resident','restore_progress','restore_progress_early','restore_progress_hold','resident_incremental']
COMPARISONS=[('restore_progress','restore_progress_early'),('restore_progress_early','restore_progress_hold'),('resident','resident_incremental'),('resident','restore_progress')]


def parts(r):
    out=base_parts(r)
    for name,p in r['phases'].items():
        for key in ['wall_ms','user_ms','system_ms','minor_faults','major_faults','in_ops','out_ops','voluntary','involuntary','executed']:out[name+'_'+key]=p[key]
        out[name+'_cpu_ms']=p['user_ms']+p['system_ms'];out[name+'_spawn_ms']=(p['spawn_end_ns']-p['start_ns'])/1e6
        if name!='compile':
            out[name+'_pre_main_ms']=(p['main_ns']-p['start_ns'])/1e6
            out[name+'_compute_ms']=(p['computed_ns']-p['main_ns'])/1e6
            out[name+'_exit_tail_ms']=(p['end_ns']-p['computed_ns'])/1e6
            assert math.isclose(sum(out[name+'_'+f] for f in ['pre_main_ms','compute_ms','exit_tail_ms']),p['wall_ms'],abs_tol=1e-8)
    out['same_artifact_rerun_saving_ms']=out['run_wall_ms']-out['rerun_wall_ms']
    out['other_tool_ms']=out['tool_fn_ms']-sum(p['wall_ms'] for p in r['phases'].values())
    return out


def stage_summary(rows):
    result=[]
    for mode in MODES:
        g=[parts(r) for r in rows if r['kind']=='timing' and r.get('phase')=='evaluation' and r['mode']==mode]
        result.append(dict(scenario='compile_test',words=768,mode=mode,samples=len(g),**{f:dict(mean=st.mean(r[f] for r in g),median=st.median(r[f] for r in g),min=min(r[f] for r in g),max=max(r[f] for r in g)) for f in g[0]}))
    return result


def per_task(rows):
    result=[]
    for t in task_accounting(rows):
        g=[r for r in rows if r['kind']=='timing' and all(r[k]==t[k] for k in ['phase','scenario','words','mode','rep'])]
        result.append(dict(t,**{name+'_'+f+'_sum':sum(parts(r)[name+'_'+f] for r in g) for name in ['compile','run','rerun'] for f in ['wall_ms','cpu_ms']},compile_calls=sum(r['phases']['compile']['executed'] for r in g)))
    return result


def pair_rows(rows):
    tasks=per_task(rows);result=[]
    for baseline,mode in COMPARISONS:
        select=lambda m:{r['rep']:r for r in tasks if r['phase']=='evaluation' and r['mode']==m};a,b=select(baseline),select(mode)
        for rep in a:
            fields=['task_ms','pre_tool_ms','tool_sum_ms','post_tool_sum_ms','inter_tool_ms','bookkeeping_ms',*[n+'_'+f+'_sum' for n in ['compile','run','rerun'] for f in ['wall_ms','cpu_ms']]]
            result.append(dict(scenario='compile_test',words=768,baseline=baseline,mode=mode,rep=rep,baseline_task_index=a[rep]['record_index'],candidate_task_index=b[rep]['record_index'],**{f:a[rep][f]-b[rep][f] for f in fields}))
    return result


def paired_summary(rows):
    pairs=pair_rows(rows);result=[]
    for baseline,mode in COMPARISONS:
        g=[r for r in pairs if r['baseline']==baseline and r['mode']==mode]
        result.append(dict(scenario='compile_test',words=768,baseline=baseline,mode=mode,pairs=len(g),**{f:dict(median_saving=st.median(r[f] for r in g),min_saving=min(r[f] for r in g),max_saving=max(r[f] for r in g),positive_pairs=sum(r[f]>0 for r in g)) for f in g[0] if f not in ['scenario','words','baseline','mode','rep','baseline_task_index','candidate_task_index']}))
    return result


def launch_summary(rows):
    out=[]
    for cached in [False,True]:
        g=[r for r in rows if r['kind']=='timing' and r['phase']=='evaluation' and bool(not r['phases']['compile']['executed'])==cached]
        for name in ['run','rerun']:
            v=[parts(r) for r in g];fields=[name+'_'+f for f in ['wall_ms','cpu_ms','spawn_ms','pre_main_ms','compute_ms','exit_tail_ms']]
            out.append(dict(compiled_this_call=not cached,execution=name,samples=len(g),**{f:dict(mean=st.mean(r[f] for r in v),median=st.median(r[f] for r in v),min=min(r[f] for r in v),max=max(r[f] for r in v)) for f in fields}))
    return out


def check(data):
    rows=data['rows'];assert len(rows)==167
    timings=[r for r in rows if r['kind']=='timing'];assert len(timings)==123
    identities={}
    for r in timings:
        assert r['correct'] and not r['wasted_tokens'] and not r['rebuilds'] and not r['trace_resources']
        identity=(r['receipt_sha256'],r['canonical_sha256'],r['greedy_token']);assert identities.setdefault(r['step'],identity)==identity
        pp=r['phases'];assert set(pp)=={'compile','run','rerun'}
        assert r['tool_fn_start_ns']<=pp['compile']['start_ns']<=pp['compile']['end_ns']<=pp['run']['start_ns']<=pp['run']['end_ns']<=pp['rerun']['start_ns']<=pp['rerun']['end_ns']<=r['tool_end_ns']
        for name in ['run','rerun']:
            p=pp[name];assert p['start_ns']<=p['main_ns']<p['computed_ns']<=p['end_ns']
            assert p['start_ns']<=p['spawn_end_ns']<=p['end_ns']
            assert (p['computed_ns']-p['main_ns'])/1e6>.5 # verifies loop really lies inside measured boundaries on this compiler
        assert pp['compile']['executed']==(0 if r['mode']=='resident_incremental' and r['step']==2 else 1)
        if r['mode'].endswith('_hold'):assert r['early_drop_rc']==r['drop_rc']==0
        parts(r)
    for mode in MODES:
        base='cal_resident' if mode in ['resident','resident_incremental'] else 'cal_restore'
        for step in range(3):
            train=[r for r in timings if r['phase']=='calibration' and r['mode']==base and r['step']==step];assert len(train)==3
            tool=st.median(r['tool_ms'] for r in train);rate=st.median(r['prepare_ms']/r['prefix_tokens'] for r in train);post=st.median((r['tool_end_ns']-r['spawn_end_ns'])/1e6 for r in train)
            group=sorted([r for r in timings if r['phase']=='evaluation' and r['mode']==mode and r['step']==step],key=lambda r:r['rep']);assert len(group)==7
            for i,r in enumerate(group):
                assert math.isclose(r['initial_tool_estimate_ms'],tool,abs_tol=2e-8) and math.isclose(r['predicted_prepare_ms'],rate*r['prefix_tokens'],abs_tol=2e-8)
                assert math.isclose(r['predicted_post_spawn_ms'],post,abs_tol=2e-8)
                if mode.startswith('restore_progress'):
                    elapsed=(r['progress_ns']-r['tool_start_ns'])/1e6
                    assert math.isclose(r['planned_delay_ms'],elapsed+max(0,post-rate*r['prefix_tokens']-5),abs_tol=2e-6)
                tool+=ALPHA*(r['tool_ms']-tool);post+=ALPHA*((r['tool_end_ns']-r['progress_ns'])/1e6-post)
                if mode.startswith('restore_progress'):rate+=ALPHA*(r['prepare_ms']/r['prefix_tokens']-rate)
    assert data['summary']==summaries(rows) and data['stage_summary']==stage_summary(rows) and data['task_accounting']==per_task(rows)
    assert data['pair_rows']==pair_rows(rows) and data['paired_summary']==paired_summary(rows) and data['launch_summary']==launch_summary(rows)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938');ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--finalize-existing',action='store_true')
    args=ap.parse_args();args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT) or (args.out/'evidence.json').exists():ap.error('fresh output required')
    if not args.finalize_existing:subprocess.run([sys.executable,str(ROOT/'tools/build_agent_native.py'),'--sdk',str(args.sdk),'--runtime',str(args.runtime),'--model',str(args.model),'--out',str(args.out),'--bench-tool-launch','--repeats','7'],check=True)
    m=json.loads((args.out/'manifest.json').read_text());assert not m['sanitizers'] and m['llama_commit']==COMMIT
    rows=[json.loads(s) for s in (args.out/'tool-prepare.jsonl').read_text().splitlines()];sources={k:v for k,v in m['sources_sha256'].items() if not k.startswith('/')}
    for n in ['tests/agent_resource.h','tests/agent_kv_worker.h','tests/agent_tool_prepare.h','tools/build_agent_native.py','tools/bench_tool_launch.py','tools/bench_tool_isolate.py','tools/bench_tool_taskcost.py','tools/bench_restore_balance.py','tools/bench_restore_timing.py','tools/bench_tool_prepare.py','tools/bench_roofline_estimator.py']:sources[n]=digest(ROOT/n)
    assert all(digest(ROOT/n)==v for n,v in sources.items())
    offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',(args.out/'tool-prepare-engine.log').read_text());assert len(offloads)==1 and all(a==b for a,b in offloads)
    data=dict(date='2026-10-31',scope='New-task main-entry executable-launch attribution and explicit incremental-build control; common repeated check cost included',hardware=hardware(),model=args.model.name,model_sha256=digest(args.model),llama_commit=COMMIT,sources_sha256=sources,runtime_libraries_sha256={n:digest(args.runtime/n) for n in ['libllama.dylib','libllama-common.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']},rows=rows,summary=summaries(rows),stage_summary=stage_summary(rows),task_accounting=per_task(rows),pair_rows=pair_rows(rows),paired_summary=paired_summary(rows),launch_summary=launch_summary(rows),origin_directory=str(args.out.relative_to(ROOT)),raw_jsonl_sha256=digest(args.out/'tool-prepare.jsonl'),engine_log_sha256=digest(args.out/'tool-prepare-engine.log'),verified_full_gpu_offload=True,
        method=dict(words=768,cpu_test_iterations=12000000,repeats=7,modes=MODES,alpha=ALPHA,margin_ms=5,context_tokens=8192,batch_tokens=512,threads=4,
           workload='New calc(x)=3*x-2 repair checked at3 and9,12M rotate/xor; every tool runs actual check twice with matching status; second stdout discarded, both executions charged',
           controls='Always compile versus fixed incremental recipe; delete check at trial reset; step0 compile, step1 source correction/rebuild, incremental step2 reuses artifact but still runs both tests',
           launch='Child main/compute-end MONOTONIC_RAW plus driver spawn start/return and wait4 end; pre-main includes launch and is not added to it; no assumption spawn return precedes main on every scheduler',
           calibration='Three complete tasks per state; independent per-mode past-only EWMA; no evaluation tuning',order='Rotate modes by replicate; same paired task counts and work; no best-run selection',costs='Save/tools/repair/final suffix/metrics parsing/cleanup and repeated checks included; initial history/load/calibration/oracle separately recorded'),
        limits=['First new-artifact execution delay is located by timestamps, not attributed to a specific macOS security service or kernel mechanism','Incremental compilation is an explicit benchmark/deployment recipe, not automatic Agent tool-result caching; tests are never skipped','No per-process resource probes during measured wrapper; child wait4 metrics and private numeric labels are common overhead','E24 missing child-entry boundaries cannot be retrospectively assigned to this new measurement; old negative remains','One M5/Metal/model, warm system, scripted actual tools; no physical KV shrinking, Windows/CUDA or autonomous-model claim'])
    check(data)
    with zipfile.ZipFile(args.out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
        for n in sorted(sources):z.write(ROOT/n,n)
        z.writestr('sources-sha256.json',json.dumps(sources,indent=2)+'\n')
    (args.out/'evidence.json').write_text(json.dumps(data,indent=2)+'\n')
    for r in data['paired_summary']:print(r['baseline'],'->',r['mode'],r['task_ms'],flush=True)
    for r in data['launch_summary']:print('LAUNCH',r['compiled_this_call'],r['execution'],r['samples'],r[r['execution']+'_wall_ms'],r[r['execution']+'_pre_main_ms'],flush=True)
    print('Native records',len(rows),flush=True)


if __name__=='__main__':main()
