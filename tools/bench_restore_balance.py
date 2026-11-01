#!/usr/bin/env python3
"""Frozen spawn/progress ablation on new tool/history fixtures."""
import argparse
import json
import math
from pathlib import Path
import re
import statistics as st
import subprocess
import sys
from bench_roofline_estimator import ROOT,hardware,digest,COMMIT
from bench_tool_prepare import ALPHA
from bench_restore_timing import summaries,parts as timing_parts

MODES=['resident','restore_fork_immediate','restore_fork_timed','restore_fork_progress',
       'restore_spawn_immediate','restore_spawn_timed','restore_spawn_progress']


def parts(row):
    return timing_parts(dict(row,mode='resident') if not row['prefix_start_ns'] else row)


def stage_summary(rows):
    groups={}
    for row in rows:
        if row['kind']=='timing' and row['phase']=='evaluation':
            groups.setdefault((row['scenario'],row['words'],row['mode']),[]).append(parts(row))
    return [dict(scenario=s,words=w,mode=m,samples=len(g),**{f:dict(mean=st.mean(r[f] for r in g),median=st.median(r[f] for r in g),min=min(r[f] for r in g),max=max(r[f] for r in g)) for f in g[0]}) for (s,w,m),g in sorted(groups.items())]


def pairs(rows):
    out=[]
    comparisons=[('restore_fork_immediate','restore_fork_timed'),('restore_fork_timed','restore_fork_progress'),
                 ('restore_spawn_immediate','restore_spawn_timed'),('restore_spawn_timed','restore_spawn_progress'),
                 ('restore_fork_timed','restore_spawn_timed'),('restore_fork_progress','restore_spawn_progress'),
                 ('restore_fork_timed','restore_spawn_progress'),('restore_spawn_immediate','restore_spawn_progress'),
                 ('resident','restore_spawn_progress')]
    for scenario in ['read','compile_test']:
        for words in [320,1792]:
            select=lambda mode:{r['rep']:r for r in rows if r['kind']=='task_timing' and r['phase']=='evaluation' and r['scenario']==scenario and r['words']==words and r['mode']==mode}
            for baseline,mode in comparisons:
                a,b=select(baseline),select(mode);assert len(a)==len(b)==7
                both=sum(b[i]['post_tool_sum_ms']<a[i]['post_tool_sum_ms'] and b[i]['early_hold_sum_ms']<a[i]['early_hold_sum_ms'] for i in range(7))
                out.append(dict(scenario=scenario,words=words,baseline=baseline,mode=mode,pairs=7,both_wait_and_hold_lower_pairs=both,
                                **{field:dict(median_saving_ms=st.median(a[i][field]-b[i][field] for i in range(7)),min_saving_ms=min(a[i][field]-b[i][field] for i in range(7)),max_saving_ms=max(a[i][field]-b[i][field] for i in range(7)),positive_pairs=sum(a[i][field]>b[i][field] for i in range(7))) for field in ['task_ms','post_tool_sum_ms','early_hold_sum_ms','tool_sum_ms']}))
    return out


