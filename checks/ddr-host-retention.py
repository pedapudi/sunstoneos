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
p.add_argument('--survey-build',type=Path,help='diagnostic restore build (prepare-ddr.py --survey) for the survey cases')
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=False)
if bool(a.restore_build)!=bool(a.restore_record):p.error('restore build and record must be supplied together')
if a.survey_build and not a.restore_build:p.error('--survey-build needs --restore-build and --restore-record')
spec=importlib.util.spec_from_file_location('ddr_fixture_base',ROOT/'tools/check-a733-smp.py');base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
real_spec=importlib.util.spec_from_file_location;real_stop=base.stop_after_failure
spec=real_spec('training_fixture',ROOT/'tools/training-record.py');training=importlib.util.module_from_spec(spec);spec.loader.exec_module(training)
build=a.evidence/'build';build.mkdir();files={}
for name,size in [('boot0-head.bin',12288),('boot0-body.bin',233472),('ddr-entry.bin',128),('cache-off.bin',40)]:
    data=bytes([size%251])*size;(build/name).write_bytes(data);files[name]={'size':size,'sha256':hashlib.sha256(data).hexdigest()}
(build/'manifest.json').write_text(json.dumps({'files':files,'clock_point_word':15}));fel=a.evidence/'fake-fel';fel.write_bytes(b'Never executed')
# The init-only capture variant (tools/prepare-ddr.py --variant init-only) runs with --allow-untrained.
init_build=a.evidence/'build-init-only';init_build.mkdir()
for name in files:(init_build/name).write_bytes((build/name).read_bytes())
(init_build/'manifest.json').write_text(json.dumps({'files':files,'variant':'init-only','clock_point_word':15}))
# A build from before the CPU clock return: its manifest has no clock_point_word.
old_build=a.evidence/'build-old';old_build.mkdir()
for name in files:(old_build/name).write_bytes((build/name).read_bytes())
(old_build/'manifest.json').write_text(json.dumps({'files':files}))
results=[]
cases=['nominal','pre-device-refusal','version-timeout','upload-mismatch','post-exe-timeout','RAM-mismatch','quarantine-failure','journal-failure','init-only','init-only-flag-missing','clock-not-rom','clock-lfm-on','build-before-clock-return']
WDT_MODE,WDT_CONFIG,WDT_OUTPUT,WDT_RESTART=0x02050014,0x02050010,0x02050018,0x0205000c
# CPU PLL and mux words after the wrapper: the ROM point for CPU-L and the DSU,
# boot0's for CPU-B, pattern and LFM words zero. The clock cases leave CPU-L
# at boot0's 1014 MHz or the DSU's LFM enabled.
ROM_CLOCKS={0x08871000:0xf8802600,0x0887101c:0x03010000,0x08872000:0xf8802700,0x0887201c:0x03000000,
            0x08873000:0xf8802600,0x0887301c:0x03010105}
# Restore cases: the mailbox error, the expected exit, and whether the run
# ends in a WDT0 reset (deliberate after a returned failure, or after a hang).
RESTORE={'restore-nominal':(0,0,False),'restore-clock-not-rom':(0,3,True),'restore-header-crc':(2,3,True),'restore-readback':(7,3,True),
         'restore-memory':(9,3,True),'restore-ram-pages':(0,3,True),'restore-hang':(None,3,True),
         'restore-chip-mismatch':(None,3,False),'restore-no-return':(None,1,False),
         'restore-unexplained-reset':(None,1,False),'restore-fallback-fails':(7,1,True),
         'restore-arm-readback':(None,1,False),'restore-wdt-enabled':(None,1,False),
         'restore-disarm-fails':(None,3,True),'restore-record-upload':(None,1,False),
         'restore-helper-upload':(None,1,False),'restore-record-missing':(None,1,False),
         'restore-fallback-missing':(None,1,False),'restore-no-session':(None,1,False),
         'restore-fallback-before-clock-return':(None,1,False)}
