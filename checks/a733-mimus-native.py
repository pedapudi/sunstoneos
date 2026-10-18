#!/usr/bin/env python3
"""Check native admission packaging, watchdog lifecycle and durable capture.

The real linked supervisor executes under instruction emulation. Recognition
arithmetic and existing parity evidence are outside this lifecycle check.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_X0

ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--evidence',type=Path,required=True)
    args=parser.parse_args();args.evidence.mkdir(parents=True,exist_ok=False)
    host=module('native_mimus_host',ROOT/'tools/check-a733-mimus-native.py')
    existing=module('native_storage_fixture',ROOT/'checks/a733-storage-maintenance.py')
    results=[]
    for label,progress,finished,revoked,feeds in [
        ('scheduler activity earns no renewal',0,0,False,0),
        ('completed inference work earns renewal',1,0,False,1),
        ('finished recognition stops renewal',1,1,False,0),
        ('revocation prevents renewal',1,0,True,0)]:
        native=existing.Native(args.image);native.sys['cntpct_el0']=24000000
        def put(name,value,size=8):native.u.mem_write(native.symbols[name],value.to_bytes(size,'little'))
        put('state',5 if revoked else 1,4);put('finished',finished,4)
        put('live_until',5000000);put('minimum',4000000);put('inference_progress',progress)
        native.u.mem_write(native.symbols['window'],struct.pack('<2Q3I',30000000,1000,0,0,0))
        observed=[]
        def step(u,pc,size,unused):
            if pc==native.symbols['a733_watchdog_hw_reload']:
                observed.append('feed')
                before=native.reg('x6');after=native.reg('x7')
                u.mem_write(before,struct.pack('<Q',1000000));u.mem_write(after,struct.pack('<Q',1000001))
                u.reg_write(UC_ARM64_REG_X0,1);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
            elif pc==native.symbols['vTaskDelay']:
                native.stop='tick';u.emu_stop()
        native.u.hook_add(UC_HOOK_CODE,step)
        native.call('supervise')
        assert len(observed)==feeds,(label,observed)
        results.append(label)
    native=existing.Native(args.image)
    native.call('mimus_native_input_progress');assert native.q(native.symbols['inference_progress'])==0
    native.call('mimus_native_waiting',(1,));native.call('mimus_native_input_progress')
    assert native.q(native.symbols['inference_progress'])==1
    native.call('mimus_native_waiting',(0,));native.call('mimus_native_input_progress')
    assert native.q(native.symbols['inference_progress'])==1
    results.append('paced sample progress earns renewal only while waiting for input')
    config=dict(minimum_reload_us=3900000,maximum_recovery_us=5000000,guard_us=1000,
                reset_by_us=40000000,deadline_us=30000000,rtc=[0,0,0],owners=[22,33],topology=1,
                qualification_sha256='67'*32,topology_artifact='89'*32,usb_supply_uv=[3300000,3300000,1200000,900000],
                fabric=[1,2,3],fabric_hz=[24000000]*3,usb_cold_reference=1,pmic_expected=[[0,0,0],[0,0,0]])
    assets=bytearray(320);assets[:8]=b'SUNMIM01';struct.pack_into('<3Q',assets,8,1,1,1)
    (args.evidence/'assets.bin').write_bytes(assets)
    (args.evidence/'configuration.json').write_text(json.dumps(config))
    bundle=args.evidence/'bundle'
    host.prepare(args.image,args.evidence/'assets.bin',args.evidence/'configuration.json',bundle,True,
                 '/usr/bin/clang','/home/sunil/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    host.validate_bundle(bundle);results.append('actual native ELF and shared ARM launcher package without hardware access')
    prepared=json.loads((bundle/'prepared.json').read_text());prepared['segments'][0]['address']=0x0205000c
    (bundle/'prepared.json').write_text(json.dumps(prepared))
    try:host.validate_bundle(bundle)
    except ValueError:results.append('modified upload address refused before physical transaction')
    else:raise AssertionError('modified upload address accepted')
    for label,initially_complete in [('payload-terminal',True),('empty-terminal',False)]:
        calls=[];payload=b'PASS mimus: fixture capture\n'
        def exchange(request):
            offset,ack=struct.unpack('<2I',request);calls.append((offset,ack))
            if len(calls)==1:
                return struct.pack('<4I',len(payload),int(initially_complete),0,len(payload))+payload
            assert (offset,ack)==(len(payload),len(payload))
            return struct.pack('<4I',len(payload),1,offset,0)
        receipt=host.capture(exchange,args.evidence/label)
        assert receipt['durable'] and len(calls)==2
        assert (args.evidence/label/'recognition.log').read_bytes()==payload
        results.append(label+' capture persists output before terminal acknowledgement')
    def malformed(request):return struct.pack('<4I',0,1,1,0)
    try:host.capture(malformed,args.evidence/'malformed')
    except ValueError:results.append('malformed capture offset refused')
    else:raise AssertionError('malformed capture accepted')
    record=dict(passed=True,image_sha256=host.digest(args.image),cases=results,physical_access=False)
    (args.evidence/'results.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(record,indent=2))


if __name__=='__main__':main()
