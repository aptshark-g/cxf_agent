#!/usr/bin/env python3
"""Paired resource isolation of snapshot unlink and last-close versus tool variability."""
import argparse,json,math,re,statistics as st,subprocess,sys,zipfile
from pathlib import Path
from bench_roofline_estimator import ROOT,hardware,digest,COMMIT
from bench_tool_prepare import ALPHA,TIMING,TASK
from bench_tool_taskcost import parts as task_parts,task_accounting

MODES=['resident','restore_late','restore_progress','restore_progress_early','restore_progress_hold']
COMPARISONS=[('resident','restore_late'),('restore_late','restore_progress'),('restore_progress','restore_progress_early'),('restore_progress_early','restore_progress_hold'),('restore_progress','restore_progress_hold')]


def parts(r):
    out=task_parts(r)
    for phase,p in r['phases'].items():
        out.update({phase+'_'+k:v for k,v in p.items() if k not in ['start_ns','end_ns']})
        out[phase+'_cpu_ms']=p['user_ms']+p['system_ms']
    out['other_tool_ms']=out['tool_fn_ms']-sum(p['wall_ms'] for p in r['phases'].values())
    for prefix in ['tool_resource','early_drop','drop']:
        a,b=(r[prefix+'_'+k] for k in ['before','after'])
        if not a['valid'] or not b['valid']:continue
        for k in a:
            if k!='valid':out[prefix+'_'+k+'_delta']=b[k]-a[k]
        out[prefix+'_cpu_ms']=sum(b[k]-a[k] for k in ['user_ns','system_ns'])/1e6
    return out


def summary(rows):
    result=[]
    for mode in MODES:
        for kind,steps,fields in [('task_timing',[None],TASK),('timing',range(3),TIMING)]:
            for step in steps:
                g=[r for r in rows if r['kind']==kind and r.get('phase')=='evaluation' and r['mode']==mode and (step is None or r['step']==step)]
                result.append(dict(kind=kind,phase='evaluation',scenario='compile_test',words=512,mode=mode,**({} if step is None else {'step':step}),samples=len(g),**{f:dict(median=st.median(r[f] for r in g),min=min(r[f] for r in g),max=max(r[f] for r in g)) for f in fields}))
    return result


def stage_summary(rows):
    result=[]
    for mode in MODES:
        g=[parts(r) for r in rows if r['kind']=='timing' and r.get('phase')=='evaluation' and r['mode']==mode]
        common=set.intersection(*(set(r) for r in g))
        result.append(dict(scenario='compile_test',words=512,mode=mode,samples=len(g),**{f:dict(mean=st.mean(r[f] for r in g),median=st.median(r[f] for r in g),min=min(r[f] for r in g),max=max(r[f] for r in g)) for f in sorted(common)}))
    return result


def per_task(rows):
    result=[]
    for t in task_accounting(rows):
        g=[r for r in rows if r['kind']=='timing' and all(r[k]==t[k] for k in ['phase','scenario','words','mode','rep'])]
        result.append(dict(t,**{phase+'_'+f+'_sum':sum(parts(r)[phase+'_'+f] for r in g) for phase in ['compile','run'] for f in ['wall_ms','cpu_ms','in_ops','out_ops','major_faults','involuntary']},
                           exposed_drop_sum_ms=sum(parts(r)['exposed_drop_ms'] for r in g)))
    return result


def pair_rows(rows):
    out=[];tasks=per_task(rows)
    for baseline,mode in COMPARISONS:
        select=lambda m:{r['rep']:r for r in tasks if r['phase']=='evaluation' and r['mode']==m}
        a,b=select(baseline),select(mode)
        for rep in sorted(a):
            fields=['task_ms','tool_sum_ms','pre_tool_ms','post_tool_sum_ms','inter_tool_ms','bookkeeping_ms','exposed_drop_sum_ms',*[p+'_'+f+'_sum' for p in ['compile','run'] for f in ['wall_ms','cpu_ms','in_ops','out_ops','major_faults','involuntary']]]
            out.append(dict(scenario='compile_test',words=512,baseline=baseline,mode=mode,rep=rep,baseline_task_index=a[rep]['record_index'],candidate_task_index=b[rep]['record_index'],**{f:a[rep][f]-b[rep][f] for f in fields}))
    return out


