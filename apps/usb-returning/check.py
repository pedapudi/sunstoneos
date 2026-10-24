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
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--case',action='append',help='Run only named affected cases')
p.add_argument('--refused-image',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/board-bundles-20261023/rank-9-usb-v2/build/probe.elf',help='ELF of the image refused on the board on 2026-10-23');a=p.parse_args();e=a.evidence.resolve()
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
import re
lock_enum=re.search(r'enum a733_platform_lock \{([^}]*)\}',(r/'include/a733_platform_locks.h').read_text()).group(1)
lock_count=[x.strip() for x in lock_enum.split(',')].index('A733_PLATFORM_LOCK_COUNT')
variants=['complete','partial-callbacks','no-host','pending-before-mask','pending-stuck','active-dma','active-irq','held-mutex','pmic-held','write-ineffective','wrong-request','callback-overrun','provider-fault','invalid-input','invalid-fabric','device-session','session-host-request','session-host-mode','session-dma']
takeovers=['takeover-observed','takeover-softconn-stuck','takeover-host-request','takeover-host-mode','takeover-dma-request','takeover-no-b-device','takeover-dma-channel']
if a.case:
    assert set(a.case)<=set(variants+takeovers),a.case
    variants=[v for v in variants if v in a.case]
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
    if variant=='device-session' or variant.startswith('session-'):regs[0x04100040]=0x00029940
    if variant=='session-host-request':regs[0x04100040]|=1<<9
    if variant=='session-host-mode':regs[0x04100040]|=1<<10
    if variant=='session-dma':regs[0x04100540]=1<<31
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
            # a733_platform_locks_init creates one mutex per platform lock.
            assert len(state['tasks'])==2 and state['mutexes']==lock_count,(state['mutexes'],lock_count)
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
        # USB0+0x40 packs the byte registers PCTL, DEVCTL, EPIND and VEND0.
        # As observed on 2026-10-23, a word store there leaves PCTL unchanged;
        # a byte store replaces PCTL.
        if address==0x04100040:
            assert size in (1,4),size;state['writes'].append([address,value,size])
            if size==1:regs[address]=(regs[address]&~255)|value
            return
        assert size==4 and address in regs,(hex(address),size)
        state['writes'].append([address,value])
        if variant=='write-ineffective' and address==0x04100048:put(address,regs[address]);return
        regs[address]=value
        if address==0x04100050 and not value and variant!='pending-stuck':regs[0x03400214]=0
    u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_READ,read,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x02000000,end=0x07ffffff)
    u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
    u.emu_start(image.address('kernel_main'),0,count=10000000)
    length=get(0x42000018,8);log=bytes(u.mem_read(0x42000100,length)).decode()
    success=variant in ('complete','partial-callbacks','no-host','pending-before-mask','device-session')
    assert state['exit']==(0 if success else 1),(variant,log,state)
    if success:
        assert 'COMPLETE native USB bounded observation' in log and 'USB_QUIESCED=0x0000000000000001' in log
        assert 'USB_OBSERVATION_OUTCOME=0x'+('0000000000000001' if variant=='no-host' else '0000000000000000') in log
        assert state['writes']==[[0x04100048,0],[0x04100050,0],[0x04100500,0],[0x04100400,0xf000],[0x04100040,0,1]],state['writes']
        assert state['delays'][-1]==150
        if variant!='no-host':assert len(state['tx'])==40 and struct.unpack('<5Q',state['tx'])[:3]==(0x3154455253425355,1,1)
    else:assert 'COMPLETE native USB bounded observation' not in log
    if variant not in ('invalid-input','invalid-fabric'):assert state['constructors']
    if variant in ('active-dma','active-irq','held-mutex','pmic-held','wrong-request','callback-overrun','provider-fault','invalid-input','invalid-fabric','session-host-request','session-host-mode','session-dma'):assert not state['writes'],(variant,state['writes'])
    (e/(variant+'.log')).write_text(log)
    cases.append({'name':variant,'passed':True,'exit':state['exit'],'actual_constructors_accepted':state['constructors'],'mmio_writes':state['writes'],'copied_response_bytes':len(state['tx'])})
