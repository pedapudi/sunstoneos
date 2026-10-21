#!/usr/bin/env python3
"""One additional WPA2 owner case; reuse frozen production objects and peer vectors."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import socket
import struct
import subprocess
import time
ROOT=Path(__file__).resolve().parents[1]
VECTOR_SHA='7ef51392ce381093e73eeab8369e9556257c700ddcac6949c2a40e9fb9857e49'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def write_json(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def packet(channel,command):
    def receive(n):
        data=b''
        while len(data)<n:
            part=channel.recv(n-len(data))
            if not part:raise ConnectionError('GDB EOF')
            data+=part
        return data
    raw=command.encode();channel.sendall(b'$'+raw+b'#'+f'{sum(raw)%256:02x}'.encode())
    while True:
        prefix=receive(1)
        if prefix==b'$':break
        if prefix!=b'+':raise RuntimeError('Unexpected GDB acknowledgment '+repr(prefix))
    response=b''
    while True:
        c=receive(1)
        if c==b'#':break
        response+=c
    check=receive(2);assert int(check,16)==sum(response)%256;channel.sendall(b'+');return response.decode()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--open-evidence',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/radio-reconnect-20261020/native')
    parser.add_argument('--vectors',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/radio-security-20261016/model/vectors.bin')
    parser.add_argument('--phase',choices=['build','run'],required=True)
    args=parser.parse_args();out=args.evidence.resolve();out.mkdir(parents=True,exist_ok=True)
    old=args.open_evidence.resolve();build=old/'build/base';prior=json.loads((old/'build-result.json').read_text())
    assert sha(Path(prior['artifact']))==prior['elf_sha256']
    if args.phase=='build':
        assert not (out/'build-result.json').exists();assert sha(args.vectors)==VECTOR_SHA
        vector_bytes=args.vectors.read_bytes();cursor=0;sizes=[]
        while cursor<len(vector_bytes):
            assert cursor+2<=len(vector_bytes);size=int.from_bytes(vector_bytes[cursor:cursor+2],'little');cursor+=2
            assert size<=256 and cursor+size<=len(vector_bytes);sizes.append(size);cursor+=size
        assert sizes==[32,16,16,16,22,99,121,155,99,131,99],sizes
        closure=json.loads((old/'compiled-closure.json').read_text());records=closure['objects']
        for name,row in records.items():assert sha(Path(name))==row['sha256'],name
        # Only the old fixture's included declaration gained optional storage.
        for name,h in closure['inputs'].items():
            if name!=str(ROOT/'checks/a733-radio-session.c'):assert sha(Path(name))==h,name
        result={'open_elf_sha256':prior['elf_sha256'],'vectors':{'path':str(args.vectors),'sha256':VECTOR_SHA},'commands':[],'reused_objects':{n:r for n,r in records.items() if not n.endswith('/checks/radio-reconnect.c.obj')}}
        commands=json.loads((old/'build/compile_commands.json').read_text())
        original=next(c for c in commands if c['file']==str(ROOT/'checks/radio-reconnect.c'))
        asm=next(c for c in commands if c['file'].endswith('/initial/firmware.S') and 'radio-session-model.dir' in c['output'])
        source=out/'wpa-vectors.S';source.write_text('.section .rodata.radio_wpa_vectors,"a"\n.balign 16\n.global radio_wpa_vectors\nradio_wpa_vectors:\n.incbin "'+str(args.vectors.resolve())+'"\n.global radio_wpa_vectors_end\nradio_wpa_vectors_end:\n')
        fixture=out/'radio-reconnect-wpa.c.obj';vectors=out/'wpa-vectors.S.obj'
        operations=[]
        for base,source_path,obj in [(original,ROOT/'checks/radio-reconnect-wpa.c',fixture),(asm,source,vectors)]:
            command=shlex.split(base['command']);command[command.index('-o')+1]=str(obj);command[command.index('-c')+1]=str(source_path)
            command+=['-MD','-MF',str(obj)+'.d'];operations.append(command)
        link=shlex.split((build/'CMakeFiles/radio-session-model.dir/link.txt').read_text())
        link[link.index('-o')+1]=str(out/'radio-reconnect-wpa.elf')
        for i,word in enumerate(link):
            if word.endswith('/checks/radio-reconnect.c.obj'):link[i]=str(fixture)
            if word.startswith('-Wl,-Map,'):link[i]='-Wl,-Map,'+str(out/'radio-reconnect-wpa.map')
        link.append(str(vectors));operations.append(link)
        for command in operations:
            run=subprocess.run(command,cwd=build,capture_output=True,text=True)
            result['commands'].append({'command':command,'directory':str(build),'returncode':run.returncode,'stdout':run.stdout,'stderr':run.stderr})
            write_json(out/'build-result.json',result)
            if run.returncode:raise SystemExit(run.returncode)
        elf=out/'radio-reconnect-wpa.elf';result.update(artifact=str(elf),elf_sha256=sha(elf));write_json(out/'build-result.json',result);print(elf)
    else:
        assert not (out/'result.json').exists()
        result=json.loads((out/'build-result.json').read_text());elf=Path(result['artifact']);assert sha(elf)==result['elf_sha256']
        symbols={};boundaries={}
        for row in subprocess.check_output(['nm','-S',str(elf)],text=True).splitlines():
            cols=row.split()
            if len(cols)==4 and cols[3]=='__assert_func':boundaries[cols[3]]=int(cols[0],16)
            if len(cols)==4 and cols[3] in ['reconnect_checkpoint_gate','reconnect_checkpoint_view','old_deletes','old_closed','eapol_sent','disconnects','connects','wpa_board']:
                assert cols[3] not in symbols;symbols[cols[3]]=(int(cols[0],16),int(cols[1],16))
        assert len(symbols)==8 and len(boundaries)==1,(symbols,boundaries)
        disk=out/'before-reconnect.qcow2';subprocess.run(['/usr/bin/qemu-img','create','-f','qcow2',str(disk),'16M'],check=True,capture_output=True)
        qmp_path=out/'qmp.sock';gdb_path=out/'gdb.sock'
        accepted=json.loads((Path.home()/'.cache/sunstoneos/evidence/radio-session-20261016/initial/results.json').read_text())
        command=accepted['runs'][0]['command'][:];command[-1]=str(elf)
        command+=['-qmp',f'unix:{qmp_path},server=on,wait=off','-gdb',f'unix:{gdb_path},server=on,wait=off','-drive',f'if=none,id=runtime_checkpoint,file={disk},format=qcow2']
        stdout=out/'stdout.log';stderr=out/'stderr.log';journal={'command':command,'elf_sha256':sha(elf),'symbols':symbols,
            'fixture_inputs':{str(p):sha(p) for p in [Path(__file__).resolve(),ROOT/'checks/radio-reconnect-wpa.c',ROOT/'checks/a733-radio-session.c',args.vectors]}}
        with stdout.open('w') as output,stderr.open('w') as errors:
            process=subprocess.Popen(command,stdout=output,stderr=errors);journal['pid']=process.pid
            try:
                end=time.monotonic()+60
                while 'CHECKPOINT WPA2 owner before reconnect' not in stdout.read_text():
                    assert process.poll() is None,'Stopped before checkpoint; logs preserved'
                    assert time.monotonic()<end,'Checkpoint wait expired; live PID preserved'
                    time.sleep(.01)
                monitor=socket.socket(socket.AF_UNIX);monitor.settimeout(10);monitor.connect(str(qmp_path));channel=monitor.makefile('rwb',buffering=0);json.loads(channel.readline())
                def qmp(name,arguments=None):
                    channel.write((json.dumps({'execute':name,'arguments':arguments or {}})+'\n').encode())
                    while True:
                        answer=json.loads(channel.readline())
                        if 'return' in answer or 'error' in answer:return answer
                journal['capabilities']=qmp('qmp_capabilities');journal['stop']=qmp('stop');journal['status']=qmp('query-status');assert journal['status']['return']['status']=='paused'
                debugger=socket.socket(socket.AF_UNIX);debugger.settimeout(10);debugger.connect(str(gdb_path))
                for address in boundaries.values():assert packet(debugger,f'Z0,{address:x},4')=='OK'
                journal['debugger_boundaries']=boundaries
                saved={}
                for name,(address,size) in symbols.items():
                    data=b''
                    for offset in range(0,size,1024):data+=bytes.fromhex(packet(debugger,f'm{address+offset:x},{min(1024,size-offset):x}'))
                    path=out/(name+'.bin');path.write_bytes(data);saved[name]={'path':str(path),'sha256':sha(path)}
                assert (out/'reconnect_checkpoint_gate.bin').read_bytes()==bytes(4)
                for name in ['old_deletes','old_closed','eapol_sent','disconnects','connects']:assert int.from_bytes((out/(name+'.bin')).read_bytes(),'little')==0
                view=struct.unpack('<8Q',(out/'reconnect_checkpoint_view.bin').read_bytes());assert view[:4]==(1,1,1,1) and view[4]==view[5]!=0 and view[6:]==(0,0),view
                journal['savevm']=qmp('human-monitor-command',{'command-line':'savevm before-reconnect'})
                assert journal['savevm'].get('return')=='',journal['savevm']
                journal['snapshots']=qmp('human-monitor-command',{'command-line':'info snapshots'});assert 'before-reconnect' in journal['snapshots']['return']
                journal['checkpoint_files']=saved;write_json(out/'checkpoint.json',journal)
                assert packet(debugger,f'M{symbols["reconnect_checkpoint_gate"][0]:x},4:01000000')=='OK'
                assert packet(debugger,f'm{symbols["reconnect_checkpoint_gate"][0]:x},4')=='01000000'
                journal['continue']=qmp('cont');assert 'return' in journal['continue']
                limit=time.monotonic()+90
                while process.poll() is None:
                    try:state=qmp('query-status')['return']
                    except (OSError,ValueError):
                        process.wait(timeout=1);break
                    assert state['status']!='debug','Fixture assertion breakpoint; terminal RAM preserved'
                    assert time.monotonic()<limit,'Bounded runtime wait expired'
                    time.sleep(.1)
                code=process.returncode;debugger.close()
                journal.update(returncode=code,stdout=stdout.read_text(),stderr=stderr.read_text(),checkpoint_disk_sha256=sha(disk))
                write_json(out/'result.json',journal);print(journal['stdout'],end='');print(journal['stderr'],end='')
                assert code==0 and 'PASS WPA2 board owner reconnect' in journal['stdout']
            except Exception as error:
                if process.poll() is None:
                    try:
                        if 'qmp' not in locals():
                            monitor=socket.socket(socket.AF_UNIX);monitor.settimeout(10);monitor.connect(str(qmp_path))
                            channel=monitor.makefile('rwb',buffering=0);json.loads(channel.readline())
                            def qmp(name,arguments=None):
                                channel.write((json.dumps({'execute':name,'arguments':arguments or {}})+'\n').encode())
                                while True:
                                    answer=json.loads(channel.readline())
                                    if 'return' in answer or 'error' in answer:return answer
                            journal['failure_capabilities']=qmp('qmp_capabilities')
                        journal['failure_stop']=qmp('stop');journal['failure_status']=qmp('query-status')
                        assert journal['failure_status']['return']['status'] in ['paused','debug']
                        journal['failure_registers']=qmp('human-monitor-command',{'command-line':'info registers'})
                        journal['failure_savevm']=qmp('human-monitor-command',{'command-line':'savevm failed-reconnect'})
                        journal['failure_snapshots']=qmp('human-monitor-command',{'command-line':'info snapshots'})
                    except Exception as stop_error:journal['failure_preservation_error']=repr(stop_error)
                journal.update(error=repr(error),returncode=process.poll(),stdout=stdout.read_text(),stderr=stderr.read_text())
                write_json(out/'stopped.json',journal)
                # Do not terminate a live machine: exact QMP/GDB endpoints and
                # saved runtime snapshot permit affected continuation.
                raise
if __name__=='__main__':main()
