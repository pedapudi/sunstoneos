#!/usr/bin/env python3
"""One A32 survey instruction/return case; privileged reads are explicit models."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_CPSR,UC_ARM_REG_PC,UC_CPU_ARM_CORTEX_A15
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
binary=(a.build/'launch-survey.bin').read_bytes();assert 0<len(binary)<=0x400
cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15);cpu.mem_map(0x40000,0xc0000)
cpu.mem_write(0x85000,binary);nonce=bytes(range(1,9));cpu.mem_write(0x86000,bytes(120)+nonce)
stack=bytes(range(256))*32;cpu.mem_write(0x8b000,stack)
saved={UC_ARM_REG_R0+i:0x12340000+i*0x101 for i in range(13)}
saved.update({UC_ARM_REG_SP:0x8c000,UC_ARM_REG_LR:0x90000})
cpu.reg_write(UC_ARM_REG_CPSR,0xa80f0153);saved[UC_ARM_REG_CPSR]=cpu.reg_read(UC_ARM_REG_CPSR)
for register,value in saved.items():cpu.reg_write(register,value)
registers={0xee110f10:0xc50838,0xee100fb0:0x80000000,0xee1e0f10:0,0xee1c0f50:0}
ticks=[];writes=[];reads=[]
def instruction(uc,address,size,unused):
    word=struct.unpack('<I',uc.mem_read(address,4))[0]
    if word&0xffff0fff in registers:
        key=word&0xffff0fff;reads.append(key);uc.reg_write(UC_ARM_REG_R0+((word>>12)&15),registers[key]);uc.reg_write(UC_ARM_REG_PC,address+4)
    elif word&0xfff00fff==0xec500f0e:
        value=0x1234567800000000+len(ticks);ticks.append(value)
        uc.reg_write(UC_ARM_REG_R0+((word>>12)&15),value&0xffffffff);uc.reg_write(UC_ARM_REG_R0+((word>>16)&15),value>>32);uc.reg_write(UC_ARM_REG_PC,address+4)
    elif word&0x0f000010==0x0e000010:
        raise AssertionError('Unexpected coprocessor instruction/write: '+hex(word))
def write(uc,access,address,size,value,unused):
    assert 0x85000<=address<0x85400 or 0x86000<=address<0x86078,hex(address)
    writes.append((address,size))
cpu.hook_add(UC_HOOK_CODE,instruction);cpu.hook_add(UC_HOOK_MEM_WRITE,write)
cpu.emu_start(0x85000,0x90000,count=1000)
assert cpu.reg_read(UC_ARM_REG_PC)==0x90000
for register,value in saved.items():assert cpu.reg_read(register)==value,(register,hex(cpu.reg_read(register)),hex(value))
assert bytes(cpu.mem_read(0x8b000,len(stack)))==stack
assert sorted(reads)==sorted(registers) and len(ticks)==2
raw=bytes(cpu.mem_read(0x86000,128));v=struct.unpack('<32I',raw)
assert v[:4]==(0x3153564c,1,128,1) and raw[120:]==nonce and v[6]==0 and v[29]==0
assert v[4:10]==(saved[UC_ARM_REG_CPSR],0xc50838,0,0x80000000,0,0)
assert v[10]|v[11]<<32==ticks[0] and v[12]|v[13]<<32==ticks[1]
assert v[14:16]==(saved[UC_ARM_REG_SP],saved[UC_ARM_REG_LR])
assert list(v[16:29])==[saved[UC_ARM_REG_R0+i] for i in range(13)]
result={'schema':'a733-launch-survey-software-v1','physical_access':False,'return_preserved':True,
 'binary_sha256':hashlib.sha256(binary).hexdigest(),'check_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
 'cp15_reads':[hex(v) for v in reads],'counter_reads':2,'memory_writes':writes,
 'limits':'Modeled register responses only. No proof of real privilege, SCR access, counter progress, FEL return, or hardware operation.'}
a.output.write_text(json.dumps(result,indent=2)+'\n');print('PASS one survey instruction/return case; all caller registers, CPSR and stack bytes preserved.')
