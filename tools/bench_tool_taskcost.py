#!/usr/bin/env python3
"""Complete-task accounting, progress cost controls and optional early snapshot release."""
import argparse
import json
import math
from pathlib import Path
import re
import statistics as st
import subprocess
import sys
import zipfile
from bench_roofline_estimator import ROOT,hardware,digest,COMMIT
from bench_tool_prepare import ALPHA
from bench_restore_timing import summaries
from bench_restore_balance import parts as old_parts

MODES=['resident','restore_timed_noevent','restore_timed','restore_progress','restore_progress_early']
KEYS=['phase','scenario','words','mode','rep']
COMPONENTS=['pre_tool_ms','tool_sum_ms','post_tool_sum_ms','inter_tool_ms','bookkeeping_ms']
COMPARISONS=[('restore_timed_noevent','restore_timed'),('restore_timed','restore_progress'),
             ('restore_progress','restore_progress_early'),('restore_timed','restore_progress_early'),
             ('resident','restore_progress_early')]


def task_parts(rows,t):
    indexed=[(i,r) for i,r in enumerate(rows) if r['kind']=='timing' and all(r[k]==t[k] for k in KEYS)]
    assert len(indexed)==3
    pre=sum((r['tool_start_ns']-r['entered_ns'])/1e6 for i,r in indexed)
    result={k:t[k] for k in ['task_ms','tool_sum_ms','post_tool_sum_ms','inter_tool_ms']}
    result.update(pre_tool_ms=pre,bookkeeping_ms=t['task_ms']-pre-t['tool_sum_ms']-t['post_tool_sum_ms']-t['inter_tool_ms'],
                  save_sum_ms=t['save_sum_ms'],source_call_indices=','.join(str(i) for i,r in indexed))
    assert result['bookkeeping_ms']>=-1e-6
    assert math.isclose(sum(result[k] for k in COMPONENTS),t['task_ms'],abs_tol=1e-8)
    return result


def e23_audit():
    path=ROOT/'docs/evidence/restore-balance-20261031.json';rows=json.loads(path.read_text())['rows'];out=[]
    for words in [320,1792]:
        select=lambda mode:{r['rep']:(i,r) for i,r in enumerate(rows) if r['kind']=='task_timing' and r['phase']=='evaluation' and r['scenario']=='compile_test' and r['words']==words and r['mode']==mode}
        a,b=select('restore_spawn_timed'),select('restore_spawn_progress')
        for rep in range(7):
            ia,ta=a[rep];ib,tb=b[rep];pa,pb=task_parts(rows,ta),task_parts(rows,tb)
            delta={k:pb[k]-pa[k] for k in ['task_ms',*COMPONENTS,'save_sum_ms']}
            assert math.isclose(delta['task_ms'],sum(delta[k] for k in COMPONENTS),abs_tol=1e-8)
            out.append(dict(source_experiment='E23',words=words,rep=rep,baseline_task_index=ia,candidate_task_index=ib,
                            baseline_call_indices=pa['source_call_indices'],candidate_call_indices=pb['source_call_indices'],**delta))
    return dict(source_sha256=digest(path),delta_convention='candidate progress minus baseline timed; positive means slower; save is a subset of pre_tool, not additive',pairs=out)


def parts(r):
    out=old_parts(r)
    out.update(pre_tool_ms=(r['tool_start_ns']-r['entered_ns'])/1e6,
               callback_ms=(r['callback_end_ns']-r['callback_start_ns'])/1e6,
               thread_create_ms=(r['thread_launch_end_ns']-r['thread_launch_start_ns'])/1e6,
               start_to_fn_ms=(r['tool_fn_start_ns']-r['tool_start_ns'])/1e6,
               tool_fn_ms=(r['tool_end_ns']-r['tool_fn_start_ns'])/1e6,
               early_drop_ms=(r['early_drop_end_ns']-r['early_drop_start_ns'])/1e6,
               exposed_drop_ms=sum(max(0,r[b]-max(r[a],r['tool_end_ns']))/1e6 for a,b in [('early_drop_start_ns','early_drop_end_ns'),('drop_start_ns','drop_end_ns')]))
    return out


def stage_summary(rows):
    groups={}
    for r in rows:
        if r['kind']=='timing' and r['phase']=='evaluation':groups.setdefault((r['scenario'],r['words'],r['mode']),[]).append(parts(r))
    return [dict(scenario=s,words=w,mode=m,samples=len(g),**{f:dict(mean=st.mean(r[f] for r in g),median=st.median(r[f] for r in g),min=min(r[f] for r in g),max=max(r[f] for r in g)) for f in g[0]}) for (s,w,m),g in sorted(groups.items())]


def task_accounting(rows):
    return [dict(record_index=i,**{k:t[k] for k in KEYS},**task_parts(rows,t)) for i,t in enumerate(rows) if t['kind']=='task_timing']


