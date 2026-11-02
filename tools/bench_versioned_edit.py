#!/usr/bin/env python3
"""Matched versioned-read/edit overhead; serial, unsanitized, immutable outputs.

Baseline basic implementation is extracted from the public pre-change commit.
No permissions disabled. Large full-write controls are C handler calls, not CLI.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
BASE='ad5ac5a5712cff4c6c25e921d52761ecd1f0c127'


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def cells(rows):
    result=[]
    for key in sorted({(r['case'],r['file_bytes'],r['variant']) for r in rows if not r['warmup']}):
        group=[r for r in rows if not r['warmup'] and (r['case'],r['file_bytes'],r['variant'])==key]
        row=dict(zip(['case','file_bytes','variant'],key));row['samples']=len(group)
        for field in ['wall_ns','user_ns','system_ns']:
            vals=[r[field]/r['iterations']/1000 for r in group];row[field.replace('_ns','_us')]={'median':statistics.median(vals),'min':min(vals),'max':max(vals)}
        result.append(row)
    return result


def paired(rows):
    result=[]
    for case,size in sorted({(r['case'],r['file_bytes']) for r in rows if r['case']!='stale'}):
        group=[r for r in rows if not r['warmup'] and r['case']==case and r['file_bytes']==size];pairs=[]
        for block in sorted({r['block'] for r in group}):
            by={r['variant']:r for r in group if r['block']==block};assert set(by)=={'baseline','current'}
            old,new=by['baseline'],by['current'];pairs.append(new['wall_ns']/new['iterations']/1000-old['wall_ns']/old['iterations']/1000)
        result.append({'case':case,'file_bytes':size,'pairs':len(pairs),'overhead_us':{'median':statistics.median(pairs),'min':min(pairs),'max':max(pairs)},'current_slower':sum(v>0 for v in pairs)})
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);args=ap.parse_args()
    if args.out.exists():ap.error('fresh --out required')
    args.out.mkdir(parents=True);out=args.out.resolve();baseline=out/'baseline_basic.c'
    baseline.write_bytes(subprocess.check_output(['git','show',BASE+':tests/agent_basic.c'],cwd=ROOT))
    commands=[]
    def run(command):
        commands.append(command);r=subprocess.run(command,text=True,capture_output=True)
        with (out/'compile.log').open('a') as log:log.write(r.stdout+r.stderr)
        if r.returncode:raise RuntimeError('build failed; logs retained')
    common=['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Wpedantic','-I'+str(ROOT/'include'),'-I'+str(ROOT/'tests')]
    for variant in ['baseline','current']:
        objects=[]
        for i,source in enumerate([baseline if variant=='baseline' else ROOT/'tests/agent_basic.c',ROOT/'tests/agent_policy.c',ROOT/'tests/agent_sandbox.c',ROOT/'tests/agent_fs.c',ROOT/'tests/bench_versioned_edit.c']):
            obj=out/f'{variant}-{i}.o';run(common+(['-DCXF_EDIT_CURRENT'] if variant=='current' else [])+['-c',str(source),'-o',str(obj)]);objects.append(str(obj))
        run(['cc',*objects,'-o',str(out/variant)])
    sources=['tests/agent_basic.c','tests/agent_basic.h','tests/agent_fs.c','tests/agent_policy.c','tests/agent_sandbox.c','tests/bench_versioned_edit.c','tools/bench_versioned_edit.py']
    (out/'manifest.json').write_text(json.dumps({'baseline_commit':BASE,'baseline_basic_sha256':sha(baseline),'source_sha256':{n:sha(ROOT/n) for n in sources},'commands':commands,'platform':platform.system(),'machine':platform.machine(),'sanitizers':False},indent=2)+'\n')
    rows=[];started=time.monotonic()
    with tempfile.TemporaryDirectory(prefix='cxf-edit-overhead-') as workspace,(out/'rows.jsonl').open('w') as stream:
        for block in range(11):
            for size in [1024,16384,65536]:
                for case in ['read','single','multi','workflow','same','stale']:
                    variants=['current'] if case=='stale' else ['baseline','current']
                    if block%2:variants.reverse()
                    for order,variant in enumerate(variants):
                        iterations=64 if case=='read' else 8
                        r=subprocess.run([str(out/variant),case,workspace,str(size),str(iterations)],text=True,capture_output=True)
                        if r.returncode:
                            (out/'failure.json').write_text(json.dumps({'variant':variant,'case':case,'file_bytes':size,'block':block,'returncode':r.returncode,'stderr':r.stderr})+'\n');raise SystemExit('measurement failed; prior rows retained')
                        row=json.loads(r.stdout);row.update(variant=variant,block=block,warmup=block<2,order=order)
                        stream.write(json.dumps(row)+'\n');stream.flush();rows.append(row)
    assert len(rows)==363
    result={'rows':rows,'summary':cells(rows),'paired_summary':paired(rows),'elapsed_s':time.monotonic()-started,
            'scope':'C handler overhead; per-row means over8 or64 operations,2 warmup+9 evaluation blocks, rotating variant order; all pairs retained',
            'exclusions':['Fixture/reset, initial known-version preparation, input formatting and byte oracle outside handler timer. Workflow includes actual read and patch framing.','Large whole-file writes are handler-only controls; CLI tool args remain1KiB.','Cloud model/network/relay, process startup, physical KV and process RSS are not tool microbenchmark results.']}
    (out/'summary.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result['paired_summary'],indent=2))


if __name__=='__main__':main()
