#!/usr/bin/env python3
"""Build/run deterministic versioned-edit security/failure acceptance, optionally ASan/UBSan."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--sanitize',action='store_true');args=ap.parse_args()
    if args.out.exists():ap.error('fresh --out required')
    args.out.mkdir(parents=True);commands=[]
    names=['tests/agent_basic.c','tests/agent_policy.c','tests/agent_sandbox.c','tests/test_versioned_edit.c']
    command=['cc','-std=c11','-g','-O1' if args.sanitize else '-O2','-Wall','-Wextra','-Wpedantic','-DCXF_EDIT_TEST','-I'+str(ROOT/'include'),'-I'+str(ROOT/'tests')]
    if args.sanitize:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    command +=[str(ROOT/n) for n in names]+['-o',str(args.out/'test-versioned-edit')];commands.append(command)
    result=subprocess.run(command,text=True,capture_output=True);(args.out/'compile.log').write_text(result.stdout+result.stderr)
    if result.returncode:raise SystemExit('compile failed; logs retained')
    with (args.out/'rows.jsonl').open('w') as stdout,(args.out/'stderr.log').open('w') as stderr:
        result=subprocess.run([str((args.out/'test-versioned-edit').resolve())],stdout=stdout,stderr=stderr)
    manifest={'sanitizers':args.sanitize,'commands':commands,'source_sha256':{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in names+['tests/agent_basic.h']}}
    (args.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    if result.returncode:raise SystemExit('tests failed; all partial rows and stderr retained')
    rows=[json.loads(s) for s in (args.out/'rows.jsonl').read_text().splitlines()]
    assert rows[-1]['passed'] and rows[-1]['cases']>=33
    print(f"versioned-edit: {rows[-1]['cases']} cases passed"+(' (ASan/UBSan)' if args.sanitize else ''))


if __name__=='__main__':main()
