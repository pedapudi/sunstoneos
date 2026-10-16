#!/usr/bin/env python3
"""One composed host session using the actual observer frame encoder; no USB."""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--observer-source',type=Path,required=True)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--cc',default='clang-21')
p.add_argument('--reuse-local-expiry',type=Path)
p.add_argument('--reuse-late-deadline',type=Path)
p.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
a=p.parse_args(); root=Path(__file__).resolve().parents[1]; a.evidence.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('observer_host',root/'tools/check-a733-smp-observe.py')
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
if a.reuse_late_deadline:
    old=a.reuse_late_deadline
    frame=(old/'capture/observer-01.bin').read_bytes()
    prior=json.loads((old/'results.json').read_text())
    assert hashlib.sha256(frame).hexdigest()==prior['artifacts']['capture/observer-01.bin']
    prepared={'epoch':31,'generation':37,'lease':41,'build_id':bytes(range(32)).hex()}
    challenge=hashlib.sha256(b'one composed observer session').digest()
    late=bytearray(frame);struct.pack_into('<I',late,4,2)
    struct.pack_into('<Q',late,16+64,48001024)
    late[-32:]=hashlib.sha256(late[16:-32]).digest()
    clock=[1];calls=[];record={'scheduler_result':'unobserved','complete':False}
    def exchange(packet,timeout):
        calls.append(packet)
        if len(calls)==1:return frame
        clock[0]=100;return bytes(late)
    try:
        host.capture_until_recovery(exchange,prepared,challenge,a.evidence,record,lambda:None,100,
                                    now=lambda:clock[0],pause=lambda _:None)
    except ValueError as error:assert 'deadline changed' in str(error)
    else:raise AssertionError('Late inconsistent deadline was not fatal')
    assert len(calls)==2 and clock[0]==100 and 'capture_limit' not in record and not record['complete']
    snapshots={}
    for path in (Path(__file__),root/'tools/check-a733-smp-observe.py',root/'tools/check-a733-smp.py'):
        target=a.evidence/path.name;shutil.copy2(path,target);snapshots[str(path)]=host.digest(path)
    result={'result':'PASS: late complete reply with changed session deadline remains fatal before local-expiry classification',
            'inputs':snapshots,'reused_frame':{'path':str(old/'capture/observer-01.bin'),'sha256':host.digest(old/'capture/observer-01.bin')},
            'prior_result':{'path':str(old/'results.json'),'sha256':host.digest(old/'results.json')},'physical_access':False}
    (a.evidence/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(result['result']);sys.exit(0)
if a.reuse_local_expiry:
    old=a.reuse_local_expiry
    frame=(old/'capture/observer-01.bin').read_bytes()
    prior=json.loads((old/'results.json').read_text())
    assert hashlib.sha256(frame).hexdigest()==prior['artifacts']['capture/observer-01.bin']
    prepared={'epoch':31,'generation':37,'lease':41,'build_id':bytes(range(32)).hex()}
    challenge=hashlib.sha256(b'one composed observer session').digest()
    times=iter((1,1,100));calls=[];record={'scheduler_result':'unobserved','complete':False};saved=[]
    def exchange(packet,timeout):calls.append(packet);return frame
    assert host.capture_until_recovery(exchange,prepared,challenge,a.evidence,record,
        lambda:saved.append(dict(record)),100,now=lambda:next(times),pause=lambda _:None) is False
    assert len(calls)==1 and record['scheduler_result']=='unobserved' and not record['complete'] and 'durable_result' not in record
    # No more native traffic: a separate fresh ROM observation cannot create success.
    record['fresh_rom_response']=True
    assert not record['complete'] and len(calls)==1
    # An invalid late reply is a protocol error, never a local-expiry outcome.
    try:
        host.capture_until_recovery(lambda p,t:frame[:-1],prepared,challenge,a.evidence,{},lambda:None,100,now=lambda:1)
    except ValueError:pass
    else:raise AssertionError('Malformed reply treated as expiry')
    # No native appearance has no response frame and remains recovery-only.
    absent={'scheduler_result':'unobserved','fresh_rom_response':True}
    assert absent['scheduler_result']!='observed-success'
    snapshots={}
    for path in (Path(__file__),root/'tools/check-a733-smp-observe.py',root/'tools/check-a733-smp.py'):
        target=a.evidence/path.name;shutil.copy2(path,target);snapshots[str(path)]=host.digest(path)
    result={'result':'PASS: local expiry after valid pending reply permits recovery-only observation; malformed reply remains fatal; no native result cannot imply success',
            'inputs':snapshots,'reused_frame':{'path':str(old/'capture/observer-01.bin'),'sha256':host.digest(old/'capture/observer-01.bin')},
            'prior_result':{'path':str(old/'results.json'),'sha256':host.digest(old/'results.json')},'physical_access':False}
    (a.evidence/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(result['result']);sys.exit(0)
inputs={}
for name in ('tools/check-a733-smp-observe.py','tools/check-a733-smp.py','checks/a733-smp-observe-host.py',
             'platform/a733-smp-observe/launch.S','platform/a733-smp-observe/launch.c','platform/a733-smp-observe/launch.ld',
             'platform/a733-smp/launch.S','platform/a733-smp/launch.c','platform/a733-smp/launch.ld'):
    path=root/name; target=a.evidence/'inputs'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
    inputs[str(path)]={'sha256':host.digest(path),'snapshot':str(target)}
for name in ('result.h','result.c','boot.h'):
    path=(a.observer_source.parent/'a733-smp-cached/boot.h') if name=='boot.h' else a.observer_source/name;target=a.evidence/'observer'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
    inputs[str(path)]={'sha256':host.digest(path),'snapshot':str(target)}
# Reviewed launcher instructions are reused; only request/contract identities differ.
assert (root/'platform/a733-smp/launch.S').read_bytes()==(root/'platform/a733-smp-observe/launch.S').read_bytes()
assert (root/'platform/a733-smp/launch.ld').read_bytes()==(root/'platform/a733-smp-observe/launch.ld').read_bytes()
new=(root/'platform/a733-smp-observe/launch.c').read_text();new=new[new.index('#include'):]
assert new.replace('0x415337334f4c4331','0x415337334c4e4331')==(root/'platform/a733-smp/launch.c').read_text()
commands=[]
def run(command,name):
    command=list(map(str,command));commands.append(command)
    r=subprocess.run(command,capture_output=True,text=True)
    (a.evidence/name).write_text(r.stdout+r.stderr);assert r.returncode==0,name
wrapper=a.evidence/'observer/abi.c'
wrapper.write_text('#include "boot.h"\n#include <stddef.h>\nuint64_t contract_magic(void){return A733_SMP_CONTRACT_MAGIC;}\nunsigned contract_size(void){return sizeof(struct a733_smp_contract);}\nunsigned deadline_offset(void){return offsetof(struct a733_smp_contract,diagnostic_deadline);}\n')
library=a.evidence/'result.so'
run([a.cc,'-DA733_SMP_CONTRACT_MAGIC=0x415337334f424331ULL','-std=c11','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC',a.evidence/'observer/result.c',wrapper,'-o',library],'result-compile.log')
lib=ctypes.CDLL(str(library.resolve()));lib.contract_magic.restype=ctypes.c_uint64
assert lib.contract_magic()==0x415337334f424331 and lib.contract_size()==224 and lib.deadline_offset()==96
flags=['-DA733_SMP_CONTRACT_MAGIC=0x415337334f424331ULL','--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin',
       '-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror',
       '-I'+str(a.evidence/'observer')]
for source,name in [('launch.S','entry.o'),('launch.c','launch.o')]:
    run([a.cc,*flags,'-c',a.evidence/'inputs/platform/a733-smp-observe'/source,'-o',a.evidence/name],name+'.log')
run([a.lld,'-T',a.evidence/'inputs/platform/a733-smp-observe/launch.ld',a.evidence/'entry.o',a.evidence/'launch.o','-o',a.evidence/'launch.elf'],'link.log')
run(['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',a.evidence/'launch.elf',a.evidence/'launch.bin'],'objcopy.log')
assert 0<(a.evidence/'launch.bin').stat().st_size<=0xc00
class Identity(ctypes.Structure): _fields_=[(n,ctypes.c_uint64) for n in ('epoch','generation','lease','armed_at','deadline')]
class Snapshot(ctypes.Structure): _fields_=[('start',ctypes.c_uint64),('end',ctypes.c_uint64),('core',(ctypes.c_uint64*5)*8)]
lib.a733_observe_frame.argtypes=[ctypes.c_void_p,ctypes.POINTER(Identity),ctypes.POINTER(Snapshot),ctypes.c_void_p,ctypes.c_void_p]
lib.a733_observe_request.argtypes=[ctypes.c_void_p,ctypes.POINTER(Identity),ctypes.POINTER(ctypes.c_uint32),ctypes.c_void_p]
bundle=a.evidence/'bundle';bundle.mkdir();capture_dir=a.evidence/'capture';capture_dir.mkdir()
build=bytes(range(32)); challenge=hashlib.sha256(b'one composed observer session').digest()
identity=Identity(31,37,41,1000,48001000);snapshot=Snapshot(2000,900000)
for core in range(8): snapshot.core[core][:]=(32,17+core,2+core,0,40 if not core else 0)
request,contract=host.request_bytes(0x41000100,build,31,37,41)
assert struct.unpack_from('<Q',request)[0]==0x415337334f4c4331 and len(contract)==4096
files={'segment-0.bin':b'reviewed observer ELF segment fixture','request.bin':request,'contract.bin':contract,
       'result-zero.bin':bytes(16384),'report-zero.bin':bytes(128),'launch.bin':(a.evidence/'launch.bin').read_bytes()}
for name,data in files.items():(bundle/name).write_bytes(data)
prepared={'epoch':31,'generation':37,'lease':41,'build_id':build.hex(),'segments':[{'name':'segment-0.bin','address':0x41000000}],
          'files':{k:hashlib.sha256(v).hexdigest() for k,v in files.items()},'_sizes':{k:len(v) for k,v in files.items()}}
record={'launch_attempted':False,'scheduler_result':'unobserved','complete':False}; events=[]; transfers={}; exe=0

def save():
    if record.get('scheduler_result')=='observed-success':
        item=record['durable_result'];assert host.digest(capture_dir/item['path'])==item['sha256']
        assert ('file-fsync',str((capture_dir/item['path']).resolve())) in events
        assert events.index(('file-fsync',str((capture_dir/item['path']).resolve()))) < max(i for i,e in enumerate(events) if e==('directory-fsync',str(capture_dir.resolve())))
    host.save_json(capture_dir/'session.json',record)

# Spy only this new capture's actual durability calls; os.fsync still executes.
real_fsync=os.fsync
def fsync(fd):
    result=real_fsync(fd)
    path=os.readlink('/proc/self/fd/'+str(fd))
    if os.path.isdir('/proc/self/fd/'+str(fd)):events.append(('directory-fsync',path))
    else:events.append(('file-fsync',path))
    return result
host.os.fsync=fsync

def transaction(*args,pass_fds=()):
    global exe
    if args[0]=='read':Path(args[3]).write_bytes(bytes(args[2]));return ''
    if args[0]=='readl':return '\n'.join('0' for _ in range(len(args)//2))
    if args[0]=='write':
        assert pass_fds and str(args[2]).startswith('/proc/self/fd/')
        data=Path(args[2]).read_bytes();assert len(data)==args[5]
        transfers[int(args[1],16)]=data;Path(args[6]).write_bytes(data);return ''
    assert args==('exe','0x52000');exe+=1;assert exe==1 and len(transfers)==6
    events.append(('exe',));return ''

def encoded(sequence,complete):
    payload=ctypes.create_string_buffer(512)
    assert lib.a733_observe_frame(payload,ctypes.byref(identity),ctypes.byref(snapshot) if complete else None,challenge,build)==1
    return struct.pack('<4I',host.MESSAGE_MAGIC,sequence,0x80000031,512)+payload.raw

def observe(started):
    exchanges=[]
    def exchange(packet,timeout):
        seq=ctypes.c_uint32();copied=ctypes.create_string_buffer(32)
        assert lib.a733_observe_request(packet,ctypes.byref(identity),ctypes.byref(seq),copied)==1
        assert copied.raw==challenge and seq.value==len(exchanges)+1 and 0<timeout<=500
        exchanges.append(packet);return encoded(seq.value,seq.value==2)
    host.capture(exchange,prepared,challenge,capture_dir,record,save,100,now=lambda:1,pause=lambda _:None)
    assert len(exchanges)==2 and record['scheduler_result']=='observed-success' and 'fresh_rom_response' not in record
    assert len((capture_dir/'observer-02.bin').read_bytes())==528
    # Untrusted variations from this same encoded session never validate as success.
    frame=encoded(2,True)
    def refused(data,seq=2,nonce=challenge):
        try:host.response(data,seq,prepared,nonce)
        except ValueError:return
        raise AssertionError('Invalid frame accepted')
    refused(frame[:-1]);refused(frame+b'\0');refused(frame,1);refused(frame,nonce=bytes(32))
    bad=bytearray(frame);bad[48]^=1;bad[-32:]=hashlib.sha256(bad[16:-32]).digest();refused(bad)
    bad=bytearray(frame);bad[16+160]=31;bad[-32:]=hashlib.sha256(bad[16:-32]).digest();refused(bad)
    # A failed transport gets exactly one call and cannot publish a result.
    attempts=[]
    def failed(packet,timeout):attempts.append(packet);raise OSError('modeled disconnect')
    fresh={'scheduler_result':'unobserved'}
    try:host.capture(failed,prepared,challenge,capture_dir,fresh,lambda:None,100,now=lambda:1)
    except OSError:pass
    else:raise AssertionError('Failed exchange accepted')
    assert len(attempts)==1 and fresh['scheduler_result']=='unobserved'
    events.append(('fresh-rom-response',));record['fresh_rom_response']=True;save()

host.launch_once(transaction,observe,bundle,prepared,capture_dir,record,save,lambda:events.append(('admitted',)))
assert exe==1 and record['complete'] and events.index(('exe',))<events.index(('fresh-rom-response',))
assert record['durable_result']['path']=='observer-02.bin'
result={'result':'PASS: one composed preload/launch, actual C pending→complete frames, strict session/criteria validation, durable capture, separate recovery, fail-stop exchange',
        'inputs':inputs,'commands':commands,'artifacts':{str(p.relative_to(a.evidence)):host.digest(p) for p in a.evidence.rglob('*') if p.is_file()},
        'limits':'Host fake plus native compilation and source-equivalent launcher. No USB, board, cache/coherence or watchdog qualification.'}
(a.evidence/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(result['result'])
