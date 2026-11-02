#!/usr/bin/env python3
"""E54: serial same-Xcode controls; no permission grants or global cache edits."""
import argparse,hashlib,json,os,re,shutil,statistics,subprocess,tempfile,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
XCODE='/Applications/Xcode.app/Contents/Developer'
BIN=XCODE+'/Toolchains/XcodeDefault.xctoolchain/usr/bin'
SDK=XCODE+'/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
CASES={
 'noop':('./phase noop /usr/bin/true',['off','on']),
 'version':('./phase version /usr/bin/cc --version',['off','on']),
 'compile':('./phase compile /usr/bin/cc unit.c -o unit && ./unit',['off','on','quiet']),
 'cold_env':('d=$(mktemp -d) || exit 2; HOME="$d" TMPDIR="$d" ./phase cold /usr/bin/cc unit.c -o cold; rc=$?; rm -rf "$d"; exit $rc',['off','on']),
 'reuse':('./phase first /usr/bin/cc unit.c -o first && ./phase second /usr/bin/cc unit.c -o second',['off','on']),
 'invalidate':('./phase first /usr/bin/cc unit.c -o first && rm -f "$TMPDIR/xcrun_db" && ./phase second /usr/bin/cc unit.c -o second',['on']),
 'sdk_only':(f'SDKROOT={SDK} ./phase sdk /usr/bin/cc unit.c -o sdk',['off','on']),
 'pinned':(f'PATH={BIN}:/usr/bin:/bin:/usr/sbin:/sbin SDKROOT={SDK} ./phase pinned cc unit.c -o pinned && ./pinned',['off','on']),
 'platform':(f'./phase platform {XCODE}/usr/bin/xcodebuild -sdk {SDK} -version PlatformPath',['off','on']),
}
def hashes():
    files=[ROOT/'Makefile',ROOT/'cxf.sh',Path(__file__),ROOT/'tools/probe_sandbox_cc.c',ROOT/'tools/probe_cc_phase.c']
    for folder in ['include','src','tests']:
        files.extend(p for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.cpp','.mm','.py'))
    return {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(files))}
def summarize(rows):
    result=[]
    for case,mode in sorted({(r['case'],r['mode']) for r in rows}):
        group=[r for r in rows if (r['case'],r['mode'],r['phase'])==(case,mode,'evaluation')]
        out=dict(case=case,mode=mode,n=len(group))
        for key in ['wall_ms','prepare_ms','cleanup_ms','spawn_ms','phase_first_ms','phase_second_ms','outside_phase_ms']:
            values=[r[key] for r in group if key in r]
            if values:out[key]=dict(median=statistics.median(values),min=min(values),max=max(values))
        result.append(out)
    return result
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--driver',type=Path,required=True);ap.add_argument('--quiet-driver',type=Path,required=True);ap.add_argument('--phase-probe',type=Path,required=True);ap.add_argument('--reps',type=int,default=5);a=ap.parse_args()
    a.out.mkdir(parents=True,exist_ok=False);source=hashes();(a.out/'source-sha256.json').write_text(json.dumps(source,indent=2)+'\n')
    with zipfile.ZipFile(a.out/'measured-source.zip','x',zipfile.ZIP_DEFLATED) as z:
        for name in source:z.write(ROOT/name,name)
    rows=[]
    with tempfile.TemporaryDirectory(prefix='cxf-cc-diagnostic-') as td:
        root=Path(td);shutil.copy2(a.phase_probe,root/'phase');(root/'unit.c').write_text('int main(void){return 0;}\n')
        with (a.out/'rows.jsonl').open('w') as out:
            names=list(CASES)
            for rep in range(a.reps+2):
                order=names[rep%len(names):]+names[:rep%len(names)]
                if rep%2:order=order[::-1]
                for case in order:
                    command,modes=CASES[case];modes=modes[rep%len(modes):]+modes[:rep%len(modes)]
                    for mode in modes:
                        driver=a.quiet_driver if mode=='quiet' else a.driver
                        p=subprocess.run([str(driver.resolve()),str(root),'0' if mode=='off' else '1',command],capture_output=True,text=True,env={'PATH':'/usr/bin:/bin','LANG':'C'},timeout=40)
                        output=p.stdout+p.stderr;(a.out/f'{rep}-{case}-{mode}.log').write_text(output)
                        metric=re.search(r'CC_METRICS (.*)',output);phases=[json.loads(v) for v in re.findall(r'CC_PHASE (.*)',output)]
                        passed=p.returncode==0 and metric is not None and len(phases)==(2 if case in ('reuse','invalidate') else 1) and all(v['exit']==0 for v in phases) and 'exit=0 signal=0 timeout=0 io_error=0' in output
                        row=dict(kind='cc_diagnostic',case=case,mode=mode,rep=rep,phase='warmup' if rep<2 else 'evaluation',passed=passed,output_sha256=hashlib.sha256(output.encode()).hexdigest(),fsevents_errors=output.count('Failed to start fs event stream'),cache_errors=output.count('DARWIN_USER_CACHE_DIR'))
                        if metric:row.update(json.loads(metric.group(1)))
                        if phases:
                            row['phase_first_ms']=phases[0]['wall_ms'];row['phase_first_user_ms']=phases[0]['user_ms'];row['phase_first_sys_ms']=phases[0]['sys_ms']
                            row['outside_phase_ms']=row['wall_ms']-sum(v['wall_ms'] for v in phases)
                        if len(phases)>1:row['phase_second_ms']=phases[1]['wall_ms']
                        rows.append(row);out.write(json.dumps(row,separators=(',',':'))+'\n');out.flush()
                        if not passed:raise RuntimeError(f'failed {case}/{mode}; artifacts retained')
                print('block complete',rep,flush=True)
    (a.out/'rows.json').write_text(json.dumps(rows,indent=2)+'\n');(a.out/'summary.json').write_text(json.dumps(summarize(rows),indent=2)+'\n');print('PASS',len(rows),'rows',flush=True)
if __name__=='__main__':main()
