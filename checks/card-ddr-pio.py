#!/usr/bin/env python3
"""Exercise actual pinned PIO wait instructions through the restore timer shim."""
import argparse,json,struct,hashlib
from pathlib import Path
from capstone import Cs,CS_ARCH_ARM,CS_MODE_THUMB
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm_const import *
p=argparse.ArgumentParser(description=__doc__);p.add_argument('build',type=Path)
p.add_argument('--boot0',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir();m=json.loads((a.build/'manifest.json').read_text())
blob=(a.build/'boot0-card-restore.bin').read_bytes();original=a.boot0.read_bytes();md=Cs(CS_ARCH_ARM,CS_MODE_THUMB)
sites={}
for section in m['bootstrap_sections']:
    address=0x476f8+(section['slot']-8)*128
    for ins in md.disasm(blob[address-0x47000:address-0x47000+section['bytes']],address):
        if ins.mnemonic=='mrrc':sites[ins.address]=(UC_ARM_REG_R2,UC_ARM_REG_R3)
results=[]
def write(cpu,address,value):cpu.mem_write(address,struct.pack('<I',value))
def fixture(data):
    cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.mem_map(0x40000,0xc0000);cpu.mem_map(0x04020000,0x1000)
    cpu.mem_write(0x47000,data);cpu.reg_write(UC_ARM_REG_CPSR,0x1d3)
    cpu.reg_write(UC_ARM_REG_SP,0x9e000);cpu.reg_write(UC_ARM_REG_LR,0xf0001)
    return cpu
# Exact inactive return registers and flags for the same counter, before the
# bootstrap has touched scratch SRAM. Loaded flag is already zero.
for ticks in (0,0xffffffff,0x100000100,0x123456789abcdef):
    states=[]
    for data,patched in ((original,False),(blob,True)):
        cpu=fixture(data)
        for index,reg in enumerate((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4)):cpu.reg_write(reg,0x12340000+index)
        def hook(uc,address,size,_):
            if address in (sites if patched else {0x48d6c:0}):
                uc.reg_write(UC_ARM_REG_R2,ticks&0xffffffff);uc.reg_write(UC_ARM_REG_R3,ticks>>32)
                uc.reg_write(UC_ARM_REG_PC,address+4|1)
        cpu.hook_add(UC_HOOK_CODE,hook);cpu.emu_start(0x48d6d,0xf0000,count=100)
        states.append([cpu.reg_read(reg) for reg in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_CPSR,UC_ARM_REG_SP)])
    assert states[0]==states[1],states
    results.append({'case':'inactive-timer','counter':ticks,'pass':True})