# ROM takeover by the actual linked USB provider and native callbacks, from
# the board state that the 2026-10-23 port 3-3 run refused: its input, its
# captured clock words and the USB0 word it observed. The register model is
# the one above; the ROM's interrupt enables follow the matched nonsecure ROM
# (INTUSBE=7 at offset 0x1d68, INTTxE bit 0 at 0x1d78). CCU USB controls are
# the values the ROM baseline admits. Scheduler, mutexes and rail health
# are modeled as READY; PMIC readiness is the power-authority word.
OBSERVED=Path.home()/'.cache/sunstoneos/evidence/board-session-20261023/port-3-3'
OBSERVED_WORD=0x00029940
def takeover(elf,variant):
    im=layout.Image(elf);u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x06000000);u.mem_map(0x40000000,0x04000000)
    for address,data in im.parts:
        if data:u.mem_write(address,data)
    board_input=(OBSERVED/'r9-v2-package/input.bin').read_bytes();u.mem_write(0x43000000,board_input)
    u.reg_write(UC_ARM64_REG_PSTATE,5);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
    regs={int(k,0):v for k,v in json.loads((OBSERVED/'r9-capture/session.json').read_text())['words'].items()}
    regs.update({0x03400114:0,0x03400214:0,0x03400314:0,0x02003300:0x40000000,0x02003304:0x01000100,0x02003340:0x80000000,
                 0x04100040:OBSERVED_WORD,0x04100048:1,0x04100050:7,0x04100500:0,0x04100400:0x3000,0x04100410:0,0x04100418:0,0x04100420:0})
    regs.update({0x04100540+i*16:0 for i in range(16)})
    if variant=='takeover-host-request':regs[0x04100040]|=1<<9
    if variant=='takeover-host-mode':regs[0x04100040]|=1<<10
    if variant=='takeover-dma-request':regs[0x04100040]|=1<<25
    if variant=='takeover-no-b-device':regs[0x04100040]&=~0x8000
    if variant=='takeover-dma-channel':regs[0x04100540+15*16]=1<<31
    st={'time':1000000,'writes':[],'tasks':[],'exit':None,'phase':'constructors'}
    def put(address,value,size=4):u.mem_write(address,int(value).to_bytes(size,'little'))
    def get(address,size=4):return int.from_bytes(u.mem_read(address,size),'little')
    def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
    names=['platform_init','xQueueCreateMutexStatic','xTaskCreateStatic','vTaskStartScheduler','platform_exit','platform_panic','xTaskGetCurrentTaskHandle','xQueueGetMutexHolder','a733_clock_token_status','a733_rail_status','a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','a733_clock_retain']
    hooks={im.address(name):name for name in names}
    def code(cpu,address,size,unused):
        word=get(address);kind=word&0xffffffe0;reg=word&31
        if kind in (0xd5384240,0xd53be000,0xd53be020):
            value=4 if kind==0xd5384240 else 24000000 if kind==0xd53be000 else st['time']*24
            if kind==0xd53be020:st['time']+=1
            u.reg_write(UC_ARM64_REG_X0+reg,value);u.reg_write(UC_ARM64_REG_PC,address+4);return
        name=hooks.get(address)
        if name is None:return
        args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name=='platform_init':ret()
        elif name=='xQueueCreateMutexStatic':ret(0x43010000+len(st['tasks'])*256+256)
        elif name=='xTaskCreateStatic':st['tasks'].append(args[0]);ret(0x43020000+len(st['tasks'])*256)
        elif name=='vTaskStartScheduler':u.emu_stop()
        elif name in ('xTaskGetCurrentTaskHandle','xQueueGetMutexHolder'):ret(0x43020200)
        elif name in ('a733_platform_lock_enter','a733_platform_lock_retain'):ret(1)
        elif name in ('a733_clock_token_status','a733_rail_status','a733_platform_lock_leave','a733_clock_retain'):ret(0)
        elif name=='platform_exit':st['exit']=args[0];u.emu_stop()
        else:raise AssertionError('Unexpected panic')
    def read(cpu,access,address,size,value,unused):
        assert size==4 and address in regs,(hex(address),size);put(address,regs[address])
    def write(cpu,access,address,size,value,unused):
        assert st['phase']=='provider' and address in regs,hex(address)
        st['writes'].append([address,value,size])
        if address==0x04100040:
            assert size in (1,4),size
            if size==1 and variant!='takeover-softconn-stuck':regs[address]=(regs[address]&~255)|value
            return
        assert size==4,(hex(address),size);regs[address]=value
    u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_READ,read,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x02000000,end=0x07ffffff)
    def call(name,args=(),unit=None):
        for i,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,v)
        u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
        u.emu_start(im.address(name,unit=unit),0x43fff000,count=2000000);return u.reg_read(UC_ARM64_REG_X0)
    call('kernel_main');assert len(st['tasks'])==2 and not st['writes'],st
    st['phase']='provider';sh=im.address('shared');nl=im.layouts['a733_native_usb'];ul=im.layouts['a733_usb_platform'];pl=im.layouts['a733_pmic'];tl=im.layouts['a733_clock_token']
    usb=sh+nl['usb'];pmic=sh+nl['pmic']
    # Board-passed preconditions: PMIC READY, four supply snapshots reaped, and
    # the clock token and authority of the provider's ACQUIRE/AUTHORIZE stages.
    put(sh+nl['usb_rail_index'],4);put(usb+ul['token']+tl['id'],42,8);put(usb+ul['token']+tl['owner'],3,8)
    put(sh+nl['usb_rail']+im.layouts['a733_rail_token']['owner'],3,8);put(pmic+pl['stage'],4)
    clock=pmic+pl['clock'];put(clock+tl['owner'],2,8);put(clock+tl['shared'],(1<<69)|(1<<32)|3,16);put(clock+tl['exclusive'],1<<55,16)
    lifetime=sh+nl['usb_power'];ll=im.layouts['a733_pmic_lifetime'];put(lifetime+ll['provider'],pmic,8);put(lifetime+ll['owner'],2,8)
    put(lifetime+ll['identity'],int.from_bytes(board_input[24:32],'little'),8);put(usb+ul['stage'],2)
    # PMIC readiness is the rail ledger's power-authority word (A733_POWER_PROVIDER_READY).
    put(sh+nl['rails']+im.layouts['a733_rail_leases']['authority'],1)
    # One poll per 1 ms tick, as the USB task's vTaskDelay(1) paces them.
    polls=0
    for polls in range(1,3001):
        result=call('a733_usb_platform_poll',[usb])
        if result!=0:break
        st['time']+=1000
    fl=im.layouts['a733_usb_failure'];fb=usb+ul['failure']
    fault={k:get(fb+fl[k],8 if k=='address' else 4) for k in ('kind','stage','address','mask','expected','observed','writes_attempted','writes_completed')}
    return {'result':result,'stage':get(usb+ul['stage']),'fault':fault,'writes':st['writes'],'word':regs[0x04100040],'polls':polls}
