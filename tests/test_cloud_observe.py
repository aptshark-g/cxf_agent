#!/usr/bin/env python3
"""Offline usage/serialization/forwarding tests and real-loopback CLI prompt/timing checks."""
import argparse
import base64
import io
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import urllib.error
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from bench_cloud_observe import encode,usage_of,ObservedRelay,timing_rows,NoRedirect
from test_agentctl_repl import Fixture,env_for,tool_call
ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'build/agentctl'

def local_tests():
    body={'messages':[{'role':'system','content':'中文 café 😀\n"\\'},{'role':'assistant','tool_calls':[{'function':{'arguments':json.dumps({'args':'unit.c\n中文\\n'},ensure_ascii=False)}}]}]}
    a,b=encode(body,'before'),encode(body,'after')
    assert json.loads(a)==json.loads(b)==body and len(b)<len(a) and '中文'.encode() in b
    assert all(usage_of({})[k] is None for k in ('prompt_tokens','cached_tokens','prompt_cache_hit_tokens'))
    invalid=usage_of({'usage':{'prompt_tokens':True,'completion_tokens':-1,'total_tokens':'4'}})
    assert len(invalid['usage_issues'])==3
    valid=usage_of({'usage':{'prompt_tokens':10,'completion_tokens':3,'total_tokens':13,'prompt_cache_hit_tokens':0,'prompt_cache_miss_tokens':10,'prompt_tokens_details':{'cached_tokens':0}}})
    assert not valid['usage_issues'] and valid['prompt_cache_hit_tokens']==0
    assert usage_of({'usage':{'prompt_tokens_details':{'cached_tokens':2}}})['prompt_cache_hit_tokens'] is None
    conflicts=usage_of({'usage':{'prompt_tokens':10,'completion_tokens':3,'total_tokens':14,'prompt_cache_hit_tokens':3,'prompt_cache_miss_tokens':6,'prompt_tokens_details':{'cached_tokens':2}}})
    assert set(conflicts['usage_issues'])=={'total_conflict','cache_hit_conflict','cache_sum_conflict'}
    print('PASS: UTF-8/nested JSON equivalence and missing/zero/invalid/conflicting usage')
    class Response(io.BytesIO):status=200
    class Opener:
        def __init__(self,payload=None,error=None):self.payload=payload;self.error=error;self.calls=0
        def open(self,request,timeout):
            self.calls+=1;self.wire=request.data;self.content_type=request.get_header('Content-type')
            if self.error:raise self.error
            return Response(self.payload)
    profile={'id':'fixture-model','apiKey':'-'.join(['fixture','credential','for','test']),'url':'http://127.0.0.1:9/chat/completions'}
    for mode in ['before','after']:
        relay=object.__new__(ObservedRelay);relay.profile=profile;relay.mode=mode
        for payload,error,expected in [(b'{"usage":{"prompt_tokens":1}}',None,None),(b'{}',None,None),(b'[]',None,'invalid_json_shape'),(b'x'*65537,None,'capacity'),(json.dumps({'text':profile['apiKey']}).encode(),None,'credential_withheld'),(base64.b64encode(profile['apiKey'].encode()),None,'credential_withheld'),(None,urllib.error.HTTPError(profile['url'],429,'denied',{},None),'http_rejected'),(None,TimeoutError(),'transport')]:
            relay.opener=Opener(payload,error);row={};status,result=relay.request(body,row)
            assert row['error']==expected and relay.opener.calls==1
            assert json.loads(relay.opener.wire)==body and 'charset=utf-8' in relay.opener.content_type
            assert row['api_wait_ms']>=0 and row['serialize_ms']>=0
            assert profile['apiKey'].encode() not in result
            if expected and expected!='invalid_json_shape':assert status>=400
    assert NoRedirect().redirect_request(None,None,None,None,None,None) is None
    print('PASS: both encodings, HTTP/timeout/capacity/credential/shape failures, no retries/redirects')

def cli_tests():
    fixture=Fixture(['DONE']*48+[tool_call('fs.read','unit.c'),'DONE','DONE'])
    try:
        with tempfile.TemporaryDirectory(prefix='cxf-cloud-work-') as td,tempfile.TemporaryDirectory(prefix='cxf-cloud-config-') as cd:
            root=Path(td);(root/'unit.c').write_text('hello\n');config=Path(cd)/'empty.json';config.write_text('{}')
            base=[str(BIN),'--config',str(config),'--root',str(root),'--model-scope','local']
            for flags in itertools.product([False,True],repeat=6):
                cmd=base.copy()
                for flag,on,value in zip(['--diff','--create','--write','--append','--net','--test-command'],flags,[td,td,td,td,'127.0.0.1:9','true']):
                    if on:cmd += [flag,value]
                p=subprocess.run(cmd,input='Inspect.\n',text=True,capture_output=True,timeout=10,env=env_for(fixture.url))
                if flags[1] and flags[2]:
                    assert p.returncode!=0 and '--create' in p.stderr and '--write' in p.stderr
                    continue
                assert p.returncode==0,p.stderr
                system=fixture.bodies[-1]['messages'][0]['content'];tools={t['function']['name'] for t in fixture.bodies[-1]['tools']}
                assert len(system.encode('utf-8'))<512
                assert ('整文件写入 args' in system)==(flags[1] or flags[2])
                assert ('fs.edit' in system)==flags[2] and ('fs.edit' in tools)==flags[2]
                assert system.startswith('你是工作区代码助手。') and '每轮只调一个工具' in system
                if not any(flags):assert not any(x in system for x in ['写入','fs.edit','fs.append'])
            trace=Path(cd)/'trace.txt'
            p=subprocess.run(base+['--trace',str(trace)],input='Read unit.c\n',text=True,capture_output=True,timeout=10,env=env_for(fixture.url))
            ts=timing_rows(trace.read_text());assert p.returncode==0 and len(ts)==3,p.stderr
            assert [t['kind'] for t in ts]==['provider','tool','provider']
            assert ts[1]['name']=='impl/fs.read.fs' and all(t['valid'] and t['total_ns']>=t['exec_ns'] for t in ts)
            for t in ts:assert t['total_ns']==t['before_ns']+t['exec_ns']+t['after_ns']
            p=subprocess.run(base,input='Done\n',text=True,capture_output=True,timeout=10,env=env_for(fixture.url))
            assert p.returncode==0 and '[timing]' not in p.stdout+p.stderr
            print('PASS: all64 capability combinations /512B budget and traced real tool/untraced default')
    finally:fixture.close()
if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--binary',type=Path,default=BIN);BIN=ap.parse_args().binary.resolve()
    local_tests();cli_tests();print('cloud-observe: 3 suites (64 capability combinations,16 forwarding paths) passed')
