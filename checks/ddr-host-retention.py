#!/usr/bin/env python3
"""Run the production DDR script with fake FEL and a real private file lock."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import importlib.abc
import importlib.util
import json
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import types
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--restore-build',type=Path);p.add_argument('--restore-record',type=Path)
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=False)
if bool(a.restore_build)!=bool(a.restore_record):p.error('restore build and record must be supplied together')
spec=importlib.util.spec_from_file_location('ddr_fixture_base',ROOT/'tools/check-a733-smp.py');base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
real_spec=importlib.util.spec_from_file_location;real_stop=base.stop_after_failure
build=a.evidence/'build';build.mkdir();files={}
for name,size in [('boot0-head.bin',12288),('boot0-body.bin',233472),('ddr-entry.bin',128),('cache-off.bin',40)]:
    data=bytes([size%251])*size;(build/name).write_bytes(data);files[name]={'size':size,'sha256':hashlib.sha256(data).hexdigest()}
(build/'manifest.json').write_text(json.dumps({'files':files}));fel=a.evidence/'fake-fel';fel.write_bytes(b'Never executed')
# The init-only capture variant (tools/prepare-ddr.py --variant init-only) runs with --allow-untrained.
init_build=a.evidence/'build-init-only';init_build.mkdir()
for name in files:(init_build/name).write_bytes((build/name).read_bytes())
(init_build/'manifest.json').write_text(json.dumps({'files':files,'variant':'init-only'}))
results=[]
cases=['nominal','pre-device-refusal','version-timeout','upload-mismatch','post-exe-timeout','RAM-mismatch','quarantine-failure','journal-failure','init-only','init-only-flag-missing']
restore_errors={'restore-nominal':0,'restore-wrong-board':4,'restore-header-crc':2,'restore-payload-crc':3,'restore-readback':7,'restore-memory':9}
if a.restore_build:cases+=list(restore_errors)+['restore-record-upload','restore-helper-upload','restore-record-missing']
for case in cases:
    out=a.evidence/case;out.mkdir();lock=out/'private.lock';events=[];memory={};node=Path('/fixture-usb/003/072')
    @contextmanager
    def board_lock():
        with lock.open('a') as owner:
            fcntl.flock(owner,fcntl.LOCK_EX|fcntl.LOCK_NB);yield node
    def held():
        with lock.open('a') as other:
            try:fcntl.flock(other,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:return
            raise AssertionError('DDR released its lock before retention')
    def require_rom(n):
        if case=='pre-device-refusal':raise ValueError('Modeled pre-device refusal')
    def quarantine(*unused):
        held();events.append('quarantine')
        if case=='quarantine-failure':raise OSError('Modeled quarantine failure')
    def hold():held();events.append('hold')
    access=types.SimpleNamespace(board_lock=board_lock,require_rom=require_rom,identity=lambda n:[1,2,3,4,5],quarantine_instance=quarantine)
    def stop(record,save,quarantine,error):return real_stop(record,save,quarantine,error,hold=hold)
    class Loader(importlib.abc.Loader):
        def create_module(self,spec):return access if spec.name=='usb_access' else base
        def exec_module(self,module):pass
    def selected_spec(name,path,*args,**kwargs):
        return importlib.util.spec_from_loader(name,Loader()) if name in ('usb_access','ddr_base') else real_spec(name,path,*args,**kwargs)
    def fake_fel(command,**kwargs):
        held();assert command[:3]==[str(fel),'-d','3:72'];arguments=command[3:];assert kwargs['timeout']==30
        assert 'quarantine' not in events and 'hold' not in events;events.append(arguments)
        if arguments==['version']:
            if case in ('version-timeout','quarantine-failure','journal-failure'):raise subprocess.TimeoutExpired(command,30)
            return types.SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')
        if arguments==['exe','0x85000'] and case=='post-exe-timeout':raise subprocess.TimeoutExpired(command,30)
        i=0
        while i<len(arguments):
            op,address=arguments[i],int(arguments[i+1],16)
            if op=='read':
                size=int(arguments[i+2]);data=memory.get(address,bytes(size))[:size]
                if address==0x86000:
                    data=struct.pack('<7I',0x53524444,5,0 if case=='init-only' else 4096,0,0,0,0)+bytes(100)
                    if case in restore_errors:
                        error=restore_errors[case]
                        data=struct.pack('<7I',0x53524444,5,12288 if error in (0,9) else 0,0,0,0,0)
                        writes=1016 if error in (0,9) else 1 if error==7 else 0
                        data+=struct.pack('<7I',error,writes,int(error in (0,7,9)),
                                           278528 if not error else 4 if error==9 else 0,0,0,0)+bytes(72)
                if (case=='upload-mismatch' and address==0x8e000 and address in memory) or (case=='RAM-mismatch' and address==0x41000000):data=bytes([data[0]^1])+data[1:]
                if address in memory and ((case=='restore-record-upload' and address==0x87000) or (case=='restore-helper-upload' and address==0x92000)):
                    data=bytes([data[0]^1])+data[1:]
                Path(arguments[i+3]).write_bytes(data);i+=4
            elif op=='write':memory[address]=Path(arguments[i+2]).read_bytes();i+=3
            else:assert op=='exe';i+=2
        return types.SimpleNamespace(returncode=0,stdout='',stderr='')
    real_write=Path.write_text
    def write(path,data,*args,**kwargs):
        if case=='journal-failure' and path.name=='session.json' and '"failure"' in data:raise OSError('Modeled journal failure')
        return real_write(path,data,*args,**kwargs)
    guard=types.ModuleType('usb_guard');guard.check_health=lambda:None
    selected_build=a.restore_build if case.startswith('restore-') else init_build if case.startswith('init-only') else build
    argv=[str(ROOT/'tools/run-ddr.py'),'--fel',str(fel),'--build',str(selected_build),'--evidence',str(out/'runtime')]+(['--allow-untrained'] if case=='init-only' else [])
    if case.startswith('restore-') and case!='restore-record-missing':argv+=['--record',str(a.restore_record)]
    error=None
    with patch.object(sys,'argv',argv),patch.object(importlib.util,'spec_from_file_location',selected_spec),patch.object(base,'stop_after_failure',stop),patch.object(base,'hold_ownership',hold),patch.object(subprocess,'run',fake_fel),patch.object(Path,'write_text',write),patch.dict(sys.modules,{'usb_guard':guard}):
        try:runpy.run_path(str(ROOT/'tools/run-ddr.py'),run_name='__main__')
        except (SystemExit,OSError) as failure:error=str(failure)
    assert (error is None)==(case in ('nominal','init-only','restore-nominal')),(case,error)
    retained=case not in ('nominal','pre-device-refusal','init-only','init-only-flag-missing','restore-nominal','restore-record-missing')
    assert events.count('hold')==int(retained),(case,events)
    assert events.count('quarantine')==int(retained and case!='journal-failure'),(case,events)
    if case in ('pre-device-refusal','init-only-flag-missing','restore-record-missing'):assert not events
    if case.startswith('restore-') and case!='restore-nominal':
        assert not any(isinstance(x,list) and ('0x41000000' in x or '0x42000000' in x) for x in events),(case,events)
    if case in restore_errors:
        record=json.loads((out/'runtime/session.json').read_text())
        assert record['restore_result']['error']==restore_errors[case]
        assert record['training_record_sha256']==hashlib.sha256(a.restore_record.read_bytes()).hexdigest()
        assert any(isinstance(x,list) and '0x9b000' in x and '0x87000' in x and '0x92000' in x for x in events)
    if case=='init-only':
        record=json.loads((out/'runtime/session.json').read_text())
        assert record['complete'] and record['untrained'] and not any(isinstance(x,list) and '0x41000000' in x for x in events)
    if case=='nominal':assert json.loads((out/'runtime/session.json').read_text())['complete']
    with lock.open('a') as released:fcntl.flock(released,fcntl.LOCK_EX|fcntl.LOCK_NB)
    results.append({'case':case,'pass':True,'retained':retained,'FEL_requests':sum(isinstance(x,list) for x in events)})
(a.evidence/'results.json').write_text(json.dumps({'physical_access':False,'pass':True,'cases':results,'host_sha256':hashlib.sha256((ROOT/'tools/run-ddr.py').read_bytes()).hexdigest()},indent=2)+'\n')
print('PASS DDR ownership cases:',len(results))
