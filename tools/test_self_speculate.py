#!/usr/bin/env python3
"""Build/run model-free D-SPEC security/state-machine acceptance in a fresh directory."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--sanitize',action='store_true');args=ap.parse_args()
    if args.out.exists():ap.error('fresh --out required')
    args.out.mkdir(parents=True);commands=[];objects=[];sources=['tests/agent_basic.c','tests/agent_policy.c','tests/agent_sandbox.c','tests/agent_speculate.cpp','tests/test_self_speculate.cpp']
    flags=['-g','-O1' if args.sanitize else '-O2','-Wall','-Wextra','-Wpedantic','-I'+str(ROOT/'include'),'-I'+str(ROOT/'tests')]
    if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    def run(command):
        commands.append(command);result=subprocess.run(command,text=True,capture_output=True)
        with (args.out/'build.log').open('a') as log:log.write(result.stdout+result.stderr)
        if result.returncode:raise SystemExit('build failed; logs retained')
    for i,name in enumerate(sources):
        obj=args.out/f'{i}.o';cpp=name.endswith('.cpp');run(['c++' if cpp else 'cc','-std=c++17' if cpp else '-std=c11',*flags,'-c',str(ROOT/name),'-o',str(obj)]);objects.append(str(obj))
    binary=args.out/'test-self-speculate';run(['c++',*flags,*objects,'-pthread','-o',str(binary)])
    manifest={'sanitizers':args.sanitize,'commands':commands,'sources_sha256':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in sources}}
    (args.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    with (args.out/'rows.jsonl').open('w') as stdout,(args.out/'stderr.log').open('w') as stderr:result=subprocess.run([str(binary.resolve())],stdout=stdout,stderr=stderr)
    if result.returncode:raise SystemExit('acceptance failed; all logs retained')
    rows=[json.loads(line) for line in (args.out/'rows.jsonl').read_text().splitlines()];assert rows[-1]['cases']==24
    print('24 model-free D-SPEC cases passed'+(' (ASan/UBSan)' if args.sanitize else ''))


if __name__=='__main__':main()
