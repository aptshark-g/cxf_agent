#!/usr/bin/env python3
"""Restore deadline attribution: real tool traces and past-label-only scheduling."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics as st
import subprocess
import sys
from bench_roofline_estimator import hardware,digest,COMMIT
from bench_tool_prepare import TIMING,TASK,ALPHA

ROOT=Path(__file__).resolve().parents[1]
MODES=['resident','restore_late','restore_immediate','restore_timed']


def common_parts(r):
    if r['mode']=='resident':return {}
    start=(r['prefix_start_ns']-r['tool_start_ns'])/1e6
    signed=(r['prefix_ready_ns']-r['tool_end_ns'])/1e6
    offset=start-r['planned_delay_ms']
    tool_error=r['predicted_tool_ms']-r['tool_ms']
    service_error=r['prepare_ms']-r['predicted_prepare_ms']
    clamp=r['planned_delay_ms']-(r['predicted_tool_ms']-r['predicted_prepare_ms']-5)
    assert math.isclose(signed,tool_error+service_error-5+offset+clamp,abs_tol=1e-8)
    wait=max(0,signed)
    return dict(prefix_wait_ms=wait,post_prefix_ms=r['post_tool_first_ms']-wait,signed_ready_error_ms=signed,
                tool_eta_over_ms=tool_error,service_under_ms=service_error,planned_start_offset_ms=offset,
                clamp_ms=clamp,outside_native_ms=r['prepare_ms']-r['restore_ms'])


def parts(r):
    result=common_parts(r)
    if r['scenario']=='compile_test':
        result.update(spawn_ms=(r['spawn_end_ns']-r['spawn_begin_ns'])/1e6,
                      after_spawn_ms=(r['tool_end_ns']-r['spawn_end_ns'])/1e6)
    if not r['mode'].startswith('restore_'):return result
    boundaries=['prefix_start_ns','kv_queued_ns','kv_started_ns','kv_restore_start_ns','kv_restore_end_ns','kv_ready_ns','kv_finish_owner_ns','kv_finished_ns','kv_finish_return_ns','prefix_ready_ns']
    assert all(r[a]<=r[b] for a,b in zip(boundaries,boundaries[1:])),boundaries
    names=['admission_ms','queue_ms','before_native_ms','native_ms','ready_tail_ms','finish_queue_ms','finish_owner_ms','finish_return_ms','coordinator_tail_ms']
    for name,a,b in zip(names,boundaries,boundaries[1:]):result[name]=(r[b]-r[a])/1e6
    assert math.isclose(sum(result[n] for n in names),r['prepare_ms'],abs_tol=1e-8)
    result.update(thread_start_offset_ms=(r['thread_enter_ns']-r['tool_start_ns'])/1e6,
                  deadline_anchor_offset_ms=(r['timer_deadline_ns']-r['tool_start_ns'])/1e6-r['planned_delay_ms'] if r['timer_deadline_ns'] else 0,
                  timer_wake_offset_ms=(r['timer_wake_ns']-r['timer_deadline_ns'])/1e6 if r['timer_deadline_ns'] else 0,
                  join_tail_ms=(r['join_return_ns']-max(r['tool_end_ns'],r['prefix_ready_ns']))/1e6,
                  drop_ms=(r['drop_end_ns']-r['drop_start_ns'])/1e6,
                  assembly_ms=(r['assembly_end_ns']-r['assembly_start_ns'])/1e6,
                  frontend_ms=sum(r[f] for f in ['final_render_ms','final_tokenize_ms','final_guard_ms','final_commit_ms']),
                  final_prefill_ms=r['final_prefill_ms'],greedy_ms=(r['first_token_ns']-r['request_ready_ns'])/1e6)
    if r['scenario']=='compile_test':
        result['spawn_prepare_overlap_ms']=max(0,min(r['prefix_ready_ns'],r['spawn_end_ns'])-max(r['prefix_start_ns'],r['spawn_begin_ns']))/1e6
    return result


def grouped_parts(rows,legacy=False):
    groups={}
    for i,r in enumerate(rows):
        if r['kind']!='timing' or r['phase']!='evaluation' or not r['mode'].startswith('restore_'):continue
        key=(r['scenario'],r['words'],r['mode']);value=common_parts(r) if legacy else parts(r)
        groups.setdefault(key,[]).append(value)
    return [dict(scenario=s,words=w,mode=m,samples=len(g),**{field:dict(mean=st.mean(x[field] for x in g),median=st.median(x[field] for x in g),min=min(x[field] for x in g),max=max(x[field] for x in g)) for field in g[0]}) for (s,w,m),g in sorted(groups.items())]


def summaries(rows):
    out=[]
    keys=sorted({(r['scenario'],r['words'],r['mode']) for r in rows if r.get('phase')=='evaluation'})
    for scenario,words,mode in keys:
        for kind,steps,fields in [('task_timing',[None],TASK),('timing',range(3),TIMING)]:
            for step in steps:
                filters=dict(kind=kind,phase='evaluation',scenario=scenario,words=words,mode=mode)
                if step is not None:filters['step']=step
                g=[r for r in rows if all(r.get(k)==v for k,v in filters.items())];assert len(g)==7
                out.append(dict(filters,samples=7,**{f:dict(median=st.median(r[f] for r in g),min=min(r[f] for r in g),max=max(r[f] for r in g)) for f in fields}))
    return out


def pairs(rows):
    out=[]
    for scenario,words in sorted({(r['scenario'],r['words']) for r in rows if r.get('phase')=='evaluation'}):
        select=lambda m:{r['rep']:r for r in rows if r['kind']=='task_timing' and r['phase']=='evaluation' and r['scenario']==scenario and r['words']==words and r['mode']==m}
        modes=sorted({r['mode'] for r in rows if r.get('phase')=='evaluation'})
        for mode in modes:
            if mode in ['resident','restore_late']:continue
            for baseline in ['restore_late','restore_immediate','restore_timed']:
                if mode==baseline:continue
                a,b=select(baseline),select(mode)
                out.append(dict(scenario=scenario,words=words,baseline=baseline,mode=mode,pairs=7,
                                **{field:dict(median_saving_ms=st.median(a[i][field]-b[i][field] for i in range(7)),
                                              min_saving_ms=min(a[i][field]-b[i][field] for i in range(7)),max_saving_ms=max(a[i][field]-b[i][field] for i in range(7)),positive_pairs=sum(a[i][field]>b[i][field] for i in range(7)))
                                   for field in ['task_ms','post_tool_sum_ms','early_hold_sum_ms']}))
    return out


def check(data):
    rows=data['rows'];assert len(rows)==547
    assert sum(r.get('kind')=='timing' and r['phase']=='evaluation' for r in rows)==336
    for r in rows:
        if r['kind']=='timing':
            assert r['correct'] and not r['wasted_tokens'] and r['rebuilds']==0
            assert r['entered_ns']<=r['tool_start_ns']<=r['tool_end_ns']<=r['request_ready_ns']<=r['first_token_ns']
            assert r['suffix_evaluated_tokens']==r['committed_tokens']-r['prefix_tokens']
            if r['scenario']=='compile_test':
                assert r['tool_start_ns']<=r['tool_trace_enter_ns']<=r['spawn_begin_ns']<=r['spawn_end_ns']<=r['tool_trace_finished_ns']<=r['tool_end_ns']
            if r['mode']!='resident':
                assert r['restored_tokens']==r['prefix_tokens'] and r['restored_bytes']>0
                parts(r)
        if r['kind']=='task_timing':
            assert math.isclose(r['task_ms'],r['task_wall_ms']-r['excluded_in_task_oracle_ms'],abs_tol=1e-8)
            g=[t for t in rows if t['kind']=='timing' and all(t[k]==r[k] for k in ['phase','scenario','words','mode','rep'])];assert len(g)==3
            for a,b in [('post_tool_sum_ms','post_tool_first_ms'),('prepare_sum_ms','prepare_ms'),('early_hold_sum_ms','early_hold_ms')]:assert math.isclose(r[a],sum(t[b] for t in g),abs_tol=1e-8)
    for scenario,words,mode,step in sorted({(r['scenario'],r['words'],r['mode'],r['step']) for r in rows if r['kind']=='timing' and r['phase']=='evaluation'}):
        base='resident' if mode=='resident' else 'restore_late'
        train=[r for r in rows if r['kind']=='timing' and r['phase']=='calibration' and r['scenario']==scenario and r['words']==words and r['mode']==base and r['step']==step];assert len(train)==3
        tool=st.median(r['tool_ms'] for r in train);rate=st.median(r['prepare_ms']/r['prefix_tokens'] for r in train)
        g=sorted([r for r in rows if r['kind']=='timing' and r['phase']=='evaluation' and r['scenario']==scenario and r['words']==words and r['mode']==mode and r['step']==step],key=lambda r:r['rep'])
        for i,r in enumerate(g):
            assert r['tool_observations_before']==3+i and r['prepare_observations_before']==(3 if mode=='resident' else 3+i)
            assert math.isclose(r['predicted_tool_ms'],tool,abs_tol=1e-8) and math.isclose(r['predicted_prepare_ms'],rate*r['prefix_tokens'],abs_tol=1e-8)
            delay=max(0,tool-rate*r['prefix_tokens']-5) if mode=='restore_timed' else 0
            assert math.isclose(r['planned_delay_ms'],delay,abs_tol=1e-8)
            tool+=ALPHA*(r['tool_ms']-tool)
            if mode!='resident':rate+=ALPHA*(r['prepare_ms']/r['prefix_tokens']-rate)
    assert summaries(rows)==data['summary'] and grouped_parts(rows)==data['stage_summary'] and pairs(rows)==data['paired_summary']
    e21=ROOT/'docs/evidence/tool-prepare-20261031.json'
    assert digest(e21)==data['e21_audit']['source_sha256']
    assert grouped_parts(json.loads(e21.read_text())['rows'],True)==data['e21_audit']['grouped']


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938');ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--finalize-existing',action='store_true')
    args=ap.parse_args();args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT) or (args.out/'evidence.json').exists():ap.error('fresh project-local output required')
    if not args.finalize_existing:
        if (args.out/'manifest.json').exists():ap.error('fresh out required')
        subprocess.run([sys.executable,str(ROOT/'tools/build_agent_native.py'),'--sdk',str(args.sdk),'--runtime',str(args.runtime),'--model',str(args.model),'--out',str(args.out),'--bench-tool-timing','--repeats','7'],check=True)
    rows=[json.loads(s) for s in (args.out/'tool-prepare.jsonl').read_text().splitlines()]
    manifest=json.loads((args.out/'manifest.json').read_text());assert manifest['llama_commit']==COMMIT and not manifest['sanitizers']
    offload=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',(args.out/'tool-prepare-engine.log').read_text());assert len(offload)==1 and all(a==b for a,b in offload)
    sources={k:v for k,v in manifest['sources_sha256'].items() if not k.startswith('/')}
    for name in ['tests/agent_tool_prepare.h','tests/agent_kv_worker.h','tests/agent_basic.h','tools/build_agent_native.py','tools/bench_restore_timing.py','tools/bench_tool_prepare.py','tools/bench_roofline_estimator.py']:sources[name]=digest(ROOT/name)
    e21=ROOT/'docs/evidence/tool-prepare-20261031.json'
    data=dict(date='2026-10-31',scope='Instrumentation-only restore timing diagnosis, old scheduling retained, real tools',hardware=hardware(),model=args.model.name,model_sha256=digest(args.model),llama_commit=COMMIT,
              sources_sha256=sources,runtime_libraries_sha256={n:digest(args.runtime/n) for n in ['libllama.dylib','libllama-common.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']},
              rows=rows,summary=summaries(rows),stage_summary=grouped_parts(rows),paired_summary=pairs(rows),
              e21_audit=dict(source='docs/evidence/tool-prepare-20261031.json',source_sha256=digest(e21),grouped=grouped_parts(json.loads(e21.read_text())['rows'],True)),
              origin_directory=str(args.out.relative_to(ROOT)),raw_jsonl_sha256=digest(args.out/'tool-prepare.jsonl'),engine_log_sha256=digest(args.out/'tool-prepare-engine.log'),verified_full_gpu_offload=True,
              method=dict(modes=MODES,repeats=7,words=[128,2048],context_tokens=8192,batch_tokens=512,threads=4,clock='Darwin libc++ steady_clock and CLOCK_MONOTONIC_RAW; other platforms require separate validation',
                          calibration='Same3 per-operation actual calibration tasks and past-label-only EWMA as E21; rotation by replicate',
                          instrumentation='Parent fork/first output/reap, preparer thread/timer, worker queue/restore/finish/return, cleanup and final frontend/prefill; no policy change',
                          attribution='Adjacent ordered timestamps partition preparation exactly; overlap is descriptive, no OS-internal causal attribution',
                          costs='Initial history/load/calibration/oracles excluded and recorded; save/evict/real tools/repair/final suffix included'),
              limits=['Single M5/Metal/model and warm OS cache; no bandwidth/frequency/cold-SSD/Windows/CUDA inference',
                      'Stage wall time locates delay but cannot prove why a thread was paused',
                      'Three measured calls plus one included real file repair in compile tasks; scripted actions, not autonomous-model success'])
    check(data);(args.out/'evidence.json').write_text(json.dumps(data,indent=2)+'\n')
    for r in data['stage_summary']:
        if r['scenario']=='compile_test':print(r['words'],r['mode'],{k:round(r[k]['mean'],3) for k in ['prefix_wait_ms','post_prefix_ms','spawn_ms','deadline_anchor_offset_ms','queue_ms','before_native_ms','native_ms','finish_queue_ms','finish_return_ms']},flush=True)
    print('Native records',len(rows),flush=True)


if __name__=='__main__':main()
