#!/usr/bin/env python3
"""Check production survey failure ownership with a private lock and fake FEL."""
import argparse
from contextlib import contextmanager
import fcntl
import importlib.util
import json
from pathlib import Path
import subprocess
import struct
import sys
import types
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
for name in ('build','software','evidence'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('survey_host',ROOT/'tools/run-launch-survey.py');host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
fel=a.evidence/'fixture-fel';fel.write_bytes(b'Not executable: software-only FEL fixture')
results=[]
for case in ('complete-without-process-records','pre-device-refusal','version-timeout','short-backup','identity-loss','post-exe-timeout','restore-mismatch','restored-report-refusal','quarantine-failure','journal-failure'):
    out=a.evidence/case;out.mkdir();lock=out/'private.lock';events=[];memory={};node=Path('/fixture-usb/003/072');executed=False
    @contextmanager
    def board_lock():
        with lock.open('a') as owned:
            fcntl.flock(owned,fcntl.LOCK_EX|fcntl.LOCK_NB);yield node
    def held():
        with lock.open('a') as other:
            try:fcntl.flock(other,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:return
            raise AssertionError('Released survey flock before retention')
    def quarantine(*unused):
        held();events.append('quarantine')
        if case=='quarantine-failure':raise OSError('Modeled quarantine failure')
    def identity(n):
        return [9,2,3,4,5] if case=='identity-loss' and events else [1,2,3,4,5]
    def require_rom(n):
        if case=='pre-device-refusal':raise ValueError('Live target is not ROM')
    access=types.SimpleNamespace(board_lock=board_lock,require_rom=require_rom,identity=identity,target=lambda n:Path('/fixture-port/3-1'),quarantine_instance=quarantine)
    def fake_fel(command,**kwargs):
        global executed
        held();assert command[:3]==[str(fel),'-d','3:72'];arguments=command[3:];events.append(arguments)
        assert 'quarantine' not in events and 'held-private-flock' not in events
        if arguments==['version']:
            if case in ('version-timeout','quarantine-failure','journal-failure'):raise subprocess.TimeoutExpired(command,15)
            return types.SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')
        if arguments==['exe','0x85000']:
            executed=True
            if case=='complete-without-process-records':
                words=[0]*32;words[:4]=[host.MAGIC,1,128,1]
                words[4:16]=[19,0,0,0x80000000,24000000,0,1,0,2,0,0x8c000,0x90000]
                memory[0x86000]=struct.pack('<30I',*words[:30])+memory[0x86000][120:]
            if case=='post-exe-timeout':raise subprocess.TimeoutExpired(command,15)
            return types.SimpleNamespace(returncode=0,stdout='',stderr='')
        i=0
        while i<len(arguments):
            op,address=arguments[i],int(arguments[i+1],16)
            if op=='read':
                size=int(arguments[i+2]);data=memory.get(address,bytes([0xa5])*size)[:size]
                if case=='short-backup' or (case=='restore-mismatch' and executed and address==0x85000):data=data[:-1]
                Path(arguments[i+3]).write_bytes(data);i+=4
            else:assert op=='write' and kwargs['pass_fds'];memory[address]=Path(arguments[i+2]).read_bytes();i+=3
        return types.SimpleNamespace(returncode=0,stdout='',stderr='')
    def hold():held();events.append('held-private-flock')
    real_stop=host.base.stop_after_failure
    def stop(record,save,quarantine,error):return real_stop(record,save,quarantine,error,hold=hold)
    real_save=host.save
    def save(path,value):
        if case=='journal-failure' and value.get('failure'):raise OSError('Modeled journal failure')
        real_save(path,value)
    guard=types.ModuleType('usb_guard');guard.check_health=lambda:None
    runtime=out/'runtime';runtime.mkdir();(runtime/'earlier.txt').write_text('preserve evidence')
    args=types.SimpleNamespace(build=a.build.resolve(),software=a.software.resolve(),database=out/'absent.db',claim=None,owner=None,fel=fel,evidence=out/'runtime')
    with patch.object(host,'module',lambda *a:access),patch.object(host.base,'FEL_HASH',host.digest(fel)),patch.object(host.subprocess,'run',fake_fel),patch.object(host.base,'stop_after_failure',stop),patch.object(host.base,'hold_ownership',hold),patch.object(host,'save',save),patch.dict(sys.modules,{'usb_guard':guard}):
        try:host.run(args)
        except (ValueError,OSError,subprocess.TimeoutExpired):pass
        else:assert case=='complete-without-process-records'
    assert (runtime/'earlier.txt').read_text()=='preserve evidence' and args.evidence.parent==runtime
    retained=case not in ('complete-without-process-records','pre-device-refusal','restored-report-refusal')
    assert events.count('held-private-flock')==int(retained),(case,events)
    assert events.count('quarantine')==int(retained and case!='journal-failure'),(case,events)
    if case=='pre-device-refusal':assert not events
    elif case!='journal-failure':
        record=json.loads((args.evidence/'session.json').read_text());assert record['complete']==(case=='complete-without-process-records')
        if case=='restored-report-refusal':assert record['sram_restored'] and record['rom_usable']
    with lock.open('a') as released:fcntl.flock(released,fcntl.LOCK_EX|fcntl.LOCK_NB)
    results.append({'case':case,'pass':True,'retained':retained,'FEL_requests':sum(isinstance(x,list) for x in events)})
host.save(a.evidence/'results.json',{'physical_access':False,'pass':True,'cases':results,'host_sha256':host.digest(Path(host.__file__))})
print('PASS survey ownership cases:',len(results))
