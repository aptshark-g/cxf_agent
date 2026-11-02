#!/usr/bin/env python3
"""E56: fixed-target C++ stage/cache controls under the unchanged sandbox."""
import argparse,hashlib,json,re,shutil,subprocess,tempfile,zipfile
from pathlib import Path
from bench_sandbox_cc import BIN,SDK,hashes
from bench_sandbox_cc_tasks import SOURCES,file_sha
ROOT=Path(__file__).resolve().parents[1]
FLAGS='-std=c++17 -O2 -mmacosx-version-min=26.0'
CASES={
 'noop':'./phase noop /usr/bin/true',
 'version':'./phase version /bin/sh -c \'c++ --version >/dev/null\'',
 'preprocess':f'./phase pp c++ {FLAGS} -E templates.cpp -o "$TMPDIR/out.ii"',
 'syntax':f'./phase syntax c++ {FLAGS} -fsyntax-only templates.cpp',
 'object':f'./phase object c++ {FLAGS} -c templates.cpp -o "$TMPDIR/out.o"',
 'preprocessed':f'./phase ii c++ {FLAGS} -c prepared.ii -o "$TMPDIR/out.o"',
 'link':'./phase link c++ -mmacosx-version-min=26.0 prepared.o -o "$TMPDIR/prog"',
 'fresh_run':'cp prepared "$TMPDIR/prog" && ./phase run "$TMPDIR/prog"',
 'chain':f'./phase object c++ {FLAGS} -c templates.cpp -o "$TMPDIR/out.o" && ./phase link c++ -mmacosx-version-min=26.0 "$TMPDIR/out.o" -o "$TMPDIR/prog" && ./phase run "$TMPDIR/prog"',
 'reuse':f'./phase first c++ {FLAGS} -c templates.cpp -o "$TMPDIR/a.o" && ./phase second c++ {FLAGS} -c templates.cpp -o "$TMPDIR/b.o"',
 'modules':f'./phase first c++ {FLAGS} -fmodules -fcxx-modules -fimplicit-module-maps -c templates.cpp -o "$TMPDIR/a.o" && ./phase second c++ {FLAGS} -fmodules -fcxx-modules -fimplicit-module-maps -c templates.cpp -o "$TMPDIR/b.o"',
 'pch':f'./phase build c++ {FLAGS} -x c++-header common.hpp -o "$TMPDIR/common.pch" && ./phase use c++ {FLAGS} -include-pch "$TMPDIR/common.pch" -c templates.cpp -o "$TMPDIR/out.o"',
 'stat':f'./phase build clang-stat-cache -f "$SDKROOT" -o "$TMPDIR/sdk.cache" && ./phase use c++ {FLAGS} -ivfsstatcache "$TMPDIR/sdk.cache" -c templates.cpp -o "$TMPDIR/out.o"',
 'plain3':f'for n in 1 2 3; do ./phase use c++ {FLAGS} -c templates.cpp -o "$TMPDIR/$n.o" || exit; done',
 'pch3':f'./phase build c++ {FLAGS} -x c++-header common.hpp -o "$TMPDIR/common.pch" && for n in 1 2 3; do ./phase use c++ {FLAGS} -include-pch "$TMPDIR/common.pch" -c templates.cpp -o "$TMPDIR/$n.o" || exit; done',
}
EXPECTED={c:2 if c in ['reuse','modules','pch','stat'] else 3 if c in ['chain','plain3'] else 4 if c=='pch3' else 1 for c in CASES}
def command(cmd,on):
    cmd=cmd+'; rc=$?; ./cache "$TMPDIR" || exit 7; exit "$rc"'
    return cmd if on else 'd=$(mktemp -d) || exit 2; (export HOME="$d" TMPDIR="$d" CLANG_MODULE_CACHE_PATH="$d/modules"; '+cmd+'); rc=$?; rm -rf "$d"; exit "$rc"'
