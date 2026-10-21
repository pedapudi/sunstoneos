#!/usr/bin/env python3
"""Run every HEVC VE access through the real lower native access policy.

Compiles the decoder platform and unit sources for AArch64 and executes
a733_video_platform_native_write/read in Unicorn for each register write the
host model recorded (checks/hevc-decode-model.py trace.json) and for the two
status reads. Neighboring offsets, byte writes and a page without a
Device-nGnRnE identity mapping must be refused with no MMIO access.
Translation and MMIO are modeled; no device is accessed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE,UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,
    UC_ARM64_REG_LR,UC_ARM64_REG_SP,UC_ARM64_REG_PC)
ROOT=Path(__file__).resolve().parents[1]
VE=0x01c0e000
RETURN=0x410f0000


def main():
    p=argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--trace',type=Path,required=True,help='trace.json from checks/hevc-decode-model.py')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--clang',default='/usr/bin/clang-21')
    p.add_argument('--lld',default=str(Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'))
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    image=a.output/'native-access.elf'
    command=[a.clang,'--target=aarch64-none-elf','-O2','-g','-ffunction-sections','-fdata-sections',
        '-I'+str(ROOT/'include'),'-nostdlib','-fuse-ld='+a.lld,'-Wl,--gc-sections',
        '-Wl,-e,a733_video_platform_native_write','-Wl,--undefined=a733_video_platform_native_read',
        '-Wl,-Ttext=0x41000000',str(ROOT/'drivers/video/a733_video_platform.c'),
        str(ROOT/'drivers/video/a733_video_unit.c'),str(ROOT/'arch/aarch64/device_memory.c'),'-o',str(image)]
    subprocess.run(command,check=True);(a.output/'command.json').write_text(json.dumps(command,indent=2)+'\n')
    with image.open('rb') as f:
        elf=ELFFile(f);segments=[(s['p_vaddr'],s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    trace=json.loads(a.trace.read_text())
    writes=sorted({(addr,value) for addr,value in trace['events'] if VE+0x500<=addr<VE+0x600 or addr-VE in (0xc4,0xc8,0xcc,0xe8,0xec)})
    def call(function,args,mapped=True):
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        cpu.mem_map(0x200000,0x1000);cpu.mem_map(0x40ff0000,0x200000);cpu.mem_map(0x50000000,0x10000);cpu.mem_map(VE,0x1000)
        for address,data in segments:cpu.mem_write(address,data)
        cpu.mem_write(VE+0x538,(0x1234).to_bytes(4,'little'))
        accesses=[]
        cpu.hook_add(UC_HOOK_MEM_READ,lambda c,kind,address,size,value,unused:accesses.append(('r',address,size)) if VE<=address<VE+0x1000 else None)
        cpu.hook_add(UC_HOOK_MEM_WRITE,lambda c,kind,address,size,value,unused:accesses.append(('w',address,size,value)) if VE<=address<VE+0x1000 else None)
        def translate(c,address,size,unused):
            # Modeled stage-1 translation: identity Device-nGnRnE, or Normal memory.
            c.mem_write(c.reg_read(UC_ARM64_REG_X2),struct.pack('<QII',c.reg_read(UC_ARM64_REG_X0),0 if mapped else 0x44,2))
            c.reg_write(UC_ARM64_REG_X0,1);c.reg_write(UC_ARM64_REG_PC,c.reg_read(UC_ARM64_REG_LR))
        target=symbols['aarch64_device_translate'];cpu.hook_add(UC_HOOK_CODE,translate,begin=target,end=target)
        for register,value in zip((UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3),args):cpu.reg_write(register,value)
        cpu.reg_write(UC_ARM64_REG_SP,0x5000f000);cpu.reg_write(UC_ARM64_REG_LR,RETURN)
        cpu.emu_start(symbols[function],RETURN,count=2000)
        result=cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
        return (1 if result==1 else -1 if result==0xffffffff else result),accesses,cpu
    cases=[]
    for addr,value in writes:
        result,accesses,_=call('a733_video_platform_native_write',(0,addr,4,value))
        assert result==1 and accesses==[('w',addr,4,value)],(hex(addr),result,accesses)
        cases.append(dict(kind='write',offset=hex(addr-VE),result=1))
    for off in (0x538,0x53c):
        result,accesses,cpu=call('a733_video_platform_native_read',(0,VE+off,0x50000000))
        assert result==1 and accesses==[('r',VE+off,4)]
        cases.append(dict(kind='read',offset=hex(off),result=1))
    refused=[('write',0x570,4),('write',0x5e8,4),('write',0x534,1),('write',0x53e,4),('read',0x534,4),('read',0x5e4,4)]
    for kind,off,width in refused:
        args=(0,VE+off,width,8) if kind=='write' else (0,VE+off,0x50000000)
        result,accesses,_=call('a733_video_platform_native_'+kind,args)
        assert result==-1 and not accesses,(kind,hex(off),result,accesses)
        cases.append(dict(kind=kind,offset=hex(off),width=width,result=-1))
    for kind in ('write','read'):
        args=(0,VE+0x534,4,8) if kind=='write' else (0,VE+0x538,0x50000000)
        result,accesses,_=call('a733_video_platform_native_'+kind,args,mapped=False)
        assert result==-1 and not accesses
        cases.append(dict(kind=kind,offset='0x534' if kind=='write' else '0x538',device_mapping=False,result=-1))
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
        image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),trace_sha256=hashlib.sha256(a.trace.read_bytes()).hexdigest(),
        cases=cases,scope='Actual AArch64 native VE access functions and allowlists for every modeled HEVC write and '
        'status read; translation, MMIO and register values modeled.'),indent=2)+'\n')
    print(f'PASS {len(writes)} HEVC writes and 2 status reads admitted; neighbors, byte writes and non-Device pages refused')


if __name__=='__main__':main()
