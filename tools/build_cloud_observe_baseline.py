#!/usr/bin/env python3
"""Reconstruct E62 old-prompt CLI with the same current trace instrumentation."""
import argparse
from pathlib import Path
import shlex
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def baseline_source(text):
    edits=[('"你是工作区代码助手。"','"你是本地代码助手。"'),
           ('        "不得虚报成功。";','        "整文件写入 args 必须是路径、换行、完整内容，不能仅给路径。不得虚报成功。";'),
           ('    if (create || write) CTL_APPEND("整文件写入 args 必须是路径、换行、完整内容，不能仅给路径。");\n','')]
    for old,new in edits:
        if text.count(old)!=1:raise ValueError('candidate source contract changed; audit baseline reconstruction')
        text=text.replace(old,new)
    return text

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);args=ap.parse_args()
    if args.out.exists():ap.error('fresh --out required')
    args.out.mkdir(parents=True);source=args.out.resolve()/'agentctl-before.c';source.write_text(baseline_source((ROOT/'tests/agentctl.c').read_text()))
    subprocess.run(['make','build/agentctl'],cwd=ROOT,check=True)
    lines=subprocess.check_output(['make','-Bn','build/agentctl'],cwd=ROOT,text=True).splitlines()
    line=next(s for s in lines if ' -o build/agentctl ' in s);cmd=shlex.split(line)
    cmd[cmd.index('-o')+1]=str(args.out.resolve()/'agentctl-before');cmd[cmd.index('tests/agentctl.c')]=str(source);cmd.insert(1,'-Itests')
    subprocess.run(cmd,cwd=ROOT,check=True)
if __name__=='__main__':main()
