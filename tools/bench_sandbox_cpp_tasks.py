#!/usr/bin/env python3
"""E57: new fixed-target C++ CLI tasks; PCH cold cost and grouped-rule control."""
import argparse,hashlib,json,re,shlex,shutil,subprocess,sys,tempfile,time
from pathlib import Path
from bench_sandbox_cc import BIN,SDK
from bench_sandbox_cc_tasks import file_sha
from bench_sandbox_cpp import snapshot
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from test_agentctl_repl import Fixture,env_for,tool_call
from test_agent_foundation import tool_results
CASES={
 'single_map':dict(units=1,headers=['map','string','vector','numeric','iostream'],body='std::map<std::string,int> m;for(int i=0;i<37;i++)m[std::to_string(i)]=i*i;int s=0;for(auto&p:m)s+=p.second;std::cout<<"RESULT="<<s<<"\\n";',expected=sum(i*i for i in range(37))),
 'single_regex':dict(units=1,headers=['regex','string','vector','iostream'],body='std::regex p("item[0-9]+");int n=0;for(auto s:{"item17","bad","item2","item","item999"})n+=std::regex_match(s,p);std::cout<<"RESULT="<<n<<"\\n";',expected=3),
 'batch3':dict(units=3,headers=['vector','algorithm','numeric','string','sstream','iostream'],expected=sum(sum((i+1)*j for j in range(1,43)) for i in range(2))),
 'batch5':dict(units=5,headers=['map','set','vector','string','sstream','algorithm','numeric','iostream'],expected=sum(sum((i+1)*j for j in range(1,57)) for i in range(4))),
}
MODES=['off','on','grouped_on','pch_off','pch_on']
def create(root,case,definition,probes):
    root.mkdir();shutil.copy2(ROOT/'tools/run_cpp_batch.sh',root/'batch.sh')
    shutil.copy2(probes/'phase',root/'phase');shutil.copy2(probes/'compiler',root/'compiler')
    (root/'common.hpp').write_text('#pragma once\n'+''.join('#include <'+h+'>\n' for h in definition['headers']))
    names=[]
    if definition['units']==1:
        (root/'main.cpp').write_text('int main(){'+definition['body']+'}\n');names=['main.cpp']
    else:
        count=definition['units']-1;length=43 if case=='batch3' else 57
        main=''.join(f'int part{i}();\n' for i in range(count))+'int main(){std::cout<<"RESULT="<<'+'+'.join(f'part{i}()' for i in range(count))+'<<"\\n";}\n'
        (root/'main.cpp').write_text(main);names=['main.cpp']
        for i in range(count):
            name=f'part{i}.cpp';names.append(name)
            (root/name).write_text(f'int part{i}(){{std::vector<int> v;for(int n=1;n<{length};n++)v.push_back({i+1}*n);std::sort(v.begin(),v.end());std::ostringstream out;out<<v.size();return std::accumulate(v.begin(),v.end(),0);}}\n')
    return names

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--probes',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--reps',type=int,default=7);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
    snapshot(a.out,['tools/bench_sandbox_cpp_tasks.py','tools/run_cpp_batch.sh','tools/probe_cpp_compiler.c','tools/build_sandbox_group_candidate.py'])
    identity=json.loads((ROOT/'build/sandbox-cc-tasks-final-20261101/identity.json').read_text());identity.update(agentctl_sha256=file_sha(ROOT/'build/agentctl'),candidate_sha256=file_sha(a.candidate),compiler_probe_sha256=file_sha(a.probes/'compiler'),recipe_sha256=file_sha(ROOT/'tools/run_cpp_batch.sh'))
    assert file_sha(Path(BIN)/'c++')==identity['compiler_sha256']
    (a.out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    rows=[];primary=Fixture([v for _ in range(4*5*(a.reps+2)) for v in [tool_call('test.run'),'CPP_TASK_DONE']])
    try:
        with tempfile.TemporaryDirectory(prefix='cxf-cpp-holdout-') as td:
            base=Path(td);workspaces={}
            for case,d in CASES.items():
                root=base/case;sources=create(root,case,d,a.probes);config=root/'runtime.json';config.write_text(json.dumps({'api':{'url':primary.url,'model':'fixture','key':''}}));workspaces[case]=(root,sources,config)
            (a.out/'fixture-sha256.json').write_text(json.dumps({case:{p.name:file_sha(p) for p in root.iterdir() if p.suffix in ['.cpp','.hpp']} for case,(root,_,_) in workspaces.items()},indent=2)+'\n')
            with (a.out/'rows.jsonl').open('w') as out:
                for rep in range(a.reps+2):
                    cases=list(CASES);cases=cases[rep%4:]+cases[:rep%4]
                    if rep%2:cases=cases[::-1]
                    for ci,case in enumerate(cases):
                        root,sources,config=workspaces[case];order=MODES[(rep+ci)%5:]+MODES[:(rep+ci)%5]
                        if rep%2:order=order[::-1]
                        for mode in order:
                            recipe='pch' if mode.startswith('pch') else 'plain'
                            build='CXX=./compiler CPP_BENCH_COMPILER='+shlex.quote(BIN+'/c++')+' /bin/sh ./batch.sh '+recipe+' 26.0 common.hpp "$TMPDIR/unit" '+' '.join(sources)
                            command='./phase total /bin/sh -c '+shlex.quote(build)
                            if mode.endswith('off'):command='d=$(mktemp -d) || exit 2; (export TMPDIR="$d" HOME="$d" CLANG_MODULE_CACHE_PATH="$d/modules"; '+command+'); rc=$?; rm -rf "$d"; exit "$rc"'
                            binary=a.candidate.resolve() if mode=='grouped_on' else ROOT/'build/agentctl'
                            args=[str(binary),'--config',str(config),'--root',str(root),'--model-scope','local','--test-toolchain',BIN,'--test-sdk',SDK,'--test-isolation','off' if mode.endswith('off') else 'on','--test-command',command]
                            before=len(primary.bodies);continuous=time.clock_gettime_ns(time.CLOCK_MONOTONIC);start=time.perf_counter_ns();r=subprocess.run(args,input='run the tests\n',text=True,capture_output=True,cwd=ROOT,env=env_for(primary.url),timeout=60);wall=(time.perf_counter_ns()-start)/1e6;continuous_ms=(time.clock_gettime_ns(time.CLOCK_MONOTONIC)-continuous)/1e6
                            receipt=tool_results(primary.bodies[-1]) if len(primary.bodies)>before+1 else '';stages=[json.loads(v) for v in re.findall('CC_PHASE (.*)',receipt)];total=[s for s in stages if s['label']=='total'];compiler=[s for s in stages if s['label']!='total']
                            expected='RESULT='+str(CASES[case]['expected']);passed=r.returncode==0 and len(primary.bodies)==before+2 and 'CPP_TASK_DONE' in r.stdout and expected in receipt and len(total)==1 and len(compiler)==CASES[case]['units']+1+(recipe=='pch') and all(s['exit']==0 for s in stages) and 'exit=0 signal=0 timeout=0 io_error=0' in receipt
                            (a.out/f'{rep}-{case}-{mode}.log').write_text(r.stdout+r.stderr+'\nRECEIPT\n'+receipt)
                            passed=passed and abs(continuous_ms-wall)<100
                            row=dict(kind='cpp_cli',case=case,units=CASES[case]['units'],mode=mode,rep=rep,phase='warmup' if rep<2 else 'evaluation',passed=bool(passed),wall_ms=wall,continuous_ms=continuous_ms,clock_gap_ms=continuous_ms-wall,inner_ms=total[0]['wall_ms'] if total else 0,requests=len(primary.bodies)-before,correct_output=expected in receipt)
                            for label in ['pch','object','link']:
                                group=[s for s in compiler if s['label']==label]
                                for field in ['wall_ms','user_ms','sys_ms']:row[label+'_'+field]=sum(s[field] for s in group)
                            row['pch_file_bytes']=sum(s['output_file_bytes'] for s in compiler if s['label']=='pch')
                            row['outside_ms']=wall-row['inner_ms'];row['inside_other_ms']=row['inner_ms']-sum(s['wall_ms'] for s in compiler)
                            rows.append(row);out.write(json.dumps(row,separators=(',',':'))+'\n');out.flush()
                            if not passed:raise RuntimeError(f'{case}/{mode} failed; raw rows retained')
                    print('block complete',rep,flush=True)
    finally:primary.close()
    (a.out/'rows.json').write_text(json.dumps(rows,indent=2)+'\n');print('PASS',len(rows),'rows',flush=True)
if __name__=='__main__':main()
