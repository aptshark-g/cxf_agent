#!/usr/bin/env python3
"""Local native CLI smoke test: actual authorized compiler tool, no HTTP config."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',type=Path,required=True);ap.add_argument('--model',type=Path,required=True);ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument("--scripted-actions",action="store_true",help="use the compiled deterministic Native-parser CLI harness")
    ap.add_argument("--policy",choices=["late","immediate","timed","progress"],default="timed")
    ap.add_argument("--snapshot-release",choices=["return","early"],default="return")
    args=ap.parse_args();out=args.out.resolve()
    if out.exists():ap.error('fresh output required')
    out.mkdir(parents=True);project=out/'workspace';project.mkdir()
    (project/'check.c').write_text('int main(void){return 0;}\n')
    env=os.environ.copy();env['CXF_LLM_SYSTEM']='Call test.run exactly once with args="" to run configured tests. After receiving its tool result, report the actual result briefly. Do not call any other tool.'
    command=[str(args.binary.resolve()),'--llama-model',str(args.model.resolve()),'--llama-backend',str(args.runtime.resolve()),'--llama-context','8192','--llama-max-tokens','256',
             '--root',str(project),'--test-command','printf x >> invocations; cc check.c -o check && ./check','--rounds','4','--tool-prepare',args.policy,'--tool-kv','restore','--tool-snapshot-release',args.snapshot_release]
    p=subprocess.run(command,input='Run the configured tests once and tell me the result.\n',text=True,capture_output=True,env=env,timeout=120)
    (out/'stdout.txt').write_text(p.stdout);(out/'stderr.txt').write_text(p.stderr)
    receipts=re.findall(r'\[tool-prepare\].*prepare_rc=(-?\d+)',p.stderr)
    invocations=len((project/'invocations').read_bytes()) if (project/'invocations').exists() else 0
    result=dict(passed=bool(receipts) and all(int(rc)==0 for rc in receipts) and invocations==len(receipts),
                native_cli_returncode=p.returncode,tool_wrapper_calls=len(receipts),actual_process_executions=invocations,
                model_completed=None if args.scripted_actions else p.returncode==0,model_obeyed_once=None if args.scripted_actions else invocations==1,
                action_source="scripted" if args.scripted_actions else "model",
                scope='Native CLI registration/store/worker/tool path with deterministic Native-parser actions' if args.scripted_actions else 'Native CLI registration/worker/tool path; single model smoke probe, not autonomous capability benchmark',
                stdout_sha256=hashlib.sha256(p.stdout.encode()).hexdigest(),stderr_sha256=hashlib.sha256(p.stderr.encode()).hexdigest())
    (out/'validation.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)


if __name__=='__main__':main()
