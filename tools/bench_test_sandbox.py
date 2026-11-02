#!/usr/bin/env python3
"""Matched current off/on sandbox overhead; real commands, scripted HTTP choices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from test_agentctl_repl import Fixture,env_for,tool_call
from test_agent_foundation import tool_results


def pairs(rows):
    groups={}
    for r in rows:
        if r['phase']!='evaluation':continue
        groups.setdefault((r['kind'],r['case'],r['files'],r['rep']),{})[r['mode']]=r
    out=[]
    for (kind,case,files,rep),g in sorted(groups.items()):
        assert set(g)=={'off','on'}
        row=dict(kind=kind,case=case,files=files,rep=rep)
        for key in ['wall_ms','inner_ms','prepare_ms','cleanup_ms','spawn_ms']:
            if key in g['on']:row[key+'_overhead']=g['on'][key]-g['off'][key]
        row['outside_inner_ms_overhead']=row['wall_ms_overhead']-row['inner_ms_overhead']
        out.append(row)
    return out


def summarize(rows):
    out=[]
    for kind,case,files in sorted({(r['kind'],r['case'],r['files']) for r in rows}):
        group=[r for r in pairs(rows) if (r['kind'],r['case'],r['files'])==(kind,case,files)]
        r=dict(kind=kind,case=case,files=files,pairs=len(group),on_slower=sum(v['wall_ms_overhead']>0 for v in group))
        for field in group[0]:
            if field.endswith('_overhead'):
                values=[v[field] for v in group];r[field]=dict(median=statistics.median(values),min=min(values),max=max(values))
        for mode in ['off','on']:
            g=[v for v in rows if v['phase']=='evaluation' and (v['kind'],v['case'],v['files'],v['mode'])==(kind,case,files,mode)]
            r[mode+'_wall_median_ms']=statistics.median(v['wall_ms'] for v in g)
        out.append(r)
    return out


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--reps',type=int,default=9);p.add_argument('--cli-reps',type=int,default=7);a=p.parse_args()
    if a.out.exists():p.error('choose a fresh output directory')
    a.out.mkdir(parents=True);rows=[]
    files=[ROOT/'Makefile',ROOT/'cxf.sh',Path(__file__).resolve()]
    for folder in ['include','src','tests']:
        files += [v for v in (ROOT/folder).rglob('*') if v.is_file() and v.suffix in ('.c','.h','.cpp','.mm','.py')]
    hashes={str(v.relative_to(ROOT)):hashlib.sha256(v.read_bytes()).hexdigest() for v in sorted(set(files))}
    (a.out/'source-sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
    with tempfile.TemporaryDirectory(prefix='cxf-sandbox-bench-') as td:
        for count in [8,256]:
            root=Path(td)/str(count);root.mkdir();shutil.copy2(ROOT/'build/sandbox_probe',root/'probe')
            (root/'unit.c').write_text('int main(void){return 0;}\n');(root/'runtime.json').write_text('{}')
            (root/'corpus.txt').write_text(('ordinary corpus line\n'*8192)+'needle\n')
            for i in range(count):(root/f'data-{i:04d}.txt').write_text('ordinary\n')
            result=subprocess.run([str(ROOT/'build/bench_test_sandbox'),str(root),str(count),str(a.reps)],capture_output=True,text=True,timeout=240)
            (a.out/f'api-{count}.jsonl').write_text(result.stdout);(a.out/f'api-{count}.stderr').write_text(result.stderr)
            parsed=[json.loads(x) for x in result.stdout.splitlines()];rows+=parsed
            if result.returncode or not all(r['passed'] for r in parsed):raise RuntimeError('API timing failed; artifacts retained')
            print('API completed:',count,flush=True)
        root=Path(td)/'cli';root.mkdir();shutil.copy2(ROOT/'build/sandbox_probe',root/'probe');(root/'unit.c').write_text('int main(void){return 0;}\n')
        cases={'noop':'true','clt_compile_tmp':'d=$(mktemp -d) || exit 2; DEVELOPER_DIR=/Library/Developer/CommandLineTools cc unit.c -o "$d/unit" && "$d/unit"; rc=$?; rm -rf "$d"; exit $rc','system_cc_compile_write':'cc unit.c -o unit && ./unit'}
        primary=Fixture([v for _ in range(3*(a.cli_reps+2)*2) for v in [tool_call('test.run'),'BENCH_DONE']])
        try:
            config=root/'runtime.json';config.write_text(json.dumps({'api':{'url':primary.url,'model':'fixture','key':''}}));env=env_for(primary.url)
            with (a.out/'cli.jsonl').open('w') as raw:
                for rep in range(a.cli_reps+2):
                    for case_index,(case,command) in enumerate(cases.items()):
                        for order in range(2):
                            on=(rep+case_index+order)%2;before=len(primary.bodies)
                            args=[str(ROOT/'build/agentctl'),'--config',str(config),'--root',str(root),'--model-scope','local','--test-command',"./probe measure '"+command+"'",'--test-isolation','on' if on else 'off']
                            if case=='system_cc_compile_write':args+=['--test-write',str(root)]
                            start=time.perf_counter_ns();result=subprocess.run(args,input='run tests\n',text=True,capture_output=True,env=env,cwd=ROOT,timeout=40);wall=(time.perf_counter_ns()-start)/1e6
                            receipt=tool_results(primary.bodies[-1]);m=re.search(r'INNER_NS=(\d+)',receipt)
                            passed=result.returncode==0 and len(primary.bodies)==before+2 and 'BENCH_DONE' in result.stdout and 'exit=0 ' in receipt and m is not None
                            row=dict(kind='cli_timing',case=case,files=0,rep=rep,phase='warmup' if rep<2 else 'evaluation',order=order,mode='on' if on else 'off',passed=passed,wall_ms=wall,inner_ms=int(m.group(1))/1e6 if m else 0,requests=len(primary.bodies)-before)
                            rows.append(row);raw.write(json.dumps(row,separators=(',',':'))+'\n');raw.flush()
                            if not passed:raise RuntimeError('CLI timing failed; raw rows retained')
                    print('CLI block completed:',rep,flush=True)
        finally:primary.close()
    (a.out/'rows.json').write_text(json.dumps(rows,indent=2)+'\n');(a.out/'pairs.json').write_text(json.dumps(pairs(rows),indent=2)+'\n');(a.out/'summary.json').write_text(json.dumps(summarize(rows),indent=2)+'\n')
    print('PASS:',len(rows),'raw rows,',len(pairs(rows)),'evaluation pairs')
if __name__=='__main__':main()
