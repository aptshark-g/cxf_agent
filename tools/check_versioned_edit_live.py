#!/usr/bin/env python3
"""Real cloud-model fs.edit acceptance on synthetic workspaces; secrets stay in memory.

Reads one explicitly selected WorkBuddy profile; never edits it or logs its URL,
headers or credential. Local trace/stdout remain under a fresh private build dir.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import secrets
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler,HTTPServer
import urllib.error
import urllib.request
from urllib.parse import urlsplit

ROOT=Path(__file__).resolve().parents[1]
INITIAL='#include <stdio.h>\nstatic int value(void) {\n    int a = 19;\n    int b = 20;\n    return a + b;\n}\nint main(void) { printf("%d\\n", value()); return 0; }\n'
TEST='cc unit.c -o unit && actual=$(./unit) && printf "actual=%s\\n" "$actual" && test "$actual" = 42'


class Relay:
    def __init__(self,profile,workspace=None,conflict=False):
        self.profile=profile;self.workspace=workspace;self.conflict=conflict;self.mutated=False
        self.calls=0;self.statuses=[];self.events=[];self.local_auth='local-relay-'+secrets.token_hex(20)
        owner=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*_args):pass
            def do_POST(self):
                if self.headers.get('Authorization')!=owner.local_auth:
                    self.send_error(403);return
                size=int(self.headers.get('Content-Length','0'))
                if not 0<size<=16384:self.send_error(413);return
                body=json.loads(self.rfile.read(size));owner.calls+=1
                if owner.calls>16:self.send_error(429);return
                if owner.conflict and not owner.mutated and any(m.get('role')=='tool' and '#file unit.c ' in m.get('content','') for m in body.get('messages',[])):
                    target=owner.workspace/'unit.c';target.write_text(target.read_text()+'// concurrent writer: preserve this line\n')
                    owner.mutated=True;owner.events.append({'event':'external_writer_after_read','before_api_call':owner.calls})
                body.update(model=profile['id'],stream=False,max_tokens=2048,thinking={'type':'disabled'})
                status,payload=owner.request(body)
                self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(payload)));self.send_header('Connection','close');self.end_headers()
                try:self.wfile.write(payload)
                except (BrokenPipeError,ConnectionResetError):pass
        self.server=HTTPServer(('127.0.0.1',0),Handler)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
    @property
    def url(self):return f'http://127.0.0.1:{self.server.server_port}/v1/chat/completions'
    def request(self,body):
        key=self.profile['apiKey'];auth=key if key.lower().startswith('bearer ') else 'Bearer '+key
        request=urllib.request.Request(self.profile['url'],data=json.dumps(body,ensure_ascii=False,separators=(',',':')).encode('utf-8'),headers={'Content-Type':'application/json; charset=utf-8','Authorization':auth})
        try:
            with urllib.request.urlopen(request,timeout=90) as response:
                status=response.status;payload=response.read(65537)
            if len(payload)>65536:status,payload=502,b'{"error":"upstream response capacity"}'
            variants=[key.encode(),key.removeprefix('Bearer ').encode()]
            variants+=[base64.b64encode(v) for v in variants]+[v.hex().encode() for v in variants]
            if any(v and v in payload for v in variants):status,payload=502,b'{"error":"credential content withheld"}'
        except urllib.error.HTTPError as error:
            status=error.code;payload=json.dumps({'error':'upstream rejected request','status':status}).encode()
        except Exception as error:
            status=502;payload=json.dumps({'error':'upstream transport failure','type':type(error).__name__}).encode()
        self.statuses.append(status);return status,payload
    def close(self):self.server.shutdown();self.server.server_close();self.thread.join(timeout=5)


def profile_from(path,identifier):
    entries=json.loads(path.read_text());matches=[m for m in entries if m.get('id')==identifier]
    if len(matches)!=1:raise ValueError('profile ID must select exactly one entry')
    profile=matches[0];parsed=urlsplit(profile['url'])
    if parsed.scheme not in ('http','https') or not parsed.hostname or parsed.username or parsed.password or not parsed.path.endswith('/chat/completions'):
        raise ValueError('explicit chat-completions endpoint required')
    if not profile.get('apiKey') or not profile.get('supportsToolCall'):raise ValueError('credential and tool-capable profile required')
    return profile


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--models-file',type=Path,required=True);ap.add_argument('--profile',required=True);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--binary',type=Path,default=ROOT/'build/agentctl');ap.add_argument('--probe',action='store_true')
    args=ap.parse_args()
    if args.out.exists():ap.error('fresh --out required')
    args.out.mkdir(parents=True,mode=0o700)
    try:profile=profile_from(args.models_file,args.profile)
    except Exception as error:raise SystemExit('profile loading failed: '+type(error).__name__)
    if args.probe:
        relay=Relay(profile)
        try:status,payload=relay.request({'model':profile['id'],'messages':[{'role':'user','content':'Reply OK.'}],'stream':False,'max_tokens':16,'thinking':{'type':'disabled'}})
        finally:relay.close()
        result={'kind':'connectivity','model':profile['id'],'status':status,'json_response':False}
        try:result['json_response']=bool(json.loads(payload).get('choices'))
        except Exception:pass
        (args.out/'probe.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result));return
    reports=[]
    for case in ['normal','concurrent','readonly']:
        out=args.out/case;out.mkdir(mode=0o700);workspace=out/'workspace';workspace.mkdir();(workspace/'unit.c').write_text(INITIAL)
        relay=Relay(profile,workspace,case=='concurrent');config=out/'agentctl.local.json'
        with config.open('x') as stream:os.chmod(config,0o600);json.dump({'api':{'url':relay.url,'model':profile['id'],'key':relay.local_auth,'timeout_ms':100000}},stream)
        trace=out/'trace.txt';cmd=[str(args.binary.resolve()),'--config',str(config.resolve()),'--root',str(workspace.resolve()),'--model-scope','cloud','--cloud-egress','workspace','--rounds','14','--trace',str(trace.resolve())]
        if case!='readonly':cmd+=['--write',str(workspace.resolve()),'--test-write',str(workspace.resolve()),'--test-command',TEST,'--test-timeout-ms','15000','--test-toolchain','/Library/Developer/CommandLineTools/usr/bin','--test-sdk','/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'] if os.uname().sysname=='Darwin' else ['--write',str(workspace.resolve())]
        question='请用 fs.read 读取 unit.c，然后仅用 fs.edit 将 value 函数中的 a 改为 20、b 改为 22，作为一个多行块替换。必须原样复制版本号，冲突时重读后重新编辑。不要使用 fs.write_apply、fs.replace_line 或 fs.append。随后 test.run 验证输出42，最后再用 fs.read 复查。只有真实测试通过才能报告通过。'
        if case=='readonly':question='当前只有读取权限。请检查 unit.c；如果没有 fs.edit 权限，明确说明无法修改。不要尝试其他写入或命令工具。'
        started=time.monotonic()
        env={k:v for k,v in os.environ.items() if not k.startswith(('CXF_','OPENAI_','ANTHROPIC_','DEEPSEEK_'))}
        try:
            proc=subprocess.run(cmd,input=question+'\n',text=True,capture_output=True,timeout=300,env=env)
            (out/'stdout.txt').write_text(proc.stdout);(out/'stderr.txt').write_text(proc.stderr);exit_code=proc.returncode
        except subprocess.TimeoutExpired as error:
            exit_code='timeout'
            for name,value in [('stdout.txt',error.stdout),('stderr.txt',error.stderr)]:
                (out/name).write_text(value.decode(errors='replace') if isinstance(value,bytes) else value or '')
        finally:relay.close();config.unlink()
        elapsed=time.monotonic()-started;text=trace.read_text() if trace.exists() else '';content=(workspace/'unit.c').read_text()
        call_lines=[s for s in text.splitlines() if s.startswith('CALL ')]
        names=[s.split()[1] for s in call_lines if len(s.split())>1]
        if case=='readonly':independent=content==INITIAL
        else:
            verified=subprocess.run(['/bin/sh','-c',TEST],cwd=workspace,text=True,capture_output=True,timeout=20)
            (out/'independent-test.txt').write_text(verified.stdout+verified.stderr);independent=verified.returncode==0
        saw_conflict='status=conflict' in text;saw_applied='status=applied' in text
        tested='test.run' in names and '#test exit=0 signal=0 timeout=0 io_error=0' in text
        passed=exit_code==0 and independent
        if case!='readonly':passed=passed and 'fs.edit' in names and saw_applied and tested and not any(s in names for s in ['fs.write_apply','fs.replace_line','fs.append'])
        if case=='concurrent':passed=passed and relay.mutated and saw_conflict and names.count('fs.read')>=3 and '// concurrent writer: preserve this line' in content
        report={'kind':'model_task','case':case,'model':profile['id'],'passed':bool(passed),'agent_exit':exit_code,'api_calls':relay.calls,'http_statuses':relay.statuses,
                'tool_calls':names,'saw_conflict':saw_conflict,'saw_applied':saw_applied,'model_ran_passing_test':tested,'independent_valid':independent,'external_writer':relay.mutated,
                'elapsed_seconds':round(elapsed,3),'latency_is_not_tool_overhead':True}
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');reports.append(report);print(json.dumps(report),flush=True)
    summary={'model':profile['id'],'thinking':'disabled','max_tokens':2048,'tasks':reports,'passed':all(r['passed'] for r in reports),'limits':['Three synthetic tasks, not general agent capability.','Cloud timings include network/model/relay; excluded from tool microbenchmarks.','Upstream transport follows the explicitly selected local profile; no TLS guarantee for HTTP.']}
    (args.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')


if __name__=='__main__':main()
