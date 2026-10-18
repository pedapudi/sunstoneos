#!/usr/bin/env python3
"""Focused shared supervised transport and board capture checks, without hardware.

Every USB/access/FEL/guard operation is a fixture. The fixed production lock path
is redirected to a separate real flock; no physical-board process is inspected.
"""
import argparse
import copy
from contextlib import nullcontext
import fcntl
import hashlib
import importlib.util
import inspect
import json
from pathlib import Path
import shutil
import struct
import sys
import types
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


class Clock:
    def __init__(self):self.value=100.0
    def now(self):return self.value
    def pause(self,seconds):self.value+=max(seconds,.01)


def session(workload):
    raw=bytearray(592);struct.pack_into('<6Q',raw,0,0x4153373342535332,2,592,31,37,41)
    struct.pack_into('<I',raw,400,workload)
    profile=None
    if workload&1:
        profile=bytearray(1856);struct.pack_into('<QII',profile,0,0x3152464f52545353,2,len(profile))
        struct.pack_into('<4Q',profile,432,*[0x43010000+n*4096 for n in range(4)])
        struct.pack_into('<4Q',profile,464,*([4096]*4))
        struct.pack_into('<H',profile,len(profile)-520,3);profile=bytes(profile)
        raw[368:400]=hashlib.sha256(profile).digest();struct.pack_into('<2Q',raw,352,0x43000000,len(profile))
    return bytes(raw),profile


def frame(raw,usb_state,radio_state):
    header=struct.pack('<6I2Q',0x52414f42,1,usb_state,512 if usb_state==1 else 0,
        radio_state,3 if radio_state==2 else 0,*struct.unpack_from('<2Q',raw,24))
    return header+raw[368:400]+(bytes([0x5a])*512 if usb_state==1 else b'')


