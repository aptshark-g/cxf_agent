#!/usr/bin/env python3
"""Fixed-shape native increments under idle, arithmetic and memory pressure."""
import argparse,json,subprocess,zipfile,statistics as st
from pathlib import Path
from bench_roofline_estimator import ROOT,COMMIT,digest,hardware
from bench_batch_capacity import allocation
from analyze_metal_timeline import scope

def analyze(d):
 groups={}
 for r in d['metal_commands']:
  if 'scope_id' in r:groups.setdefault(r['scope_id'],[]).append(r)
 return [dict(r,**{'metal_'+k:v for k,v in scope(groups[r['scope_id']]).items()}) for r in d['rows'] if r['kind']=='isolation' and r['phase']=='evaluation']
def summary(d):
 groups={}
 for r in analyze(d):groups.setdefault((r['prefix_tokens'],r['new_tokens'],r['pressure'],r['policy']),[]).append(r)
 fields=['wall_ms','owner_cpu_ms','metal_gpu_union_ms','metal_queue_no_own_gpu_ms','metal_sync_completion_delivery_ms','metal_sync_after_notify_return_ms','worker_cpu_ms','worker_wall_ms']
 return [dict(prefix_tokens=k[0],new_tokens=k[1],pressure=k[2],policy=k[3],samples=len(g),**{f:dict(median=st.median(r[f] for r in g),min=min(r[f] for r in g),max=max(r[f] for r in g)) for f in fields}) for k,g in sorted(groups.items())]
def contrasts(d):
 rs=analyze(d);out=[]
 for p,n in sorted({(r['prefix_tokens'],r['new_tokens']) for r in rs}):
  groups={(m,c):{r['rep']:r for r in rs if (r['prefix_tokens'],r['new_tokens'],r['pressure'],r['policy'])==(p,n,m,c)} for m in ['idle','arithmetic','memory'] for c in ['split','coalesce']}
  pairs=[(('idle','split'),('idle','coalesce'))]+[((a,c),(b,c)) for a,b in [('idle','arithmetic'),('arithmetic','memory')] for c in ['split','coalesce']]
  for a,b in pairs:
   fields={};x,y=groups[a],groups[b]
   for f in ['wall_ms','metal_gpu_union_ms','metal_queue_no_own_gpu_ms','metal_sync_completion_delivery_ms','metal_sync_after_notify_return_ms']:
    v=[y[j][f]-x[j][f] for j in x];fields[f]=dict(median_extra=st.median(v),min_extra=min(v),max_extra=max(v),slower_pairs=sum(t>0 for t in v))
   out.append(dict(prefix_tokens=p,new_tokens=n,baseline='/'.join(a),mode='/'.join(b),pairs=len(x),**fields))
 return out

def check(d):
 repeats=d['method']['repeats'];assert len(d['rows'])==48*(repeats+1)+3
 assert d['rows'][-2]['passed'] and d['rows'][-2]['samples']==48*(repeats+1)
 same={};raw=d['metal_commands'];commands=[r for r in raw if r['kind']=='metal_command'];assert all(r['valid'] and r['status']==4 and r['batch']>0 for r in commands)
 batches={r['batch']:r for r in raw if r['kind']=='metal_batch'};assert all(r['batch'] in batches for r in commands)
 for r in d['rows']:
  if r['kind']!='isolation':continue
  key=(r['prefix_tokens'],r['new_tokens']);assert r['correct'] and same.setdefault(key,r['greedy_token'])==r['greedy_token']
  count=2 if r['new_tokens']>512 and r['policy']=='split' else 1;assert r['decode_calls']==count
  assert (r['logical_memory_bytes']>0)==(r['pressure']=='memory')
  if r['pressure']!='idle':assert r['worker_operations']>0
 assert d['isolation_samples']==analyze(d) and d['isolation_summary']==summary(d) and d['isolation_contrasts']==contrasts(d)

