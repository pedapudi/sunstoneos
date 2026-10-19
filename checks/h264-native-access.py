#!/usr/bin/env python3
"""Exercise the real lower native VE read policy with inert MMIO storage."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_LR,UC_ARM64_REG_SP
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--clang',default='/usr/bin/clang-21')
    p.add_argument('--lld',default='/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21')
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    image=a.output/'native-access.elf'
    command=[a.clang,'--target=aarch64-none-elf','-O2','-g','-ffunction-sections','-fdata-sections',
        '-I'+str(ROOT/'include'),'-nostdlib','-fuse-ld='+a.lld,'-Wl,--gc-sections',
        '-Wl,-e,a733_video_platform_native_read','-Wl,-Ttext=0x41000000',
        str(ROOT/'drivers/video/a733_video_platform.c'),'-o',str(image)]
    subprocess.run(command,check=True);(a.output/'command.json').write_text(json.dumps(command,indent=2)+'\n')
    with image.open('rb') as f:
        elf=ELFFile(f);segments=[(s['p_vaddr'],s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD'];entry=elf.header['e_entry']
    cases=[]
    for offset,output,expected in [(0x22c,0x50000000,1),(0x228,0x50000000,1),
            (0x230,0x50000000,-1),(0x22d,0x50000000,-1),(0x22c,0,-1)]:
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x200000,0x1000);cpu.mem_map(0x40ff0000,0x200000);cpu.mem_map(0x50000000,0x10000);cpu.mem_map(0x01c0e000,0x1000)
        for address,data in segments:cpu.mem_write(address,data)
        cpu.mem_write(0x01c0e000+offset,(1200).to_bytes(4,'little'));reads=[]
        cpu.hook_add(UC_HOOK_MEM_READ,lambda c,kind,address,size,value,unused:reads.append(address) if 0x01c0e000<=address<0x01c0f000 else None)
        cpu.reg_write(UC_ARM64_REG_X0,0);cpu.reg_write(UC_ARM64_REG_X1,0x01c0e000+offset);cpu.reg_write(UC_ARM64_REG_X2,output)
        cpu.reg_write(UC_ARM64_REG_SP,0x5000f000);cpu.reg_write(UC_ARM64_REG_LR,0x410f0000);cpu.emu_start(entry,0x410f0000,count=1000)
        result=cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff;assert result==(expected&0xffffffff)
        assert reads==([0x01c0e000+offset] if expected==1 else [])
        if expected==1:assert int.from_bytes(cpu.mem_read(output,4),'little')==1200
        cases.append(dict(offset=hex(offset),nonnull_output=bool(output),result=expected,mmio_reads=len(reads)))
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),cases=cases,scope='Actual native read function and whitelist; MMIO and returned register values modeled.'),indent=2)+'\n')
    print('PASS native decoded-MB read admission and neighboring/null refusal')
if __name__=='__main__':main()
