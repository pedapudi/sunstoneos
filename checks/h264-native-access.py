#!/usr/bin/env python3
"""Exercise the real lower native VE read policy with inert MMIO storage."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_MEM_READ,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_LR,UC_ARM64_REG_SP,UC_ARM64_REG_PC
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
        str(ROOT/'drivers/video/a733_video_platform.c'),str(ROOT/'drivers/video/a733_video_unit.c'),
        str(ROOT/'arch/aarch64/device_memory.c'),'-o',str(image)]
    subprocess.run(command,check=True);(a.output/'command.json').write_text(json.dumps(command,indent=2)+'\n')
    with image.open('rb') as f:
        elf=ELFFile(f);segments=[(s['p_vaddr'],s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD'];entry=elf.header['e_entry']
        translate=next(s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name=='aarch64_device_translate')
    cases=[]
    # The last case maps the page without Device-nGnRnE identity attributes.
    for offset,output,expected,mapped in [(0x22c,0x50000000,1,True),(0x228,0x50000000,1,True),
            (0x230,0x50000000,-1,True),(0x22d,0x50000000,-1,True),(0x22c,0,-1,True),(0x22c,0x50000000,-1,False)]:
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x200000,0x1000);cpu.mem_map(0x40ff0000,0x200000);cpu.mem_map(0x50000000,0x10000);cpu.mem_map(0x01c0e000,0x1000)
        for address,data in segments:cpu.mem_write(address,data)
        cpu.mem_write(0x01c0e000+offset,(1200).to_bytes(4,'little'));reads=[]
        cpu.hook_add(UC_HOOK_MEM_READ,lambda c,kind,address,size,value,unused:reads.append(address) if 0x01c0e000<=address<0x01c0f000 else None)
        def device(c,address,size,unused,mapped=mapped):
            # Modeled stage-1 translation: identity Device-nGnRnE, or Normal memory.
            c.mem_write(c.reg_read(UC_ARM64_REG_X2),struct.pack('<QII',c.reg_read(UC_ARM64_REG_X0),0 if mapped else 0x44,2))
            c.reg_write(UC_ARM64_REG_X0,1);c.reg_write(UC_ARM64_REG_PC,c.reg_read(UC_ARM64_REG_LR))
        cpu.hook_add(UC_HOOK_CODE,device,begin=translate,end=translate)
        cpu.reg_write(UC_ARM64_REG_X0,0);cpu.reg_write(UC_ARM64_REG_X1,0x01c0e000+offset);cpu.reg_write(UC_ARM64_REG_X2,output)
        cpu.reg_write(UC_ARM64_REG_SP,0x5000f000);cpu.reg_write(UC_ARM64_REG_LR,0x410f0000);cpu.emu_start(entry,0x410f0000,count=1000)
        result=cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff;assert result==(expected&0xffffffff)
        assert reads==([0x01c0e000+offset] if expected==1 else [])
        if expected==1:assert int.from_bytes(cpu.mem_read(output,4),'little')==1200
        cases.append(dict(offset=hex(offset),nonnull_output=bool(output),device_mapping=mapped,result=expected,mmio_reads=len(reads)))
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),cases=cases,scope='Actual native read function, whitelist and Device-mapping check; translation, MMIO and returned register values modeled.'),indent=2)+'\n')
    print('PASS native decoded-MB read admission and neighboring/null/non-Device refusal')
if __name__=='__main__':main()
