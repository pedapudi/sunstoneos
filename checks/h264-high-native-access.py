#!/usr/bin/env python3
"""Check new weight register through the actual linked lowest native policy."""
import argparse,hashlib,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_MEM_WRITE,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PC

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(exist_ok=False)
 with a.image.open('rb') as f:
  elf=ELFFile(f);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()};segments=[(s['p_vaddr'],s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
 rows=[]
 # The last case maps the register page as Normal memory instead of Device-nGnRnE.
 for address,width,expected,mapped in [(0x01c0e210,4,1,True),(0x01c0e210,1,-1,True),(0x01c0e211,4,-1,True),(0x01c0d210,4,-1,True),(0x01c0e210,4,-1,False)]:
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  for at,n in [(0x41000000,0x800000),(0x01c0d000,0x2000),(0x7a000000,0x1000000)]:u.mem_map(at,n)
  for at,data in segments:
   if data:u.mem_write(at,data)
  writes=[]
  u.hook_add(UC_HOOK_MEM_WRITE,lambda m,access,at,n,value,unused:writes.append((at,n,value)),begin=0x01c0d000,end=0x01c0efff)
  def device(m,at,n,unused,mapped=mapped):
   # Modeled stage-1 translation: identity Device-nGnRnE, or Normal memory.
   m.mem_write(m.reg_read(UC_ARM64_REG_X2),struct.pack('<QII',m.reg_read(UC_ARM64_REG_X0),0 if mapped else 0x44,2))
   m.reg_write(UC_ARM64_REG_X0,1);m.reg_write(UC_ARM64_REG_PC,m.reg_read(UC_ARM64_REG_LR))
  u.hook_add(UC_HOOK_CODE,device,begin=symbols['aarch64_device_translate'],end=symbols['aarch64_device_translate'])
  for reg,value in [(UC_ARM64_REG_X0,0),(UC_ARM64_REG_X1,address),(UC_ARM64_REG_X2,width),(UC_ARM64_REG_X3,0x55),(UC_ARM64_REG_SP,0x7afff000),(UC_ARM64_REG_LR,0x7afff100)]:u.reg_write(reg,value)
  u.emu_start(symbols['a733_video_platform_native_write'],0x7afff100,count=10000)
  assert u.reg_read(UC_ARM64_REG_X0)&0xffffffff==expected&0xffffffff
  assert writes==([(address,4,0x55)] if expected==1 else [])
  rows.append(dict(address=hex(address),width=width,device_mapping=mapped,result=expected,physical_writes=writes))
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),physical_access=False,cases=rows,scope='Actual linked lowest native access policy and Device-mapping check; modeled translation and inert mapped MMIO. No provider callback substitution, hardware or startup execution.'),indent=2)+'\n');print('PASS High weight denominator native write and width/base/alignment/non-Device refusals')
if __name__=='__main__':main()
