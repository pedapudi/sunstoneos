#!/usr/bin/env python3
"""Focused supervised admission, exact boot entry, launcher and capture lifecycle.

No hardware access. Existing scheduler/coherence/USB instruction evidence remains
separate; the new cases cover changed admission and completion behavior only.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import inspect
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from contextlib import contextmanager
from types import SimpleNamespace, ModuleType
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--clang',default='/usr/lib/llvm-21/bin/clang')
    p.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    a=p.parse_args();e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=True)
    host=module('supervised_host',ROOT/'tools/check-a733-smp-supervised.py')
    image=a.image.resolve();image_hash=host.digest(image)
    state_path=e/'results.json'
    state=json.loads(state_path.read_text()) if state_path.exists() else {'physical_access':False,'image_sha256':image_hash,'steps':{}}
    assert state['image_sha256']==image_hash
    # Resume only unchanged inputs; completed steps are never replayed.
    inputs={str(p):host.digest(p) for p in [Path(__file__),ROOT/'tools/check-a733-smp-supervised.py',
        ROOT/'tools/check-a733-smp-observe.py',ROOT/'tools/check-a733-smp.py',
        ROOT/'platform/a733-smp-supervised/admission.c',ROOT/'platform/a733-smp-supervised/launch.c',
        ROOT/'platform/a733-smp-supervised/boot.patch',ROOT/'checks/a733-smp-supervised-launch.py',
        ROOT/'checks/a733-smp-cached.py']}
    state.setdefault('invocations',[]).append(inputs)
    shutil.copy2(__file__,e/f'executed-check-{len(state["invocations"])}.py')
    def save():host.save_json(state_path,state)
    def run(command,name):
        command=list(map(str,command));result=subprocess.run(command,capture_output=True,text=True)
        log=e/(name+'.log')
        if log.exists():log=e/(name+'-'+str(len(state.get('commands',[])))+'.log')
        log.write_text(result.stdout+result.stderr)
        state.setdefault('commands',[]).append(command);save()
        if result.returncode:raise RuntimeError('Failed '+name+'; resume after resolving its cause.')
    def step(name,fn):
        dependencies={'launcher':['platform/a733-smp-supervised/launch.c','checks/a733-smp-supervised-launch.py'],
            'admission':['platform/a733-smp-supervised/admission.c'],
            'boot':['checks/a733-smp-cached.py','platform/a733-smp-supervised/boot.patch'],
            'entry-records':['tools/check-a733-smp-supervised.py','tools/check-a733-smp-observe.py'],
            'host':['tools/check-a733-smp-supervised.py','tools/check-a733-smp-observe.py','tools/check-a733-smp.py']}
        identity=hashlib.sha256(inspect.getsource(fn).encode()+''.join(inputs[str(ROOT/n)] for n in dependencies[name]).encode()).hexdigest()
        if name in state['steps']:
            assert state['steps'][name]['input_identity']==identity,'Completed step inputs changed: '+name
        else:
            result=fn();result['input_identity']=identity;state['steps'][name]=result;save()
    bundle=e/'fixture-bundle';bundle.mkdir(exist_ok=True)
    _,build_address,build_id=host.image_parts(image)
    request,contract=host.request_bytes(build_address,build_id,31,37,41,43,hashlib.sha256(b'explicit software fixture only').digest())
    segments,_,_=host.image_parts(image)
    def launcher():
        # This fixture deliberately lacks a deployable prepared.json schema.
        # Real physical admission is supplied separately by the coordinator.
        for name in ('launch.c','launch.S','launch.ld'):
            source=ROOT/('platform/a733-smp-supervised' if name=='launch.c' else 'platform/a733-smp-observe')/name
            shutil.copy2(source,bundle/name)
        shutil.copy2(ROOT/'platform/a733-smp-cached/boot.h',bundle/'boot.h')
        flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding',
            '-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
            '-Os','-g','-Wall','-Wextra','-Werror','-DA733_SMP_CONTRACT_MAGIC=0x4153373353554333ULL','-I'+str(bundle)]
        for src,obj in [('launch.S','entry.o'),('launch.c','launch.o')]:
            run([a.clang,*flags,'-c',bundle/src,'-o',bundle/obj],obj)
        run([a.lld,'-T',bundle/'launch.ld',bundle/'entry.o',bundle/'launch.o','-o',bundle/'launch.elf'],'launcher-link')
        run(['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',bundle/'launch.elf',bundle/'launch.bin'],'launcher-binary')
        (bundle/'request.bin').write_bytes(request);(bundle/'contract.bin').write_bytes(contract)
        payloads=[]
        for i,s in enumerate(segments):
            if s['data']:
                name=f'segment-{i}.bin';(bundle/name).write_bytes(s['data']);payloads.append({'name':name,'address':s['address']})
        host.save_json(bundle/'prepared.json',{'schema':'software-fixture-never-deploy','segments':payloads})
        run([sys.executable,ROOT/'checks/a733-smp-supervised-launch.py','--bundle',bundle,'--output',e/'launcher-cases.json'],'launcher-cases')
        return {'result':'pass','cases':10,'launcher_sha256':host.digest(bundle/'launch.bin')}
    step('launcher',launcher)
    def admission():
        wrapper=e/'identity.c';wrapper.write_text('const unsigned char a733_smp_build_id[32]={'+','.join(map(str,build_id))+'};\n')
        library=e/'admission.so'
        run([a.clang,'-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-DA733_SMP_CONTRACT_MAGIC=0x4153373353554333ULL',
            '-I'+str(ROOT/'platform/a733-smp-cached'),ROOT/'platform/a733-smp-supervised/admission.c',wrapper,'-o',library],'admission-compile')
        lib=ctypes.CDLL(str(library));lib.a733_smp_contract_valid.argtypes=[ctypes.c_void_p,ctypes.c_uint64]
        data=bytearray(contract[:224]);struct.pack_into('<Q',data,0,host.CONTRACT_MAGIC)
        struct.pack_into('<4Q',data,80,1000,24001000,480001000,480001000)
        def accepted(value,now=2000):return lib.a733_smp_contract_valid(ctypes.create_string_buffer(bytes(value)),now)==1
        assert accepted(data)
        for offset,value in [(8,1),(120,0x41),(176,0),(96,480001001)]:
            changed=data.copy();struct.pack_into('<Q',changed,offset,value);assert not accepted(changed)
        assert not accepted(data,24001000)
        return {'result':'pass','cases':6}
    step('admission',admission)
    def boot():
        model=module('cached_boot_model',ROOT/'checks/a733-smp-cached.py')
        from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_SP
        outcomes=[]
        for name,mode,version,expected in [('disabled',0,3,'admitted'),('armed',0x41,3,'refused'),('old-contract',0,1,'refused')]:
            machine=model.Machine(image);machine.mmio.update({0x02050014:mode,0x02050010:0,0x070501c0:0,0x070501d4:0})
            data=bytearray(contract[:224]);struct.pack_into('<Q',data,0,host.CONTRACT_MAGIC);struct.pack_into('<Q',data,8,version)
            struct.pack_into('<4Q',data,80,0,24000000,480000000,480000000)
            ctypes.memmove(machine.ram_pointer+0x1004000,bytes(data),len(data))
            outcome=[]
            def hook(u,address,size,c):
                if address==machine.symbols['a733_cached_prepare']:
                    outcome.append('admitted');u.emu_stop();return
                if address==machine.symbols['a733_smp_stop']:
                    outcome.append('refused');u.emu_stop();return
            # Existing hooks resolve the bound method at registration; replace code hook only.
            from unicorn import UC_HOOK_CODE
            machine.cpus[0].hook_add(UC_HOOK_CODE,hook,0)
            # The unchanged cold assembly clears a large BSS; enter the changed
            # boot function with its real stack and already-zero modeled RAM.
            machine.cpus[0].reg_write(UC_ARM64_REG_SP,machine.symbols['__a733_smp_boot_stack_top'])
            machine.cpus[0].emu_start(machine.symbols['a733_smp_boot'],0,count=100000)
            assert outcome==[expected],(name,outcome)
            assert all(machine.mmio[address]==value for address,value in
                [(0x02050014,mode),(0x02050010,0),(0x070501c0,0),(0x070501d4,0)])
            outcomes.append({'case':name,'result':expected})
        return {'result':'pass','cases':outcomes,'limits':'Modeled architectural entry/readback; no physical coherence or all-core runtime claim.'}
    step('boot',boot)
    def lifecycle():
        prepared={'epoch':31,'generation':37,'lease':41,'build_id':build_id.hex()}
        challenge=hashlib.sha256(b'supervised capture fixture').digest()
        def frame(sequence,success=True):
            payload=bytearray(512)
            struct.pack_into('<QII8Q',payload,0,host.FRAME_MAGIC,1,512,0 if success else 1,31,37,41,
                2000 if success else 0,900000 if success else 0,480001000,255 if success else 0)
            payload[80:112]=challenge;payload[112:144]=build_id
            if success:
                for c in range(8):struct.pack_into('<5Q',payload,160+40*c,32,17+c,2+c,0,40 if not c else 0)
            payload[480:]=hashlib.sha256(payload[:480]).digest()
            return struct.pack('<4I',host.MESSAGE_MAGIC,sequence,0x80000031,512)+payload
        capture=e/'capture';capture.mkdir(exist_ok=True);calls=[];record={'complete':False}
        def exchange(packet,timeout):
            calls.append(packet);return frame(len(calls),len(calls)==2)
        host.capture(exchange,prepared,challenge,capture,record,lambda:None,100,now=lambda:1,pause=lambda _:None)
        assert len(calls)==2 and record['scheduler_result']=='observed-success' and host.digest(capture/'observer-02.bin')==record['durable_result']['sha256']
        # A USB failure or malformed result propagates immediately; no retry/recovery request.
        for error in [OSError('CPU0 halt loses USB'),ValueError('malformed reply')]:
            attempts=[]
            def broken(packet,timeout):attempts.append(packet);raise error
            try:host.capture(broken,prepared,challenge,e,{},lambda:None,100,now=lambda:1)
            except type(error):pass
            else:raise AssertionError('Transport failure suppressed')
            assert len(attempts)==1
        # Terminal session result is capture success, independent of ROM return.
        events=[];record={'scheduler_result':'unobserved'}
        saved_preload=host.base.preload
        def preload(*args):prepared['_backup_hashes']={};events.append('preload')
        host.base.preload=preload
        def observe(started):events.append('capture');record['scheduler_result']='observed-success'
        def transaction(*args,**kwargs):events.append(args);return '0'
        try:host.launch_once(transaction,observe,e,prepared,e,record,lambda:None,lambda:events.append('review'))
        finally:host.base.preload=saved_preload
        assert record['complete'] and record['manual_reconnect_required'] and 'fresh_rom_response' not in record
        assert sum(x==('exe','0x52000') for x in events)==1
        retained=[]
        host.retain_after_capture(record,lambda:retained.append('saved'),lambda:retained.append('quarantined'),lambda:retained.append('held'))
        assert retained==['saved','quarantined','held']
        try:host.launch_once(transaction,observe,e,prepared,e,record,lambda:None,lambda:None)
        except ValueError:pass
        else:raise AssertionError('Second attempt accepted')
        # Exercise the shared operational transport without queue, review,
        # recovery or admission records. The endpoint and discovery are modeled.
        evidence=e/'shared-session';evidence.mkdir();(evidence/'prior.txt').write_text('preserve prior evidence')
        port=e/'port';port.mkdir()
        for name,text in [('bcdDevice','0101'),('product',host.PRODUCT),('serial',host.SERIAL)]:
            (port/name).write_text(text)
        initial=Path('/dev/bus/usb/003/051');native=Path('/dev/bus/usb/003/052');held=[];requests=[];memory={}
        @contextmanager
        def lock():
            held.append(True)
            try:yield initial
            finally:held.pop()
        @contextmanager
        def enumeration(*args):yield
        access=SimpleNamespace(board_lock=lock,require_session=lambda n:None,require_rom=lambda n:None,
            identity=lambda n:[1,2,3,4,51 if n==initial else 52],target=lambda n:port,
            session_node=lambda config:native,quarantine_instance=lambda *a:None)
        def fel(command,**kwargs):
            assert held
            parts=command[3:];requests.append(parts)
            if parts[0]=='version':return SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')
            if parts[0]=='readl':return SimpleNamespace(returncode=0,stdout='0',stderr='')
            if parts[0]!='exe':
                while parts:
                    op,address=parts[:2];parts=parts[2:]
                    if op=='read':
                        size=int(parts[0]);Path(parts[1]).write_bytes(memory.get(address,bytes(size)));parts=parts[2:]
                    elif op=='write':memory[address]=Path(parts[0]).read_bytes();parts=parts[1:]
                    else:raise AssertionError(op)
            return SimpleNamespace(returncode=0,stdout='',stderr='')
        device=SimpleNamespace(bcdDevice=0x0101,set_configuration=lambda value:None)
        usb=ModuleType('usb');core=ModuleType('usb.core');usb.core=core;core.find=lambda **kwargs:device
        guard=SimpleNamespace(check_health=lambda:None,reenumeration=enumeration,__file__=str(ROOT/'tools/usb_guard.py'))
        def preload_session(*args):prepared['_backup_hashes']={}
        def capture_device(device,same,deadline,record,save):same();record['scheduler_result']='observed-success';save()
        def retain_session(record,save,quarantine):
            assert held and record['complete'];save();quarantine()
        session_args=SimpleNamespace(fel=e/'unreviewed-fel',evidence=evidence)
        # The FEL bytes are not a release credential; this fake uses no file.
        with patch.object(host,'module',lambda *a:access),patch.dict(sys.modules,{'usb_guard':guard,'usb':usb,'usb.core':core}), \
             patch.object(host.base,'preload',preload_session),patch.object(host.base.subprocess,'run',fel), \
             patch.object(host.os,'access',lambda *a:True),patch.object(host,'finish_after_observation',retain_session), \
             patch.object(host.base,'stop_after_failure',lambda record,save,quarantine,error:(_ for _ in ()).throw(error)):
            assert host.run_session(session_args,bundle,prepared,lambda:None,lambda identity,port:None,capture_device,
                product=host.PRODUCT,serial=host.SERIAL,bcd_device=0x0101,capture_seconds=19,result_key='scheduler_result')==0
        assert not held and (evidence/'prior.txt').read_text()=='preserve prior evidence'
        assert session_args.evidence.parent==evidence and sum(x[0]=='exe' for x in requests)==1
        saved=json.loads((session_args.evidence/'session.json').read_text())
        assert saved['complete'] and 'claim' not in saved and 'owner' not in saved
        return {'result':'pass','cases':6,'without_process_records':True,
                'limits':'Host lifecycle uses modeled I/O; no physical USB qualification.'}
    step('host',lifecycle)
    def entry_records():
        identity=[1,2,3,4,5];port='fixture-port'
        host.validate_admission({'reservation':43},image_hash,identity,port)
        value={'reservation':43, 'image_sha256':image_hash, 'identity':identity, 'port':port}
        host.validate_admission(value,image_hash,identity,port)
        failures=[]
        for field,replacement in [('identity',[1,2,3,4,6]),('image_sha256','00'*32),('port','other'),('reservation',0)]:
            changed=dict(value);changed[field]=replacement
            try:host.validate_admission(changed,image_hash,identity,port)
            except ValueError:failures.append(field)
            else:raise AssertionError('Invalid native session accepted: '+field)
        return {'result':'pass','refusals':failures,'without_process_records':True,
                'limits':'Native session validation only; no physical qualification.'}
    step('entry-records',entry_records)
    def ref(path):return {'path':str(path.resolve()),'sha256':host.digest(path)}
    preserved={}
    for name,path in [('host_tool',Path(host.__file__)),('base_host_tool',Path(host.base.__file__)),
                      ('observer_host_tool',Path(host.observer.__file__))]:
        destination=e/(name+'.py');shutil.copy2(path,destination);preserved[name]=ref(destination)
    state['result']='PASS: supervised admission, linked boot, launcher and host lifecycle';save()
    host.save_json(e/'software.json',{'schema':'a733-smp-supervised-software-v1','physical_access':False,
        'admission_cases':True,'launcher_cases':True,'host_lifecycle':True,
        'image':ref(image),'launcher':ref(bundle/'launch.bin'),**preserved,'evidence':[ref(state_path)]})
    print(state['result'])

if __name__=='__main__':main()
