#!/usr/bin/env python3
"""Producer CPU supply with model absent/immediate-idle/settled-idle/closed.
No external process inventory, no profiling other PIDs, no model quality claim.
"""
import argparse,ctypes,json,os,queue,shutil,struct,subprocess,threading,time,zipfile
from pathlib import Path
from bench_roofline_estimator import ROOT,COMMIT,digest,hardware
from analyze_decode_schedule import producer

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--warm-prefill',action='store_true');ap.add_argument('--out',type=Path,required=True);ap.add_argument('--binary-dir',type=Path,required=True);ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--model',type=Path,required=True);a=ap.parse_args();out=a.out.resolve();assert out.is_relative_to(ROOT) and not out.exists();out.mkdir();fixture=out/'workspace';fixture.mkdir();shutil.copy2(a.binary_dir/'tool-output-producer',fixture/'tool-output-producer');lib=ctypes.CDLL('/usr/lib/libproc.dylib');lib.proc_pid_rusage.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.c_void_p]
 def cpu(pid):
  buf=ctypes.create_string_buffer(512);assert lib.proc_pid_rusage(pid,4,buf)==0
  return sum(struct.unpack_from('QQ',buf.raw,16))
 rows=[dict(kind='metadata',repeats=7,words=[43,53],iterations=16000000,states=['absent-before','resident-immediate','resident-settled','absent-after'],producer_calls_per_state=3,settle_ms=100)]
 env={k:v for k,v in os.environ.items() if not k.startswith('CXF_LLM_')};env['CXF_LLM_SYSTEM']='Use the configured workspace tools. Run tests and verify receipts.'
 identities={}
 for w in [43,53]:
  for rep in range(-1,7):
   (fixture/'case.txt').write_text(f'{w} 16000000\n');phase='warmup' if rep<0 else 'evaluation'
   def controls(state,p=None):
    for i in range(3):
     before=cpu(p.pid) if p else 0;parent=time.process_time_ns();subprocess.run(['./tool-output-producer'],cwd=fixture,stdout=subprocess.DEVNULL,check=True);parent=time.process_time_ns()-parent;after=cpu(p.pid) if p else 0
     raw=json.loads((fixture/'producer-metrics.jsonl').read_text().splitlines()[-1]);scale=raw['timebase_numer']/raw['timebase_denom']/1e6
     rows.append(dict(kind='idle_control',words=w,producer_iterations=16000000,rep=rep,phase=phase,mode=state,control=i,producer=raw,model_cpu_ticks=after-before,model_cpu_ms=(after-before)*scale,parent_cpu_ms=parent/1e6,model_present=bool(p)))
   controls('absent-before');q=queue.Queue();log=(out/f'engine-{w}-{rep}.log').open('w');stdout=(out/f'stdout-{w}-{rep}.txt').open('w')
   cmd=[str((a.binary_dir/'test-tool-output-ctl').resolve()),'--llama-model',str(a.model),'--llama-backend',str(a.runtime),'--llama-context','8192','--llama-max-tokens','64','--llama-threads','4','--root',str(fixture),'--test-command','./tool-output-producer','--test-timeout-ms','10000','--budget','65536','--rounds','8','--tool-output-prefill','on' if a.warm_prefill else 'off','--repl']
   p=subprocess.Popen(cmd,cwd=fixture,env=env,stdin=subprocess.PIPE,stdout=stdout,stderr=subprocess.PIPE,text=True,bufsize=1)
   def reader():
    for line in p.stderr:
     log.write(line)
     for prefix in ['[tool-output] ','[output-e2e] ']:
      if line.startswith(prefix):q.put(json.loads(line[len(prefix):]));break
    q.put(dict(kind='process_end',rc=p.wait()))
   t=threading.Thread(target=reader);t.start()
   try:
    assert q.get(timeout=120)['kind']=='ready';before=(fixture/'invocations').stat().st_size;p.stdin.write('/tool-output-policy '+('progress-race' if a.warm_prefill else 'off-race')+'\nRun the configured tests three times. '+'ctx '*256+'\n');p.stdin.flush();trial=[]
    while True:
     r=q.get(timeout=120);assert r['kind']!='process_end',r;trial.append(r)
     if r['kind']=='task_timing':break
    steps=[r for r in trial if r['kind']=='step'];tools=[r for r in trial if r['kind']=='tool'];task=trial[-1]
    assert len(steps)==4 and len(tools)==3 and task['correct'] and task['calls']==4 and task['executed']==7
    assert (fixture/'invocations').stat().st_size-before==3
    ident=[(r['canonical_sha256'],r['transcript_sha256'],r['greedy_token']) for r in steps];assert identities.setdefault(w,ident)==ident
    rows.append(dict(kind='model_fixture',words=w,rep=rep,phase='fixture_validation',correct=True,tool_invocations=3,canonical_greedy_equal=True))
    controls('resident-immediate',p);time.sleep(.1);controls('resident-settled',p)
    p.stdin.write('/quit\n');p.stdin.flush();p.stdin.close();assert p.wait(timeout=30)==0;t.join();log.close();stdout.close()
   finally:
    if p.poll() is None:p.kill();p.wait()
   controls('absent-after');print(w,rep,'complete',flush=True)
 rows += [dict(kind='correctness',passed=True,fixtures=16,controls=192),dict(kind='closed',passed=True,no_model_process_left=True)]
 assert len(rows)==211
 sources={n:digest(ROOT/n) for n in ['tools/probe_model_idle.py','tools/analyze_decode_schedule.py','tools/bench_roofline_estimator.py']}
 manifest=json.loads((a.binary_dir/'manifest.json').read_text());assert manifest['llama_commit']==COMMIT and not manifest['sanitizers']
 d=dict(date='2026-10-31',scope='Same producer with model absent / resident idle / exited; diagnostic only',hardware=hardware(),model=a.model.name,model_sha256=digest(a.model),llama_commit=COMMIT,sources_sha256=sources,binary_sources_sha256=manifest['sources_sha256'],binary_measured_source_sha256=digest(a.binary_dir/'measured-source.zip'),binary_sha256=digest(a.binary_dir/'test-tool-output-ctl'),producer_sha256=digest(a.binary_dir/'tool-output-producer'),rows=rows,method=dict(warm_prefill=a.warm_prefill,order='Fixed absent-before / immediate /100ms settled / absent-after per rep;1 warmup +7 evaluations;3 producer calls each state',metrics='Raw producer counters and model process raw user+system ticks sampled immediately before/after child subprocess. Tick scale from producer Mach timebase. Parent process CPU measured separately.',excluded='Model load, real fixture task,100ms settle, model exit are outside producer window; stdout /dev/null throughout',limits='Only arithmetic producer, two shapes, fixed order temporal drift, no other process inventory; model CPU counts identify own background execution, not GPU execution or competitor PID.'),validation=dict(fixture_oracles_passed=True,actual_native_tools=48,model_processes_closed=16))
 (out/'raw.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows));d['raw_jsonl_sha256']=digest(out/'raw.jsonl')
 with zipfile.ZipFile(out/'collector-source.zip','x',zipfile.ZIP_DEFLATED) as z:
  for n in sources:z.write(ROOT/n,n)
 d['collector_source_archive_sha256']=digest(out/'collector-source.zip');d['origin_directory']=str(out.relative_to(ROOT));(out/'evidence.json').write_text(json.dumps(d,indent=2)+'\n');print('records',len(rows))
if __name__=='__main__':main()