def pairs(rows):
    out=[]
    for scenario in ['read','compile_test']:
        for words in [512,2304]:
            select=lambda m:{r['rep']:r for r in rows if r['kind']=='task_timing' and r['phase']=='evaluation' and r['scenario']==scenario and r['words']==words and r['mode']==m}
            for baseline,mode in COMPARISONS:
                a,b=select(baseline),select(mode);assert len(a)==len(b)==7
                fields=['task_ms','post_tool_sum_ms','early_hold_sum_ms',*COMPONENTS]
                pa={i:task_parts(rows,r) for i,r in a.items()};pb={i:task_parts(rows,r) for i,r in b.items()}
                out.append(dict(scenario=scenario,words=words,baseline=baseline,mode=mode,pairs=7,
                    both_wait_and_hold_lower_pairs=sum(b[i]['post_tool_sum_ms']<a[i]['post_tool_sum_ms'] and b[i]['early_hold_sum_ms']<a[i]['early_hold_sum_ms'] for i in a),
                    **{f:dict(median_saving_ms=st.median((a[i] if f in a[i] else pa[i])[f]-(b[i] if f in b[i] else pb[i])[f] for i in a),
                              min_saving_ms=min((a[i] if f in a[i] else pa[i])[f]-(b[i] if f in b[i] else pb[i])[f] for i in a),
                              max_saving_ms=max((a[i] if f in a[i] else pa[i])[f]-(b[i] if f in b[i] else pb[i])[f] for i in a),
                              positive_pairs=sum((a[i] if f in a[i] else pa[i])[f]>(b[i] if f in b[i] else pb[i])[f] for i in a)) for f in dict.fromkeys(fields)}))
    return out


