#!/usr/bin/env python3
"""E55: fresh tasks, actual agentctl/test.run, same Xcode/SDK, serial timing."""
import argparse,hashlib,json,re,shlex,shutil,subprocess,sys,tempfile,time,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from test_agentctl_repl import Fixture,env_for,tool_call
from test_agent_foundation import tool_results
from bench_sandbox_cc import XCODE,BIN,SDK,hashes
SOURCES={
 'checksum.c':'#include <stdint.h>\n#include <stdio.h>\nint main(void){uint32_t h=2166136261u;for(unsigned i=0;i<8191;i++)h=(h^(i%251u))*16777619u;printf("CHECKSUM=%u\\n",h);return 0;}\n',
 'math.c':'int sum_squares(int n){int s=0;for(int i=1;i<=n;i++)s+=i*i;return s;}\n',
 'main.c':'#include <stdio.h>\nint sum_squares(int);int main(void){printf("ARCHIVE=%d\\n",sum_squares(137));return 0;}\n',
 'templates.cpp':'#include <array>\n#include <numeric>\n#include <iostream>\ntemplate<unsigned N> constexpr unsigned triangular(){return N*(N+1)/2;}\nint main(){std::array<int,43>a{};std::iota(a.begin(),a.end(),1);std::cout<<"TEMPLATE="<<std::accumulate(a.begin(),a.end(),0)+triangular<71>()<<"\\n";}\n',
}
CASES={
 'checksum':'cc -std=c11 -O2 checksum.c -o "$TMPDIR/checksum" && "$TMPDIR/checksum"',
 'archive':'cc -std=c11 -O2 -c math.c -o "$TMPDIR/math.o" && ar rcs "$TMPDIR/libmath.a" "$TMPDIR/math.o" && cc -std=c11 -O2 main.c "$TMPDIR/libmath.a" -o "$TMPDIR/archive" && "$TMPDIR/archive"',
 'templates':'c++ -std=c++17 -O2 templates.cpp -o "$TMPDIR/templates" && "$TMPDIR/templates"',
}
EXPECTED={'checksum':f"CHECKSUM={__import__('functools').reduce(lambda h,i:((h^(i%251))*16777619)&0xffffffff,range(8191),2166136261)}",'archive':f'ARCHIVE={sum(i*i for i in range(1,138))}','templates':f'TEMPLATE={sum(range(1,44))+71*72//2}'}
def file_sha(path):
    h=hashlib.sha256()
    with open(path,'rb') as f:
        for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
    return h.hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--phase-probe',type=Path,required=True);ap.add_argument('--reps',type=int,default=7);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=False)
    source=hashes();source[str(Path(__file__).relative_to(ROOT))]=file_sha(__file__)
    (a.out/'source-sha256.json').write_text(json.dumps(source,indent=2)+'\n')
    with zipfile.ZipFile(a.out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
        for name in source:z.write(ROOT/name,name)
    env={'PATH':'/usr/bin:/bin','LANG':'C'}
    found=subprocess.run(['/usr/bin/xcrun','--find','cc'],env=env,text=True,capture_output=True,check=True).stdout.strip()
    sdk=subprocess.run(['/usr/bin/xcrun','--show-sdk-path'],env=env,text=True,capture_output=True,check=True).stdout.strip()
    assert Path(found).resolve()==Path(BIN,'cc').resolve() and Path(sdk).resolve()==Path(SDK).resolve()
    identity={'compiler_sha256':file_sha(found),'compiler_realpath':str(Path(found).resolve()),'sdk_realpath':str(Path(sdk).resolve()),'sdk_settings_sha256':file_sha(Path(sdk)/'SDKSettings.json'),'version':subprocess.run([found,'--version'],env=env,text=True,capture_output=True,check=True).stdout.strip(),'agentctl_sha256':file_sha(ROOT/'build/agentctl'),'phase_probe_sha256':file_sha(a.phase_probe)}
    (a.out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    rows=[];modes=['off','on','pinned_off','pinned_on']
    primary=Fixture([v for _ in range(3*(a.reps+2)*4) for v in [tool_call('test.run'),'CC_TASK_DONE']])
    try:
        with tempfile.TemporaryDirectory(prefix='cxf-cc-new-tasks-') as td:
            root=Path(td);shutil.copy2(a.phase_probe,root/'phase')
            for name,value in SOURCES.items():(root/name).write_text(value)
            config=root/'runtime.json';config.write_text(json.dumps({'api':{'url':primary.url,'model':'fixture','key':''}}))
            env=env_for(primary.url)
            with (a.out/'rows.jsonl').open('w') as out:
                for rep in range(a.reps+2):
                    names=list(CASES);names=names[rep%3:]+names[:rep%3]
                    if rep%2:names=names[::-1]
                    for ci,case in enumerate(names):
                        order=modes[(rep+ci)%4:]+modes[:(rep+ci)%4]
                        if rep%2:order=order[::-1]
                        for mode in order:
                            cmd='./phase task /bin/sh -c '+shlex.quote(CASES[case]);args=[str(ROOT/'build/agentctl'),'--config',str(config),'--root',str(root),'--model-scope','local','--test-command',cmd,'--test-isolation','off' if mode.endswith('off') else 'on']
                            # The private process tmp belongs to the sandbox. Off controls need an equivalent writable tmp.
                            if mode.endswith('off'):
                                cmd='d=$(mktemp -d) || exit 2; TMPDIR="$d" HOME="$d" '+cmd+'; rc=$?; rm -rf "$d"; exit "$rc"';args[args.index('--test-command')+1]=cmd
                            if mode.startswith('pinned'):args+=['--test-toolchain',BIN,'--test-sdk',SDK]
                            before=len(primary.bodies);start=time.perf_counter_ns()
                            p=subprocess.run(args,input='run the tests\n',capture_output=True,text=True,env=env,timeout=40,cwd=ROOT)
                            wall=(time.perf_counter_ns()-start)/1e6
                            receipt=tool_results(primary.bodies[-1]) if len(primary.bodies)>before+1 else ''
                            phase=re.findall(r'CC_PHASE (.*)',receipt);stage=json.loads(phase[0]) if len(phase)==1 else {}
                            passed=p.returncode==0 and len(primary.bodies)==before+2 and 'CC_TASK_DONE' in p.stdout and EXPECTED[case] in receipt and stage.get('exit')==0 and 'exit=0 signal=0 timeout=0 io_error=0' in receipt
                            (a.out/f'{rep}-{case}-{mode}.log').write_text(p.stdout+p.stderr+'\nRECEIPT\n'+receipt)
                            row=dict(kind='cc_cli',case=case,mode=mode,rep=rep,phase='warmup' if rep<2 else 'evaluation',passed=passed,wall_ms=wall,inner_ms=stage.get('wall_ms',0),inner_user_ms=stage.get('user_ms',0),inner_sys_ms=stage.get('sys_ms',0),requests=len(primary.bodies)-before,correct_output=EXPECTED[case] in receipt,fsevents_errors=receipt.count('Failed to start fs event stream'),cache_errors=receipt.count('DARWIN_USER_CACHE_DIR'))
                            row['outside_inner_ms']=row['wall_ms']-row['inner_ms'];rows.append(row);out.write(json.dumps(row,separators=(',',':'))+'\n');out.flush()
                            if not passed:raise RuntimeError(f'failed {case}/{mode}; artifacts retained')
                    print('block complete',rep,flush=True)
    finally:primary.close()
    (a.out/'rows.json').write_text(json.dumps(rows,indent=2)+'\n');print('PASS',len(rows),'rows',flush=True)
if __name__=='__main__':main()