def board_fault():
    log=(OBSERVED/'r9-v2-usb/kernel.log').read_text()
    values={k:int(v,16) for k,v in (line.split('=') for line in log.splitlines() if line.startswith('USB_FAULT_'))}
    return {k:values['USB_FAULT_'+k.upper()] for k in ('kind','stage','address','mask','expected','observed','writes_attempted','writes_completed')}
# The model reproduces the refusal of the image that ran on the board.
refused_elf=a.refused_image.resolve()
old=takeover(refused_elf,'takeover-observed')
assert old['result']==3 and old['fault']==board_fault(),(old,board_fault())
assert [w[2] for w in old['writes'] if w[0]==0x04100040]==[4]
cases.append({'name':'takeover-observed-refused-image','passed':True,'elf_sha256':hashlib.sha256(refused_elf.read_bytes()).hexdigest(),'fault':old['fault'],'mmio_writes':old['writes']})
for variant in takeovers:
    if a.case and variant not in a.case:continue
    r=takeover(e/'build/probe.elf',variant);f=r['fault']
    if variant=='takeover-observed':
        assert r['result']==2 and r['stage']==18 and not f['kind'],r
        assert r['word']==OBSERVED_WORD&~64 and [w for w in r['writes'] if w[0]==0x04100040]==[[0x04100040,0,1]],r['writes']
        assert [w[0] for w in r['writes']][:4]==[0x04100048,0x04100050,0x04100040,0x02003304],r['writes']
    elif variant=='takeover-softconn-stuck':
        # Re-read without writing until the deadline, then retain.
        assert r['result']==3 and f['kind']==1 and f['stage']==29 and f['address']==0x04100040 and f['observed']==OBSERVED_WORD,r
        assert [w[0] for w in r['writes']].count(0x04100040)==1,r['writes']
        assert not any(w[0]==0x02003304 for w in r['writes']),r['writes']
    else:
        assert r['result']==3 and f['kind']==4 and f['stage']==20 and not r['writes'],r
    cases.append({'name':variant,'passed':True,'result':r['result'],'polls':r['polls'],'fault':f,'mmio_writes':r['writes']})
result={'passed':True,'physical_access':False,'scope':'Actual linked input copy, task construction, native PMIC/USB and resource constructors, finite client, IN callback accounting and native MMIO shutdown guards; actual USB provider ROM takeover and native callbacks from the 2026-10-23 port 3-3 input, clock capture and observed USB0 word. Scheduler, mutexes, rail/PMIC health, TinyUSB vendor FIFO/events and register effects modeled; a word store to USB0+0x40 is modeled as leaving PCTL unchanged, as observed. No physical enumeration, transfers, scheduling or ROM return established.','elf_sha256':hashlib.sha256((e/'build/probe.elf').read_bytes()).hexdigest(),'refused_elf_sha256':hashlib.sha256(refused_elf.read_bytes()).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'cases':cases}
(e/'check-result.json').write_text(json.dumps(result,indent=2)+'\n');print(f'{len(cases)} focused linked USB cases passed')
