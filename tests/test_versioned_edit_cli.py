#!/usr/bin/env python3
"""Actual CLI registration, dynamic versions and conflict recovery with local scripted decisions."""
import json
import re
from pathlib import Path
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler,HTTPServer
from test_agentctl_repl import env_for,tool_call
ROOT=Path(__file__).resolve().parents[1]


def execute(case):
    with tempfile.TemporaryDirectory(prefix='cxf-edit-cli-') as workspace,tempfile.TemporaryDirectory(prefix='cxf-edit-config-') as configs:
        root=Path(workspace);target=root/'unit.c';initial='old one\nold two\n';target.write_text(initial)
        (root/'.env').write_text('SYNTHETIC_FILE_SHOULD_NOT_LEAK\n');config=Path(configs)/'agentctl.local.json';config.write_text('{}\n');trace=Path(configs)/'trace.txt'
        state={'calls':0,'bodies':[],'errors':[],'conflict_seen':False,'applied':False}
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*_args):pass
            def do_POST(self):
                body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));state['bodies'].append(body);state['calls']+=1
                try:
                    tools=[t['function']['name'] for t in body.get('tools',[])];assert ('fs.edit' in tools)==(case!='readonly')
                    results=[m.get('content','') for m in body['messages'] if m.get('role')=='tool'];last=results[-1] if results else ''
                    if state['calls']==1:
                        if case in ['readonly','protected']:
                            name='unit.c' if case=='readonly' else '.env';reply=tool_call('fs.edit',f'{name}\nv1:0\n<<<<<<< SEARCH\nold\n=======\nnew\n>>>>>>> REPLACE')
                        else:reply=tool_call('fs.read','unit.c')
                    elif '#file unit.c ' in last and not state['applied']:
                        version=re.search(r' version=(\S+)',last).group(1)
                        if case=='conflict' and not state.get('mutated'):
                            target.write_text(initial+'external\n');state['mutated']=True
                        reply=tool_call('fs.edit',f'unit.c\n{version}\n<<<<<<< SEARCH\nold one\nold two\n=======\nnew one\nnew two\n>>>>>>> REPLACE')
                    elif 'status=conflict' in last:
                        state['conflict_seen']=True;reply=tool_call('fs.read','unit.c')
                    elif 'status=applied' in last:
                        state['applied']=True;reply=tool_call('fs.read','unit.c')
                    else:reply={'choices':[{'message':{'role':'assistant','content':'DONE'}}]}
                    if 'choices' not in reply:reply={'choices':[{'message':{'role':'assistant',**reply}}]}
                    payload=json.dumps(reply).encode();self.send_response(200)
                except Exception as error:
                    state['errors'].append(type(error).__name__);payload=b'{"error":"fixture assertion"}';self.send_response(500)
                self.send_header('Content-Length',str(len(payload)));self.send_header('Connection','close');self.end_headers();self.wfile.write(payload)
        server=HTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        command=[str(ROOT/'build/agentctl'),'--config',str(config),'--root',workspace,'--model-scope','local','--rounds','10','--trace',str(trace)]
        if case!='readonly':command+=['--write',workspace]
        try:p=subprocess.run(command,input='Edit unit.c using fs.edit.\n',text=True,capture_output=True,env=env_for(f'http://127.0.0.1:{server.server_port}/v1/chat/completions','fixture-model'),timeout=30)
        finally:server.shutdown();server.server_close();thread.join(timeout=5)
        assert not state['errors'],state['errors']
        text=trace.read_text() if trace.exists() else ''
        if case in ['normal','conflict']:
            assert p.returncode==0 and state['applied'],p.stderr
            assert target.read_text()=='new one\nnew two\n'+('external\n' if case=='conflict' else '')
            if case=='conflict':assert state['conflict_seen']
        else:
            assert target.read_text()==initial and 'status=applied' not in text
            assert 'SYNTHETIC_FILE_SHOULD_NOT_LEAK' not in json.dumps(state['bodies'])+text+p.stdout+p.stderr
            assert (root/'.env').read_text()=='SYNTHETIC_FILE_SHOULD_NOT_LEAK\n'
        assert not list(root.glob('.cxf-patch-*.tmp'))
        print('PASS: '+case)


if __name__=='__main__':
    for case in ['normal','conflict','readonly','protected']:execute(case)
    print('versioned-edit CLI: 4 suites passed')