def paired_summary(rows):
    pairs=pair_rows(rows);out=[]
    for baseline,mode in COMPARISONS:
        g=[r for r in pairs if r['baseline']==baseline and r['mode']==mode]
        out.append(dict(scenario='compile_test',words=512,baseline=baseline,mode=mode,pairs=len(g),**{f:dict(median_saving=st.median(r[f] for r in g),min_saving=min(r[f] for r in g),max_saving=max(r[f] for r in g),positive_pairs=sum(r[f]>0 for r in g)) for f in g[0] if f not in ['scenario','words','baseline','mode','rep','baseline_task_index','candidate_task_index']}))
    return out


def check(data):
    rows=data['rows'];assert len(rows)==227
    timings=[r for r in rows if r['kind']=='timing'];assert len(timings)==168
    identities={}
    for r in timings:
        assert r['correct'] and not r['wasted_tokens'] and not r['rebuilds'] and r['trace_resources']
        identity=(r['receipt_sha256'],r['canonical_sha256'],r['greedy_token']);assert identities.setdefault(r['step'],identity)==identity
        c,p=r['phases']['compile'],r['phases']['run']
        assert r['tool_fn_start_ns']<=c['start_ns']<=c['end_ns']<=p['start_ns']<=p['end_ns']<=r['tool_end_ns']
        assert r['tool_resource_before']['valid']==r['tool_resource_after']['valid']==7
        if r['mode'].endswith('_hold'):
            assert r['hold_snapshot_file'] and r['early_drop_rc']==r['drop_rc']==0
            assert r['early_drop_end_ns']<=r['join_return_ns']<=r['drop_start_ns']<=r['drop_end_ns']
        parts(r)
    actual=[r['mode'] for r in rows if r['kind']=='timing' and r.get('phase')=='evaluation' and r['step']==0]
    expected=[MODES[(i+rep)%5 if rep<5 else ((rep-5)-i+5)%5] for rep in range(10) for i in range(5)]
    assert actual==expected
    for mode in MODES:
        base='cal_resident' if mode=='resident' else 'cal_restore'
        for step in range(3):
            train=[r for r in timings if r['phase']=='calibration' and r['mode']==base and r['step']==step];assert len(train)==3
            tool=st.median(r['tool_ms'] for r in train);rate=st.median(r['prepare_ms']/r['prefix_tokens'] for r in train);post=st.median((r['tool_end_ns']-r['spawn_end_ns'])/1e6 for r in train)
            group=sorted([r for r in timings if r['phase']=='evaluation' and r['mode']==mode and r['step']==step],key=lambda r:r['rep']);assert len(group)==10
            for i,r in enumerate(group):
                assert r['tool_observations_before']==3+i and r['prepare_observations_before']==(3 if mode=='resident' else 3+i)
                assert math.isclose(r['initial_tool_estimate_ms'],tool,abs_tol=2e-8) and math.isclose(r['predicted_post_spawn_ms'],post,abs_tol=2e-8)
                assert math.isclose(r['predicted_prepare_ms'],rate*r['prefix_tokens'],abs_tol=2e-8)
                if mode.startswith('restore_progress'):
                    elapsed=(r['progress_ns']-r['tool_start_ns'])/1e6
                    assert math.isclose(r['planned_delay_ms'],elapsed+max(0,post-rate*r['prefix_tokens']-5),abs_tol=2e-6)
                tool+=ALPHA*(r['tool_ms']-tool);post+=ALPHA*((r['tool_end_ns']-r['progress_ns'])/1e6-post)
                if mode!='resident':rate+=ALPHA*(r['prepare_ms']/r['prefix_tokens']-rate)
    assert data['summary']==summary(rows) and data['stage_summary']==stage_summary(rows)
    assert data['task_accounting']==per_task(rows) and data['pair_rows']==pair_rows(rows) and data['paired_summary']==paired_summary(rows)
    for r in rows:
        if r['kind']=='task_timing':assert math.isclose(r['task_ms'],r['task_wall_ms']-r['excluded_in_task_oracle_ms'],abs_tol=1e-8)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938');ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--finalize-existing',action='store_true')
    args=ap.parse_args();args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT) or (args.out/'evidence.json').exists():ap.error('fresh output required')
    if not args.finalize_existing:subprocess.run([sys.executable,str(ROOT/'tools/build_agent_native.py'),'--sdk',str(args.sdk),'--runtime',str(args.runtime),'--model',str(args.model),'--out',str(args.out),'--bench-tool-isolate','--repeats','10'],check=True)
    m=json.loads((args.out/'manifest.json').read_text());assert not m['sanitizers'] and m['llama_commit']==COMMIT
    rows=[json.loads(s) for s in (args.out/'tool-prepare.jsonl').read_text().splitlines()]
    sources={k:v for k,v in m['sources_sha256'].items() if not k.startswith('/')}
    for n in ['tests/agent_resource.h','tests/agent_kv_worker.h','tests/agent_tool_prepare.h','tools/build_agent_native.py','tools/bench_tool_isolate.py','tools/bench_tool_taskcost.py','tools/bench_restore_balance.py','tools/bench_restore_timing.py','tools/bench_tool_prepare.py','tools/bench_roofline_estimator.py']:sources[n]=digest(ROOT/n)
    assert all(digest(ROOT/n)==v for n,v in sources.items())
    offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',(args.out/'tool-prepare-engine.log').read_text());assert len(offloads)==1 and all(a==b for a,b in offloads)
    data=dict(date='2026-10-31',scope='Resource isolation of E24 short-task regression: compiler/run CPU, unlink and last close',hardware=hardware(),model=args.model.name,model_sha256=digest(args.model),llama_commit=COMMIT,
       sources_sha256=sources,runtime_libraries_sha256={n:digest(args.runtime/n) for n in ['libllama.dylib','libllama-common.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']},
       rows=rows,summary=summary(rows),stage_summary=stage_summary(rows),task_accounting=per_task(rows),pair_rows=pair_rows(rows),paired_summary=paired_summary(rows),origin_directory=str(args.out.relative_to(ROOT)),raw_jsonl_sha256=digest(args.out/'tool-prepare.jsonl'),engine_log_sha256=digest(args.out/'tool-prepare-engine.log'),verified_full_gpu_offload=True,
       method=dict(words=512,cpu_test_iterations=18000000,repeats=10,modes=MODES,alpha=ALPHA,margin_ms=5,context_tokens=8192,batch_tokens=512,threads=4,
          controls='resident; restore after tool; progress return drop; progress early unlink; early unlink with private fd retained through tool completion',
          calibration='Three complete tasks per state, same initial labels, independent per-mode past-only EWMA',order='Forward and reverse cyclic Latin order; each mode occupies every position twice',
          resource_units='wait4 user/system ms, faults and IO operations; Mach gauges bytes and global VM counters pages; global deltas not exclusive attribution',costs='Save/tools/repair/final suffix/metrics parsing/cleanup included; initial history/load/calibration/measured oracle excluded'),
       references=dict(wait4='https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/wait4.2.html',unlink='https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/unlink.2.html',rusage='https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html'),
       limits=['Sparse diagnostic sampling and trusted tool driver add common overhead; no cross-run subtraction from E24','Cached IO counters may be zero; zero counts do not exclude filesystem CPU or memory contention','CPU may exceed wall for parallel compiler children; wall minus CPU is not an IO estimate','Global VM deltas include external system activity; no frequency, physical SSD bandwidth or per-page cache tracing','Only same live engine/session/revision; normal resident path does not require snapshots; logical eviction does not shrink physical KV','One M5/Metal/Qwen0.6B; scripted real tools, no autonomous-model or Windows/CUDA claim'])
    check(data)
    with zipfile.ZipFile(args.out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
        for n in sorted(sources):z.write(ROOT/n,n)
        z.writestr('sources-sha256.json',json.dumps(sources,indent=2)+'\n')
    (args.out/'evidence.json').write_text(json.dumps(data,indent=2)+'\n')
    for r in data['paired_summary']:print(r['baseline'],'->',r['mode'],r['task_ms'],r['compile_wall_ms_sum'],r['run_wall_ms_sum'],flush=True)
    print('Native records',len(rows),flush=True)


if __name__=='__main__':main()
