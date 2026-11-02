#!/usr/bin/env python3
"""Optional private-PCH recipe acceptance; synthetic files and local receivers."""
import argparse,json,shutil,socket,tempfile
from pathlib import Path
import test_agent_sandbox as sandbox
ROOT=Path(__file__).resolve().parents[1]
FLAGS=['--test-toolchain',sandbox.TOOLCHAIN,'--test-sdk',sandbox.SDK]
SECRET='CPP_SYNTHETIC_PRIVATE_62941'
def setup(root):
    shutil.copy2(ROOT/'tools/run_cpp_batch.sh',root/'batch.sh')
    (root/'common.hpp').write_text('#pragma once\n#include <iostream>\n#define VALUE 111\n')
    (root/'main.cpp').write_text('int extra();int main(){std::cout<<"BATCH="<<VALUE+extra()<<"\\n";}\n')
    (root/'extra.cpp').write_text('int extra(){return 7;}\n')
def execute(root,mode='pch',extra=(),command=None):
    if command is None:command=f'/bin/sh ./batch.sh {mode} 26.0 common.hpp "$TMPDIR/unit" main.cpp extra.cpp'
    return sandbox.run(root,command,FLAGS+list(extra))
def test_plain_and_pch_normal_cleanup():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root)
        for mode in ['plain','pch']:
            command=f'/bin/sh ./batch.sh {mode} 26.0 common.hpp "$TMPDIR/unit" main.cpp extra.cpp; rc=$?; printf "LEFT="; find "$TMPDIR" -type d -name "cxf-cpp-*" | wc -l; exit "$rc"'
            _,r,_=execute(root,command=command);assert 'BATCH=118' in r and 'exit=0 ' in r and 'LEFT=       0' in r
        assert not list(root.glob('*.o')) and not list(root.glob('*.pch'))
def test_invalid_arguments_before_compiler():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root)
        for args in ['unknown 26.0 common.hpp out main.cpp','pch bad common.hpp out main.cpp','pch 26.0 missing.hpp out main.cpp','pch 26.0 common.hpp out '+' '.join(['main.cpp']*33)]:
            _,r,_=execute(root,command='/bin/sh ./batch.sh '+args);assert 'exit=2 ' in r and not (root/'out').exists()
def test_compile_failure_cleans_private_cache():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root);(root/'main.cpp').write_text('#error EXPECTED_COMPILE_FAILURE\n')
        command='/bin/sh ./batch.sh pch 26.0 common.hpp "$TMPDIR/unit" main.cpp; rc=$?; printf "LEFT="; find "$TMPDIR" -type d -name "cxf-cpp-*" | wc -l; exit "$rc"'
        _,r,_=execute(root,command=command);assert 'exit=1 ' in r and 'EXPECTED_COMPILE_FAILURE' in r and 'LEFT=       0' in r
def test_each_task_uses_current_header():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root)
        _,r,_=execute(root);assert 'BATCH=118' in r
        (root/'common.hpp').write_text('#pragma once\n#include <iostream>\n#define VALUE 222\n')
        _,r,_=execute(root);assert 'BATCH=229' in r and 'BATCH=118' not in r
def test_protected_header_and_config_not_cached():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root);(root/'.env').write_text(SECRET)
        for name in ['.env','runtime.json']:
            _,r,bodies=execute(root,command=f'/bin/sh ./batch.sh pch 26.0 {name} "$TMPDIR/unit" main.cpp')
            assert 'exit=0 ' not in r and SECRET not in r+bodies
def test_compiled_program_keeps_file_network_policy():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root);(root/'.env').write_text(SECRET)
        receiver=socket.socket();receiver.bind(('127.0.0.1',0));receiver.listen(2);receiver.settimeout(.1);port=receiver.getsockname()[1]
        try:
            (root/'common.hpp').write_text('#pragma once\n#include <cstdio>\n#include <sys/socket.h>\n#include <arpa/inet.h>\n#include <unistd.h>\n')
            (root/'main.cpp').write_text('int main(){FILE*a=fopen(".env","r"),*b=fopen("runtime.json","r");int s=socket(AF_INET,SOCK_STREAM,0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons('+str(port)+');inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);int ok=connect(s,(sockaddr*)&addr,sizeof addr)==0;if(ok)write(s,"probe",5);printf("FILE=%d CONFIG=%d NET=%d\\n",a!=nullptr,b!=nullptr,ok);if(a)fclose(a);if(b)fclose(b);close(s);}\n')
            for isolated in [True,False]:
                cmd='/bin/sh ./batch.sh pch 26.0 common.hpp "$TMPDIR/unit" main.cpp'
                if not isolated:cmd='d=$(mktemp -d); export TMPDIR="$d" HOME="$d"; '+cmd+'; rc=$?; rm -rf "$d"; exit "$rc"'
                _,r,bodies=execute(root,command=cmd,extra=[] if isolated else ['--test-isolation','off'])
                assert ('FILE=0 CONFIG=0 NET=0' if isolated else 'FILE=1 CONFIG=1 NET=1') in r and SECRET not in r+bodies
                received=False
                try:
                    conn,_=receiver.accept()
                    with conn:received=conn.recv(16)==b'probe'
                except socket.timeout:pass
                assert received== (not isolated)
        finally:receiver.close()
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,default=sandbox.BIN);p.add_argument('--report',type=Path);a=p.parse_args();sandbox.BIN=a.binary.resolve();passed=[]
    for name,test in list(globals().items()):
        if name.startswith('test_') and callable(test):test();passed.append(name);print('PASS:',name,flush=True)
    if a.report:a.report.write_text(json.dumps(dict(passed=True,cases=passed),indent=2)+'\n')
    print('private PCH recipe:',len(passed),'suites passed')
if __name__=='__main__':main()
