#!/usr/bin/env python3
"""Real agentctl/test.run resident output off/on. Fixed actions, identical work."""
import argparse,json,os,queue,re,shutil,statistics as st,subprocess,sys,threading,zipfile
from pathlib import Path
from bench_roofline_estimator import ROOT,COMMIT,digest,hardware
TASK_FIELDS=['task_ms','task_wall_ms','excluded_oracle_ms','tool_sum_ms','post_tool_sum_ms','stream_prefill_ms','final_prefill_ms','prefill_sum_ms','stream_front_ms','stream_tokenized_tokens','evaluated_tokens','adopted_tokens','discarded_tokens','join_ms','wrapper_ms']
def tasks(rows):return [dict(record_index=i,**r) for i,r in enumerate(rows) if r['kind']=='task_timing' and r['phase']=='evaluation']
def summary(rows):
    groups={}
    for r in tasks(rows):groups.setdefault((r['words'],r['producer_iterations'],r['mode']),[]).append(r)
    return [dict(kind='task_timing',phase='evaluation',words=k[0],producer_iterations=k[1],mode=k[2],samples=len(g),**{f:dict(mean=st.mean(r[f] for r in g),median=st.median(r[f] for r in g),min=min(r[f] for r in g),max=max(r[f] for r in g)) for f in TASK_FIELDS}) for k,g in sorted(groups.items())]
def baseline(g):return next(m for m in ['off','off-race','off-split-bare','off-coalesce-bare'] if m in g)
def pair_rows(rows):
    groups={}
    for r in tasks(rows):groups.setdefault((r['words'],r['producer_iterations'],r['rep']),{})[r['mode']]=r
    return [dict(words=k[0],producer_iterations=k[1],rep=k[2],baseline=baseline(g),mode=m,baseline_record_index=g[baseline(g)]['record_index'],candidate_record_index=g[m]['record_index'],**{f:g[baseline(g)][f]-g[m][f] for f in TASK_FIELDS}) for k,g in sorted(groups.items()) for m in sorted(g) if m!=(baseline(g))]

def paired_summary(rows):
    groups={}
    for r in pair_rows(rows):groups.setdefault((r['words'],r['producer_iterations'],r['mode']),[]).append(r)
    return [dict(words=k[0],producer_iterations=k[1],**({'mode':k[2]} if k[2]!='on' else {}),pairs=len(g),**{f:dict(median_saving=st.median(r[f] for r in g),min_saving=min(r[f] for r in g),max_saving=max(r[f] for r in g),positive_pairs=sum(r[f]>0 for r in g)) for f in TASK_FIELDS}) for k,g in sorted(groups.items())]
def check(d):
    rows=d['rows'];repeats=d['method']['repeats'];expected=3+4*len(d['method'].get('modes',['off','on']))*(repeats+1)*8+(12*(repeats+1) if d['method'].get('producer_alone') else 0);assert len(rows)==expected,(len(rows),expected)
    identities={};alltasks=[r for r in rows if r['kind']=='task_timing'];assert len(alltasks)==4*len(d['method'].get('modes',['off','on']))*(repeats+1)
    for r in rows:
        if r['kind']=='step':
            key=(r['words'],r['producer_iterations'],r['step']);ident=(r['canonical_sha256'],r['transcript_sha256'],r['greedy_token']);assert identities.setdefault(key,ident)==ident
        elif r['kind']=='tool':
            assert not r['cancelled'] and r['tool_rc']==0 and r['stream_rc']==0
            assert r['entered_ns']<=r['tool_start_ns']<=r['tool_end_ns']<=r['joined_ns']<=r['returned_ns']
            if r['mode'].removesuffix('-bare').removesuffix('-notify') in ('off','off-event','off-race','off-drain','off-split','off-coalesce'):assert not any(r[x] for x in ['updates','evaluated_tokens','tokenized_tokens'])
        elif r['kind']=='task_timing':
            assert r['correct'] and r['calls']==4 and r['executed']==7 and r['invocations']==3 and r['idle'] and r['failed']==r['capped']==0
            assert abs(r['task_ms']-(r['task_wall_ms']-r['excluded_oracle_ms']))<2e-6
    assert d['summary']==summary(rows) and d['paired_summary']==paired_summary(rows) and d['pair_rows']==pair_rows(rows)
    assert rows[-1]['passed'] and rows[-1]['private_kv_files']==0

