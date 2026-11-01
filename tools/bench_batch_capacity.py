#!/usr/bin/env python3
"""Rotating, model-reopened capacity controls; resident KV within every task/load."""
import argparse,copy,hashlib,json,re,shutil,subprocess,sys,zipfile
from pathlib import Path
from bench_roofline_estimator import ROOT,digest
import bench_tool_output as cli
import analyze_metal_timeline as metal
CAPACITIES=[512,640,1024]
PAIRS=[['off-512-bare','off-640-bare'],['off-1024-bare','off-640-bare'],['growth-512-bare','growth-640-bare'],['growth-1024-bare','growth-640-bare'],['off-640-bare','growth-640-bare'],['off-512-notify','off-640-notify'],['off-1024-notify','off-640-notify']]
def alias(mode,cap):return ('growth' if mode.startswith('progress') else 'off')+'-'+str(cap)+('-bare' if mode.endswith('-bare') else '-notify')
def flatten(r,cap,block,run,index):
    r=copy.deepcopy(r);r.update(capacity=cap,block=block,source_run=run,source_record_index=index)
    if 'mode' in r:r['source_mode']=r['mode'];r['mode']=alias(r['mode'],cap)
    if r.get('phase')=='evaluation':r['rep']=block
    if 'metal' in r:r['metal']['scope_id']=str(run)+'/'+str(r['metal']['scope_id'])
    return r

def check(d):
    blocks=d['method']['blocks'];rows=d['rows'];assert len(rows)==585*blocks
    assert len(d['capacity_runs'])==3*blocks
    identities={};ts=[r for r in rows if r['kind']=='task_timing'];assert len(ts)==72*blocks
    for run in d['capacity_runs']:
        subset=rows[run['row_begin']:run['row_end']];assert len(subset)==195
        assert all(r['source_run']==run['id'] and r['capacity']==run['capacity'] and r['block']==run['block'] for r in subset)
        assert subset[0]['kind']=='metadata' and subset[0]['batch_tokens']==run['capacity'] and subset[-1]['private_kv_files']==0
        assert sum(r['kind']=='task_timing' for r in subset)==24
        assert all(run['allocation_mib'][k]>0 for k in ['metal_compute','cpu_compute','physical_kv'])
    for r in rows:
        if r['kind']=='step':
            key=(r['words'],r['producer_iterations'],r['step']);value=(r['canonical_sha256'],r['transcript_sha256'],r['greedy_token']);assert identities.setdefault(key,value)==value
        elif r['kind']=='tool':
            assert r['tool_rc']==r['stream_rc']==0 and not r['cancelled']
            if r['mode'].startswith('off-'):assert r['updates']==r['evaluated_tokens']==r['tokenized_tokens']==0
        elif r['kind']=='task_timing':
            assert r['correct'] and r['calls']==4 and r['invocations']==3 and r['idle'] and r['failed']==r['capped']==0
            assert abs(r['task_ms']-r['task_wall_ms']+r['excluded_oracle_ms'])<2e-6
    assert d['summary']==cli.summary(rows)
    groups={}
    for t in metal.tasks(d):groups.setdefault((t['words'],t['producer_iterations'],t['mode']),[]).append(t['rep'])
    assert len(groups)==36 and all(sorted(v)==list(range(blocks)) for v in groups.values())
    metal.check(d)

def allocation(log):
    result={}
    for key,pat in [('metal_compute',r'MTL0 compute buffer size =\s*([\d.]+) MiB'),('cpu_compute',r'CPU compute buffer size =\s*([\d.]+) MiB'),('physical_kv',r'MTL0 KV buffer size =\s*([\d.]+) MiB')]:
        values=re.findall(pat,log);assert len(values)==1,(key,values);result[key]=float(values[0])
    return result