def check(data):
    rows=data['rows'];assert len(rows)==659
    assert sum(r['kind']=='timing' and r['phase']=='evaluation' for r in rows)==420
    identities={}
    for r in rows:
        if r['kind']!='timing':continue
        assert r['correct'] and not r['rebuilds'] and not r['wasted_tokens']
        assert r['entered_ns']<=r['tool_start_ns']<=r['tool_fn_start_ns']<=r['tool_end_ns']<=r['request_ready_ns']<=r['first_token_ns']
        assert r['suffix_evaluated_tokens']==r['committed_tokens']-r['prefix_tokens']
        key=(r['scenario'],r['words'],r['step']);identity=(r['canonical_sha256'],r['receipt_sha256'],r['greedy_token'])
        assert identities.setdefault(key,identity)==identity
        event=r['scenario']=='compile_test' and r['event_enabled']
        assert bool(r['progress_ns'])==event and bool(r['callback_start_ns'])==event
        if event:assert r['spawn_end_ns']==r['progress_ns']<=r['callback_start_ns']<=r['callback_end_ns']<=r['tool_end_ns']
        if r['scenario']=='compile_test':assert r['process_backend']==2
        if r['prefix_start_ns']:
            assert r['restored_tokens']==r['prefix_tokens'] and r['restored_bytes']>0
            if r['release_snapshot_early']:
                assert r['early_drop_rc']==0 and r['prefix_ready_ns']<=r['early_drop_start_ns']<=r['early_drop_end_ns']<=r['join_return_ns']
                assert not r['drop_start_ns']
            else:assert r['drop_rc']==0 and r['tool_end_ns']<=r['drop_start_ns']<=r['drop_end_ns']
        parts(r)
    for scenario in ['read','compile_test']:
        for words in [512,2304]:
            for mode in MODES:
                for step in range(3):
                    base='cal_resident' if mode=='resident' else 'cal_restore'
                    train=[r for r in rows if r['kind']=='timing' and r['phase']=='calibration' and r['scenario']==scenario and r['words']==words and r['mode']==base and r['step']==step];assert len(train)==3
                    tool=st.median(r['tool_ms'] for r in train);rate=st.median(r['prepare_ms']/r['prefix_tokens'] for r in train)
                    post=st.median((r['tool_end_ns']-r['spawn_end_ns'])/1e6 for r in train) if scenario=='compile_test' else 0
                    group=sorted([r for r in rows if r['kind']=='timing' and r['phase']=='evaluation' and r['scenario']==scenario and r['words']==words and r['mode']==mode and r['step']==step],key=lambda r:r['rep'])
                    for count,r in enumerate(group):
                        event=scenario=='compile_test' and mode!='restore_timed_noevent'
                        assert r['tool_observations_before']==3+count and r['prepare_observations_before']==(3 if mode=='resident' else 3+count)
                        assert r['progress_observations_before']==(3+(count if event else 0) if scenario=='compile_test' else 0)
                        assert math.isclose(r['initial_tool_estimate_ms'],tool,abs_tol=2e-8)
                        assert math.isclose(r['predicted_prepare_ms'],rate*r['prefix_tokens'],abs_tol=2e-8)
                        assert math.isclose(r['predicted_post_spawn_ms'],post,abs_tol=2e-8)
                        effective=tool;delay=max(0,tool-rate*r['prefix_tokens']-5) if mode.startswith('restore_timed') else 0
                        if mode.startswith('restore_progress'):
                            if event:
                                elapsed=(r['progress_ns']-r['tool_start_ns'])/1e6;effective=elapsed+post;delay=elapsed+max(0,post-rate*r['prefix_tokens']-5)
                            else:assert r['prefix_start_ns']>=r['tool_end_ns']
                        assert math.isclose(r['predicted_tool_ms'],effective,abs_tol=2e-8) and math.isclose(r['planned_delay_ms'],delay,abs_tol=2e-6)
                        tool+=ALPHA*(r['tool_ms']-tool)
                        if mode!='resident':rate+=ALPHA*(r['prepare_ms']/r['prefix_tokens']-rate)
                        if event:post+=ALPHA*((r['tool_end_ns']-r['progress_ns'])/1e6-post)
    for t in rows:
        if t['kind']=='task_timing':
            assert math.isclose(t['task_ms'],t['task_wall_ms']-t['excluded_in_task_oracle_ms'],abs_tol=1e-8)
            task_parts(rows,t)
    assert data['summary']==summaries(rows) and data['stage_summary']==stage_summary(rows)
    assert data['task_accounting']==task_accounting(rows) and data['paired_summary']==pairs(rows)
    assert data['e23_audit']==e23_audit()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938');ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--finalize-existing',action='store_true')
    args=ap.parse_args();args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT) or (args.out/'evidence.json').exists():ap.error('fresh local output required')
    if not args.finalize_existing:subprocess.run([sys.executable,str(ROOT/'tools/build_agent_native.py'),'--sdk',str(args.sdk),'--runtime',str(args.runtime),'--model',str(args.model),'--out',str(args.out),'--bench-tool-taskcost','--repeats','7'],check=True)
    manifest=json.loads((args.out/'manifest.json').read_text());assert manifest['llama_commit']==COMMIT and not manifest['sanitizers']
    rows=[json.loads(s) for s in (args.out/'tool-prepare.jsonl').read_text().splitlines()]
    offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',(args.out/'tool-prepare-engine.log').read_text());assert len(offloads)==1 and all(a==b for a,b in offloads)
    sources={k:v for k,v in manifest['sources_sha256'].items() if not k.startswith('/')}
    for name in ['tests/agent_tool_prepare.h','tests/agent_kv_worker.h','tests/agent_basic.h','tools/build_agent_native.py','tools/bench_tool_taskcost.py','tools/bench_restore_balance.py','tools/bench_restore_timing.py','tools/bench_tool_prepare.py','tools/bench_roofline_estimator.py']:sources[name]=digest(ROOT/name)
    for name,value in sources.items():assert digest(ROOT/name)==value,(name,'changed during measurement')
    with zipfile.ZipFile(args.out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
        for name in sorted(sources):z.write(ROOT/name,name)
        z.writestr('sources-sha256.json',json.dumps(sources,indent=2)+'\n')
    data=dict(date='2026-10-31',scope='E23 additive task audit plus new real-tool callback and early-snapshot-release ablation',hardware=hardware(),model=args.model.name,model_sha256=digest(args.model),llama_commit=COMMIT,
        sources_sha256=sources,runtime_libraries_sha256={n:digest(args.runtime/n) for n in ['libllama.dylib','libllama-common.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']},
        rows=rows,summary=summaries(rows),stage_summary=stage_summary(rows),paired_summary=pairs(rows),task_accounting=task_accounting(rows),e23_audit=e23_audit(),origin_directory=str(args.out.relative_to(ROOT)),
        raw_jsonl_sha256=digest(args.out/'tool-prepare.jsonl'),engine_log_sha256=digest(args.out/'tool-prepare-engine.log'),verified_full_gpu_offload=True,
        method=dict(modes=MODES,words=[512,2304],cpu_test_iterations=18000000,repeats=7,context_tokens=8192,batch_tokens=512,threads=4,alpha=ALPHA,margin_ms=5,
            workload='New affine repair calc(x)=2*x+1 checked at 3 and 9, rotate/xor digest; actual FAIL/write/PASS/PASS; fast read control; no synthetic performance sleep',
            training='Three complete tasks per case/state before evaluation; identical restore calibration labels and independent mode keys; EWMA only past own-mode labels',
            controls='Timed no-event vs event isolates observation pathway; progress return vs early release isolates cleanup placement; absolute timestamps for callback and thread launch',
            costs='Save/evict/tools/repair/suffix/cleanup included; initial history/load/calibration/oracle excluded and recorded; full additive task identity',
            order='Modes rotate by replicate; fixed alpha/margin/strategy before new evaluation; no best-run selection'),
        limits=['One M5/Metal/model, warm OS file cache; no Windows/CUDA/cold SSD claim','Tool duration can include interference and OS/frequency/cache variation; matched repeats do not isolate all causal resource contention','Direct callback wall time is not all possible system interference; stage medians are not additive','Early file release does not shrink physical KV buffer; default snapshot cleanup and timed policy remain unchanged','Scripted real tools and fresh-KV oracle; not autonomous model ability'])
    check(data);(args.out/'evidence.json').write_text(json.dumps(data,indent=2)+'\n')
    for r in data['paired_summary']:
        if r['scenario']=='compile_test':print(r['words'],r['baseline'],r['mode'],'task',r['task_ms'],'wait',r['post_tool_sum_ms'],flush=True)
    print('Native records',len(rows),flush=True)


if __name__=='__main__':main()
