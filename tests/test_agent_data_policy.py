#!/usr/bin/env python3
"""Data policy acceptance: synthetic credentials, scripted providers, real tools."""
import argparse
import base64
import json
import os
from pathlib import Path
import subprocess
import tempfile
from test_agentctl_repl import Fixture, env_for, tool_call
from test_agent_foundation import Local, response, tool_results
ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'build/agentctl'
SECRET = 'synthetic-primary-value-8291'
AUX = 'synthetic-aux-value-4052'


def launch(primary, root, task='ordinary task\n', extra=(), scope='local', egress='deny', local=None,
           config_name='runtime.json', repl=False, trace=None):
    config = root / config_name
    config.write_text(json.dumps({'api': {'url': primary.url, 'model': 'fixture', 'key': SECRET}}))
    env = env_for(primary.url)
    env.pop('CXF_MODEL_SCOPE', None)
    env.pop('CXF_NET_SCOPE', None)
    env.pop('CXF_CLOUD_EGRESS', None)
    env['CXF_LLM_KEY'] = SECRET
    args = [str(BIN), '--config', str(config), '--root', str(root), '--rounds', '32', '--budget', '65536']
    if scope: args += ['--model-scope', scope]
    if egress: args += ['--cloud-egress', egress]
    if local:
        path = root / 'aux-runtime.json'
        path.write_text(json.dumps({'api': {'url': local.url, 'model': 'aux', 'key': AUX}}))
        args += ['--local-config', str(path)]
    if repl: args += ['--repl']
    if trace: args += ['--trace', str(trace)]
    return subprocess.run(args + list(extra), input=task, text=True, capture_output=True,
                          env=env, cwd=ROOT, timeout=30, check=False)


def clean(proc, trace=None, forbidden=()):
    view = proc.stdout + proc.stderr + (trace.read_text() if trace and trace.exists() else '')
    for value in (SECRET, AUX, *forbidden):
        assert value not in view, 'sensitive bytes reached a user-visible surface'


def bodies_clean(receiver, forbidden=(SECRET, AUX)):
    text = json.dumps(receiver.bodies)
    for value in forbidden:
        assert value not in text, 'sensitive bytes reached an HTTP body'


def test_default_cloud_zero_requests():
    primary = Fixture(['UNEXPECTED'])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            for scope in (None, 'cloud'):
                proc=launch(primary,root,scope=scope,egress=None)
                assert proc.returncode != 0 and not primary.bodies and not primary.headers
                clean(proc)
    finally: primary.close()


def test_task_scope_plain_text_and_reject_call():
    primary = Fixture(['TASK_OK', tool_call('fs.write_apply', 'owned.txt\nowned')])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            proc=launch(primary,root,scope='cloud',egress='task')
            assert proc.returncode==0 and 'TASK_OK' in proc.stdout
            proc=launch(primary,root,scope='cloud',egress='task',extra=['--write',td])
            assert proc.returncode != 0 and not (root/'owned.txt').exists()
            assert len(primary.bodies)==2 and all('tools' not in b for b in primary.bodies)
            clean(proc);bodies_clean(primary)
    finally: primary.close()


def test_workspace_read_edit_test_roundtrip():
    primary=Fixture([tool_call('fs.read','unit.txt'),tool_call('fs.replace_line','unit.txt\nold\nnew'),
                     tool_call('test.run'),'WORKSPACE_OK'])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);(root/'unit.txt').write_text('old\n')
            proc=launch(primary,root,scope='cloud',egress='workspace',extra=['--write',td,'--test-command','grep -q new unit.txt && printf verified'])
            assert proc.returncode==0 and 'WORKSPACE_OK' in proc.stdout
            assert len(primary.bodies)==4 and (root/'unit.txt').read_text()=='new\n'
            assert 'verified' in tool_results(primary.bodies[-1])
            clean(proc);bodies_clean(primary)
    finally: primary.close()


