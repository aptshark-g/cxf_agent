#!/usr/bin/env python3
"""Actual workspace tools + Native owner overlap; no credentials/config/network."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics as st
import subprocess
import sys
from bench_roofline_estimator import hardware, digest, COMMIT

ROOT=Path(__file__).resolve().parents[1]
MODES=['resident','prefill_late','prefill_immediate','prefill_timed','restore_late','restore_immediate','restore_timed']
ALPHA=.09512492197250393
TIMING=['tool_ms','post_tool_ready_ms','post_tool_first_ms','total_first_ms','checkpoint_ms','save_ms','evict_ms','prepare_ms','restore_ms','restored_bytes','overlap_ms','early_hold_ms','planned_delay_ms','predicted_tool_ms','predicted_prepare_ms','prefix_tokens','committed_tokens','suffix_evaluated_tokens','wasted_tokens']
TASK=['task_ms','task_wall_ms','excluded_in_task_oracle_ms','initial_history_ms','inter_tool_ms','post_tool_sum_ms','tool_sum_ms','save_sum_ms','prepare_sum_ms','overlap_sum_ms','early_hold_sum_ms']


def summaries(rows):
    out=[]
    for scenario in ['read','compile_test']:
        for words in [128,2048]:
            for mode in MODES:
                for kind,steps,fields in [('task_timing',[None],TASK),('timing',range(3),TIMING)]:
                    for step in steps:
                        filters=dict(kind=kind,phase='evaluation',scenario=scenario,words=words,mode=mode)
                        if step is not None:filters['step']=step
                        group=[r for r in rows if all(r.get(k)==v for k,v in filters.items())]
                        assert len(group)==7
                        out.append(dict(filters,samples=len(group),**{f:dict(median=st.median(r[f] for r in group),min=min(r[f] for r in group),max=max(r[f] for r in group)) for f in fields}))
    return out


def paired(rows):
    out=[]
    for scenario in ['read','compile_test']:
        for words in [128,2048]:
            for mode in MODES[1:]:
                baselines=['resident']
                if not mode.endswith('_late'):baselines.insert(0,mode.split('_')[0]+'_late')
                selected=lambda m:{r['rep']:r for r in rows if r['kind']=='task_timing' and r['phase']=='evaluation' and r['scenario']==scenario and r['words']==words and r['mode']==m}
                for baseline in baselines:
                    a,b=selected(baseline),selected(mode);assert len(a)==len(b)==7
                    changes={f:[a[i][f]-b[i][f] for i in range(7)] for f in ['task_ms','post_tool_sum_ms','tool_sum_ms','early_hold_sum_ms']}
                    out.append(dict(scenario=scenario,words=words,baseline=baseline,mode=mode,pairs=7,
                                    **{f:dict(median_saving_ms=st.median(v),min_saving_ms=min(v),max_saving_ms=max(v),positive_pairs=sum(x>0 for x in v)) for f,v in changes.items()}))
    return out


def check(data):
    rows=data['rows']
    assert len(rows)==931
    assert len([r for r in rows if r['kind']=='timing' and r['phase']=='calibration'])==108
    assert len([r for r in rows if r['kind']=='timing' and r['phase']=='evaluation'])==588
    assert all(r['passed'] for r in rows if r['kind'] in ['correctness','closed'])
    for r in rows:
        if r['kind']=='task_timing':
            assert math.isclose(r['task_ms'],r['task_wall_ms']-r['excluded_in_task_oracle_ms'],abs_tol=1e-8)
            group=[t for t in rows if t['kind']=='timing' and all(t[k]==r[k] for k in ['phase','scenario','words','mode','rep'])]
            assert len(group)==3
            for out,source in [('post_tool_sum_ms','post_tool_first_ms'),('tool_sum_ms','tool_ms'),('save_sum_ms','save_ms'),('prepare_sum_ms','prepare_ms'),('overlap_sum_ms','overlap_ms'),('early_hold_sum_ms','early_hold_ms')]:
                assert math.isclose(r[out],sum(t[source] for t in group),abs_tol=1e-8)
        if r['kind']!='timing':continue
        assert r['correct'] and not r['wasted_tokens'] and r['rebuilds']==0
        assert r['entered_ns']<=r['tool_start_ns']<=r['tool_end_ns']<=r['request_ready_ns']<=r['first_token_ns']
        assert r['suffix_evaluated_tokens']==r['committed_tokens']-r['prefix_tokens']
        for field,a,b in [('tool_ms','tool_start_ns','tool_end_ns'),('post_tool_ready_ms','tool_end_ns','request_ready_ns'),('post_tool_first_ms','tool_end_ns','first_token_ns'),('total_first_ms','entered_ns','first_token_ns')]:
            assert math.isclose(r[field],(r[b]-r[a])/1e6,abs_tol=1e-8)
        if r['mode']=='resident':assert not r['prefix_start_ns'] and not r['save_ms'] and not r['restored_bytes']
        else:
            assert r['entered_ns']<=r['prefix_start_ns']<=r['prefix_ready_ns']<=r['request_ready_ns']
            if r['mode'].endswith('_late'):assert r['prefix_start_ns']>=r['tool_end_ns'] and not r['overlap_ms']
            if r['mode'].startswith('restore_'):assert r['restored_bytes']>0 and r['restored_tokens']==r['prefix_tokens']
            else:assert not r['restored_bytes']
            overlap=max(0,min(r['prefix_ready_ns'],r['tool_end_ns'])-max(r['prefix_start_ns'],r['tool_start_ns']))/1e6
            hold=max(0,r['tool_end_ns']-r['prefix_ready_ns'])/1e6
            assert math.isclose(overlap,r['overlap_ms'],abs_tol=1e-8) and math.isclose(hold,r['early_hold_ms'],abs_tol=1e-8)
    # Recompute every evaluation prediction using calibration labels + prior
    # actuals only; no same-label timing or retrospective scheduling.
    for scenario in ['read','compile_test']:
        for words in [128,2048]:
            for mode in MODES:
                base='resident' if mode=='resident' else mode.split('_')[0]+'_late'
                for step in range(3):
                    train=[r for r in rows if r['kind']=='timing' and r['phase']=='calibration' and r['scenario']==scenario and r['words']==words and r['mode']==base and r['step']==step]
                    assert len(train)==3
                    tool=st.median(r['tool_ms'] for r in train);rate=st.median(r['prepare_ms']/r['prefix_tokens'] for r in train)
                    group=sorted([r for r in rows if r['kind']=='timing' and r['phase']=='evaluation' and r['scenario']==scenario and r['words']==words and r['mode']==mode and r['step']==step],key=lambda r:r['rep'])
                    for i,r in enumerate(group):
                        assert r['estimate_key']==MODES.index(mode)*3+step+1 and r['tool_observations_before']==3+i
                        assert r['prepare_observations_before']==(3 if mode=='resident' else 3+i)
                        assert math.isclose(r['predicted_tool_ms'],tool,abs_tol=1e-8)
                        assert math.isclose(r['predicted_prepare_ms'],rate*r['prefix_tokens'],abs_tol=1e-8)
                        delay=max(0,tool-rate*r['prefix_tokens']-5) if mode.endswith('_timed') else 0
                        assert math.isclose(delay,r['planned_delay_ms'],abs_tol=1e-8)
                        tool+=ALPHA*(r['tool_ms']-tool)
                        if mode!='resident':rate+=ALPHA*(r['prepare_ms']/r['prefix_tokens']-rate)
    assert summaries(rows)==data['summary'] and paired(rows)==data['paired_summary']


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938');ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True);ap.add_argument('--finalize-existing',action='store_true')
    args=ap.parse_args();args.out=args.out.resolve()
    if not args.out.is_relative_to(ROOT):ap.error('project-local output required')
    if (args.out/'evidence.json').exists():ap.error('never overwrite evidence')
    if not args.finalize_existing:
        if (args.out/'manifest.json').exists():ap.error('fresh out required')
        cmd=[sys.executable,str(ROOT/'tools/build_agent_native.py'),'--sdk',str(args.sdk),'--runtime',str(args.runtime),'--model',str(args.model),'--out',str(args.out),'--bench-tool-prepare','--repeats','7']
        subprocess.run(cmd,check=True)
    rows=[json.loads(s) for s in (args.out/'tool-prepare.jsonl').read_text().splitlines()]
    manifest=json.loads((args.out/'manifest.json').read_text());assert manifest['llama_commit']==COMMIT and not manifest['sanitizers']
    log=(args.out/'tool-prepare-engine.log').read_text();offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',log)
    assert len(offloads)==1 and all(a==b and int(a)>0 for a,b in offloads)
    sources={k:v for k,v in manifest['sources_sha256'].items() if not k.startswith('/')}
    extras=['tests/agent_tool_prepare.h','tests/agent_native.h','tests/agent_kv_worker.h','tests/agent_llm.h','tests/agent_basic.h','tests/agent_fs.h','include/cxf/llama.h','src/llama_internal.h','tools/build_agent_native.py','tools/bench_roofline_estimator.py']
    for name in extras:sources[name]=digest(ROOT/name)
    sources[str(Path(__file__).relative_to(ROOT))]=digest(Path(__file__))
    profile=dict(hardware=hardware(),model=args.model.name,model_sha256=digest(args.model),llama_commit=COMMIT,
                 configuration=dict(context_tokens=8192,batch_tokens=512,cpu_threads=4,kv_type='f16',backend='Metal',disk_budget_bytes=1073741824,keys=32,alpha=ALPHA,margin_ms=5))
    data=dict(date='2026-10-31',scope='Real fs.read / test.run / file repair with optional Native tool-window preparation; deterministic action selection',
              **profile,profile_sha256=hashlib.sha256(json.dumps(profile,sort_keys=True).encode()).hexdigest(),
              runtime_libraries_sha256={n:digest(args.runtime/n) for n in ['libllama.dylib','libllama-common.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']},
              sources_sha256=sources,external_sources_sha256={str(Path(k).relative_to(args.sdk.resolve())):v for k,v in manifest['sources_sha256'].items() if k.startswith('/')},rows=rows,summary=summaries(rows),paired_summary=paired(rows),
              origin_directory=str(args.out.relative_to(ROOT)),raw_jsonl_sha256=digest(args.out/'tool-prepare.jsonl'),engine_log_sha256=digest(args.out/'tool-prepare-engine.log'),verified_full_gpu_offload=True,
              method=dict(repeats=7,calibration_tasks_per_operation_per_case=3,tools_per_task=3,words=[128,2048],scenarios=['read','compile_test'],modes=MODES,
                          calibration='Actual tools and actual preparation: each policy starts from the same per-operation median of3 calibration labels; then only its own past evaluation labels update EWMA',
                          order='Within each case:7 modes, order rotates by replicate; model already resident; initial history computed before task, excluded and recorded',
                          execution='Real capability functions, deployed C compiler + test process. compile_test observes FAIL, writes full correction, observes PASS twice; read invokes real workspace file reads',
                          oracle='12 independent full-upstream-render/tokenize/fresh-KV greedy references, one per case/step; every measurement checks exact IDs/greedy. Validation after first-token timestamp; first two validation costs subtracted from task wall',
                          timing='Tool entry includes checkpoint/save/eviction; after-tool includes final rendering/tokenization/suffix prefill/greedy. File repair and intermediate model preparation included in task time',
                          scope='Same host module is opt-in in agentctl-native test.run. Timing actions scripted, not autonomous-model success; normal resident KV baseline retained',
                          cache='Warm process/device/OS-file-cache. Same live session and revision only; saves are charged, physical KV buffer allocation unchanged'),
              limits=['One M5/Metal/Qwen0.6B backend; no cold SSD/Windows/CUDA/PCIe claims',
                      'Explicit recompute/restore policy creates nonresident KV, not an assumption about all ordinary local-agent calls',
                      'One tool + one preparation, no foreground multi-consumer preemption, cold-engine startup or live cloud source',
                      'Prefix bookkeeping borrows canonical tokens; complete rendering/guard and suffix work still happen after the actual receipt',
                      'Small controlled sample and CPU/compiler noise; all paired signs/ranges retained, no p95/p99 or Gaussian readiness guarantee'])
    check(data);(args.out/'evidence.json').write_text(json.dumps(data,indent=2)+'\n')
    for s in data['summary']:
        if s['kind']=='task_timing':print(s['scenario'],s['words'],s['mode'],f"task={s['task_ms']['median']:.3f} ms wait_sum={s['post_tool_sum_ms']['median']:.3f}",flush=True)
    print('Native records:',len(rows),flush=True)


if __name__=='__main__':main()