def run(args):
    out=args.out;fixture=out/'workspace';fixture.mkdir();shutil.copy2(out/'tool-output-producer',fixture/'tool-output-producer');(fixture/'case.txt').write_text('32 0\n')
    warm=[]
    for _ in range(2):
        import time
        t=time.monotonic_ns();subprocess.run(['./tool-output-producer'],cwd=fixture,stdout=subprocess.DEVNULL,check=True);warm.append((time.monotonic_ns()-t)/1e6)
    env={k:v for k,v in os.environ.items() if not k.startswith('CXF_LLM_')};env['CXF_LLM_SYSTEM']='Use the configured workspace tools. Run tests and verify receipts.'
    if args.metal_probe:env['CXF_METAL_PROBE_OUT']=str(out/'metal-raw.jsonl')
    command=[str(out/'test-tool-output-ctl'),'--llama-model',str(args.model),'--llama-backend',str(args.runtime),'--llama-context','8192','--llama-batch',str(args.batch_tokens),'--llama-max-tokens','64','--llama-threads','4','--root',str(fixture),'--test-command','./tool-output-producer','--test-timeout-ms','10000','--budget','65536','--rounds','8','--tool-output-prefill','off','--repl']
    # Native explicitly skips all HTTP configs; no --config and isolated cwd.
    (out/'command.json').write_text(json.dumps(command,indent=2)+'\n');q=queue.Queue();log=(out/'engine.log').open('w');stdout=(out/'stdout.txt').open('w');p=subprocess.Popen(command,cwd=fixture,env=env,stdin=subprocess.PIPE,stdout=stdout,stderr=subprocess.PIPE,text=True,bufsize=1)
    def reader():
        for line in p.stderr:
            log.write(line);log.flush()
            for prefix in ['[tool-output] ','[output-e2e] ','[metal-probe] ']:
                if line.startswith(prefix):q.put(json.loads(line[len(prefix):]));break
        q.put({'kind':'process_end','rc':p.wait()})
    reader_thread=threading.Thread(target=reader);reader_thread.start()
    def receive():
        r=q.get(timeout=120)
        if r['kind']=='process_end':raise RuntimeError('CLI ended; inspect engine.log: '+str(r))
        return r
    rows=[dict(kind='metadata',repeats=args.repeats,context_tokens=8192,batch_tokens=args.batch_tokens,prefill_min_tokens=128,threads=4,snapshot_operations=0,build_during_task=False,fixture_first_exec_ms=warm[0],fixture_warm_exec_ms=warm[1],actions='scripted Native-parser choices; real agentctl/tool execution')]
    assert receive()['kind']=='ready'
    try:
        for words in args.words:
            for iterations in [0,args.iterations]:
                for rep in range(-1,args.repeats):
                    if args.producer_alone:
                        (fixture/'case.txt').write_text(f'{words} {iterations}\n')
                        for control in range(3):
                            subprocess.run(['./tool-output-producer'],cwd=fixture,stdout=subprocess.DEVNULL,check=True)
                            child=json.loads((fixture/'producer-metrics.jsonl').read_text().splitlines()[-1])
                            row=dict(kind='producer_control',words=words,producer_iterations=iterations,rep=rep,control=control,phase='warmup' if rep<0 else 'evaluation',mode='producer-alone',producer=child);rows.append(row)
                            with (out/'raw.jsonl').open('a') as f:f.write(json.dumps(row)+'\n')
                    for i in range(len(args.modes)):
                        mode=args.modes[(rep+1+i)%len(args.modes)];(fixture/'case.txt').write_text(f'{words} {iterations}\n');before=(fixture/'invocations').stat().st_size
                        p.stdin.write('/tool-output-policy '+mode+'\n/tool-output-prefill '+('off' if mode.removesuffix('-bare').removesuffix('-notify') in ('off','off-event','off-race','off-drain','off-split','off-coalesce') else 'on')+'\n'+'Run the configured tests three times. '+'ctx '*256+'\n');p.stdin.flush();trial=[];pending_metal={}
                        while True:
                            r=receive()
                            if r['kind']=='metal':pending_metal[r['scope_kind']]=r;continue
                            if r['kind'] in ('step','tool') and (1 if r['kind']=='step' else 2) in pending_metal:r['metal']=pending_metal.pop(1 if r['kind']=='step' else 2)
                            r.update(scenario='agentctl_tool_output',words=words,producer_iterations=iterations,rep=rep,phase='warmup' if rep<0 else 'evaluation',mode=mode);trial.append(r)
                            if r['kind']=='task_timing':break
                        steps=[r for r in trial if r['kind']=='step'];tools=[r for r in trial if r['kind']=='tool'];assert len(steps)==4 and len(tools)==3
                        producer=[json.loads(x) for x in (fixture/'producer-metrics.jsonl').read_text().splitlines()][-3:] if (fixture/'producer-metrics.jsonl').exists() else []
                        for tool,child in zip(tools,producer):tool['producer']=child
                        task=trial[-1];task['invocations']=(fixture/'invocations').stat().st_size-before
                        task.update(tool_sum_ms=sum((r['tool_end_ns']-r['tool_start_ns'])/1e6 for r in tools),post_tool_sum_ms=sum((steps[j+1]['first_token_ns']-r['tool_end_ns'])/1e6 for j,r in enumerate(tools)),stream_prefill_ms=sum(r['prefill_ms'] for r in tools),final_prefill_ms=sum(r['prefill_ms'] for r in steps),stream_front_ms=sum(r['front_ms'] for r in tools),stream_tokenized_tokens=sum(r['tokenized_tokens'] for r in tools),evaluated_tokens=sum(r['evaluated_tokens'] for r in tools+steps),adopted_tokens=sum(r['adopted_tokens'] for r in steps),discarded_tokens=sum(r['discarded_tokens'] for r in steps),join_ms=sum((r['joined_ns']-r['tool_end_ns'])/1e6 for r in tools),wrapper_ms=sum((r['returned_ns']-r['entered_ns']-r['tool_end_ns']+r['tool_start_ns'])/1e6 for r in tools))
                        task['prefill_sum_ms']=task['stream_prefill_ms']+task['final_prefill_ms'];rows+=trial
                        with (out/'raw.jsonl').open('a') as f:
                            for r in trial:f.write(json.dumps(r)+'\n')
                        print(f"{words}/{iterations} {mode} rep{rep}: task={task['task_ms']:.3f}ms wait={task['post_tool_sum_ms']:.3f}ms adopt={task['adopted_tokens']}",flush=True)
        p.stdin.write('/quit\n');p.stdin.flush();p.stdin.close();assert p.wait(timeout=30)==0;reader_thread.join();log.close();stdout.close()
    finally:
        if p.poll() is None:p.kill();p.wait()
    assert not list(out.rglob('*.kv'))
    rows += [dict(kind='correctness',passed=True,final_tokens_and_transcripts_equal=True,tool_invocations=len([r for r in rows if r['kind']=='tool']),same_greedy=True),dict(kind='closed',passed=True,private_kv_files=0)]
    (out/'raw.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows));return rows

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--repeats',type=int,default=7);ap.add_argument('--finalize-existing',action='store_true');ap.add_argument('--correctness-only',action='store_true');ap.add_argument('--words',type=int,nargs=2,default=[32,56]);ap.add_argument('--iterations',type=int,default=40000000);ap.add_argument('--batch-tokens',type=int,default=512);ap.add_argument('--metal-probe',action='store_true');ap.add_argument('--producer-alone',action='store_true');ap.add_argument('--modes',nargs='+',default=['off','on']);args=ap.parse_args();args.out=args.out.resolve();assert args.out.is_relative_to(ROOT) and not (args.out/'evidence.json').exists();assert 1<=args.repeats<=7
    manifest=json.loads((args.out/'manifest.json').read_text());assert manifest['llama_commit']==COMMIT
    if any(m.endswith(('-bare','-notify')) for m in args.modes):assert args.metal_probe
    if args.metal_probe:assert 'tests/agent_metal_probe.mm' in manifest['sources_sha256']
    assert not manifest['sanitizers'] or args.correctness_only
    rows=[json.loads(s) for s in (args.out/'raw.jsonl').read_text().splitlines()] if args.finalize_existing else run(args)
    sources={k:v for k,v in manifest['sources_sha256'].items() if not k.startswith('/')}
    for n in ['tests/agentctl.c','tests/agent_native.h','tests/agent_basic.h','tests/agent_native_messages.h','tests/agent_tool_prepare.h','tests/agent_kv_worker.h','src/llama_internal.h','include/cxf/llama.h','tools/build_agent_native.py','tools/bench_tool_output.py','tools/bench_roofline_estimator.py','tests/agent_metal_probe.h']:sources[n]=digest(ROOT/n)
    assert all(digest(ROOT/n)==v for n,v in sources.items())
    offloads=re.findall(r'offloaded (\d+)/(\d+) layers to GPU',(args.out/'engine.log').read_text());assert len(offloads)==1 and offloads[0][0]==offloads[0][1]
    d=dict(date='2026-10-31',scope='Actual agentctl Native/registry/AgentStore/test.run resident output off/on; scripted model choices',hardware=hardware(),model=args.model.name,model_sha256=digest(args.model),llama_commit=COMMIT,sources_sha256=sources,runtime_libraries_sha256={n:digest(args.runtime/n) for n in ['libllama.dylib','libllama-common.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']},rows=rows,summary=summary(rows),paired_summary=paired_summary(rows),pair_rows=pair_rows(rows),origin_directory=str(args.out.relative_to(ROOT)),raw_jsonl_sha256=digest(args.out/'raw.jsonl'),engine_log_sha256=digest(args.out/'engine.log'),verified_full_gpu_offload=True,method=dict(batch_tokens=args.batch_tokens,metal_probe=args.metal_probe,producer_alone=args.producer_alone,modes=args.modes,repeats=args.repeats,sanitizers=manifest['sanitizers'],residency='one model process; reset session only between independent tasks; no KV save/evict/restore',receipt='both off/on body-first then final status footer; default CLI legacy layout unchanged',workload='three real test.run calls plus four real Native prepare/first-greedy/actions through CLI REPL and AgentStore; fixed Native-parser tool choices;8 plain lines; actual words/iterations recorded on every row',order='each case1 off/on warmup then7 eval pairs; rotate modes; frozen coalescing512 new bytes,512 token budget,128 min batch;10ms initial observation gate and skip queued work after tool completion',metrics='CLI tick wall including tools, transcript assembly, Native and numeric logs; exact post-token independent tokenizer oracle measured separately and subtracted; raw wall retained',excluded='model load, helper first/warm execution, per-task ordinary Native reset; no build optimization'),limits=['One M5/Metal/Qwen0.6B; real CLI/tool execution, not autonomous model planning or full output generation','Tools bounded retained4096 bytes; test.run(empty args) and verified Qwen3 template only; full render/prefix guard remains','No snapshots, evictions, disk-KV transfer, memory capacity win, cloud SSE or Windows/CUDA acceptance','No evaluation tuning; all warmup/evaluation pairs and negative results retained; per-task reset/model-resident workload only'])
    check(d)
    with zipfile.ZipFile(args.out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
        for n in sorted(sources):z.write(ROOT/n,n)
        z.writestr('sources-sha256.json',json.dumps(sources,indent=2)+'\n')
    (args.out/'evidence.json').write_text(json.dumps(d,indent=2)+'\n')
    for r in d['paired_summary']:print(r,flush=True)
    print('records',len(rows),flush=True)
if __name__=='__main__':main()
