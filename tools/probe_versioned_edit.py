#!/usr/bin/env python3
"""Isolate file-version, advisory-lock and unchanged text-policy costs; no policy bypass."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]


def summary(rows):
    result=[]
    for case,size in sorted({(r['case'],r['file_bytes']) for r in rows if r['kind']=='timing'}):
        vals=[r['wall_ns']/r['iterations']/1000 for r in rows if r['kind']=='timing' and not r['warmup'] and r['case']==case and r['file_bytes']==size]
        result.append({'case':case,'file_bytes':size,'samples':len(vals),'wall_us':{'median':statistics.median(vals),'min':min(vals),'max':max(vals)}})
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);args=ap.parse_args()
    if args.out.exists():ap.error('fresh --out required')
    args.out.mkdir(parents=True);out=args.out.resolve()
    names=['tests/agent_basic.c','tests/agent_policy.c','tests/agent_sandbox.c','tests/probe_versioned_edit.c']
    command=['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Wpedantic','-I'+str(ROOT/'include'),'-I'+str(ROOT/'tests'),*[str(ROOT/n) for n in names],'-o',str(out/'probe')]
    build=subprocess.run(command,text=True,capture_output=True);(out/'compile.log').write_text(build.stdout+build.stderr)
    if build.returncode:raise SystemExit('build failed; logs retained')
    (out/'manifest.json').write_text(json.dumps({'commands':[command],'source_sha256':{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in names+['tools/probe_versioned_edit.py']},'platform':platform.system(),'sanitizers':False},indent=2)+'\n')
    with tempfile.TemporaryDirectory(prefix='cxf-edit-probe-') as workspace,(out/'rows.jsonl').open('w') as stdout,(out/'stderr.log').open('w') as stderr:r=subprocess.run([str(out/'probe'),workspace],stdout=stdout,stderr=stderr)
    if r.returncode:raise SystemExit('probe failed; all partial rows retained')
    rows=[json.loads(s) for s in (out/'rows.jsonl').read_text().splitlines()];assert len(rows)==153
    result={'rows':rows,'summary':summary(rows),'limits':['Independent isolated operations; do not sum medians to reconstruct full edit time.','policy_0/1/16 each measures one complete scan with0/1/16 registered synthetic secrets; production edit scans source and result.','Lock probe includes explicit unlock; production holds source lock through cleanup.','Receipt baseline byte count is calculated from the archived old format, not a model token count.','No policy/inference code changes or timing sanitizers.']}
    (out/'summary.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result['summary'],indent=2))


if __name__=='__main__':main()
