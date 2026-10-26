#!/usr/bin/env python3
"""Execute card hooks and vendor PIO waits with modeled MMIO; no hardware access."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from capstone import Cs,CS_ARCH_ARM,CS_MODE_THUMB
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm_const import *
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('build',type=Path);p.add_argument('--record',type=Path,required=True)
p.add_argument('--other-record',type=Path,required=True);p.add_argument('--boot0',type=Path,required=True)
p.add_argument('--case',action='append',help='Run only named new/affected cases');p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir()
m=json.loads((a.build/'manifest.json').read_text());blob=(a.build/'boot0-card-restore.bin').read_bytes()
helper=(a.build/'card-ddr-helper.bin').read_bytes();original=a.boot0.read_bytes()
spec=importlib.util.spec_from_file_location('record',ROOT/'tools/training-record.py');t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
record=a.record.read_bytes();parsed=t.parse(record);other=a.other_record.read_bytes()
phy=[(run['base']+2*i,v) for run in parsed['runs'][1:] for i,v in enumerate(run['values'])]
masks={v['address']:v['mask'] for v in parsed['masks']};results=[]
regs=[UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7,UC_ARM_REG_R8,UC_ARM_REG_R9,UC_ARM_REG_R10,UC_ARM_REG_R11]
def w(cpu,address,value):cpu.mem_write(address,struct.pack('<I',value))
def r(cpu,address):return struct.unpack('<I',cpu.mem_read(address,4))[0]
def ret(cpu,value=0):cpu.reg_write(UC_ARM_REG_R0,value);cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
def fixture():
    cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM)
    for address,size in [(0x40000,0xc0000),(0x03006000,0x1000),(0x04020000,0x1000),(0x0a900000,0x200000),(0x41000000,0x1000),(0x42000000,0x1000)]:cpu.mem_map(address,size)
    cpu.mem_write(0x47000,blob);cpu.mem_write(0x831f8,original[0x574:0x5f4])
    cpu.mem_write(0x03006200,struct.pack('<4I',*parsed['chip_id']))
    for address,value in [(0x83c14,1),(0x83648,0x838f8),(0x838f8+0x20,0x83858),(0x838f8+0xa8,0x5b32d),(0x838f8+0x90,512),(0x838f8+0xe8,0xffff),(0x838f8+0xec,131072),(0x83858,0x04020000),(0x83858+0x14,0x04020200)]:w(cpu,address,value)
    cpu.mem_write(0x60670,b'\x05');cpu.mem_write(0x0aaa0000,b'\x01\x00')
    cpu.reg_write(UC_ARM_REG_CPSR,0x1d3);cpu.reg_write(UC_ARM_REG_SP,0x8c000);cpu.reg_write(UC_ARM_REG_LR,0xf0001)
    cpu.reg_write(UC_ARM_REG_R0,0);cpu.reg_write(UC_ARM_REG_R1,0x831f8)
    for i,reg in enumerate(regs):cpu.reg_write(reg,0x12340000+i)
    return cpu
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB)
counter_sites={}
for section in m['bootstrap_sections']:
    address=0x476f8+(section['slot']-8)*128
    for ins in md.disasm(blob[address-0x47000:address-0x47000+section['bytes']],address):
        if ins.mnemonic in ('mrrc','mrc'):counter_sites[ins.address]=(ins.mnemonic,ins.op_str)
counter_sites[0x48d6c]=('mrrc','p15, #0, r2, r3, c14')
# The patched timer entry branches; its displaced instruction is in the shim.
counter_sites.pop(0x48d6c)
def counters(cpu,address,state):
    if address not in counter_sites:return False
    mnemonic,operands=counter_sites[address]
    if mnemonic=='mrc':
        cpu.reg_write(UC_ARM_REG_R0,24000000 if 'c14' in operands else 0)
    else:
        state['ticks']+=state.get('tick_step',100)
        low,high=state['ticks']&0xffffffff,state['ticks']>>32
        pair=(UC_ARM_REG_R0,UC_ARM_REG_R1) if 'r0, r1' in operands else (UC_ARM_REG_R2,UC_ARM_REG_R3)
        cpu.reg_write(pair[0],low);cpu.reg_write(pair[1],high)
    cpu.reg_write(UC_ARM_REG_PC,address+4|1);return True

def mutate(data,kind):
    data=bytearray(data)
    if kind=='crc':data[136]^=1
    if kind=='geometry':struct.pack_into('<I',data,16,0xffffffff)
    if kind=='strap':data[56]^=1
    if kind in ('geometry','strap'):struct.pack_into('<I',data,124,t.crc(data[:124]))
    return bytes(data)

cases=['newest-B','tie-A','only-A','wrong-board','record-crc','geometry','strap','helper-crc','read-error','readback','size','memory','final-readback','capacity','metadata','host-base','fifo-pointer','helper-A-crc','high-generation-A','lba-overflow']
if not m['experimental_unknown_temperature']:cases=['default-temperature']
if a.case:
    assert set(a.case)<=set(cases)
    cases=[case for case in cases if case in a.case]
for case in cases:
    cpu=fixture();state={'ticks':0,'instructions':0,'reads':0,'init':0,'timing':0,'continuation':0,'writes':0,'dram':0,'stop':0,'fault':False}
    spec_b=dict(parsed);spec_b['generation']=parsed['generation']+1
    rb=t.build(spec_b);ra=record
    if case=='tie-A':rb=ra
    if case=='only-A':rb=bytes(len(rb))
    if case=='high-generation-A':
        high=dict(parsed);high['generation']=1<<32;ra=t.build(high)
        high['generation']=0xffffffff;rb=t.build(high)
    if case=='wrong-board':ra=rb=other
    if case in ('record-crc','geometry','strap'):ra=rb=mutate(record,{'record-crc':'crc'}.get(case,case))
    if case=='capacity':w(cpu,0x838f8+0xec,100)
    if case=='lba-overflow':w(cpu,0x47ad8,0xfffffff0)
    if case=='metadata':w(cpu,0x838f8+0x90,1024)
    if case=='host-base':w(cpu,0x83858,0x42000000)
    if case=='fifo-pointer':w(cpu,0x83858+0x14,0x42000000)
    sectors={}
    for name,data in [('A',ra),('B',rb)]:
        base=m['units'][name]['first'];sectors[base]=data[:128]+bytes(384)
        for i in range((len(data)-128+511)//512):sectors[base+1+i]=data[128+i*512:128+(i+1)*512].ljust(512,b'\0')
        load=bytearray(helper)
        if case=='helper-crc' or (case=='helper-A-crc' and name=='A'):load[0]^=1
        for i in range((len(load)+511)//512):sectors[base+64+i]=bytes(load[i*512:(i+1)*512]).ljust(512,b'\0')
    def code(uc,address,size,_):
        state['instructions']+=1
        if counters(uc,address,state):return
        if address==0x5e21a:
            uc.reg_write(UC_ARM_REG_PC,0xf0000);return
        if address==0x5da64:ret(uc);return
        if address==0x5db08:
            state['reads']+=1
            assert uc.reg_read(UC_ARM_REG_R1)==1 and bytes(uc.mem_read(0x60670,1))==b'\0'
            assert r(uc,m['symbols']['read_active'])==1
            if case=='read-error':ret(uc,1);return
            sector=uc.reg_read(UC_ARM_REG_R0);dest=uc.reg_read(UC_ARM_REG_R2)
            assert 0x92000<=dest<0xae000
            uc.mem_write(dest,sectors[sector]);ret(uc);return
        if address==m['symbols']['card_stop']:
            state['stop']=uc.reg_read(UC_ARM_REG_R0)
        if address==m['symbols']['card_stop']+10:uc.emu_stop();return
        if address==0x49bdc:
            state['init']+=1;assert state['init']==1
            assert uc.reg_read(UC_ARM_REG_R0)==0 and uc.reg_read(UC_ARM_REG_R1)==0x831f8
            assert 0x8b000<=uc.reg_read(UC_ARM_REG_SP)<=0x8c000
            state['return']=uc.reg_read(UC_ARM_REG_LR);state['sp']=uc.reg_read(UC_ARM_REG_SP)
            state['saved']=[uc.reg_read(x) for x in regs]
            state['restore']=bool(r(uc,0x86038));state['calls_left']=1 if state['restore'] else 4
            if not state['restore']:assert bytes(uc.mem_read(0x831f8,128))==original[0x574:0x5f4]
            uc.reg_write(UC_ARM_REG_SP,state['sp']-0x100)
            for i,reg in enumerate(regs):uc.reg_write(reg,0xaabb0000+i)
            uc.reg_write(UC_ARM_REG_PC,0x56143);return
        if address==0x4bca8:
            state['timing']+=1;assert not state['fault'];ret(uc);return
        if address==0x56146:
            state['continuation']+=1;assert not state['fault'];state['calls_left']-=1
            if state['calls_left']:
                uc.reg_write(UC_ARM_REG_R0,0);uc.reg_write(UC_ARM_REG_R1,0x831f8);uc.reg_write(UC_ARM_REG_PC,0x56143);return
            if case=='final-readback':
                address,value=phy[0];uc.mem_write(address,struct.pack('<H',value^masks[address]))
            for reg,value in zip(regs,state['saved']):uc.reg_write(reg,value)
            uc.reg_write(UC_ARM_REG_SP,state['sp']);uc.reg_write(UC_ARM_REG_LR,state['return'])
            ret(uc,4096 if case=='size' else 12288)
    def memory(uc,access,address,size,value,_):
        if access==17 and address in masks:state['writes']+=1
        if access==16 and case=='readback' and state['writes'] and address==phy[0][0] and not state['fault']:
            uc.mem_write(address,struct.pack('<H',phy[0][1]^masks[address]));state['fault']=True
        if address>=0x40000000:
            assert not state['fault'];state['dram']+=size
            if access==16 and case=='memory':uc.mem_write(address,b'\xff'*size);state['fault']=True
    cpu.hook_add(UC_HOOK_CODE,code);cpu.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,memory)
    try:cpu.emu_start(0x5e217,0xf0000,count=5000000)
    except BaseException:
        print(case,state,'PC',hex(cpu.reg_read(UC_ARM_REG_PC)));raise
    assert state['instructions']<5000000,(case,state)
    errors={'read-error':13,'readback':7,'size':8,'memory':9,'final-readback':7}
    assert state['stop']==errors.get(case,0),(case,state)
    if case in errors:
        assert r(cpu,0x8601c)==errors[case],(case,state)
        if case in ('read-error','readback'):assert not state['dram'] and not state['continuation']
    else:
        assert cpu.reg_read(UC_ARM_REG_PC)==0xf0000 and cpu.reg_read(UC_ARM_REG_SP)==0x8c000,(case,state)
        for i,reg in enumerate(regs):assert cpu.reg_read(reg)==0x12340000+i,(case,i)
        assert bytes(cpu.mem_read(0x60670,1))==b'\x05' and r(cpu,m['symbols']['read_active'])==0
        assert state['timing']==(1 if case in ('newest-B','tie-A','only-A','helper-A-crc','high-generation-A') else 4),(case,state)
        if case in ('newest-B','tie-A','only-A','helper-A-crc','high-generation-A'):assert state['writes']==1016 and r(cpu,0x8604c)==(2 if case in ('newest-B','helper-A-crc') else 1)
        else:assert state['writes']==state['dram']==0
    results.append({'case':case,**state,'pass':True})

(a.output/'results.json').write_text(json.dumps({'manifest_sha256':hashlib.sha256((a.build/'manifest.json').read_bytes()).hexdigest(),
    'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'physical_access':False,'cases':results},indent=2)+'\n')
print('PASS card restore',len(results),'instruction cases')
