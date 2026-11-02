#!/usr/bin/env python3
"""Build a diagnostic regex-union candidate without changing production sources."""
import argparse,hashlib,json,shlex,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OLD='''    char regex[512];for(uint32_t i=0;i<agent_policy_sensitive_patterns();i++) {
        REQUIRE(agent_policy_sensitive_regex(i,regex,sizeof regex));
        REQUIRE(emit(j,"(deny file-read* file-write* file-map-executable (regex #\\\"%s\\\"))\\n",regex));
    }
'''
NEW='''    /* Experimental bounded union; all original boundaries are verified. */
    const char head[]="(^|/)(",tail[]=")(/|$)";
    char regex[512],piece[512];uint32_t patterns=agent_policy_sensitive_patterns();
    for(uint32_t i=0;i<patterns;) {
        size_t used=sizeof head-1;memcpy(regex,head,used);uint32_t first=i;
        while(i<patterns) {
            REQUIRE(agent_policy_sensitive_regex(i,piece,sizeof piece));size_t n=strlen(piece);
            REQUIRE(n>=sizeof head+sizeof tail-2&&!memcmp(piece,head,sizeof head-1)&&!memcmp(piece+n-(sizeof tail-1),tail,sizeof tail-1));
            size_t body=n-(sizeof head-1)-(sizeof tail-1),separator=i>first?1:0;
            if(used+separator+body+sizeof tail>sizeof regex)break;
            if(separator)regex[used++]='|';memcpy(regex+used,piece+sizeof head-1,body);used+=body;i++;
        }
        REQUIRE(i>first);memcpy(regex+used,tail,sizeof tail);
        REQUIRE(emit(j,"(deny file-read* file-write* file-map-executable (regex #\\\"%s\\\"))\\n",regex));
    }
'''
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
    baseline=ROOT/'tests/agent_sandbox.c';s=baseline.read_text();assert s.count(OLD)==1
    source=a.out/'agent_sandbox_grouped.c';source.write_text(s.replace(OLD,NEW,1));obj=a.out/'agent_sandbox.o'
    lines=subprocess.run(['make','-n','-W','tests/agent_sandbox.c','build/agentctl'],cwd=ROOT,capture_output=True,text=True,check=True).stdout.splitlines()
    compile=next(shlex.split(v) for v in lines if '-c -o tests/agent_sandbox.o' in v)
    compile[compile.index('tests/agent_sandbox.o')]=str(obj);compile[compile.index('tests/agent_sandbox.c')]=str(source);compile+=['-Itests']
    link=next(shlex.split(v) for v in lines if '-o build/agentctl ' in v)
    link[link.index('tests/agent_sandbox.o')]=str(obj);link[link.index('build/agentctl')]=str(a.out/'agentctl')
    with (a.out/'compile.log').open('w') as out:
        for command in [compile,link]:subprocess.run(command,cwd=ROOT,stdout=out,stderr=out,check=True)
    sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    data=dict(baseline_source_sha256=sha(baseline),candidate_source_sha256=sha(source),candidate_binary_sha256=sha(a.out/'agentctl'),baseline_binary_sha256=sha(ROOT/'build/agentctl'),production_sources_changed=False)
    (a.out/'manifest.json').write_text(json.dumps(data,indent=2)+'\n');print('Built experimental candidate only')
if __name__=='__main__':main()