def snapshot(out,extra):
    source=hashes()
    for name in ['tools/probe_sandbox_cpp.c','tools/probe_cpp_cache.c','tools/bench_sandbox_cpp.py',*extra]:source[name]=file_sha(ROOT/name)
    (out/'source-sha256.json').write_text(json.dumps(source,indent=2)+'\n')
    with zipfile.ZipFile(out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
        for name in source:z.write(ROOT/name,name)
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--probes',type=Path,required=True);ap.add_argument('--reps',type=int,default=7);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=False);snapshot(a.out,[])
    env={'PATH':BIN+':/usr/bin:/bin','SDKROOT':SDK,'LANG':'C'};rows=[]
    with tempfile.TemporaryDirectory(prefix='cxf-cpp-diagnose-') as td:
        root=Path(td)
        for name in ['phase','cache']:shutil.copy2(a.probes/name,root/name)
        (root/'templates.cpp').write_text(SOURCES['templates.cpp']);(root/'common.hpp').write_text('#pragma once\n#include <array>\n#include <numeric>\n#include <iostream>\n')
        for name,options in [('ii',['-E','-o','prepared.ii']),('object',['-c','-o','prepared.o']),('exe',['-o','prepared'])]:
            p=subprocess.run(['c++',*FLAGS.split(),'templates.cpp',*options],env=env,cwd=root,capture_output=True,text=True,check=True);(a.out/f'fixture-{name}.log').write_text(p.stdout+p.stderr)
        (a.out/'fixture-sha256.json').write_text(json.dumps({name:file_sha(root/name) for name in ['prepared.ii','prepared.o','prepared']},indent=2)+'\n')
        with (a.out/'rows.jsonl').open('w') as out:
            names=list(CASES)
            for rep in range(a.reps+2):
                order=names[rep%len(names):]+names[:rep%len(names)]
                if rep%2:order=order[::-1]
                for ci,case in enumerate(order):
                    for mode in [rep%2,1-rep%2]:
                        p=subprocess.run([str((a.probes/'run').resolve()),str(root),str(mode),command(CASES[case],mode),BIN,SDK],env={'PATH':'/usr/bin:/bin','LANG':'C'},capture_output=True,text=True,timeout=40)
                        output=p.stdout+p.stderr;(a.out/f'{rep}-{case}-{mode}.log').write_text(output)
                        metrics=re.search('CPP_METRICS (.*)',output);cache=re.search('CPP_CACHE (.*)',output);stages=[json.loads(s) for s in re.findall('CC_PHASE (.*)',output)]
                        passed=p.returncode==0 and metrics and cache and len(stages)==EXPECTED[case] and all(s['exit']==0 for s in stages) and 'exit=0 signal=0 timeout=0 io_error=0' in output
                        row=dict(kind='cpp_diagnostic',case=case,mode='on' if mode else 'off',rep=rep,phase='warmup' if rep<2 else 'evaluation',passed=bool(passed),output_sha256=hashlib.sha256(output.encode()).hexdigest())
                        if metrics:row.update(json.loads(metrics.group(1)))
                        if cache:row.update(json.loads(cache.group(1)))
                        for i,s in enumerate(stages):
                            for k in ['wall_ms','user_ms','sys_ms']:row[f'stage{i}_{k}']=s[k]
                        row['inner_ms']=sum(s['wall_ms'] for s in stages);row['user_ms']=sum(s['user_ms'] for s in stages);row['sys_ms']=sum(s['sys_ms'] for s in stages);row['outside_ms']=row['wall_ms']-row['inner_ms']
                        rows.append(row);out.write(json.dumps(row,separators=(',',':'))+'\n');out.flush()
                        if not passed:raise RuntimeError(f'{case}/{mode} failed; all artifacts retained')
                print('block complete',rep,flush=True)
    (a.out/'rows.json').write_text(json.dumps(rows,indent=2)+'\n');print('PASS',len(rows),'rows',flush=True)
if __name__=='__main__':main()
