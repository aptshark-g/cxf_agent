#!/usr/bin/env python3
"""Paired real-cloud read-only CLI observations; profile secrets remain in memory.

Use fresh --out. Public export is numeric/synthetic metadata only; never publish
private trace/stdout. No redirects, response injection, retries or cache reset.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
import urllib.error
import urllib.request
from check_versioned_edit_live import profile_from, INITIAL

ROOT = Path(__file__).resolve().parents[1]
FIELDS = ('prompt_tokens', 'completion_tokens', 'total_tokens', 'prompt_cache_hit_tokens', 'prompt_cache_miss_tokens')
TASKS = {
    'direct': '当前只有读取权限。请先用 fs.read 读取 unit.c，然后说明 value() 的返回值。若没有 fs.edit 权限，明确说明无法修改。不要调用写入、命令工具或 fs.list；只读一次即可。',
    'listed': '当前只有读取权限。先用 fs.list 列出根目录，再用 fs.read 读取 unit.c，说明 value() 返回值。若没有 fs.edit 权限，明确说明无法修改。不要调用写入或命令工具；每个工具只调用一次。',
}

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def encode(body, mode):
    return (json.dumps(body) if mode == 'before' else json.dumps(body, ensure_ascii=False, separators=(',', ':'))).encode('utf-8')

def usage_of(body):
    usage = body.get('usage')
    usage = usage if isinstance(usage, dict) else {}
    issues = []
    def count(key, value):
        if value is None: return None
        if type(value) is int and value >= 0: return value
        issues.append('invalid_'+key); return None
    result = {key: count(key, usage.get(key)) for key in FIELDS}
    details = usage.get('prompt_tokens_details')
    result['cached_tokens'] = count('cached_tokens', details.get('cached_tokens') if isinstance(details, dict) else None)
    a, b = result['prompt_cache_hit_tokens'], result['cached_tokens']
    if a is not None and b is not None and a != b: issues.append('cache_hit_conflict')
    p,c,t = (result[k] for k in FIELDS[:3])
    if None not in (p,c,t) and p+c != t: issues.append('total_conflict')
    h,m = result['prompt_cache_hit_tokens'],result['prompt_cache_miss_tokens']
    if None not in (p,h,m) and h+m != p: issues.append('cache_sum_conflict')
    result['usage_issues'] = issues
    return result

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs): return None

class ObservedRelay:
    def __init__(self,profile,mode):
        self.profile,self.mode = profile,mode
        import secrets
        self.local_auth='local-relay-'+secrets.token_hex(20)
        self.rows=[];self.requests=[]
        self.opener=urllib.request.build_opener(NoRedirect())
        owner=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*_args): pass
            def do_POST(self):
                entered=time.monotonic_ns()
                if self.headers.get('Authorization') != owner.local_auth: self.send_error(403);return
                try: size=int(self.headers.get('Content-Length','0'))
                except ValueError: self.send_error(400);return
                if not 0<size<=16384 or len(owner.rows)>=12: self.send_error(413);return
                raw=self.rfile.read(size);read_end=time.monotonic_ns()
                try: body=json.loads(raw)
                except (ValueError,UnicodeError):self.send_error(400);return
                parsed=time.monotonic_ns()
                owner.requests.append(body) # Private synthetic capture, never exported.
                body.update(model=profile['id'],stream=False,max_tokens=2048,thinking={'type':'disabled'})
                row={'kind':'cloud_round','round':len(owner.rows)+1,'mode':mode,'client_request_bytes':len(raw),
                     'system_utf8_bytes':len(body['messages'][0]['content'].encode('utf-8')),
                     'read_ms':(read_end-entered)/1e6,'parse_ms':(parsed-read_end)/1e6}
                status,payload=owner.request(body,row)
                ready=time.monotonic_ns()
                self.send_response(status);self.send_header('Content-Type','application/json; charset=utf-8')
                self.send_header('Content-Length',str(len(payload)));self.send_header('Connection','close');self.end_headers()
                try:self.wfile.write(payload);self.wfile.flush();row['delivered']=True
                except (BrokenPipeError,ConnectionResetError):row['delivered']=False
                done=time.monotonic_ns()
                row.update(http_status=status,delivery_ms=(done-ready)/1e6,relay_ms=(done-entered)/1e6)
                row['relay_local_ms']=row['relay_ms']-row['api_wait_ms']
                owner.rows.append(row)
        self.server=HTTPServer(('127.0.0.1',0),Handler)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
    @property
    def url(self): return f'http://127.0.0.1:{self.server.server_port}/v1/chat/completions'
    def request(self,body,row):
        begin=time.monotonic_ns();wire=encode(body,self.mode);encoded=time.monotonic_ns()
        row.update(upstream_request_bytes=len(wire),serialize_ms=(encoded-begin)/1e6)
        key=self.profile['apiKey'];auth=key if key.lower().startswith('bearer ') else 'Bearer '+key
        request=urllib.request.Request(self.profile['url'],data=wire,headers={'Content-Type':'application/json; charset=utf-8','Authorization':auth})
        start=time.monotonic_ns();headers=start
        row['error']=None
        try:
            with self.opener.open(request,timeout=90) as response:
                headers=time.monotonic_ns();status=response.status;payload=response.read(65537)
            done=time.monotonic_ns()
        except urllib.error.HTTPError as error:
            done=headers=time.monotonic_ns();status=error.code;payload=b'{"error":"upstream rejected request"}';row['error']='http_rejected'
        except Exception:
            done=headers=time.monotonic_ns();status=502;payload=b'{"error":"upstream transport failure"}';row['error']='transport'
        row.update(api_headers_ms=(headers-start)/1e6,api_body_ms=(done-headers)/1e6,api_wait_ms=(done-start)/1e6,response_bytes=len(payload))
        filter_start=time.monotonic_ns()
        variants=[key.encode(),key.removeprefix('Bearer ').encode()]
        variants += [base64.b64encode(v) for v in variants]+[v.hex().encode() for v in variants]
        if len(payload)>65536:status,payload=502,b'{"error":"response capacity"}';row['error']='capacity'
        elif any(v and v in payload for v in variants):status,payload=502,b'{"error":"credential withheld"}';row['error']='credential_withheld'
        try: response_body=json.loads(payload)
        except (ValueError,UnicodeError): response_body={};row['error']=row['error'] or 'invalid_json'
        if not isinstance(response_body,dict):response_body={};row['error']=row['error'] or 'invalid_json_shape'
        row.update(usage_of(response_body));row['filter_usage_ms']=(time.monotonic_ns()-filter_start)/1e6
        return status,payload
    def close(self):
        self.server.shutdown();self.server.server_close();self.thread.join(timeout=5)

def timing_rows(trace):
    return [json.loads(s[len('[timing] '):]) for s in trace.splitlines() if s.startswith('[timing] ')]

def run_task(profile,binary,mode,case,workspace,out,block,warmup,order):
    out.mkdir(mode=0o700);relay=ObservedRelay(profile,mode);config=out/'agentctl.local.json'
    with config.open('x') as stream:
        os.chmod(config,0o600);json.dump({'api':{'url':relay.url,'model':profile['id'],'key':relay.local_auth,'timeout_ms':100000}},stream)
    trace=out/'trace.txt'
    cmd=[str(binary.resolve()),'--config',str(config.resolve()),'--root',str(workspace.resolve()),'--model-scope','cloud','--cloud-egress','workspace','--rounds','8','--trace',str(trace.resolve())]
    env={k:v for k,v in os.environ.items() if not k.startswith(('CXF_','OPENAI_','ANTHROPIC_','DEEPSEEK_'))}
    started=time.monotonic_ns()
    try:
        p=subprocess.run(cmd,input=TASKS[case]+'\n',text=True,capture_output=True,timeout=240,env=env)
        exit_code=p.returncode;stdout,stderr=p.stdout,p.stderr
    except subprocess.TimeoutExpired as e:
        exit_code='timeout';stdout=e.stdout or b'';stderr=e.stderr or b''
        stdout=stdout.decode(errors='replace') if isinstance(stdout,bytes) else stdout
        stderr=stderr.decode(errors='replace') if isinstance(stderr,bytes) else stderr
    finally:
        ended=time.monotonic_ns();relay.close();config.unlink()
    (out/'stdout.txt').write_text(stdout);(out/'stderr.txt').write_text(stderr)
    text=trace.read_text() if trace.exists() else '';timings=timing_rows(text)
    providers=[r for r in timings if r['kind']=='provider'];tools=[r for r in timings if r['kind']=='tool']
    calls=[line.split()[1] for line in text.splitlines() if line.startswith('CALL ')]
    expected=['fs.read'] if case=='direct' else ['fs.list','fs.read']
    task={'kind':'cloud_task','case':case,'mode':mode,'block':block,'warmup':warmup,'order':order,'agent_exit':exit_code,
          'task_ms':(ended-started)/1e6,'api_calls':len(relay.rows),'tool_calls':calls,'tools_expected':calls==expected,
          'file_unchanged':(workspace/'unit.c').read_text()==INITIAL,'timing_valid':len(providers)==len(relay.rows) and all(r['valid'] for r in timings),
          'tool_exec_ms':sum(r['exec_ns'] for r in tools)/1e6,'tool_policy_ms':sum(r['before_ns']+r['after_ns'] for r in tools)/1e6,
          'provider_ms':sum(r['total_ns'] for r in providers)/1e6,'api_wait_ms':sum(r['api_wait_ms'] for r in relay.rows),
          'relay_local_ms':sum(r['relay_local_ms'] for r in relay.rows),'client_request_bytes':sum(r['client_request_bytes'] for r in relay.rows),
          'upstream_request_bytes':sum(r['upstream_request_bytes'] for r in relay.rows)}
    task['passed']=exit_code==0 and task['file_unchanged'] and task['tools_expected'] and task['timing_valid'] and all(r['http_status']==200 and not r['error'] for r in relay.rows)
    task['client_residual_ms']=task['provider_ms']-sum(r['relay_ms'] for r in relay.rows)
    task['outside_provider_tool_ms']=task['task_ms']-task['provider_ms']-task['tool_exec_ms']-task['tool_policy_ms']
    for i,row in enumerate(relay.rows):
        row.update(case=case,block=block,warmup=warmup,order=order,provider_ms=providers[i]['total_ns']/1e6 if i<len(providers) else None)
        row['client_residual_ms']=row['provider_ms']-row['relay_ms'] if row['provider_ms'] is not None else None
    (out/'rounds.json').write_text(json.dumps(relay.rows,indent=2)+'\n')
    (out/'private-requests.json').write_text(json.dumps(relay.requests,ensure_ascii=False,indent=2)+'\n')
    (out/'result.json').write_text(json.dumps(task,indent=2)+'\n')
    return task,relay.rows

def pairs(tasks):
    result=[]
    for case in TASKS:
        for b in sorted({t['block'] for t in tasks if t['case']==case and not t['warmup']}):
            a,c=[next(t for t in tasks if t['case']==case and t['block']==b and t['mode']==m) for m in ('before','after')]
            r={'case':case,'block':b,'both_passed':a['passed'] and c['passed']}
            for field in ('task_ms','api_wait_ms','tool_exec_ms','tool_policy_ms','relay_local_ms','client_residual_ms','outside_provider_tool_ms','client_request_bytes','upstream_request_bytes','api_calls'):
                r[field+'_delta']=c[field]-a[field]
            result.append(r)
    return result

def summary(tasks):
    result=[]
    for case in TASKS:
        group=[r for r in pairs(tasks) if r['case']==case]
        r={'case':case,'pairs':len(group),'after_slower':sum(v['task_ms_delta']>0 for v in group)}
        for field in group[0]:
            if field.endswith('_delta'):
                values=[v[field] for v in group];r[field]={'median':statistics.median(values),'min':min(values),'max':max(values)}
        result.append(r)
    return result

def replay(requests):
    old=requests[0]['messages'][0]['content'];new=requests[1]['messages'][0]['content']
    base=requests[0]
    rows=[]
    for prompt,system in [('before',old),('after',new)]:
        body=json.loads(json.dumps(base));body['messages'][0]['content']=system
        body.update(max_tokens=2048,stream=False,thinking={'type':'disabled'})
        for encoding in ['ascii_spaced','ascii_compact','utf8_compact']:
            opts={} if encoding=='ascii_spaced' else {'separators':(',',':'),'ensure_ascii':encoding=='ascii_compact'}
            wire=json.dumps(body,**opts).encode('utf-8');assert json.loads(wire)==body
            for block in range(10):
                start=time.perf_counter_ns()
                for _ in range(1000):json.dumps(body,**opts).encode('utf-8')
                rows.append({'kind':'serialization','prompt':prompt,'encoding':encoding,'block':block,'warmup':block==0,'iterations':1000,'request_bytes':len(wire),'serialize_us':(time.perf_counter_ns()-start)/1000/1000})
    return rows

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--models-file',type=Path,required=True);ap.add_argument('--profile',required=True)
    ap.add_argument('--baseline',type=Path,required=True);ap.add_argument('--candidate',type=Path,default=ROOT/'build/agentctl')
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    if args.out.exists():ap.error('fresh --out required')
    args.out.mkdir(parents=True,mode=0o700)
    try: profile=profile_from(args.models_file,args.profile)
    except Exception as e: raise SystemExit('profile loading failed: '+type(e).__name__)
    tasks=[];rounds=[];identity={'baseline_binary_sha256':sha(args.baseline),'candidate_binary_sha256':sha(args.candidate),'runner_sha256':sha(Path(__file__))}
    (args.out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    for case in TASKS:
        workspace=args.out/(case+'-workspace');workspace.mkdir();(workspace/'unit.c').write_text(INITIAL)
        for block in range(5):
            for order,mode in enumerate(['before','after'] if block%2==0 else ['after','before']):
                out=args.out/f'{case}-{block}-{mode}'
                task,rows=run_task(profile,args.baseline if mode=='before' else args.candidate,mode,case,workspace,out,block,block==0,order)
                tasks.append(task);rounds.extend(rows)
                (args.out/'progress.json').write_text(json.dumps({'tasks':tasks,'rows':rounds},indent=2)+'\n')
                print(json.dumps({k:task[k] for k in ('case','mode','block','passed','task_ms','api_calls','upstream_request_bytes')}),flush=True)
    requests=[json.loads((args.out/f'direct-0-{m}/private-requests.json').read_text())[0] for m in ('before','after')]
    data={'model':profile['id'],'thinking':'disabled','max_tokens':2048,'identity':identity,'tasks':tasks,'rows':rounds,'serialization_rows':replay(requests),'pairs':pairs(tasks),'summary':summary(tasks),'passed':all(t['passed'] for t in tasks)}
    (args.out/'summary.json').write_text(json.dumps(data,indent=2)+'\n')
    print(json.dumps({'completed_tasks':len(tasks),'rounds':len(rounds),'passed':data['passed']}))
if __name__=='__main__':main()