def run(a):
    out=a.out.resolve();assert out.is_relative_to(ROOT);out.mkdir(exist_ok=a.resume,parents=True)
    manifest=json.loads((a.build/'manifest.json').read_text());assert not manifest['sanitizers'] or a.correctness_only
    rows=[];raw=[];runs=[];sources=None;common=None
    for block in range(a.blocks):
        order=CAPACITIES[block%3:]+CAPACITIES[:block%3]
        for cap in order:
            index=len(runs);sub=out/('block%02d-cap%d'%(block,cap))
            if not (a.resume and (sub/'evidence.json').exists()):
                sub.mkdir(exist_ok=a.resume)
                for name in ['test-tool-output-ctl','tool-output-producer','manifest.json']:shutil.copy2(a.build/name,sub/name)
                stem='split' if cap==512 else 'coalesce';modes=['off-'+stem+'-bare','progress-'+stem+'-bare','off-'+stem+'-notify'];modes=modes[block%3:]+modes[:block%3]
                words=[47,54] if block%2==0 else [54,47]
                cmd=[sys.executable,str(ROOT/'tools/bench_tool_output.py'),'--out',str(sub),'--runtime',str(a.runtime),'--model',str(a.model),'--batch-tokens',str(cap),'--words',*map(str,words),'--iterations','12000000','--repeats','1','--modes',*modes,'--metal-probe']
                if a.correctness_only:cmd+=['--correctness-only']
                with (sub/'driver.txt').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
            child=json.loads((sub/'evidence.json').read_text());cli.check(child)
            if sources is None:sources=child['sources_sha256'];common={k:child[k] for k in ['date','hardware','model','model_sha256','llama_commit','runtime_libraries_sha256']}
            assert child['sources_sha256']==sources and child['runtime_libraries_sha256']==common['runtime_libraries_sha256']
            start=len(rows);rows.extend(flatten(r,cap,block,index,j) for j,r in enumerate(child['rows']))
            for r in map(json.loads,(sub/'metal-raw.jsonl').read_text().splitlines()):
                r['source_run']=index
                if 'scope_id' in r:r['scope_id']=str(index)+'/'+str(r['scope_id'])
                raw.append(r)
            runs.append(dict(id=index,capacity=cap,block=block,row_begin=start,row_end=len(rows),allocation_mib=allocation((sub/'engine.log').read_text()),child_evidence_sha256=digest(sub/'evidence.json'),metal_raw_sha256=digest(sub/'metal-raw.jsonl'),measured_source_archive_sha256=digest(sub/'measured-source.zip')))
            print('complete',block,cap,'tasks',24,'metal_compute_MiB',runs[-1]['allocation_mib']['metal_compute'],flush=True)
    sources=dict(sources);sources['tools/bench_batch_capacity.py']=digest(Path(__file__))
    assert all(digest(ROOT/n)==h for n,h in sources.items())
    d=dict(**common,scope='Fresh capacity512/split vs640/coalesce vs1024/coalesce; model reload per block/capacity, KV resident within each task; fixed choices, real CLI/tools',origin_directory=str(out.relative_to(ROOT)),sources_sha256=sources,rows=rows,metal_commands=raw,capacity_runs=runs,summary=cli.summary(rows),contrast_pairs=PAIRS,method=dict(blocks=a.blocks,capacities=CAPACITIES,words=[47,54],iterations=[0,12000000],sanitizers=manifest['sanitizers'],modes_per_capacity=3,order='Rotate capacity and mode order per block, reverse word order on odd blocks; each child one warmup and one evaluation per case/mode; reloaded per capacity/block; all processes sequential',allocation='Initialization graph reservation only, not peak RSS; physical KV stays resident within task/load',excluded='Load, helper first/warm execution and each child warmup; independent tokenizer oracle subtracted, raw whole wall retained'),limits=['One M5/Metal/Qwen0.6B; scripted model actions, no autonomous-quality score','GPU spans are buffer intervals, notification delivery and post-marker return are upper bounds including driver/handlers/scheduling, not pure CPU wakeup','Bare has no Metal timestamps; marker may perturb waits; compare bare task results separately','Capacity comparison reloads models; not a claim about switching capacity for free during live tasks; fixed case order and temporal drift remain','No snapshots/eviction or physical KV reduction, no Windows/CUDA; all negative pairs retained; seven repeats do not support tail guarantees'])
    d['metal_tasks']=metal.tasks(d);d['metal_contrasts']=metal.contrasts(d,PAIRS);d['analysis_source_sha256']=digest(ROOT/'tools/analyze_metal_timeline.py');check(d)
    with zipfile.ZipFile(out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
        for n in sorted(sources):z.write(ROOT/n,n)
        z.writestr('sources-sha256.json',json.dumps(sources,indent=2)+'\n')
    d['measured_source_archive_sha256']=digest(out/'measured-source.zip');(out/'evidence.json').write_text(json.dumps(d,indent=2)+'\n')
    print('PASS',len(rows),'main',len(raw),'Metal auxiliary records',flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--check',type=Path);p.add_argument('--build',type=Path);p.add_argument('--out',type=Path);p.add_argument('--runtime',type=Path);p.add_argument('--model',type=Path);p.add_argument('--blocks',type=int,default=7);p.add_argument('--correctness-only',action='store_true');p.add_argument('--resume',action='store_true');a=p.parse_args()
    if a.check:check(json.loads(a.check.read_text()));print('PASS capacity invariants');return
    assert all([a.build,a.out,a.runtime,a.model]) and a.blocks in [1,7];a.build=a.build.resolve();run(a)
if __name__=='__main__':main()
