#!/usr/bin/env python3
"""Execute finite USB client constructors, transfer accounting and native shutdown."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_CPACR_EL1

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence.resolve()
r=Path(__file__).resolve().parents[2]
layout=module('layout',e/'layout-helper.py');image=layout.Image(e/'build/probe.elf')
# Preserve the real earlier session producer's fields, rather than inventing
# nominal rails, pin settings or fabric parents. This is a software fixture.
transport=Path.home()/'.cache/sunstoneos/evidence/gpu-transport-only-20261018'
source=Path.home()/'.cache/sunstoneos/evidence/gpu-usb-ordering-20261018/physical-candidate-125/session-template.bin'
producer=layout.Image(transport/'image.elf');sl=producer.layouts['a733_board_session'];raw=source.read_bytes()
config={name:list(struct.unpack_from('<'+str(n)+'I',raw,sl[name])) for name,n in [('fabric',3),('fabric_hz',3),('usb_supply_uv',4)]}
config['pmic_expected']=[list(raw[sl['pmic_pins']+i*3:sl['pmic_pins']+(i+1)*3]) for i in range(2)]
prepare=module('prepare',r/'apps/usb-returning/prepare.py');payload=prepare.input_bytes(config,1)
(e/'fixture-config.json').write_text(json.dumps(config,indent=2)+'\n');(e/'fixture-input.bin').write_bytes(payload)
(e/'fixture-provenance.json').write_text(json.dumps({'source':str(source),'source_sha256':hashlib.sha256(raw).hexdigest(),'layout_elf_sha256':hashlib.sha256((transport/'image.elf').read_bytes()).hexdigest(),'producer_sha256':hashlib.sha256((r/'apps/usb-returning/prepare.py').read_bytes()).hexdigest(),'input_sha256':hashlib.sha256(payload).hexdigest(),'physical_authority':False},indent=2)+'\n')
variants=['complete','partial-callbacks','no-host','pending-before-mask','pending-stuck','active-dma','active-irq','held-mutex','pmic-held','write-ineffective','wrong-request','callback-overrun','provider-fault','invalid-input','invalid-fabric']
cases=[]
for variant in variants:
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x06000000);u.mem_map(0x40000000,0x04000000)
    for address,data in image.parts:
        if data:u.mem_write(address,data)
    u.mem_write(0x43000000,payload)
    if variant=='invalid-input':u.mem_write(0x43000000,bytes(8))
    if variant=='invalid-fabric':u.mem_write(0x43000020,struct.pack('<I',0xffffffff))
    u.reg_write(UC_ARM64_REG_PSTATE,5);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
    state={'time':1000000,'exit':None,'tasks':[],'pending':0,'tx':bytearray(),'writes':[],'delays':[],'inited':False,'mutexes':0,'constructors':False}
    regs={0x03400114:0,0x03400214:0,0x03400314:0,0x04100040:64,0x04100048:0x00020003,0x04100050:255,0x04100500:0,0x04100400:0xf070}
    for i in range(16):regs[0x04100540+i*16]=0
    def put(address,value,size=4):u.mem_write(address,int(value).to_bytes(size,'little'))
    def get(address,size=4):return int.from_bytes(u.mem_read(address,size),'little')
    def field(name,value=None):
        address=image.address(name)
        if value is None:return get(address)
        put(address,value)
    def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
    names=['platform_init','xQueueCreateMutexStatic','xTaskCreateStatic','vTaskStartScheduler','vTaskDelay','platform_exit','platform_panic','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','dcd_int_handler','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','xQueueGetMutexHolder']
    hooks={image.address(name):name for name in names}
    shared=image.address('shared');nl=image.layouts['a733_native_usb'];pl=image.layouts['a733_pmic']
    def code(cpu,address,size,unused):
        word=get(address);kind=word&0xffffffe0;reg=word&31
        if kind in (0xd5384240,0xd53be000,0xd53be020):
            value=4 if kind==0xd5384240 else 24000000 if kind==0xd53be000 else state['time']*24
            if kind==0xd53be020:state['time']+=1
            u.reg_write(UC_ARM64_REG_X0+reg,value);u.reg_write(UC_ARM64_REG_PC,address+4);return
        name=hooks.get(address)
        if name is None:return
        args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name=='platform_init':ret()
        elif name=='xQueueCreateMutexStatic':state['mutexes']+=1;ret(0x43010000+state['mutexes']*256)
        elif name=='xTaskCreateStatic':
            assert args[0]==image.address('pmic_task' if not state['tasks'] else 'usb_task',unit='/apps/usb-returning/main.c') and args[2]==4096 and args[4]==2
            state['tasks'].append(args[0]);ret(0x43020000+len(state['tasks'])*256)
        elif name=='vTaskStartScheduler':
            assert len(state['tasks'])==2 and state['mutexes']==2
            # Actual native USB init/bind and the actual PMIC/clock/pin/rail/USB
            # constructors have all accepted the producer configuration.
            state['constructors']=True
            assert get(shared+nl['usb_task'],8)==0x43020200
            assert get(shared+nl['pmic_task'],8)==0x43020100
            u.reg_write(UC_ARM64_REG_PC,state['tasks'][1]);u.reg_write(UC_ARM64_REG_X0,0)
        elif name=='a733_native_usb_step':
            # Hardware progression is reused evidence, not repeated here.
            # Supply only its terminal production metadata for shutdown cases.
            if variant=='provider-fault':ret((1<<64)-1);return
            put(shared+nl['usb_step'],5);put(shared+nl['usb_rail_index'],4)
            put(shared+nl['pmic']+pl['active'],8)
            ret(1)
        elif name=='a733_native_usb_health':ret(1)
        elif name=='tusb_rhport_init':
            state['inited']=True;field('polling',1)
            if variant=='pending-before-mask' or variant=='pending-stuck':regs[0x03400214]=1<<28
            if variant=='active-irq':regs[0x03400314]=1<<28
            if variant=='active-dma':regs[0x04100540]=1<<31
            if variant=='pmic-held':put(shared+nl['pmic']+pl['held'],1)
            ret(1)
        elif name=='tud_task_ext':
            if state['pending']:
                count=state['pending'];state['pending']=0
                u.reg_write(UC_ARM64_REG_PC,image.address('tud_vendor_tx_cb'))
                u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_X0+1,count+(1 if variant=='callback-overrun' else 0))
            else:ret()
        elif name=='dcd_int_handler':ret()
        elif name=='tud_vendor_n_available':ret(0 if variant=='no-host' or field('received') else 4)
        elif name=='tud_vendor_n_read':u.mem_write(args[1],b'bad!' if variant=='wrong-request' else b'USB?');ret(4)
        elif name=='tud_vendor_n_write':
            n=min(args[2],10 if variant=='partial-callbacks' else 40);state['tx']+=bytes(u.mem_read(args[1],n));state['pending']+=n;ret(n)
        elif name=='tud_vendor_n_write_flush':ret()
        elif name=='xQueueGetMutexHolder':ret(1 if variant=='held-mutex' else 0)
        elif name=='vTaskDelay':
            state['delays'].append(args[0]);state['time']+=args[0]*1000
            if field('pmic_stop'):field('pmic_parked',1)
            ret()
        elif name=='platform_exit':state['exit']=args[0];u.emu_stop()
        else:raise AssertionError('Unexpected panic')
    def read(cpu,access,address,size,value,unused):
        assert size==4,(hex(address),size)
        assert address in regs,hex(address)
        put(address,regs[address])
    def write(cpu,access,address,size,value,unused):
        assert size==4 and address in regs,(hex(address),size)
        state['writes'].append([address,value])
        if variant=='write-ineffective' and address==0x04100048:put(address,regs[address]);return
        regs[address]=value
        if address==0x04100050 and not value and variant!='pending-stuck':regs[0x03400214]=0
    u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_READ,read,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x02000000,end=0x07ffffff)
    u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
    u.emu_start(image.address('kernel_main'),0,count=10000000)
    length=get(0x42000018,8);log=bytes(u.mem_read(0x42000100,length)).decode()
    success=variant in ('complete','partial-callbacks','no-host','pending-before-mask')
    assert state['exit']==(0 if success else 1),(variant,log,state)
    if success:
        assert 'COMPLETE native USB bounded observation' in log and 'USB_QUIESCED=0x0000000000000001' in log
        assert 'USB_OBSERVATION_OUTCOME=0x'+('0000000000000001' if variant=='no-host' else '0000000000000000') in log
        assert state['writes']==[[0x04100048,0],[0x04100050,0],[0x04100500,0],[0x04100400,0xf000],[0x04100040,0]],state['writes']
        assert state['delays'][-1]==150
        if variant!='no-host':assert len(state['tx'])==40 and struct.unpack('<5Q',state['tx'])[:3]==(0x3154455253425355,1,1)
    else:assert 'COMPLETE native USB bounded observation' not in log
    if variant not in ('invalid-input','invalid-fabric'):assert state['constructors']
    if variant in ('active-dma','active-irq','held-mutex','pmic-held','wrong-request','callback-overrun','provider-fault','invalid-input','invalid-fabric'):assert not state['writes'],(variant,state['writes'])
    (e/(variant+'.log')).write_text(log)
    cases.append({'name':variant,'passed':True,'exit':state['exit'],'actual_constructors_accepted':state['constructors'],'mmio_writes':state['writes'],'copied_response_bytes':len(state['tx'])})
result={'passed':True,'physical_access':False,'scope':'Actual linked input copy, task construction, native PMIC/USB and resource constructors, finite client, IN callback accounting and native MMIO shutdown guards. Scheduler, hardware provider progression, TinyUSB vendor FIFO/events and register effects modeled. Earlier provider/TinyUSB cases reused; no physical enumeration, transfers, scheduling or ROM return established.','elf_sha256':hashlib.sha256((e/'build/probe.elf').read_bytes()).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'cases':cases}
(e/'check-result.json').write_text(json.dumps(result,indent=2)+'\n');print(f'{len(cases)} focused linked USB cases passed')