def check(data):
    rows=data['rows'];assert len(rows)==979
    assert sum(r['kind']=='timing' and r['phase']=='evaluation' for r in rows)==588
    for r in rows:
        if r['kind']=='timing':
            assert r['correct'] and not r['wasted_tokens'] and not r['rebuilds']
            assert r['entered_ns']<=r['tool_start_ns']<=r['tool_end_ns']<=r['request_ready_ns']<=r['first_token_ns']
            assert r['suffix_evaluated_tokens']==r['committed_tokens']-r['prefix_tokens']
            if r['scenario']=='compile_test':
                assert r['tool_start_ns']<=r['spawn_begin_ns']<=r['spawn_end_ns']==r['progress_ns']<=r['tool_end_ns']
                assert r['process_backend']==r['configured_backend']
            else:assert not r['progress_ns'] and not r['process_backend']
            parts(r)
        if r['kind']=='task_timing':
            assert math.isclose(r['task_ms'],r['task_wall_ms']-r['excluded_in_task_oracle_ms'],abs_tol=1e-8)
            g=[t for t in rows if t['kind']=='timing' and all(t[k]==r[k] for k in ['phase','scenario','words','mode','rep'])];assert len(g)==3
            for a,b in [('post_tool_sum_ms','post_tool_first_ms'),('early_hold_sum_ms','early_hold_ms'),('prepare_sum_ms','prepare_ms')]:assert math.isclose(r[a],sum(t[b] for t in g),abs_tol=1e-8)
    for scenario in ['read','compile_test']:
        for words in [320,1792]:
            for mode in MODES:
                backend='spawn' if mode=='resident' or '_spawn_' in mode else 'fork'
                base='cal_'+backend+('_resident' if mode=='resident' else '_restore')
                for step in range(3):
                    train=[r for r in rows if r['kind']=='timing' and r['phase']=='calibration' and r['scenario']==scenario and r['words']==words and r['mode']==base and r['step']==step];assert len(train)==3
                    tool=st.median(r['tool_ms'] for r in train);rate=st.median(r['prepare_ms']/r['prefix_tokens'] for r in train)
                    post=st.median((r['tool_end_ns']-r['spawn_end_ns'])/1e6 for r in train) if scenario=='compile_test' else 0
                    group=sorted([r for r in rows if r['kind']=='timing' and r['phase']=='evaluation' and r['scenario']==scenario and r['words']==words and r['mode']==mode and r['step']==step],key=lambda r:r['rep'])
                    for count,r in enumerate(group):
                        assert r['tool_observations_before']==3+count and r['prepare_observations_before']==(3 if mode=='resident' else 3+count)
                        assert r['progress_observations_before']==(3+count if scenario=='compile_test' else 0)
                        assert math.isclose(r['initial_tool_estimate_ms'],tool,abs_tol=2e-8)
                        assert math.isclose(r['predicted_prepare_ms'],rate*r['prefix_tokens'],abs_tol=2e-8)
                        assert math.isclose(r['predicted_post_spawn_ms'],post,abs_tol=2e-8)
                        effective=tool;delay=0
                        if mode.endswith('_timed'):delay=max(0,tool-rate*r['prefix_tokens']-5)
                        if mode.endswith('_progress') and scenario=='compile_test':
                            elapsed=(r['progress_ns']-r['tool_start_ns'])/1e6
                            effective=elapsed+post;delay=elapsed+max(0,post-rate*r['prefix_tokens']-5)
                            assert r['timer_deadline_ns']>=r['progress_ns']
                        if mode.endswith('_progress') and scenario=='read':assert r['prefix_start_ns']>=r['tool_end_ns']
                        assert math.isclose(r['predicted_tool_ms'],effective,abs_tol=2e-8)
                        assert math.isclose(r['planned_delay_ms'],delay,abs_tol=2e-6)
                        tool+=ALPHA*(r['tool_ms']-tool)
                        if mode!='resident':rate+=ALPHA*(r['prepare_ms']/r['prefix_tokens']-rate)
                        if scenario=='compile_test':post+=ALPHA*((r['tool_end_ns']-r['progress_ns'])/1e6-post)
    assert summaries(rows)==data['summary'] and stage_summary(rows)==data['stage_summary'] and pairs(rows)==data['paired_summary']


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938');ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--finalize-existing',action='store_true')
    args=ap.parse_args();args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT) or (args.out/'evidence.json').exists():ap.error('fresh project-local output required')
    if not args.finalize_existing:
        if (args.out/'manifest.json').exists():ap.error('fresh output required')
        subprocess.run([sys.executable,str(ROOT/'tools/build_agent_native.py'),'--sdk',str(args.sdk),'--runtime',str(args.runtime),'--model',str(args.model),'--out',str(args.out),'--bench-tool-balance','--repeats','7'],check=True)
    manifest=json.loads((args.out/'manifest.json').read_text());assert manifest['llama_commit']==COMMIT and not manifest['sanitizers']
    rows=[json.loads(s) for s in (args.out/'tool-prepare.jsonl').read_text().splitlines()]
    offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',(args.out/'tool-prepare-engine.log').read_text());assert len(offloads)==1 and all(a==b for a,b in offloads)
    sources={k:v for k,v in manifest['sources_sha256'].items() if not k.startswith('/')}
    for name in ['tests/agent_tool_prepare.h','tests/agent_kv_worker.h','tests/agent_basic.h','tools/build_agent_native.py','tools/bench_restore_balance.py','tools/bench_restore_timing.py','tools/bench_tool_prepare.py','tools/bench_roofline_estimator.py']:sources[name]=digest(ROOT/name)
    data=dict(date='2026-10-31',scope='Spawn/parent-progress ETA ablation on new real-tool fixtures, fixed EWMA and margin',hardware=hardware(),model=args.model.name,model_sha256=digest(args.model),llama_commit=COMMIT,
              sources_sha256=sources,runtime_libraries_sha256={n:digest(args.runtime/n) for n in ['libllama.dylib','libllama-common.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']},
              rows=rows,summary=summaries(rows),stage_summary=stage_summary(rows),paired_summary=pairs(rows),origin_directory=str(args.out.relative_to(ROOT)),
              raw_jsonl_sha256=digest(args.out/'tool-prepare.jsonl'),engine_log_sha256=digest(args.out/'tool-prepare-engine.log'),verified_full_gpu_offload=True,
              method=dict(modes=MODES,words=[320,1792],cpu_test_iterations=25000000,repeats=7,context_tokens=8192,batch_tokens=512,threads=4,alpha=ALPHA,margin_ms=5,
                          training='3 complete calibration tasks per backend/state/case; progress phase uses same labels; subsequent predictions use past own-mode labels and only current observed spawn-return event',
                          progression='epoch-bound parent event; ETA=actual spawn return + prior EWMA after-spawn duration; absolute dispatch=max(spawn return, ETA-prior preparation-5ms)',
                          controls='Same shell/cwd/env/stdio/process-group/command and corrected file; no extra side effects, no command rerun on spawn error',
                          costs='Save/evict/real tools/repair/final suffix included; initial history/load/calibration/oracle excluded and recorded',
                          order='Modes rotate by replicate, independent states/initial seed per mode; no evaluation tuning or best-run selection'),
              references=dict(spawn='https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/posix_spawn.2.html',header='https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/spawn/spawn.h'),
              limits=['One M5/Metal/model, warm OS cache; macOS26+ spawn validated only, older macOS/Linux keep fork and Windows/CUDA pending',
                      'Scripted actions isolate real tool execution/scheduling, not autonomous0.6B repair capability',
                      'PREFIX_READY early hold is not full KV occupancy integral or physical memory release',
                      'Progress reduces one timing uncertainty but does not guarantee simultaneous wait/hold improvement; negative pairs retained'])
    check(data);(args.out/'evidence.json').write_text(json.dumps(data,indent=2)+'\n')
    for r in data['summary']:
        if r['kind']=='task_timing':print(r['scenario'],r['words'],r['mode'],f"task={r['task_ms']['median']:.3f} wait={r['post_tool_sum_ms']['median']:.3f} hold={r['early_hold_sum_ms']['median']:.3f}",flush=True)
    print('Native records',len(rows),flush=True)


if __name__=='__main__':main()
