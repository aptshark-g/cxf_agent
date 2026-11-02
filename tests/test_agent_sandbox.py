#!/usr/bin/env python3
"""OS file/network isolation acceptance; synthetic files, only local receivers."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import tempfile
from test_agentctl_repl import Fixture, env_for, tool_call
import test_agent_data_policy as data
ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'build/agentctl'
PROBE=ROOT/'build/sandbox_probe'
PRIVATE='PRIVATE_TEST_DATA_71829'
XCODE=Path('/Applications/Xcode.app/Contents/Developer')
TOOLCHAIN=XCODE/'Toolchains/XcodeDefault.xctoolchain/usr/bin'
SDK=XCODE/'Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'


def run(root,command,extra=(),fds=(),parent_marker=False):
    primary=Fixture([tool_call('test.run'),'SANDBOX_DONE'])
    try:
        config=root/'runtime.json';config.write_text(json.dumps({'api':{'url':primary.url,'model':'fixture','key':''}}))
        args=[str(BIN),'--config',str(config),'--model-scope','local','--root',str(root),
              '--test-command',command,'--rounds','4',*map(str,extra)]
        env=env_for(primary.url)
        if parent_marker:env={'PATH':'/usr/bin:/bin','LANG':'C','CXF_MODEL_SCOPE':'local','CXF_SANDBOX_PARENT_MARKER':'PARENT_ENV_SYNTHETIC_71391'}
        p=subprocess.run(args,input='run tests\n',text=True,capture_output=True,cwd=ROOT,
                         env=env,timeout=40,pass_fds=fds)
        assert p.returncode==0 and len(primary.bodies)==2,(p.returncode,p.stderr)
        result=data.tool_results(primary.bodies[-1])
        return p,result,json.dumps(primary.bodies)
    finally:primary.close()


def setup(root):
    shutil.copy2(PROBE,root/'probe')
    (root/'ordinary.txt').write_text('readable source\n')


def test_normal_read_private_tmp_and_explicit_write():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root)
        p,r,_=run(root,'cat ordinary.txt; printf private > "$TMPDIR/normal"; cat "$TMPDIR/normal"')
        assert 'exit=0 ' in r and 'readable source' in r and 'private' in r
        p,r,_=run(root,'./probe write denied.txt');assert 'exit=3 ' in r and not (root/'denied.txt').exists()
        p,r,_=run(root,'printf "int main(void){return 0;}" > unit.c; cc unit.c -o unit && ./unit && printf COMPILED',['--test-write',root])
        assert 'exit=0 ' in r and 'COMPILED' in r and (root/'unit').exists()


def test_sensitive_names_reads_writes_and_descendants():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root)
        names=['.env','.ENV.prod','service.key','CERT.pem','keys.json','local.local.toml','ID_RSA.pub']
        for name in names:(root/name).write_text(PRIVATE)
        for name in names:
            q=shlex.quote(name)
            command=f'./probe read {q} && printf BAD_READ; ./probe write {q} && printf BAD_WRITE; ./probe child {q} && printf BAD_CHILD; true'
            _,r,bodies=run(root,command,['--test-write',root])
            assert PRIVATE not in r+bodies and not any(x in r for x in ['BAD_READ','BAD_WRITE','BAD_CHILD'])
            assert (root/name).read_text()==PRIVATE


def test_outside_reads_and_explicit_grants():
    with tempfile.TemporaryDirectory() as td:
        base=Path(td);root=base/'work';root.mkdir();outside=base/'outside';outside.mkdir();setup(root)
        f=outside/'ordinary.txt';f.write_text(PRIVATE)
        _,r,_=run(root,'./probe read '+shlex.quote(str(f)));assert 'exit=3 ' in r and PRIVATE not in r
        _,r,_=run(root,'./probe read '+shlex.quote(str(f)),['--test-read',outside]);assert PRIVATE in r
        _,r,_=run(root,'./probe write '+shlex.quote(str(f)),['--test-read',outside]);assert 'exit=3 ' in r and f.read_text()==PRIVATE
        _,r,_=run(root,'./probe write '+shlex.quote(str(f)),['--test-write',outside]);assert 'exit=0 ' in r and f.read_text()=='modified'
        # APFS Data volume aliases must not gain the broad /System read allowance.
        alias=Path('/System/Volumes/Data')/str(f.resolve()).lstrip('/')
        if alias.exists():
            _,r,_=run(root,'./probe read '+shlex.quote(str(alias)));assert 'exit=3 ' in r


def test_rename_symlink_hardlink_and_config_inode():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root);(root/'.env').write_text(PRIVATE)
        (root/'sym').symlink_to(root/'.env');os.link(root/'.env',root/'alias')
        command='cat sym alias; mv .env public.txt; ln .env new-alias; ln ordinary.txt ordinary-alias; true'
        _,r,bodies=run(root,command,['--test-write',root])
        assert PRIVATE not in r+bodies and (root/'.env').read_text()==PRIVATE and not (root/'public.txt').exists() and not (root/'new-alias').exists() and not (root/'ordinary-alias').exists()
        # Arbitrary-name config is protected even with a pre-existing hardlink.
        (root/'runtime.json').write_text('{}');os.link(root/'runtime.json',root/'config-alias')
        _,r,_=run(root,'cat config-alias >/dev/null && printf BAD_ALIAS; mv runtime.json moved.json && printf BAD_RENAME; true',['--test-write',root])
        assert 'BAD_' not in r and not (root/'moved.json').exists()


def test_protected_parent_rename_and_new_sensitive_files():
    from test_agent_data_policy import launch
    primary=Fixture([tool_call('test.run'),'PARENT_DONE'])
    try:
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);setup(root);(root/'slot').mkdir();(root/'normaldir').mkdir()
            command='mv normaldir ordinarydir; mv slot renamed; ./probe write future.key; true'
            p=launch(primary,root,config_name='slot/settings.json',extra=['--test-write',td,'--test-command',command])
            assert p.returncode==0 and (root/'ordinarydir').exists() and (root/'slot/settings.json').exists()
            assert not (root/'renamed').exists() and not (root/'future.key').exists()
    finally:primary.close()


def test_inherited_file_descriptors_closed():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root);secret=root/'.env';secret.write_text(PRIVATE)
        fd=os.open(secret,os.O_RDONLY)
        try:
            _,r,bodies=run(root,f'./probe fd {fd}',fds=(fd,))
            assert 'exit=3 ' in r and PRIVATE not in r+bodies
        finally:os.close(fd)


def test_tcp_udp_and_unix_default_deny_and_grants():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root)
        for kind in ['tcp','udp','unix']:
            for permission in ['deny','allow']:
                family=socket.AF_UNIX if kind=='unix' else socket.AF_INET
                receiver=socket.socket(family,socket.SOCK_DGRAM if kind=='udp' else socket.SOCK_STREAM)
                try:
                    if kind=='unix':
                        address=str(root/'receiver.sock');receiver.bind(address);arg=shlex.quote(address)
                    else:
                        receiver.bind(('127.0.0.1',0));arg=str(receiver.getsockname()[1])
                    if kind!='udp':receiver.listen(1)
                    receiver.settimeout(.08)
                    _,r,_=run(root,f'./probe {kind} {arg}',['--test-net',permission])
                    expected=permission=='allow'
                    got=False
                    try:
                        if kind=='udp':got=receiver.recv(16)==b'probe'
                        else:
                            conn,_=receiver.accept()
                            with conn:got=conn.recv(16)==b'probe'
                    except socket.timeout:pass
                    assert got==expected,(kind,permission,r)
                    assert ('exit=0 ' in r)==expected,(kind,permission,r)
                finally:
                    receiver.close()
                    if kind=='unix':(root/'receiver.sock').unlink()


def test_unsupported_loopback_mode_refuses_before_execution():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root);primary=Fixture([])
        try:
            p=data.launch(primary,root,extra=['--test-net','loopback','--test-command','touch SHOULD_NOT_RUN'])
            assert p.returncode==2 and not (root/'SHOULD_NOT_RUN').exists() and not primary.bodies
        finally:primary.close()


def test_profile_quoting_and_private_tmp_cleanup():
    with tempfile.TemporaryDirectory() as td:
        base=Path(td);root=base/'quoted " path';root.mkdir();setup(root);outside=base/'outside';outside.write_text('preserve')
        command='printf "%s\\n" "$TMPDIR"; ln -s '+shlex.quote(str(outside))+' "$TMPDIR/link"; printf work > "$TMPDIR/file"; cat ordinary.txt'
        _,r,_=run(root,command)
        assert 'exit=0 ' in r and 'readable source' in r and outside.read_text()=='preserve'
        import re
        paths=re.findall(r'/private/tmp/cxf-exec-[^/\s]+/',r)
        assert paths and all(not Path(p).exists() for p in paths)


def test_parent_environment_denied_even_with_network_grant():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root)
        _,r,_=run(root,'./probe parent "$PPID"',['--test-isolation','off'],parent_marker=True)
        assert 'PARENT_SCAN_RC=0' in r and 'FOUND_MARKER=1' in r
        for extra in [[],['--test-net','allow']]:
            _,r,_=run(root,'./probe parent "$PPID"',extra,parent_marker=True)
            assert 'FOUND_MARKER=0' in r and 'exit=0 ' in r


def test_off_negative_control_and_no_network_implicit_grant():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root);(root/'.env').write_text(PRIVATE)
        _,r,_=run(root,'./probe read .env',['--test-isolation','off']);assert PRIVATE in r
        _,r,bodies=run(root,'./probe read .env',['--test-net','allow','--test-write',root]);assert PRIVATE not in r+bodies and 'exit=3 ' in r


def test_capacity_invalid_grant_refuses_before_side_effect():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root);primary=Fixture([tool_call('test.run'),'REFUSED'])
        try:
            p=data.launch(primary,root,extra=['--test-write',str(root/'missing'),'--test-command','touch SHOULD_NOT_RUN'])
            assert p.returncode==0 and not (root/'SHOULD_NOT_RUN').exists()
            assert '#fail' in data.tool_results(primary.bodies[-1])
        finally:primary.close()


def test_pinned_same_xcode_compile_and_security():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root);(root/'.env').write_text(PRIVATE)
        (root/'unit.c').write_text('#include <stdio.h>\nint main(void){puts("PINNED_OK");return 0;}\n')
        flags=['--test-toolchain',TOOLCHAIN,'--test-sdk',SDK]
        receiver=socket.socket();receiver.bind(('127.0.0.1',0));receiver.listen(1);receiver.settimeout(.08)
        try:
            command='cc --version; printf "SDK=%s\\n" "$SDKROOT"; cc unit.c -o "$TMPDIR/unit" && "$TMPDIR/unit"; ./probe read .env && printf BAD_SECRET; ./probe read runtime.json && printf BAD_CONFIG; ./probe tcp '+str(receiver.getsockname()[1])+' && printf BAD_NET; ./probe parent "$PPID"; true'
            _,r,bodies=run(root,command,flags,parent_marker=True)
            assert 'PINNED_OK' in r and 'FOUND_MARKER=0' in r and PRIVATE not in r+bodies and 'BAD_' not in r
            assert 'InstalledDir: '+str(TOOLCHAIN) in r and 'SDK='+str(SDK.resolve()) in r
            try:receiver.accept();raise AssertionError('bound toolchain unexpectedly got network')
            except socket.timeout:pass
            assert not (root/'unit').exists() and (root/'.env').read_text()==PRIVATE
        finally:receiver.close()


def test_binding_does_not_grant_directories_or_inherit_old_path():
    with tempfile.TemporaryDirectory() as td:
        base=Path(td);root=base/'work';root.mkdir();setup(root);outside=base/'outside';outside.mkdir()
        shutil.copy2(PROBE,outside/'boundprobe');(outside/'ordinary.txt').write_text(PRIVATE)
        flags=['--test-toolchain',outside,'--test-sdk',outside]
        _,r,bodies=run(root,'boundprobe read "$SDKROOT/ordinary.txt" && printf BAD_READ; cat "$SDKROOT/ordinary.txt" && printf BAD_CAT; true',flags)
        assert PRIVATE not in r+bodies and 'BAD_' not in r
        _,r,_=run(root,'printf "%s\\n%s\\n" "$PATH" "$SDKROOT"',flags+['--test-env','inherit'])
        assert str(outside.resolve())+':/usr/bin:/bin:/usr/sbin:/sbin' in r and str(outside.resolve()) in r


def test_invalid_binding_fails_before_side_effect():
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);setup(root)
        for bad in [root/'missing',root/'ordinary.txt','relative','/tmp/a:b','/tmp/a\nb']:
            _,r,_=run(root,'touch SHOULD_NOT_RUN',['--test-write',root,'--test-toolchain',TOOLCHAIN,'--test-sdk',bad])
            assert '#fail' in r and not (root/'SHOULD_NOT_RUN').exists()
        primary=Fixture([])
        try:
            for flag in ['--test-toolchain','--test-sdk']:
                p=data.launch(primary,root,extra=[flag,str(root),'--test-command','touch SHOULD_NOT_RUN'])
                assert p.returncode==2 and not primary.bodies and not (root/'SHOULD_NOT_RUN').exists()
        finally:primary.close()


def main():
    global BIN,PROBE
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,default=BIN);parser.add_argument('--probe',type=Path,default=PROBE);parser.add_argument('--report',type=Path)
    args=parser.parse_args();BIN=args.binary.resolve();PROBE=args.probe.resolve();data.BIN=BIN
    passed=[]
    for name,case in list(globals().items()):
        if name.startswith('test_') and callable(case):case();passed.append(name);print('PASS:',name,flush=True)
    if args.report:args.report.write_text(json.dumps({'passed':True,'cases':passed,'platform':'macOS Seatbelt; synthetic secrets, real files/processes and loopback receivers; no real cloud'},indent=2)+'\n')
    print('test.run sandbox:',len(passed),'suites passed')
if __name__=='__main__':main()