def test_sensitive_tools_and_config_aliases():
    # Unit tests cover the full filename matrix; exercise every operation through the real host here.
    calls=[('fs.read','.env'),('fs.diff','.env\nchanged'),('fs.write_apply','.env\nchanged'),
           ('fs.append','.env\nchanged'),('fs.replace_line','.env\nprotected\nchanged'),
           ('fs.read','runtime.json'),('fs.read','alias.json'),
           ('fs.read','link.json'),('fs.read','active.trace'),('fs.list','.'),('fs.grep','SENSITIVE_FILE_MARKER')]
    primary=Fixture([*(tool_call(*c) for c in calls),'FILES_OK'])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);(root/'.env').write_text('protected\nSENSITIVE_FILE_MARKER\n')
            # The active config is an arbitrary filename and has a hardlink alias before startup.
            (root/'runtime.json').write_text('{}');os.link(root/'runtime.json',root/'alias.json')
            (root/'link.json').symlink_to(root/'.env')
            trace=root/'active.trace'
            proc=launch(primary,root,extra=['--write',td,'--append',td],trace=trace)
            assert proc.returncode==0 and 'FILES_OK' in proc.stdout and len(primary.bodies)==len(calls)+1, (proc.returncode,proc.stderr.replace(SECRET,'WITHHELD'),len(primary.bodies))
            assert (root/'.env').read_text()=='protected\nSENSITIVE_FILE_MARKER\n'
            results=tool_results(primary.bodies[-1])
            assert 'SENSITIVE_FILE_MARKER' not in results and '#fail' in results
            assert 'f .env\n' not in results and 'f active.trace\n' not in results
            clean(proc,trace);bodies_clean(primary)
    finally: primary.close()


def test_tool_credentials_do_not_enter_context_or_cloud():
    transforms=['printf "%s" "$CXF_LLM_KEY"',
                'printf "%s" "$CXF_LLM_KEY" | base64',
                'printf "%s" "$CXF_LLM_KEY" | od -An -tx1 | tr -d " \\n"']
    for command in transforms:
        primary=Fixture([tool_call('test.run'),'WITHHELD_OK'])
        try:
            with tempfile.TemporaryDirectory() as td:
                root=Path(td);trace=root/'active.trace'
                proc=launch(primary,root,task='run check\n/context\n/quit\n',repl=True,
                            scope='cloud',egress='workspace',trace=trace,
                            extra=['--test-env','inherit','--test-command',command])
                assert proc.returncode==0 and len(primary.bodies)==2
                values=(SECRET,base64.b64encode(SECRET.encode()).decode(),SECRET.encode().hex())
                clean(proc,trace,values);bodies_clean(primary,values)
                assert 'blocked tool result' in proc.stderr and '#fail' in tool_results(primary.bodies[-1])
        finally: primary.close()


def test_input_and_response_leaks_withheld():
    for value in [SECRET,base64.b64encode(SECRET.encode()).decode(),SECRET.encode().hex(),
                  'sk-'+'example012345678901234567890123', '-----BEGIN '+'PRIVATE KEY-----']:
        primary=Fixture([value])
        try:
            with tempfile.TemporaryDirectory() as td:
                root=Path(td);trace=root/'active.trace'
                proc=launch(primary,root,task='hello\n/context\n/quit\n',repl=True,trace=trace)
                assert len(primary.bodies)==1
                clean(proc,trace,[value]);bodies_clean(primary,[value])
        finally: primary.close()
    primary=Fixture(['UNEXPECTED'])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);trace=root/'active.trace'
            proc=launch(primary,root,task=SECRET+'\n/context\n/quit\n',repl=True,trace=trace)
            assert not primary.bodies;clean(proc,trace)
    finally: primary.close()


def test_auxiliary_isolated_and_response_filtered():
    primary=Fixture([tool_call('llm.local','ordinary input'),'AUX_OK'])
    local=Local([(200,response(AUX))])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);trace=root/'active.trace'
            proc=launch(primary,root,local=local,trace=trace)
            assert proc.returncode==0 and len(primary.bodies)==2 and len(local.bodies)==1
            assert '#fail' in tool_results(primary.bodies[-1])
            assert local.headers[0]['Authorization']==AUX
            assert primary.headers[0]['Authorization']==SECRET
            clean(proc,trace);bodies_clean(primary);bodies_clean(local)
    finally: primary.close();local.close()


def test_http_independent_scope_and_body_guard():
    for scope,egress,body,expected in [(None,'deny','{"message":"ordinary"}',0),('cloud','task','{"message":"ordinary"}',0),
                ('cloud','workspace','{"message":"ordinary"}',1),('local','deny','{"message":"ordinary"}',1),
                ('local','deny',SECRET,0),('local','deny',SECRET.encode().hex(),0)]:
        receiver=Local([(200,{'ok':True})])
        primary=Fixture([tool_call('http.post','/submit\n'+body),'HTTP_OK'])
        try:
            with tempfile.TemporaryDirectory() as td:
                root=Path(td);extra=['--net',f'127.0.0.1:{receiver.server.server_port}']
                if scope:extra+=['--net-scope',scope]
                proc=launch(primary,root,extra=extra,egress=egress)
                assert len(receiver.bodies)==expected
                clean(proc);bodies_clean(receiver)
        finally: primary.close();receiver.close()


