#!/usr/bin/env python3
"""Check the state machine and execute the linked native cutoff task."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"checks"))
import harness as h
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
CHECK_INPUTS=['apps/usb-vbus','drivers/i2c/a733_twi.c','runtime/deadline.c','runtime/deadline-a733.c','platform/a733/probe.c']
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--guard-only',action='store_true');p.add_argument('--init-only',action='store_true');p.add_argument('--old-image',type=Path);p.add_argument('--image',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
if not a.guard_only and not a.init_only:
    subprocess.run([str(h.CLANG),'-std=c11','-O2','-Wall','-Wextra','-Werror',str(ROOT/'apps/usb-vbus/diagnostic.c'),str(ROOT/'apps/usb-vbus/model.c'),'-o',str(a.evidence/'model')],check=True)
    subprocess.run([str(a.evidence/'model')],check=True)
image=h.Image(a.image)
def put(u,address,value,n=4):u.mem_write(address,int(value).to_bytes(n,'little'))
def get(u,address,n=4):return int.from_bytes(u.mem_read(address,n),'little')
if not a.init_only:
    results=[]
    for case in ('nominal-cutoff','late-IRQ','lost-wake','provider-stalled'):
        u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000)
        for address,data in image.parts:
            if data:u.mem_write(address,data)
        u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000);u.reg_write(UC_ARM64_REG_PSTATE,5)
        for address,value in ((0x07025004,0x330001),(0x07025010,0x1a5),(0x0701019c,0x20002)):
            put(u,address,value)
        start=999000 if case=='nominal-cutoff' else 1000
        until=1001000 if case=='nominal-cutoff' else 3000
        put(u,image.address('enable_attempt_us'),1000,8);put(u,image.address('off_confirm_us'),0,8)
        put(u,image.address('work_deadline'),1100000,8);put(u,image.address('on_until'),until,8);put(u,image.address('heartbeat'),start,8)
        state={'now':start,'waits':0,'cutoff':None,'stopped':False}
        def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
        def code(cpu,address,size,cookie):
            word=get(u,address);kind=word&0xffffffe0;reg=word&31
            if kind==0xd53be020:u.reg_write(UC_ARM64_REG_X0+reg,state['now']*24);u.reg_write(UC_ARM64_REG_PC,address+4);return
            if address==image.address('sunstone_deadline_register'):ret(1);return
            if address in (image.address('vPortEnterCritical'),image.address('vPortExitCritical')):ret();return
            if address==image.address('sunstone_deadline_wait_until'):
                until=u.reg_read(UC_ARM64_REG_X0);assert state['now']<until<=state['now']+500
                state['waits']+=1;state['now']=until
                if case=='late-IRQ':state['now']+=600
                if case=='lost-wake' and state['waits']==2:state['now']+=2100
                if case!='provider-stalled':put(u,image.address('heartbeat'),state['now'],8)
                if state['waits']==8:put(u,image.address('stop_guard'),1)
                ret(1);return
            if address==image.address('vTaskSuspend'):state['stopped']=True;u.emu_stop();return
        def write(cpu,access,address,size,value,cookie):
            if address==0x07025010 and not value&0x100 and state['cutoff'] is None:state['cutoff']=state['now']
        u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_WRITE,write)
        u.emu_start(image.address('guard',unit='apps/usb-vbus/main.c'),0,count=30000)
        assert state['stopped'] and state['cutoff'] is not None and get(u,0x07025010)==0xa5,(case,state)
        assert get(u,image.address('revoked'))==1 and get(u,image.address('guard_fault'))==0
        if case=='nominal-cutoff':
            assert state['cutoff']==1001000
            failure=get(u,image.address('diagnostic')+image.layouts['vbus_diagnostic']['failure'])
            assert failure==0
            on_us=get(u,image.address('off_confirm_us'),8)-get(u,image.address('enable_attempt_us'),8)
            assert on_us==1000000
            ds=importlib.util.spec_from_file_location('guard_decoder',ROOT/'apps/usb-vbus/decoder.py')
            decoder=importlib.util.module_from_spec(ds);ds.loader.exec_module(decoder)
            raw=struct.pack('<26Q',0x3154505256425353,1,failure,7,1,1,20000,30000,500,100,
                            get(u,image.address('guard_max_gap'),8),0,on_us,0x330001,get(u,0x07025010),1,0xf8,400000,40,1,0x04000000,0x10001,0x983fffb7,0xffffffff,0xffffffff,0xffffffff)
            assert not decoder.failures(decoder.decode(raw))
            first=get(u,image.address('first_off_request_us'),8)
            assert first==state['cutoff']
            # A later owner shutdown and repeated GPIO-low acknowledgment cannot
            # extend the 650ms origin established by the native guard.
            state['now']=first+5000
            off_at=image.address('diagnostic')+image.layouts['vbus_diagnostic']['off_at']
            put(u,off_at,state['now'],8)
            u.reg_write(UC_ARM64_REG_LR,0x43fff000)
            u.emu_start(image.address('retain_off_origin'),0x43fff000,count=1000)
            assert get(u,off_at,8)==first
            u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_X0+1,0)
            u.reg_write(UC_ARM64_REG_LR,0x43fff000)
            u.emu_start(image.address('output',unit='apps/usb-vbus/main.c'),0x43fff000,count=1000)
            assert get(u,image.address('first_off_request_us'),8)==first
            assert state['now']-get(u,off_at,8)==5000
        if case=='lost-wake':assert get(u,image.address('guard_max_gap'),8)>2000
        results.append({'case':case,**state})
    (a.evidence/'guard.json').write_text(json.dumps(results,indent=2)+'\n')
    print('PASS linked guard: nominal deadline with live enable timestamp and final decode, delayed IRQ, lost wake and stalled provider; masked PL8 shutdown')
    if a.guard_only:sys.exit(0)

    # Execute the actual linked shared probe and this image's strong EL3 hook.
    spec=importlib.util.spec_from_file_location('vbus_machine',ROOT/'checks/memtest-machine.py')
    mm=importlib.util.module_from_spec(spec);spec.loader.exec_module(mm)
    fault_results=[]
    for case in ('fault','timeout','invalid-EL1-complete','recursive','already-latched'):
        model=mm.Machine(a.image,step=1);model.sys.update(currentel=12,sctlr_el1=0)
        model.write(0x07025010,0xa5a5a5a5,4);model.write(0x07025004,0x330001,4)
        state={'writes':0,'recurse':False,'redirected':False}
        def written(cpu,access,address,size,value,cookie):
            if address==0x07025010:
                assert model.read(0x07090110,4)>>24==0xfa
                state['writes']+=1
                if case=='recursive':state['recurse']=True
        def recursive(cpu,address,size,cookie):
            if state['recurse'] and not state['redirected']:
                state['redirected']=True
                for i,v in enumerate((0xee,0xdead,0xbeef,0xcafe)):cpu.reg_write(UC_ARM64_REG_X0+i,v)
                cpu.reg_write(UC_ARM64_REG_PC,model.address('sunstone_probe_fault'))
        model.uc.hook_add(UC_HOOK_MEM_WRITE,written);model.uc.hook_add(UC_HOOK_CODE,recursive)
        sites=h.instruction_sites(model.e.parts,lambda word:word in (0xd503205f,0xd503207f))
        h.hook_addresses(model.uc,sites,lambda cpu,address,size,cookie:cpu.emu_stop())
        if case=='already-latched':model.write(0x07090110,0xfa030077,4)
        if case=='timeout':model.call('sunstone_probe_timeout',count=10000);site=2
        elif case=='invalid-EL1-complete':
            model.sys['currentel']=4
            model.call('sunstone_probe_complete',0,100000,count=10000)
            assert model.exit_status==0xffffffff
            model.sys['currentel']=12
            model.call('monitor_exception',0x733,0xffffffff,1,count=10000);site=1
        else:model.call('sunstone_probe_fault',0x33,0x44,0x55,0x66,count=10000);site=0x77 if case=='already-latched' else 0x33
        assert model.read(0x07090110,4)==0xfa030000|site,case
        assert state['writes']==(0 if case=='already-latched' else 1),(case,state)
        assert model.read(0x07025010,4)==(0xa5a5a5a5 if case=='already-latched' else 0xa5a5a4a5),case
        if case=='recursive':assert state['redirected'] and model.read(0x07090114,4)==0x44
        fault_results.append(case)
    (a.evidence/'faults.json').write_text(json.dumps(fault_results,indent=2)+'\n')
    print('PASS linked strong EL3 abort: fault, timeout, invalid EL1 completion, recursive fault, prior fault preservation')
    # Serialize the exact native report layout and run the frozen payload decoder.
    spec=importlib.util.spec_from_file_location('vbus_decoder',ROOT/'apps/usb-vbus/decoder.py')
    decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
    nominal=[0x3154505256425353,1,0,7,1,1,20000,30000,500,100,500,0,997900,0x330001,0,1,0xf8,400000,40,1,0x04000000,0x10001,0x983fffb7,0xffffffff,0xffffffff,0xffffffff]
    assert not decoder.failures(decoder.decode(struct.pack('<26Q',*nominal)))
    for index,value in ((2,0x20),(12,1000001),(12,1),(14,0x100),(8,2001),(4,0),(5,0),(9,1),(6,275000),(7,650000),(19,0),(15,7),(16,0x28),(17,100000),(18,97)):
        bad=nominal.copy();bad[index]=value
        assert decoder.failures(decoder.decode(struct.pack('<26Q',*bad))),index
    print('PASS full native payload decoding and source duration/fault checks')
if a.init_only:
    spec=importlib.util.spec_from_file_location('vbus_machine',ROOT/'checks/memtest-machine.py')
    mm=importlib.util.module_from_spec(spec);spec.loader.exec_module(mm)
# Model the real inherited clock words and delayed reset acknowledgment.
from unicorn import UC_HOOK_MEM_READ
hardware={0x07025004:0x00ffffff,0x07025010:0,0x07025018:0x12345678,
          0x07025024:5,0x0701019c:0x10001,0x07010010:0x04000000,
          0x07090160:0x983fffb7,0x02002000:0xf85f5f00,
          0x0708400c:0,0x07084010:0xf8,0x07084020:0x30}
if a.old_image:
    old=mm.Machine(a.old_image,step=1);old.map(0x02002000,4096);old.sys['currentel']=4
    for address,value in hardware.items():old.write(address,value,4)
    assert old.call('prepare',count=30000)==0
    print('PASS original native admission refuses the observed board values')
rows=[]
for case in ('control','pulse','hold','warm','busy-clears','busy-stuck','reset-stuck','lines-stuck'):
    mode={'control':0,'hold':10}.get(case,1)
    path=a.image.with_name(f'sunstone-a733-usb-vbus-{mode}.elf')
    model=mm.Machine(path,step=1);model.map(0x02002000,4096);model.sys['currentel']=4
    for address,value in hardware.items():model.write(address,value,4)
    initial=dict(hardware)
    if case in ('warm','busy-clears','busy-stuck'):
        model.write(0x0701019c,0x30003,4);model.write(0x0708400c,0x40,4)
    if case in ('busy-clears','busy-stuck'):model.write(0x07084010,0x28,4)
    state={'reset_at':None,'gate_off':None,'reset_reads':0,'gated':False,'waits':0}
    began=model.now
    def access(cpu,kind,address,size,value,cookie):
        if address==0x0701019c and kind==17:
            assert value&0x10001==0x10001,'S_TWI0 modified'
            if not value&2:state['gate_off']=model.now;state['gated']=True
            elif state['gated']:
                assert model.now-state['gate_off']>=24000,'vendor1ms delay truncated'
                state['gated']=False
        if 0x07084000<=address<0x07085000:
            assert model.read(0x0701019c,4)&2,'TWI access while gated'
        if address==0x07084018 and kind==17 and value&1:state['reset_at']=model.now
        if address==0x07084018 and kind==16 and state['reset_at'] is not None:
            state['reset_reads']+=1
            if case!='reset-stuck' and model.now-state['reset_at']>=1800:
                model.write(address,0,4);model.write(0x0708400c,0,4)
                model.write(0x07084010,0xf8,4)
                model.write(0x07084020,0x10 if case=='lines-stuck' else 0x30,4)
        if address==0x07084010 and kind==16 and case=='busy-clears' and model.now-began>=2400:
            model.write(address,0xf8,4)
    def wait(cpu,address,size,cookie):
        if address==model.address('sunstone_deadline_wait_until'):
            target=cpu.reg_read(UC_ARM64_REG_X0)
            model.now=max(model.now,target*24);state['waits']+=1
            cpu.reg_write(UC_ARM64_REG_X0,1);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
    model.uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,access)
    model.uc.hook_add(UC_HOOK_CODE,wait)
    success=model.call('prepare',count=300000)
    assert model.read(0x07010010,4)==initial[0x07010010],'shared parent retuned'
    assert model.read(0x0701019c,4)&0x10001==0x10001
    assert (success==1)==(case not in ('busy-stuck','reset-stuck','lines-stuck')),(case,success,state)
    assert state['waits'] and model.now-began<=21000*24
    if success:
        assert state['reset_reads']>=2 and model.read(0x07084014,4)==40
        assert model.read(model.address('inherited_apbs1'),4)==0x04000000
        assert model.read(model.address('inherited_xo'),4)==0x983fffb7
        assert model.call('retire_bus',count=30000)==1
        assert model.read(0x0701019c,4)==0x10001
        assert model.read(0x07025004,4)==0x00fffff1
        assert model.read(0x07025018,4)==0x12345678
        assert model.read(0x07025024,4)==5
        if case in ('control','pulse','hold'):
            for address in hardware:hardware[address]=model.read(address,4)
    rows.append({'case':case,'success':bool(success),'waits':state['waits']})
(a.evidence/'initialization.json').write_text(json.dumps(rows,indent=2)+'\n')
print('PASS observed clocks, warm/busy controller, delayed/stuck reset, stuck bus and sequential retirement')
