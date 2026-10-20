#!/usr/bin/env python3
"""Execute actual High parser/storage/allocator/table setup, with memory readiness modeled."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1];REGS=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4];STACK=0x7affeff0;STOP=0x7afff000

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','inputs','output'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--geometries',nargs='+',default=['fullhd','hd'])
 a=p.parse_args();a.output.mkdir(exist_ok=False);spec=importlib.util.spec_from_file_location('cases',ROOT/'checks/h264-application-cases.py');cases=importlib.util.module_from_spec(spec);spec.loader.exec_module(cases)
 syms,segments,types,_,_=cases.setup(a.image,a.output,True,True,high=True)
 for start,end in [('__device_pool_start','__device_pool_end'),('__uncached_pool_start','__uncached_pool_end')]:assert 0x7b000000<=syms[start] or syms[end]<=0x7a000000
 with a.image.open('rb') as f:
  elf=ELFFile(f);sizes={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_byte_size'].value for cu in elf.get_dwarf_info().iter_CUs() for d in cu.iter_DIEs() if d.tag=='DW_TAG_structure_type' and 'DW_AT_name' in d.attributes and 'DW_AT_byte_size' in d.attributes}
 reports=[]
 for name in a.geometries:
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  for at,n in [(0x40000000,0x4000000),(0x48000000,0x2000000),(0x78000000,0x1000000),(0x7a000000,0x1000000)]:u.mem_map(at,n)
  for at,data in segments:
   if data:u.mem_write(at,data)
  parked=[];low=[STACK]
  def hook(m,at,size,unused):
   if at==STOP:parked.append(1);m.emu_stop()
   else:m.reg_write(REGS[0],syms['boot_pool'] if at==syms['a733_memory_reservations_init'] else syms['uncached_pool']);m.reg_write(UC_ARM64_REG_PC,m.reg_read(UC_ARM64_REG_LR))
  for at in [STOP,syms['a733_memory_reservations_init'],syms['a733_memory_uncached_init']]:u.hook_add(UC_HOOK_CODE,hook,begin=at,end=at)
  def stack_write(m,access,at,size,value,unused):low[0]=min(low[0],at)
  u.hook_add(UC_HOOK_MEM_WRITE,stack_write,begin=STACK-65536,end=STACK-1)
  def invoke(symbol,*values):
   for reg,value in zip(REGS,values):u.reg_write(reg,value)
   parked.clear();u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,STOP);u.emu_start(syms[symbol],0,count=800000000);assert parked,symbol;return u.reg_read(REGS[0])
  def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
  assert invoke('device_memory_pool_init',syms['boot_pool'],0x48000000,0x48000000,0x30000000)==1
  assert invoke('device_memory_pool_init',syms['uncached_pool'],0x78000000,0x78000000,0x1000000)==1
  assert invoke('h264_stream_storage_init',12)==1 and invoke('h264_stream_storage_valid')==1
  st=syms['h264_stream'];sty=types['h264_stream'];app=syms['h264'];aty=types['h264_application']
  raw=(a.inputs/name/'qualification/frame-000.h264').read_bytes();pointer=get(st+sty['input']);u.mem_write(pointer,raw)
  high=get(st+sty['high']);plan=st+sty['plan'];picture=st+sty['picture'];highsize=sizes['h264_high'];low[0]=STACK
  assert invoke('h264_high_prepare',high,pointer,len(raw),plan,0x7af00000)==1
  parsed_stack=STACK-low[0];assert 0<parsed_stack<16384 and 0<highsize<=1048576
  picbytes=19*4;u.mem_write(picture,bytes(u.mem_read(plan,picbytes)))
  session=(a.inputs/name/'qualification-session.bin').read_bytes();u.mem_write(app,session)
  assert invoke('h264_allocate')==1 and invoke('h264_guards')==1
  assert invoke('a733_iommu_validate',app+aty['tables'])==1
  pooltype=types['device_memory_pool'];alloc=types['device_memory_allocation'];nc=syms['uncached_pool'];wb=syms['boot_pool']
  def ledger(pool,count):
   rows=[]
   for i in range(count):
    base=pool+pooltype['allocations']+i*sizes['device_memory_allocation'];rows.append(dict(first=get(base+alloc['first'],4),pages=get(base+alloc['pages'],4),state=get(base+alloc['state'],4)))
   return rows
  ncrows=ledger(nc,7);wbrows=ledger(wb,8)
  assert max(x['first']+x['pages'] for x in ncrows)*4096==14749696 and all(x['state']==2 for x in ncrows)
  assert max(x['first']+x['pages'] for x in wbrows)*4096==23920640 and all(x['state']==3 for x in wbrows)
  assert get(st+sty['output']+types['h264_stream_output']['payload'])==0x486f8000
  reports.append(dict(geometry=name,nc_bytes=14749696,wb_bytes=23920640,metadata_used=highsize,metadata_reserved=1048576,parser_observed_stack_bytes=parsed_stack,output_slots=5,output_capacity_each=3110400,nc=ncrows,wb=wbrows,device_writes=0))
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),physical_access=False,cases=reports,scope='Actual native parser, immutable CPU ledger, uncached allocator, guards and IOMMU construction/validation. Extended-memory readiness/getters modeled; no provider programming, CPU startup or device operation.'),indent=2)+'\n');print('PASS actual High allocation, CPU metadata/stack, copied outputs and four-window tables')
if __name__=='__main__':main()