def test_create_and_auxiliary_config_are_protected():
    primary=Fixture([tool_call('fs.write_new','.env\nchanged'),tool_call('fs.read','aux-runtime.json'),'CREATE_OK'])
    local=Local([])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            proc=launch(primary,root,extra=['--create',td],local=local)
            assert proc.returncode==0 and not (root/'.env').exists() and not local.bodies
            assert '#fail' in tool_results(primary.bodies[-1])
            clean(proc);bodies_clean(primary)
    finally: primary.close();local.close()


def test_config_cannot_elevate_cloud_permission():
    from test_agentctl_repl import line_queue, read_until
    primary=Fixture(['UNEXPECTED']);proc=None
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);config=root/'arbitrary.json'
            config.write_text(json.dumps({'api':{'url':primary.url,'model':'fixture','key':SECRET}}))
            env=env_for(primary.url);env['CXF_MODEL_SCOPE']='cloud';env['CXF_CLOUD_EGRESS']='deny'
            proc=subprocess.Popen([str(BIN),'--config',str(config),'--repl','--root',td],
                                  stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                  text=True,env=env,cwd=ROOT,bufsize=1)
            out=line_queue(proc.stdout);err=line_queue(proc.stderr)
            proc.stdin.write('/status\n');proc.stdin.flush();read_until(out,'tasks    0')
            config.write_text(json.dumps({'api':{'url':primary.url,'model':'fixture','key':SECRET},
                                          'cloud_egress':'workspace','model_scope':'local'}))
            proc.stdin.write('/reload\n');proc.stdin.flush();read_until(err,'保留')
            proc.stdin.write('/status\n');proc.stdin.flush();status=read_until(out,'tasks    0')
            assert 'model-scope 2' in status and 'cloud-egress 0' in status and SECRET not in status
            proc.stdin.write('/quit\n');proc.stdin.flush();assert proc.wait(timeout=10)==0
            assert not primary.bodies
    finally:
        if proc and proc.poll() is None:proc.kill();proc.wait()
        primary.close()


def test_trace_refusal_preserves_files_and_capacity():
    primary=Fixture([])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);protected=root/'.env';protected.write_text('unchanged marker')
            proc=launch(primary,root,trace=protected)
            assert proc.returncode==2 and protected.read_text()=='unchanged marker' and not primary.bodies
            # One config plus seven trace paths fills the bounded registration table.
            traces=[root/('trace-%d'%i) for i in range(8)]
            traces[-1].write_text('keep old log')
            script=''.join('/trace '+str(p)+'\n/trace off\n' for p in traces)+'/quit\n'
            proc=launch(primary,root,task=script,repl=True)
            assert proc.returncode==0 and traces[-1].read_text()=='keep old log'
            assert '原 trace 保持不变' in proc.stderr
            config=root/'runtime.json';before=config.read_bytes()
            proc=launch(primary,root,task='/trace '+str(config)+'\n/quit\n',repl=True)
            assert proc.returncode==0 and config.read_bytes()==before and not primary.bodies
            clean(proc)
    finally: primary.close()


def test_nonloopback_never_connect():
    primary=Fixture(['UNEXPECTED'])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            for host in ('192.0.2.1','localhost.example'):
                env=env_for('http://'+host+':9/v1/chat/completions');config=root/'empty.json';config.write_text('{}')
                proc=subprocess.run([str(BIN),'--config',str(config),'--root',td],input='hello\n',text=True,
                                    capture_output=True,env=env,cwd=ROOT,timeout=10)
                assert proc.returncode!=0
            assert not primary.bodies
    finally: primary.close()


def main():
    global BIN
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,default=BIN);parser.add_argument('--report',type=Path)
    args=parser.parse_args();BIN=args.binary.resolve();passed=[]
    for name,case in list(globals().items()):
        if name.startswith('test_') and callable(case):
            case();passed.append(name);print('PASS:',name,flush=True)
    if args.report:
        args.report.write_text(json.dumps({'passed':True,'cases':passed,'scope':'Functional synthetic-secret and real HTTP/files/process checks; no cloud, model-quality or performance claim'},indent=2)+'\n')
    print('agent data policy:',len(passed),'suites passed')
if __name__=='__main__':main()
