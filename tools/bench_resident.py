#!/usr/bin/env python3
"""Resident Native multi-tool flow and actual-process semantic streaming, independent of build optimization."""
import argparse,json,math,re,statistics as st,subprocess,sys,zipfile
from pathlib import Path
from bench_roofline_estimator import ROOT,hardware,digest,COMMIT

FLOW_TASK=['task_ms','tool_sum_ms','post_tool_sum_ms','prefill_sum_ms','evaluated_tokens']
FLOW_CALL=['tool_ms','post_tool_first_ms','call_ms','wrapper_ms','prefill_ms','render_ms','tokenize_ms','guard_ms','evaluated_tokens','tokenized_tokens','reused_prefix_tokens','committed_tokens']
STREAM=['task_complete_ms','task_first_ms','task_ready_ms','post_input_first_ms','post_input_ready_ms','input_ms','producer_ms','producer_cpu_ms','prefill_ms','render_ms','tokenize_ms','input_evaluated_tokens','input_decode_calls','tokenized_tokens','tokenizer_calls','pending_peak','backpressure']


def group_keys(kind):return ['words','mode'] if kind=='flow' else ['words','producer_iterations','mode']
def summary(kind,rows):
    result=[]
    for row_kind,fields in ([('task_timing',FLOW_TASK),('timing',FLOW_CALL)] if kind=='flow' else [('timing',STREAM)]):
        keys=group_keys(kind)+(['step'] if kind=='flow' and row_kind=='timing' else [])
        groups={}
        for r in rows:
            if r['kind']==row_kind:groups.setdefault(tuple(r[k] for k in keys),[]).append(r)
        for key,g in sorted(groups.items()):
            result.append(dict(kind=row_kind,scenario=g[0]['scenario'],**dict(zip(keys,key)),samples=len(g),**{f:dict(mean=st.mean(r[f] for r in g),median=st.median(r[f] for r in g),min=min(r[f] for r in g),max=max(r[f] for r in g)) for f in fields}))
    return result


def task_rows(kind,rows):
    if kind=='stream':return [dict(record_index=i,**r) for i,r in enumerate(rows) if r['kind']=='timing']
    out=[]
    for i,t in enumerate(rows):
        if t['kind']!='task_timing':continue
        g=[(n,r) for n,r in enumerate(rows) if r['kind']=='timing' and all(r[k]==t[k] for k in ['words','mode','rep'])];assert len(g)==6
        other=t['task_ms']-t['tool_sum_ms']-t['post_tool_sum_ms']
        out.append(dict(t,record_index=i,call_indices=','.join(str(n) for n,r in g),task_other_ms=other,full_prompt_tokens_sum=sum(r['committed_tokens'] for n,r in g)))
    return out


def pair_rows(kind,rows):
    out=[];tasks=task_rows(kind,rows);cases=[(w,) for w in [512,3072]] if kind=='flow' else [(w,i) for w in [80,240] for i in [0,40000000]]
    comparisons=[('resident_direct','resident_wrapper')] if kind=='flow' else [('wait_all','stream_coalesced'),('wait_all','stream_eager'),('stream_coalesced','stream_eager')]
    fields=FLOW_TASK+['task_other_ms'] if kind=='flow' else STREAM
    for case in cases:
        for baseline,mode in comparisons:
            select=lambda m:{r['rep']:r for r in tasks if r['mode']==m and r['words']==case[0] and (kind=='flow' or r['producer_iterations']==case[1])};a,b=select(baseline),select(mode)
            assert len(a)==len(b)==7
            for rep in a:
                out.append(dict(words=case[0],**({} if kind=='flow' else {'producer_iterations':case[1]}),baseline=baseline,mode=mode,rep=rep,baseline_record_index=a[rep]['record_index'],candidate_record_index=b[rep]['record_index'],**{f:a[rep][f]-b[rep][f] for f in fields}))
    return out


