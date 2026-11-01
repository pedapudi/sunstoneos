#!/usr/bin/env python3
"""Run the production DDR script with fake FEL and a real private file lock.

Restore cases model WDT0, the SID, two USB instances of one port and the
watchdog reset between them. No case opens a device.
"""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import importlib.abc
import importlib.util
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import time
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
WDT_MODE,WDT_CONFIG,WDT_OUTPUT,WDT_RESTART=0x02050014,0x02050010,0x02050018,0x0205000c
# Restore cases: the mailbox error, the expected exit, and whether the run
# ends in a WDT0 reset (deliberate after a returned failure, or after a hang).
RESTORE={'restore-nominal':(0,0,False),'restore-header-crc':(2,3,True),'restore-readback':(7,3,True),
         'restore-memory':(9,3,True),'restore-ram-pages':(0,3,True),'restore-hang':(None,3,True),
         'restore-chip-mismatch':(None,3,False),'restore-no-return':(None,1,False),
         'restore-unexplained-reset':(None,1,False),'restore-fallback-fails':(7,1,True),
         'restore-arm-readback':(None,1,False),'restore-wdt-enabled':(None,1,False),
         'restore-disarm-fails':(None,3,True),'restore-record-upload':(None,1,False),
         'restore-helper-upload':(None,1,False),'restore-record-missing':(None,1,False),
         'restore-fallback-missing':(None,1,False),'restore-no-session':(None,1,False)}
if a.restore_build:
    cases+=list(RESTORE)
    restore_manifest=json.loads((a.restore_build/'manifest.json').read_text())
    fallback_build=a.evidence/'build-fallback';fallback_build.mkdir()
    for name in files:(fallback_build/name).write_bytes((build/name).read_bytes())
    (fallback_build/'manifest.json').write_text(json.dumps({'files':files,'boot0_sha256':restore_manifest['boot0_sha256']}))
    chip=list(struct.unpack_from('<4I',a.restore_record.read_bytes(),32))
    sample=restore_manifest['restore']['sample_read_bytes']
OLD,NEW=Path('/fixture-usb/003/072'),Path('/fixture-usb/003/073')
IDENTITY={OLD:[1,2,3,4,5],NEW:[1,2,3,4,6]}
class Port:
    name='3-3'
    def resolve(self):return Path('/sys/devices/fixture/3-3')