def run(a):
 out=a.out.resolve();assert out.is_relative_to(ROOT);out.mkdir(parents=True);sdk=ROOT/'build/llama.cpp-b10938'
 assert subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()==COMMIT
 files=sorted((ROOT/'src').glob('*.c'))+[ROOT/'tests/probe_device_contention.cpp',ROOT/'tests/agent_metal_probe.mm'];headers=[ROOT/n for n in ['src/llama_internal.h','include/cxf/llama.h','tests/agent_metal_probe.h']];sources={str(p.relative_to(ROOT)):digest(p) for p in files+headers+[Path(__file__)]}
 flags=['-O1' if a.sanitize else '-O2','-g','-I'+str(ROOT/'include'),'-I'+str(ROOT/'tests'),'-isystem',str(sdk/'include'),'-isystem',str(sdk/'ggml/include')]+(['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if a.sanitize else [])
 objs=[];commands=[]
 with (out/'compile.txt').open('w') as log:
  for i,p in enumerate(files):
   cpp=p.suffix in ['.cpp','.mm'];obj=out/(str(i)+'.o');cmd=['c++' if cpp else 'cc','-std=c++17' if cpp else '-std=c11',*flags,'-c',str(p),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True);objs.append(str(obj))
  cmd=['c++',*flags,*objs,'-L'+str(a.runtime),'-Wl,-rpath,'+str(a.runtime),'-lllama','-lggml','-lggml-base','-framework','Metal','-framework','Foundation','-pthread','-o',str(out/'probe-device')];commands.append(cmd);subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 (out/'manifest.json').write_text(json.dumps(dict(sanitizers=a.sanitize,sources_sha256=sources,commands=commands),indent=2)+'\n')
 with (out/'raw.jsonl').open('w') as f,(out/'engine.log').open('w') as log:subprocess.run([str(out/'probe-device'),str(a.model),str(a.runtime),str(out/'metal-raw.jsonl'),str(a.repeats)],stdout=f,stderr=log,check=True)
 assert all(digest(ROOT/n)==h for n,h in sources.items());rows=[json.loads(x) for x in (out/'raw.jsonl').read_text().splitlines()];raw=[json.loads(x) for x in (out/'metal-raw.jsonl').read_text().splitlines()]
 d=dict(date='2026-10-31',scope='Isolated native fixed-prefix increment under controlled CPU/memory pressure; not agent latency',hardware=hardware(),llama_commit=COMMIT,model=a.model.name,model_sha256=digest(a.model),sources_sha256=sources,runtime_libraries_sha256={n:digest(a.runtime/n) for n in ['libllama.dylib','libggml.dylib','libggml-base.dylib','libggml-metal.dylib','libggml-cpu.dylib']},origin_directory=str(out.relative_to(ROOT)),rows=rows,metal_commands=raw,method=dict(repeats=a.repeats,sanitizers=a.sanitize,allocation_mib=allocation((out/'engine.log').read_text()),counting='Pressure setup/start handshake and initial prefix excluded; prefill timer excludes thread join and probe-file flush; worker window slightly wider.64MiB buffer exists in every condition; logical bytes are not DRAM counters.'),limits=['M5 lacks public dispatch-boundary sampling; GPU spans include execution/stalls/preemption; not exclusive kernel busy','Arithmetic/memory threads differ in instructions as well as memory traffic; compare sensitivities, not exact bandwidth attribution','Synthetic pressure is not proof of the old217ms cause; no measured GPU frequency/full-device queue trace','One fixed model/backend; no KV eviction or physical KV reduction; all negative samples retained'])
 d['isolation_samples']=analyze(d);d['isolation_summary']=summary(d);d['isolation_contrasts']=contrasts(d);check(d)
 with zipfile.ZipFile(out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
  for n in sorted(sources):z.write(ROOT/n,n)
  z.writestr('sources-sha256.json',json.dumps(sources,indent=2)+'\n')
 d['measured_source_archive_sha256']=digest(out/'measured-source.zip');(out/'evidence.json').write_text(json.dumps(d,indent=2)+'\n');print('PASS',len(rows),len(raw),flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path);p.add_argument('--runtime',type=Path);p.add_argument('--model',type=Path);p.add_argument('--repeats',type=int,default=7);p.add_argument('--sanitize',action='store_true');p.add_argument('--check',type=Path);a=p.parse_args()
 if a.check:check(json.loads(a.check.read_text()));print('PASS isolation invariants')
 else:assert a.out and a.model and a.runtime and a.repeats in [1,7];run(a)