def paired_summary(kind,rows):
    groups={}
    for r in pair_rows(kind,rows):groups.setdefault(tuple(r[k] for k in (['words','baseline','mode'] if kind=='flow' else ['words','producer_iterations','baseline','mode'])),[]).append(r)
    keys=['words','baseline','mode'] if kind=='flow' else ['words','producer_iterations','baseline','mode'];out=[]
    for key,g in sorted(groups.items()):
        fields=[f for f in g[0] if f not in keys+['rep','baseline_record_index','candidate_record_index']]
        out.append(dict(zip(keys,key),pairs=len(g),**{f:dict(median_saving=st.median(r[f] for r in g),min_saving=min(r[f] for r in g),max_saving=max(r[f] for r in g),positive_pairs=sum(r[f]>0 for r in g)) for f in fields}))
    return out


def check(data):
    kind=data['experiment_kind'];rows=data['rows'];assert len(rows)==(199 if kind=='flow' else 87)
    assert any(r['kind']=='closed' and r['passed'] and not r['private_kv_files'] for r in rows)
    identities={}
    for r in rows:
        if r['kind']=='timing':
            assert r['correct']
            key=(r['words'],r['step']) if kind=='flow' else (r['words'],r['producer_iterations'])
            ident=(r['canonical_sha256'],r['greedy_token'],r['receipt_sha256'] if kind=='flow' else r['content_sha256']);assert identities.setdefault(key,ident)==ident
            if kind=='flow':
                assert r['entered_ns']<=r['tool_start_ns']<=r['tool_end_ns']<=r['wrapper_return_ns']<=r['ready_ns']<=r['first_token_ns']
                assert r['evaluated_before']==r['committed_before'] and r['retained_during_tool']>=r['checkpoint_before']>0
                assert r['reused_prefix_tokens']==r['checkpoint_before'] and r['evaluated_tokens']==r['committed_tokens']-r['checkpoint_before']
                assert all(r[f]==0 for f in ['save_ms','evict_ms','restore_ms','speculative_prepare_ms'])
                assert math.isclose(r['call_ms'],(r['first_token_ns']-r['entered_ns'])/1e6,abs_tol=1e-8)
            else:
                assert r['source_start_ns']<=r['spawn_return_ns']<=r['producer_reaped_ns']<=r['model_ready_ns']<=r['first_token_ns']<=r['returned_ns']<=r['closed_ns']
                assert r['source_start_ns']<=r['input_end_ns']<=r['model_ready_ns']
                assert r['input_evaluated_tokens']==r['committed_tokens']-r['initial_tokens']==r['tokenized_tokens']
                assert r['evaluated_frontier']==r['committed_tokens'] and not r['snapshot_operations'] and not r['kv_files']
                assert len(r['arrivals'])==13 and [a['seq'] for a in r['arrivals']]==list(range(1,14))
                assert r['arrivals'][-1]['kind']=='end'
                for a in r['arrivals']:assert r['source_start_ns']<=a['produced_ns']<=a['received_ns']<=r['input_end_ns']
                assert math.isclose(r['task_first_ms'],r['input_ms']+r['post_input_first_ms'],abs_tol=1e-8)
                assert math.isclose(r['task_complete_ms'],r['task_complete_wall_ms']-r['oracle_ms'],abs_tol=1e-8) and r['task_complete_ms']>=r['task_first_ms']
        if r['kind']=='task_timing':
            assert not r['kv_files'] and r['calls']==6 and math.isclose(r['task_ms'],r['task_wall_ms']-r['excluded_oracle_ms'],abs_tol=1e-8)
            g=[t for t in rows if t['kind']=='timing' and all(t[k]==r[k] for k in ['words','mode','rep'])]
            for a,b in [('tool_sum_ms','tool_ms'),('post_tool_sum_ms','post_tool_first_ms'),('prefill_sum_ms','prefill_ms'),('evaluated_tokens','evaluated_tokens')]:assert math.isclose(r[a],sum(t[b] for t in g),abs_tol=1e-8)
    assert data['summary']==summary(kind,rows) and data['paired_summary']==paired_summary(kind,rows)
    assert data['tasks']==task_rows(kind,rows) and data['pair_rows']==pair_rows(kind,rows)
    assert all(r['samples']==7 for r in data['summary'])


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kind',choices=['flow','stream'],required=True);ap.add_argument('--sdk',type=Path,default=ROOT/'build/llama.cpp-b10938');ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--finalize-existing',action='store_true')
    args=ap.parse_args();args.out=args.out.resolve();label='resident-'+args.kind
    if not args.out.is_relative_to(ROOT) or (args.out/'evidence.json').exists():ap.error('fresh local output required')
    if not args.finalize_existing:subprocess.run([sys.executable,str(ROOT/'tools/build_agent_native.py'),'--sdk',str(args.sdk),'--runtime',str(args.runtime),'--model',str(args.model),'--out',str(args.out),'--bench-'+label,'--repeats','7'],check=True)
    m=json.loads((args.out/'manifest.json').read_text());assert not m['sanitizers'] and m['llama_commit']==COMMIT
    rows=[json.loads(s) for s in (args.out/(label+'.jsonl')).read_text().splitlines()];sources={k:v for k,v in m['sources_sha256'].items() if not k.startswith('/')}
    for n in ['tests/agent_native.h','tests/agent_stream.h','tests/agent_tool_prepare.h','tests/agent_kv_worker.h','tests/agent_resource.h','tests/agent_basic.h','tools/build_agent_native.py','tools/bench_resident.py','tools/bench_roofline_estimator.py']:sources[n]=digest(ROOT/n)
    assert all(digest(ROOT/n)==v for n,v in sources.items())
    offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',(args.out/(label+'-engine.log')).read_text());assert len(offloads)==1 and all(a==b for a,b in offloads)
    data=dict(date='2026-10-31',experiment_kind=args.kind,scope='Normal resident KV '+('six-turn Native actual-tool loop' if args.kind=='flow' else 'real-process semantic-unit streaming; no sleep gaps'),hardware=hardware(),model=args.model.name,model_sha256=digest(args.model),llama_commit=COMMIT,sources_sha256=sources,runtime_libraries_sha256={n:digest(args.runtime/n) for n in ['libllama.dylib','libllama-common.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']},rows=rows,summary=summary(args.kind,rows),paired_summary=paired_summary(args.kind,rows),tasks=task_rows(args.kind,rows),pair_rows=pair_rows(args.kind,rows),origin_directory=str(args.out.relative_to(ROOT)),raw_jsonl_sha256=digest(args.out/(label+'.jsonl')),engine_log_sha256=digest(args.out/(label+'-engine.log')),verified_full_gpu_offload=True,
       method=dict(repeats=7,context_tokens=8192,batch_tokens=512,prefill_min_tokens=128,threads=4,residency='No save, forced eviction or restore; preserve evaluated prefix through active task; normal session close between independent tasks',build='Identical prebuilt helper/command; no compilation or incremental-build optimization during task; first-exec/warmup recorded separately',order='Rotate modes by replicate; no best run selection, no evaluation tuning',
        flow='512/3072-word history; six actual read/test-fail/write/test-pass/read/test-pass calls; direct versus resident coordinator; Native prepare+first greedy after each; scripted actions',
        stream='1024-word initial context; actual process reads12 immutable units at80/240 words, with0/40M real compute iterations per unit; wait-all/coalesced/eager; no artificial sleep',
        metrics='Task wall and post-input/post-tool waiting separated from cumulative prefill/decode, producer CPU and actual arrival times; common work and interference charged',
        oracle='Flow12 fresh references, per-call oracle measured after first token and first5 excluded from raw six-call wall; stream4 fresh references after normal stream close, first token sampled before all oracle work'),
       limits=['One M5/Metal/Qwen0.6B; scripted actions test actual execution and resident compatibility, not model autonomy','Initial history/model load and prebuilt helper first-execution warmup excluded and recorded; lazily compiled GPU variants may remain in first measured tasks, all repeats retained','Native stable history has no additional prefill work while tools run without new data; coordinator overhead is measured rather than invented speedup','Streaming uses complete immutable independent message units from a real local process, not arbitrary cloud token chunks or vendor SSE','Stream tools remain empty, worker exclusive; no automatic agentctl stream scheduling or combined Native/Stream owner overlay','No build-command optimization added to these timing comparisons; no physical KV shrinking or Windows/CUDA acceptance'])
    check(data)
    with zipfile.ZipFile(args.out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
        for n in sorted(sources):z.write(ROOT/n,n)
        z.writestr('sources-sha256.json',json.dumps(sources,indent=2)+'\n')
    (args.out/'evidence.json').write_text(json.dumps(data,indent=2)+'\n')
    for r in data['paired_summary']:print(r,flush=True)
    print('Native records',len(rows),flush=True)


if __name__=='__main__':main()