# Survey cases: a decoded survey that agrees with the mailbox, a helper that
# left the cleared survey untouched, and a clear that does not read back.
SURVEY_CASES={'restore-survey':(7,3,True),'restore-survey-missing':(7,3,True),'restore-survey-clear-mismatch':(None,1,False)}
if a.survey_build:RESTORE.update(SURVEY_CASES)
if a.restore_build:
    cases+=list(RESTORE)
    restore_manifest=json.loads((a.restore_build/'manifest.json').read_text())
    fallback_build=a.evidence/'build-fallback';fallback_build.mkdir()
    for name in files:(fallback_build/name).write_bytes((build/name).read_bytes())
    (fallback_build/'manifest.json').write_text(json.dumps({'files':files,'boot0_sha256':restore_manifest['boot0_sha256'],'clock_point_word':15}))
    old_fallback=a.evidence/'build-fallback-old';old_fallback.mkdir()
    for name in files:(old_fallback/name).write_bytes((build/name).read_bytes())
    (old_fallback/'manifest.json').write_text(json.dumps({'files':files,'boot0_sha256':restore_manifest['boot0_sha256']}))
    chip=list(struct.unpack_from('<4I',a.restore_record.read_bytes(),32))
    sample=restore_manifest['restore']['sample_read_bytes']
    phy_count=restore_manifest['restore']['phy_registers']
    parsed=training.parse(a.restore_record.read_bytes())
    profile=[run['base']+2*n for run in parsed['runs'][1:] for n in range(len(run['values']))]
    if a.survey_build:
        spec=real_spec('ddr_survey_fixture',ROOT/'tools/ddr-survey.py');survey_tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(survey_tool)
        survey_manifest=json.loads((a.survey_build/'manifest.json').read_text())['restore']['survey']
        values=[v for run in parsed['runs'][1:] for v in run['values']];mask_of={m['address']:m['mask'] for m in parsed['masks']}
        # The first entry with a nonzero expected value reads 0 and index 5
        # reads 0x42 (two mismatches, as the mailbox reports), index 3 is
        # stored later (as a wide-memory halfword), index 4 already held its
        # value; every other entry holds.
        zero=next(n for n,(address,value) in enumerate(zip(profile,values)) if n>5 and value&mask_of[address])
        fake_survey=bytearray(struct.pack('<4I',survey_tool.MAGIC,len(profile),1,3)+bytes(48))
        for n,(address,value) in enumerate(zip(profile,values)):
            m=mask_of[address];want=value&m;before=(value^m)&0xffff
            if before==0x42:before^=0x100
            after=end=(before&~m)|want
            if n==zero:after=end=0
            if n==5:after=end=0x42
            if n==3:after=before
            if n==4:before=after
            fake_survey+=struct.pack('<4H',before,after,end,m)
        assert len(fake_survey)==survey_manifest['bytes']
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
        writes=phy_count if error in (0,7,9) else 0
        # A readback failure names its first mismatch in words 11-13 and
        # counts two in word 27; word 28 holds the second, profile index 5.
        first=(profile[0],0x1234,0) if error==7 else (0,0,0)
        data+=struct.pack('<7I',error,writes,int(error in (0,7,9)),sample if not error else 4 if error==9 else 0,*first)
        survey=struct.pack('<5I',2,5<<16|0x0042,0,0,0) if error==7 else bytes(20)
        return data+bytes(44)+struct.pack("<2I",0xfffffff0,(0xfffffff0+24000*31)&0xffffffff)+survey
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
                if address==0x96000 and node==OLD and case in SURVEY_CASES:
                    if case=='restore-survey' and board.get('launched'):data=bytes(fake_survey)
                    if case=='restore-survey-clear-mismatch':data=bytes([data[0]^1])+data[1:]
                if address in memory and ((case=='restore-record-upload' and address==0x87000) or (case=='restore-helper-upload' and address==0x92000)):
                    data=bytes([data[0]^1])+data[1:]
                Path(arguments[i+3]).write_bytes(data);i+=4
            elif op=='write':
                memory[address]=Path(arguments[i+2]).read_bytes();i+=3
                if address==0x92000:board['helper'].add(node)
            elif op=='readl':
                value={WDT_MODE:wdt['mode']|(1 if case=='restore-wdt-enabled' else 0),WDT_CONFIG:wdt['config'],WDT_OUTPUT:wdt['output']}.get(address)
                if 0x08871000<=address<0x08874000:
                    value=ROM_CLOCKS.get(address,0)
                    if address==0x08871000 and node==OLD and case in ('clock-not-rom','restore-clock-not-rom'):value=0xf8802700
                    if address==0x08873018 and case=='clock-lfm-on':value=0x80000000
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
                    board['launched']=True
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
    selected_build=(a.survey_build if case in SURVEY_CASES else a.restore_build) if restore_case else init_build if case.startswith('init-only') else old_build if case=='build-before-clock-return' else build
    argv=[str(ROOT/'tools/run-ddr.py'),'--fel',str(fel),'--build',str(selected_build),'--evidence',str(out/'runtime')]+(['--allow-untrained'] if case=='init-only' else [])
    if restore_case and case!='restore-record-missing':argv+=['--record',str(a.restore_record)]
    if restore_case and case!='restore-fallback-missing':
        argv+=['--fallback-build',str(old_fallback if case=='restore-fallback-before-clock-return' else fallback_build)]
    status=0
    with patch.object(sys,'argv',argv),patch.object(importlib.util,'spec_from_file_location',selected_spec),patch.object(base,'stop_after_failure',stop),patch.object(base,'hold_ownership',hold),patch.object(subprocess,'run',fake_fel),patch.object(Path,'write_text',write),patch.dict(sys.modules,{'usb_guard':guard}),patch.object(time,'monotonic',lambda:clock[0]),patch.object(time,'sleep',sleep),patch.object(os,'access',lambda path,mode:True):
        try:runpy.run_path(str(ROOT/'tools/run-ddr.py'),run_name='__main__')
        except SystemExit as failure:status=failure.code
        except OSError as failure:status=str(failure)
    expected=RESTORE[case][1] if restore_case else 0 if case in ('nominal','init-only') else 'failure'
    if expected=='failure' or expected==1:assert status not in (0,3,None),(case,status)
    else:assert status==expected,(case,status)
    device_free=('pre-device-refusal','init-only-flag-missing','restore-record-missing','restore-fallback-missing','restore-no-session',
                 'build-before-clock-return','restore-fallback-before-clock-return')
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
        if case=='restore-readback':
            assert record['restore_result']['mismatches']==2 and record['restore_result']['failure_address']==profile[0]
            assert record['restore_result']['later_mismatches']==[{'address':f'{profile[5]:#010x}','actual':'0x0042'}]
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
    if case=='nominal':
        record=json.loads((out/'runtime/session.json').read_text())
        assert record['complete'] and record['cpu_clocks']['cpu_l_pll']=='0xf8802600' and record['clock_point_result']==0
    if case=='clock-not-rom':
        record=json.loads((out/'runtime/session.json').read_text())
        assert not record['complete'] and 'not at the entry point' in record['failure'] and 'cpu_l_pll' in record['failure']
        assert not any('0x41000000' in e for e in fel_requests),'RAM pages after a clock refusal'
    if case=='clock-lfm-on':
        record=json.loads((out/'runtime/session.json').read_text())
        assert not record['complete'] and "'dsu_lfm': '0x80000000'" in record['failure']
        assert not any('0x41000000' in e for e in fel_requests),'RAM pages after a clock refusal'
    if case in ('build-before-clock-return','restore-fallback-before-clock-return'):
        assert 'clock return' in str(status) and not (out/'runtime').exists(),(case,status)
    if case=='restore-survey':
        result=record['restore_result']['survey']
        assert result['classes']=={'reads-zero':1,'other-value':1,'stored-later':1,'already-at-value':1,'held':len(profile)-4},result
        assert result['mismatches']==2 and result['agrees_with_mailbox'] and result['control']['micro_cont_mux_sel_at_hook']=='0x0001'
        decoded=json.loads((out/'runtime/survey.json').read_text())['entries']
        assert len(decoded)==len(profile) and decoded[5]['after']=='0x0042' and decoded[zero]['class']=='reads-zero'
    if case=='restore-survey-missing':
        assert 'did not start' in record['restore_result']['survey']['error'] and not (out/'runtime/survey.json').exists()
    if case=='restore-survey-clear-mismatch':
        assert 'survey-clear-upload.bin' in record['failure'],(case,record.get('failure'))
    if case=='restore-clock-not-rom':
        assert 'not at the entry point' in record['watchdog']['deliberate_reset'] and record['fallback']['cpu_clocks']['cpu_l_pll']=='0xf8802600'
    with lock.open('a') as released:fcntl.flock(released,fcntl.LOCK_EX|fcntl.LOCK_NB)
    results.append({'case':case,'pass':True,'exit':status if isinstance(status,int) or status is None else 'stopped','retained':retained,'FEL_requests':len(fel_requests),'resets':events.count('reset')})
(a.evidence/'results.json').write_text(json.dumps({'physical_access':False,'pass':True,'cases':results,'host_sha256':hashlib.sha256((ROOT/'tools/run-ddr.py').read_bytes()).hexdigest()},indent=2)+'\n')
print('PASS DDR ownership cases:',len(results))