for case in cases:
    out=a.evidence/case;out.mkdir();lock=out/'private.lock';events=[];clock=[1000.0]
    board={'present':OLD,'memory':{OLD:{},NEW:{}},'wdt':{OLD:{'mode':0,'config':0,'output':0},NEW:None},'reset':False,'helper':set()}
    @contextmanager
    def board_lock():
        with lock.open('a') as owner:
            fcntl.flock(owner,fcntl.LOCK_EX|fcntl.LOCK_NB);yield OLD
    def held():
        with lock.open('a') as other:
            try:fcntl.flock(other,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:return
            raise AssertionError('DDR released its lock before retention')
    def require_rom(n):
        if case=='pre-device-refusal':raise ValueError('Modeled pre-device refusal')
    def require_session(n):
        events.append(('session',str(n)))
        if case=='restore-no-session':raise ValueError('Reconnect access is not installed.')
    def quarantine(n,reason,observed):
        held();events.append(('quarantine',str(n)))
        if case=='quarantine-failure':raise OSError('Modeled quarantine failure')
    def hold():held();events.append('hold')
    def reset():
        # WDT0 fired: the old instance leaves; the mode survives on the new one.
        mode=board['wdt'][OLD]['mode']
        board['reset']=True;board['present']=None if case=='restore-no-return' else NEW
        board['wdt'][NEW]={'mode':0 if case=='restore-unexplained-reset' else mode,'config':1,'output':2}
        events.append('reset')
    access=types.SimpleNamespace(board_lock=board_lock,require_rom=require_rom,require_session=require_session,
                                 identity=lambda n:IDENTITY[n],quarantine_instance=quarantine,target=lambda n:Port(),
                                 session_node=lambda config:board['present'])
    def stop(record,save,quarantine,error):return real_stop(record,save,quarantine,error,hold=hold)
    class Loader(importlib.abc.Loader):
        def create_module(self,spec):return access if spec.name=='usb_access' else base
        def exec_module(self,module):pass
    def selected_spec(name,path,*args,**kwargs):
        return importlib.util.spec_from_loader(name,Loader()) if name in ('usb_access','ddr_base') else real_spec(name,path,*args,**kwargs)
    def mailbox(node):
        if node not in board['helper']:
            # The plain full-training wrapper; the fallback fails in one case.
            failed=case=='restore-fallback-fails' and node==NEW
            return struct.pack('<7I',0x53524444,5,0 if case=='init-only' or failed else 4096,7 if failed else 0,0,0,0)+bytes(100)
        error=RESTORE[case][0]
        assert error is not None,(case,'this case must not collect a restore result')
        data=struct.pack('<7I',0x53524444,5,12288 if error in (0,9) else 0,0,0,0,0)
        writes=1016 if error in (0,9) else 1 if error==7 else 0
        data+=struct.pack('<7I',error,writes,int(error in (0,7,9)),sample if not error else 4 if error==9 else 0,0,0,0)
        return data+bytes(44)+struct.pack("<2I",0xfffffff0,(0xfffffff0+24000*31)&0xffffffff)+bytes(20)
    def fake_fel(command,**kwargs):
        held();assert command[:2]==[str(fel),'-d'];node={'3:72':OLD,'3:73':NEW}[command[2]];arguments=command[3:]
        assert 'quarantine' not in str(events) and 'hold' not in events
        # After the reset or a failed guarded exe, nothing reaches the old instance.
        assert node==board['present'] and not (node==OLD and board['reset']),(case,arguments)
        assert kwargs['timeout']==(12 if arguments[:2]==['exe','0x85000'] and node in board['helper'] else 30)
        events.append([str(node)]+arguments);memory=board['memory'][node];wdt=board['wdt'][node];stdout=''
        if arguments==['version']:
            if case in ('version-timeout','quarantine-failure','journal-failure'):raise subprocess.TimeoutExpired(command,30)
            return types.SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')
        if arguments==['exe','0x85000'] and case=='post-exe-timeout':raise subprocess.TimeoutExpired(command,30)
        i=0
        while i<len(arguments):
            op,address=arguments[i],int(arguments[i+1],16)
            if op=='read':
                size=int(arguments[i+2]);data=memory.get(address,bytes(size))[:size]
                if address==0x86000:data=mailbox(node)
                if (case=='upload-mismatch' and address==0x8e000 and address in memory) or (case=='RAM-mismatch' and address==0x41000000):data=bytes([data[0]^1])+data[1:]
                if case=='restore-ram-pages' and node==OLD and address==0x41000000:data=bytes([data[0]^1])+data[1:]
                if address in memory and ((case=='restore-record-upload' and address==0x87000) or (case=='restore-helper-upload' and address==0x92000)):
                    data=bytes([data[0]^1])+data[1:]
                Path(arguments[i+3]).write_bytes(data);i+=4
            elif op=='write':
                memory[address]=Path(arguments[i+2]).read_bytes();i+=3
                if address==0x92000:board['helper'].add(node)
            elif op=='readl':
                value={WDT_MODE:wdt['mode']|(1 if case=='restore-wdt-enabled' else 0),WDT_CONFIG:wdt['config'],WDT_OUTPUT:wdt['output']}.get(address)
                if value is None:
                    assert 0x03006200<=address<0x03006210;value=chip[(address-0x03006200)//4]^(1 if case=='restore-chip-mismatch' else 0)
                stdout+=f'0x{value:08x}\n';i+=2
            elif op=='writel':
                value=int(arguments[i+2],16);i+=3
                if address==WDT_RESTART:
                    assert value==0x14af and wdt['mode']==0x41
                    events.append('armed')
                else:
                    assert value>>16==0x16aa
                    key={WDT_MODE:'mode',WDT_CONFIG:'config',WDT_OUTPUT:'output'}[address]
                    if not(case=='restore-arm-readback' and key=='output') and not(case=='restore-disarm-fails' and key=='mode' and value==0x16aa0000 and node==OLD):
                        wdt[key]=value&0xffff
            else:
                assert op=='exe';i+=2
                if address==0x85000 and node in board['helper']:
                    assert 'armed' in events and wdt['mode']==0x41,(case,events)
                    if case in ('restore-hang','restore-no-return','restore-unexplained-reset'):
                        reset();return types.SimpleNamespace(returncode=1,stdout='',stderr='usb_bulk_send() ERROR -1: Input/Output Error')
        # The deliberate second arming, or a disarm that did not take, lets
        # WDT0 expire after this request completes.
        second_arm=arguments[0]=='writel' and arguments[-2:]==['readl',hex(WDT_OUTPUT)] and events.count('armed')==2
        if node==OLD and wdt['mode']&1 and (second_arm or arguments[:2]==['exe','0x85000']):reset()
        return types.SimpleNamespace(returncode=0,stdout=stdout,stderr='')
    real_write=Path.write_text
    def write(path,data,*args,**kwargs):
        if case=='journal-failure' and path.name=='session.json' and '"failure"' in data:raise OSError('Modeled journal failure')
        return real_write(path,data,*args,**kwargs)
    guard=types.ModuleType('usb_guard');guard.check_health=lambda:None
    @contextmanager
    def reenumeration(name,what,count):
        assert name=='sunstoneos-a733-3-3' and count==1;outcome={'count':count};events.append('churn')
        try:yield outcome
        finally:events.append(('churn-end',outcome['count']))
    guard.reenumeration=reenumeration
    def sleep(seconds):clock[0]+=seconds
    restore_case=case.startswith('restore-')
    selected_build=a.restore_build if restore_case else init_build if case.startswith('init-only') else build
    argv=[str(ROOT/'tools/run-ddr.py'),'--fel',str(fel),'--build',str(selected_build),'--evidence',str(out/'runtime')]+(['--allow-untrained'] if case=='init-only' else [])
    if restore_case and case!='restore-record-missing':argv+=['--record',str(a.restore_record)]
    if restore_case and case!='restore-fallback-missing':argv+=['--fallback-build',str(fallback_build)]
    status=0
    with patch.object(sys,'argv',argv),patch.object(importlib.util,'spec_from_file_location',selected_spec),patch.object(base,'stop_after_failure',stop),patch.object(base,'hold_ownership',hold),patch.object(subprocess,'run',fake_fel),patch.object(Path,'write_text',write),patch.dict(sys.modules,{'usb_guard':guard}),patch.object(time,'monotonic',lambda:clock[0]),patch.object(time,'sleep',sleep),patch.object(os,'access',lambda path,mode:True):
        try:runpy.run_path(str(ROOT/'tools/run-ddr.py'),run_name='__main__')
        except SystemExit as failure:status=failure.code
        except OSError as failure:status=str(failure)
    expected=RESTORE[case][1] if restore_case else 0 if case in ('nominal','init-only') else 'failure'
    if expected=='failure' or expected==1:assert status not in (0,3,None),(case,status)
    else:assert status==expected,(case,status)
    device_free=('pre-device-refusal','init-only-flag-missing','restore-record-missing','restore-fallback-missing','restore-no-session')
    retained=status not in (0,3) and case not in device_free
    assert events.count('hold')==int(retained),(case,events)
    quarantines=[e for e in events if isinstance(e,tuple) and e[0]=='quarantine']
    assert len(quarantines)==int(retained and case!='journal-failure'),(case,events)
    if case in device_free:assert not [e for e in events if isinstance(e,list)],(case,events)
    fel_requests=[e for e in events if isinstance(e,list)]
    if restore_case and case not in device_free:
        record=json.loads((out/'runtime/session.json').read_text())
        assert record['training_record_sha256']==hashlib.sha256(a.restore_record.read_bytes()).hexdigest()
        resets=events.count('reset')
        assert resets==int(RESTORE[case][2] or case in ('restore-no-return','restore-unexplained-reset')),(case,events)
        launches=[e for e in fel_requests if e[1:3]==['exe','0x85000']]
        if case=='restore-chip-mismatch':
            # Refused before any restore byte or WDT0 write; one plain training.
            assert not [e for e in fel_requests if 'writel' in e or ('write' in e and ('0x87000' in e or '0x92000' in e))]
            assert len(launches)==1 and 'armed' not in events and record['outcome']=='fallback-full-training'
        if status in (0,3) and case!='restore-chip-mismatch':
            # The guarded launch arms first and disarms in the same request.
            guarded=next(e for e in launches if e[0]==str(OLD))
            assert guarded[3:]==['writel',hex(WDT_MODE),hex(0x16aa0000),'readl',hex(WDT_MODE)],guarded
            assert events.index('churn')<events.index('armed')
        if status==0:
            assert record['outcome']=='restored' and record['restore_result']['check_ms']==31.0
            assert ('churn-end',0) in events and record['watchdog']['mode_after_disarm']==0
        if status==3 and RESTORE[case][2]:
            # Exactly one plain training, on the new instance, after the disarm there.
            assert ('churn-end',1) in events and record['outcome']=='fallback-full-training'
            assert [e[0] for e in launches]==[str(OLD),str(NEW)],launches
            new=[e for e in fel_requests if e[0]==str(NEW)]
            assert new[0][1:]==['readl',hex(WDT_MODE),'writel',hex(WDT_MODE),hex(0x16aa0000),'readl',hex(WDT_MODE)],new[0]
            assert record['watchdog']['mode_after_reset']==0x41 and record['watchdog']['mode_after_reset_disarm']==0
            assert record['fallback']['result']['dram_mib']==4096 and (out/'runtime/fallback/ram-41000000-read.bin').exists()
            assert not [e for e in fel_requests if e[0]==str(NEW) and ('0x87000' in e or '0x92000' in e)]
        if case in ('restore-header-crc','restore-readback','restore-memory'):
            assert record['restore_result']['error']==RESTORE[case][0]
        if case in ('restore-header-crc','restore-readback','restore-memory','restore-fallback-fails','restore-ram-pages'):
            assert 'deliberate_reset' in record['watchdog'] and events.count('armed')==2
            # No host RAM page is written after a reported restore error.
            if case!='restore-ram-pages':
                assert not [e for e in fel_requests if e[0]==str(OLD) and '0x41000000' in e],(case,events)
        if case in ('restore-hang','restore-unexplained-reset','restore-no-return'):
            assert 'exe_failure' in record['watchdog'] and events.count('armed')==1
        if case=='restore-unexplained-reset':assert ('quarantine',str(NEW)) in events and record['watchdog']['mode_after_reset']==0
        if case=='restore-no-return':assert ('quarantine',str(OLD)) in events and 'returned_device' not in record
        if case=='restore-arm-readback':
            assert fel_requests[-1][1:]==['writel',hex(WDT_MODE),hex(0x16aa0000),'readl',hex(WDT_MODE)]
            assert not [e for e in fel_requests if e[1:3]==['exe','0x85000']]
        if case=='restore-wdt-enabled':assert 'armed' not in events and not [e for e in fel_requests if e[1:3]==['exe','0x85000']]
        if case in ('restore-record-upload','restore-helper-upload'):assert 'armed' not in events
    if case=='init-only':
        record=json.loads((out/'runtime/session.json').read_text())
        assert record['complete'] and record['untrained'] and not any('0x41000000' in e for e in fel_requests)
    if case=='nominal':assert json.loads((out/'runtime/session.json').read_text())['complete']
    with lock.open('a') as released:fcntl.flock(released,fcntl.LOCK_EX|fcntl.LOCK_NB)
    results.append({'case':case,'pass':True,'exit':status if isinstance(status,int) or status is None else 'stopped','retained':retained,'FEL_requests':len(fel_requests),'resets':events.count('reset')})
(a.evidence/'results.json').write_text(json.dumps({'physical_access':False,'pass':True,'cases':results,'host_sha256':hashlib.sha256((ROOT/'tools/run-ddr.py').read_bytes()).hexdigest()},indent=2)+'\n')
print('PASS DDR ownership cases:',len(results))