for case in ('nominal','low-word-wrap','fifo-stall','slow-fifo','command','data','busy','reset','clock-reset'):
    cpu=fixture(blob);start=0xffffff00 if case=='low-word-wrap' else 0xffffffff-30000 if case in ('data','reset','clock-reset') else 0
    state={'ticks':start,'step':100,'instructions':0,'fifo':0,'stop':0,'dram':0,'clock':False,'reset':False,'waits':[]}
    if case in ('fifo-stall','command','busy','reset','clock-reset'):state['step']=12000000
    if case=='slow-fifo':state['step']=2000000
    for address,value in [(0x83648,0x838f8),(0x838f8+0x20,0x83858),(0x838f8+0xa8,0x5b32d),
                          (0x838f8+0x90,512),(0x838f8+0xe8,65535),(0x838f8+0xec,131072),
                          (0x838f8+0x104,2),(0x838f8+0x40,1),(0x83858,0x04020000),(0x83858+0x14,0x04020200)]:write(cpu,address,value)
    cpu.mem_write(0x60670,b'\x05');cpu.mem_write(0x86040,struct.pack('<Q',start))
    cpu.reg_write(UC_ARM_REG_R0,57344);cpu.reg_write(UC_ARM_REG_R1,1);cpu.reg_write(UC_ARM_REG_R2,0xa0000)
    waits={0x5b68c:'fifo',0x5b608:'command',0x5b728:'data',0x5b80c:'busy',0x5b87a:'reset',0x5d112:'clock-reset'}
    def code(uc,address,size,_):
        state['instructions']+=1
        if address in sites:
            state['ticks']+=state['step'];uc.reg_write(UC_ARM_REG_R2,state['ticks']&0xffffffff);uc.reg_write(UC_ARM_REG_R3,state['ticks']>>32)
            uc.reg_write(UC_ARM_REG_PC,address+4|1);return
        if address in waits and waits[address] not in state['waits']:state['waits'].append(waits[address])
        if waits.get(address)==case and case in ('data','reset','clock-reset'):state['step']=12000000
        if address==0x4a540:raise AssertionError('read path entered unbounded UART')
        if address==m['symbols']['card_stop']:state['stop']=uc.reg_read(UC_ARM_REG_R0)
        if address==m['symbols']['card_stop']+10:uc.emu_stop()
    def memory(uc,access,address,size,value,_):
        assert address<0x40000000,'PIO reached DRAM'
        if access==17:
            if address==0x04020000 and value&7==7:state['reset']=True
            if address==0x04020018 and state['reset']:state['clock']=True
            return
        if address==0x0402003c:
            status=4 if case=='fifo-stall' else 0
            if case=='busy' and state['fifo']==128:status|=512;state['step']=12000000
            write(uc,address,status)
        if address==0x04020200:
            state['fifo']+=1;write(uc,address,0xabc00000+state['fifo'])
            if state['fifo']==128 and case=='command':state['step']=12000000
        if address==0x04020038:
            value=0 if case=='command' else 4 if case=='data' else 0x100 if case in ('reset','clock-reset') else 12
            write(uc,address,value)
        if address==0x04020000 and case=='clock-reset' and state['reset']:write(uc,address,0)
        if address==0x04020018 and state['clock']:write(uc,address,0x80000000)
    # Let FIFO complete before testing the downstream wait itself.
    if case in ('command','busy','reset','clock-reset'):state['step']=100
    cpu.hook_add(UC_HOOK_CODE,code);cpu.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,memory)
    entry=m['symbols']['card_read']|1
    if case=='busy':
        # CMD17 does not request a busy response. Exercise the shared vendor
        # busy loop directly under an already-active restore read.
        entry=0x5b7f9;state['fifo']=128;state['step']=12000000
        cpu.reg_write(UC_ARM_REG_R4,0x83858);write(cpu,m['symbols']['read_active'],1)
    try:cpu.emu_start(entry,0xf0000,count=100000)
    except BaseException:
        print(case,state,'pc',hex(cpu.reg_read(UC_ARM_REG_PC)));raise
    assert state['instructions']<100000,(case,state,hex(cpu.reg_read(UC_ARM_REG_PC)))
    if case in ('nominal','low-word-wrap'):
        assert state['stop']==0 and state['fifo']==128 and cpu.reg_read(UC_ARM_REG_PC)==0xf0000,(case,state)
        assert bytes(cpu.mem_read(0x60670,1))==b'\x05'
    else:
        assert state['stop']==12 and state['ticks']-start>=240000000,(case,state)
        assert state['ticks']-start<=264000000,(case,state)
        required={'fifo-stall':'fifo','command':'command','data':'data','busy':'busy','reset':'reset','clock-reset':'clock-reset'}.get(case)
        if required:assert required in state['waits'],(case,state)
        if case=='slow-fifo':assert 0<state['fifo']<128
    results.append({'case':case,**state,'pass':True})
(a.output/'results.json').write_text(json.dumps({'manifest_sha256':hashlib.sha256((a.build/'manifest.json').read_bytes()).hexdigest(),
    'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'physical_access':False,'cases':results},indent=2)+'\n')
print('PASS',len(results),'actual timer/PIO cases')