def observer_frame(host,packet,prepared):
    sequence=struct.unpack_from('<I',packet,4)[0];challenge=packet[32:64]
    raw=bytearray(512);struct.pack_into('<QII8Q',raw,0,host.FRAME_MAGIC,1,512,0,
        prepared['epoch'],prepared['generation'],prepared['lease'],2000,900000,480001000,255)
    raw[80:112]=challenge;raw[112:144]=bytes.fromhex(prepared['build_id'])
    for c in range(8):struct.pack_into('<5Q',raw,160+40*c,32,17+c,2+c,0,40 if not c else 0)
    raw[480:]=hashlib.sha256(raw[:480]).digest()
    return struct.pack('<4I',host.MESSAGE_MAGIC,sequence,0x80000031,512)+raw


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence',type=Path,required=True)
    args=parser.parse_args();e=args.evidence.resolve();e.mkdir(parents=True,exist_ok=True)
    board=load('board_host_checks',ROOT/'tools/check-a733-board-supervised.py');host=board.supervisor
    sources=[Path(__file__),Path(board.__file__),Path(host.__file__),Path(host.base.__file__),
        Path(host.observer.__file__),Path(board.preparation.__file__),Path(board.decoder.__file__)]
    def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
    state_path=e/'results.json'
    state=json.loads(state_path.read_text()) if state_path.exists() else {'physical_access':False,'cases':{}}
    identities={str(path.relative_to(ROOT)):sha(path) for path in sources}
    invocation=len(state.setdefault('invocations',[]))+1
    snapshots=e/f'inputs-{invocation}'
    while snapshots.exists():
        invocation+=1;snapshots=e/f'inputs-{invocation}'
    state.setdefault('snapshot_directories',[]).append(str(snapshots))
    for path in sources:
        destination=snapshots/path.relative_to(ROOT);destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,destination)
    state['invocations'].append(identities)
    def save():host.save_json(state_path,state)
    def case(name,fn,dependencies,logic):
        helpers=inspect.getsource(session)+inspect.getsource(frame)+inspect.getsource(Clock)
        if logic.__name__=='transport':helpers+=inspect.getsource(observer_frame)
        fingerprint=hashlib.sha256((name+inspect.getsource(logic)+helpers+''.join(identities[p] for p in dependencies)).encode()).hexdigest()
        if name in state['cases']:
            assert state['cases'][name]['source_identity']==fingerprint,'Changed completed inputs: '+name
            return
        directory=e/name
        if directory.exists():directory=e/(name+'-attempt-'+str(invocation))
        directory.mkdir(exist_ok=False)
        result=fn(directory);state['cases'][name]={'result':'pass','source_identity':fingerprint,**result};save()
    transport_dependencies=['tools/check-a733-smp-supervised.py','tools/check-a733-smp.py','tools/check-a733-smp-observe.py']
    board_dependencies=transport_dependencies+['tools/check-a733-board-supervised.py','apps/board/prepare.py','apps/board/observe.py']

    def transport(directory,kind,scenario):
        clock=Clock();events=[];memory={};phase=['before'];packet=[b'']
        bundle=directory/'bundle';bundle.mkdir(exist_ok=True)
        prepared={'epoch':31,'generation':37,'lease':41,'build_id':bytes(range(32)).hex(),'segments':[],'files':{},'_sizes':{}}
        for name,data in [('admission.json',b'{}'),('prepared.json',b'{"schema":"software-fixture-never-deploy"}'),
            ('result-zero.bin',bytes(16384)),('contract.bin',bytes(4096)),('request.bin',bytes(128)),
            ('report-zero.bin',bytes(128)),('launch.bin',b'fixture launcher, never executable')]:
            (bundle/name).write_bytes(data);prepared['files'][name]=sha(bundle/name);prepared['_sizes'][name]=len(data)
        fel=directory/'fixture-fel';fel.write_bytes(b'non-executable FEL fixture')
        guard_source=directory/'fixture-guard.py';guard_source.write_text('# Software fixture only.\n')
        lock_path=directory/'private-board.lock'
        port=directory/'fixture-port';port.mkdir(exist_ok=True)
        old=Path('/fixture-usb/101/010');native=Path('/fixture-usb/101/011')
        raw,profile=session(0 if scenario=='observed-failure' else 2)
        product,serial,revision=(host.PRODUCT,host.SERIAL,0x0101) if kind=='observer' else ('sunstoneos native board','sunstoneos-a733-board',0x0100)
        for name,text in [('bcdDevice',f'{revision:04x}'),('product',product),('serial',serial)]: (port/name).write_text(text)
        identity=lambda node:[1,10 if node==old else 11,3,4,5]
        access=types.SimpleNamespace(discover=lambda:old,require_session=lambda node:events.append('session-access'),
            require_rom=lambda node:events.append('rom-access'),identity=identity,target=lambda node:port,
            quarantine_instance=lambda *a:events.append('quarantine'))
        def session_node(config):
            if phase[0]=='before' or scenario=='clean-refusal':return old
            if scenario=='missing-native':return None
            if scenario=='unexpected-service':
                (port/'bcdDevice').write_text('02b3');return native
            return native
        access.session_node=session_node
        def fake_fel(command,**kwargs):
            assert command[:3]==[str(fel),'-d','101:10'],command
            arguments=command[3:];events.append(tuple(arguments))
            if arguments==['version']:return types.SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')
            if arguments==['exe','0x52000']:
                phase[0]='after'
                if scenario=='fel-transport-failure':return types.SimpleNamespace(returncode=1,stdout='',stderr='fixture failure')
                if scenario=='clean-refusal':memory[0x52d00]=struct.pack('<16Q',13,0,0,31,37,41,0x4153373352454631,*([0]*9))
                return types.SimpleNamespace(returncode=0,stdout='',stderr='')
            output=[];i=0
            while i<len(arguments):
                operation=arguments[i]
                if operation=='readl':assert arguments[i+1]=='0x02050014';output.append('0');i+=2
                elif operation=='read':
                    address,length=int(arguments[i+1],16),int(arguments[i+2]);destination=Path(arguments[i+3])
                    destination.write_bytes(memory.get(address,bytes([0x77])*length)[:length]);i+=4
                else:
                    assert operation=='write' and kwargs['pass_fds'];address=int(arguments[i+1],16)
                    memory[address]=Path(arguments[i+2]).read_bytes();i+=3
            return types.SimpleNamespace(returncode=0,stdout='\n'.join(output),stderr='')
        def write(endpoint,data,timeout):
            events.append('usb-write');assert endpoint==2
            if scenario=='usb-transport-failure':raise OSError('fixture USB failure')
            packet[0]=bytes(data);return len(data)
        def read(endpoint,length,timeout):
            events.append('usb-read');assert endpoint==0x81
            return observer_frame(host,packet[0],prepared) if kind=='observer' else frame(raw,2 if scenario=='observed-failure' else 3,0)
        device=types.SimpleNamespace(bcdDevice=revision,set_configuration=lambda value:events.append('configure'),write=write,read=read)
        usb=types.ModuleType('usb');core=types.ModuleType('usb.core');usb.core=core
        def find_native(**kw):
            assert kw=={'idVendor':0x1f3a,'idProduct':0xefe8,'bus':101,'address':11}
            events.append('find-native');return device
        core.find=find_native
        guard=types.ModuleType('usb_guard');guard.__file__=str(guard_source)
        guard.check_health=lambda:events.append('guard-health');guard.reenumeration=lambda *a:nullcontext()
        def fixture_open(path,*a,**kw):
            assert path=='/tmp/mimus-board.lock','Unexpected production open: '+str(path)
            return open(lock_path,*a,**kw)
        def hold():
            with open(lock_path,'a') as other:
                try:fcntl.flock(other,fcntl.LOCK_EX|fcntl.LOCK_NB)
                except BlockingIOError:events.append('held-private-flock')
                else:raise AssertionError('Ownership released before hold')
        real_finish,real_stop=host.finish_after_observation,host.base.stop_after_failure
        def finish(record,save,quarantine):return real_finish(record,save,quarantine,hold=hold)
        def stop(record,save,quarantine,error):return real_stop(record,save,quarantine,error,hold=hold)
        real_capture=host.capture;real_board_capture=board.capture
        def capture(*a,**kw):return real_capture(*a,**kw,now=clock.now,pause=clock.pause)
        def board_capture(*a,**kw):return real_board_capture(*a,**kw,now=clock.now,pause=clock.pause)
        def capture_device(device,same,deadline,record,save):
            events.append('capture-'+kind)
            if kind=='observer':return host.capture_observer_device(device,same,deadline,prepared,directory/'runtime',record,save)
            return board.capture_device(device,same,deadline,raw,profile,directory/'runtime',record,save)
        settings=types.SimpleNamespace(fel=fel,evidence=directory/'runtime',claim='fixture-claim',owner='fixture-owner')
        def access_module(name,path):
            assert name=='observe_usb_access' and path==ROOT/'tools/usb-access.py'
            return access
        patches=[patch.object(host,'module',access_module),patch.object(host,'open',fixture_open,create=True),
            patch.object(host.base,'FEL_HASH',sha(fel)),patch.object(host.subprocess,'run',fake_fel),
            patch.object(host.time,'monotonic',clock.now),patch.object(host.time,'sleep',clock.pause),
            patch.object(host.os,'access',lambda *a:True),patch.object(host,'finish_after_observation',finish),
            patch.object(host.base,'stop_after_failure',stop),patch.object(host,'capture',capture),patch.object(board,'capture',board_capture),
            patch.dict(sys.modules,{'usb':usb,'usb.core':core,'usb_guard':guard})]
        from contextlib import ExitStack
        error=None
        with ExitStack() as stack:
            for item in patches:stack.enter_context(item)
            try:
                code=host.run_session(settings,bundle,prepared,lambda:events.append('claim-check'),
                    lambda observed,p:events.append('entry-check'),capture_device,
                    product=product,serial=serial,bcd_device=revision,capture_seconds=.05,
                    result_key='scheduler_result' if kind=='observer' else 'board_result')
            except (ValueError,OSError,RuntimeError) as caught:error=caught;code=None
        record=json.loads((directory/'runtime/session.json').read_text())
        assert events.count(('exe','0x52000'))==1
        if scenario=='clean-refusal':
            assert not error and code==2 and record['clean_refusal'] and record['sram_restored'] and record['ownership_release_permitted']
            assert events.count(('read','0x52d00','128',str(directory/'runtime/refusal-report.bin')))==1
            assert 'held-private-flock' not in events and 'quarantine' not in events
        else:
            assert not any(isinstance(item,tuple) for item in events[events.index(('exe','0x52000'))+1:])
            assert 'held-private-flock' in events and 'quarantine' in events
            if scenario=='success':assert not error and code==0 and record['complete'] and events.count('capture-'+kind)==1
            elif scenario=='observed-failure':assert not error and code==2 and not record['complete'] and record['board_result']=='observed-failure' and events.count('usb-write')==1
            else:assert error and not record['complete']
        if scenario in ('fel-transport-failure','usb-transport-failure'):
            assert record['transport_failed'] and events.count('usb-write')==(scenario=='usb-transport-failure')
        # The sentinel returns instead of waiting forever. Verify release only
        # after run_session returned; every hold call saw the real fixture flock.
        with open(lock_path,'a') as released:fcntl.flock(released,fcntl.LOCK_EX|fcntl.LOCK_NB)
        return {'events':events,'code':code,'error':str(error) if error else None,'record':record,
            'limits':'Sentinel holds verify real private-lock ownership and return; production holds remain blocking.'}

    for kind,scenario in [('observer','success'),('board','success'),('board','clean-refusal'),
        ('board','missing-native'),('board','observed-failure'),('board','unexpected-service'),('board','fel-transport-failure'),('board','usb-transport-failure')]:
        case('transport-'+kind+'-'+scenario,lambda directory,k=kind,s=scenario:transport(directory,k,s),
            board_dependencies if kind=='board' else transport_dependencies,transport)

    def capture_case(directory,workload,variation):
        raw,profile=session(workload);clock=Clock();calls=[];received=[];record={'board_result':'unobserved','complete':False};committed=[]
        success=frame(raw,1 if workload<2 else 3,2 if workload&1 else 0)
        pending=frame(raw,0 if workload<2 else 3,1 if workload&1 else 0)
        frames=[pending,success] if variation=='pending-success' and workload!=2 else [success]
        if variation=='stale':
            value=bytearray(success);struct.pack_into('<Q',value,24,32);frames=[bytes(value)]
        if variation=='malformed':frames=[success+b'extra']
        if variation=='disabled-host':frames=[frame(raw,1,0)]
        if variation=='disabled-radio':frames=[frame(raw,3,2)]
        if variation=='failure':frames=[frame(raw,2,0)]
        if variation=='radio-failure':frames=[frame(raw,3,3)]
        def exchange(packet,timeout):
            calls.append(packet);assert packet==b'\0' and 0<timeout<=500
            if variation=='late':clock.value=200
            value=frames.pop(0);received.append(value);return value
        def save_record():
            if variation=='save-failure' and record.get('board_result')=='observed-success':raise OSError('fixture journal failure')
            committed.append(copy.deepcopy(record));host.save_json(directory/'session.json',record)
        error=None
        try:result=board.capture(exchange,raw,profile,directory,record,save_record,150,now=clock.now,pause=clock.pause)
        except (ValueError,OSError,RuntimeError) as caught:error=caught;result=None
        if variation=='pending-success':
            assert not error and len(calls)==(1 if workload==2 else 2) and record['board_result']=='observed-success'
            assert host.digest(directory/record['durable_result']['path'])==record['durable_result']['sha256']
            assert (directory/'sector.bin').exists()==(workload<2)
        elif variation in ('failure','radio-failure'):assert not error and len(calls)==1 and record['board_result']=='observed-failure'
        else:
            assert error and result is None and len(calls)==1 and not record['complete']
            assert not any(v.get('board_result')=='observed-success' for v in committed)
            if variation=='save-failure':
                assert record['board_result']=='unobserved' and 'durable_result' not in record
                assert (directory/'sector.bin').read_bytes()==success[72:]
        # Raw evidence precedes decoding, deadline checks and journal publication.
        assert (directory/'board-001.bin').read_bytes()==received[0]
        return {'calls':len(calls),'error':str(error) if error else None,'committed':committed,
            'limits':'Copied USB frames and journal failures are fixtures, not device qualification.'}
    for workload in range(4):
        case('capture-workload-'+str(workload),lambda directory,w=workload:capture_case(directory,w,'pending-success'),board_dependencies,capture_case)
    for variation,workload in [('stale',2),('malformed',2),('disabled-host',2),('disabled-radio',2),('failure',0),('radio-failure',3),('late',2),('save-failure',0)]:
        case('capture-'+variation,lambda directory,v=variation,w=workload:capture_case(directory,w,v),board_dependencies,capture_case)

    def transfer_case(directory,variation):
        raw,profile=session(2);clock=Clock();calls=[];record={'complete':False};actual_capture=board.capture
        def write(endpoint,packet,timeout):
            calls.append('write')
            if variation=='expired-after-write':clock.value=200
            return 0 if variation=='partial-write' else len(packet)
        def read(endpoint,length,timeout):calls.append('read');return b'partial frame'
        device=types.SimpleNamespace(write=write,read=read)
        with patch.object(board.time,'monotonic',clock.now),patch.object(board,'capture',
            lambda *a,**kw:actual_capture(*a,**kw,now=clock.now,pause=clock.pause)):
            try:board.capture_device(device,lambda:None,150,raw,profile,directory,record,lambda:None)
            except ValueError:pass
            else:raise AssertionError('Invalid transfer accepted')
        assert calls==(['write','read'] if variation=='partial-read' else ['write']) and not record['complete']
        if variation=='partial-read':assert (directory/'board-001.bin').read_bytes()==b'partial frame'
        return {'calls':calls}
    for variation in ('partial-write','partial-read','expired-after-write'):
        case('transfer-'+variation,lambda directory,v=variation:transfer_case(directory,v),board_dependencies,transfer_case)

    def crossmode_case(directory):
        raw,profile=session(1);legacy=bytearray(raw)
        struct.pack_into('<2Q',legacy,0,0x41533733424f4131,1)
        old_profile=bytearray(profile);struct.pack_into('<I',old_profile,8,1)
        supervised=bytearray(raw);supervised[368:400]=hashlib.sha256(old_profile).digest()
        for admitted,radio in [(bytes(legacy),profile),(bytes(supervised),bytes(old_profile))]:
            packet=frame(admitted,1,2)
            try:board.decoder.decode(packet,admitted,radio)
            except ValueError:pass
            else:raise AssertionError('Cross-mode radio profile accepted')
        return {'refused':['legacy session with raw supervised radio profile','supervised session with armed radio profile']}
    case('decoder-crossmode-profile',crossmode_case,board_dependencies,crossmode_case)

    def bundle_case(directory):
        raw,profile=session(3);prepared={'files':{}}
        for name,data in [('session.bin',raw),('profile.bin',profile)]:
            path=directory/name;path.write_bytes(data);prepared['files'][name]=sha(path)
            frozen=board.bundle_bytes(directory,prepared,name);path.write_bytes(b'mutated after validation')
            assert frozen==data
            try:board.bundle_bytes(directory,prepared,name)
            except ValueError:pass
            else:raise AssertionError('Changed bundle bytes accepted: '+name)
        return {'refused':['session.bin','profile.bin']}
    case('bundle-mutation',bundle_case,board_dependencies,bundle_case)
    state['result']='PASS: actual shared transport, board capture and retained failure ownership';save()
    print(state['result'])

if __name__=='__main__':main()
