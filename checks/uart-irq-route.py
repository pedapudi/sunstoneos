#!/usr/bin/env python3
"""Root-only linked UART route/dispatch checks; no physical or scheduling evidence."""
import argparse,hashlib,json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
REGISTERS={0x03400000:0x33,0x03400084:0x55aa0000,0x03400d04:0xffffffff,
           0x03400c08:0xaaaaaaaa,0x03400424:0x28384858}
EXPECTED={0x03400084:0x55aa0010,0x03400d04:0xffffffef,
          0x03400c08:0xaaaaa8aa,0x03400424:0x283848d8,0x03406120:0}
CASES=['nominal','enabled','pending','active','rwp']+['reject-'+hex(a) for a in EXPECTED]+['dispatch']
STOP=0x50fff000

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--case',action='append',choices=CASES)
 args=p.parse_args();raw=args.image.read_bytes();digest=hashlib.sha256(raw).hexdigest()
 with args.image.open('rb') as f:
  elf=ELFFile(f);parts=[(int(x['p_vaddr']),x.data()) for x in elf.iter_segments() if x['p_type']=='PT_LOAD'];symbols={}
  for sym in elf.get_section_by_name('.symtab').iter_symbols():
   if sym['st_value']:symbols.setdefault(sym.name,set()).add(int(sym['st_value']))
 def address(name):
  values=symbols.get(name,set());assert len(values)==1,(name,values);return next(iter(values))
 route,dispatch,handler=[address(n) for n in ['uart_irq_route','vApplicationIRQHandler','uart_application_irq']]
 args.output.mkdir(parents=True,exist_ok=True)
 for case in args.case or CASES:
  cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.reg_write(UC_ARM64_REG_PSTATE,13)
  for a,n in [(0x03400000,0x10000),(0x41000000,0x1200000),(0x50000000,0x1000000)]:cpu.mem_map(a,n)
  for a,data in parts:
   assert 0x41000000<=a and a+len(data)<=0x42200000,(hex(a),len(data))
   if data:cpu.mem_write(a,data)
  for a,v in REGISTERS.items():cpu.mem_write(a,v.to_bytes(4,'little'))
  blocked={'enabled':0x03400104,'pending':0x03400204,'active':0x03400304,'rwp':0x03400000}
  if case in blocked:cpu.mem_write(blocked[case],(1<<31 if case=='rwp' else 16).to_bytes(4,'little'))
  writes=[];calls=[]
  def write(uc,access,a,size,value,unused):
   assert a in EXPECTED,('unexpected GIC write',hex(a));writes.append((a,size,value))
  def read(uc,access,a,size,value,unused):
   if case.startswith('reject-') and a==int(case[7:],16) and any(x[0]==a for x in writes):
    uc.mem_write(a,(EXPECTED[a]^16).to_bytes(size,'little'))
  def code(uc,a,size,unused):
   if a==handler:
    calls.append(a);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
  cpu.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x03400000,end=0x0340ffff)
  cpu.hook_add(UC_HOOK_MEM_READ,read,begin=0x03400000,end=0x0340ffff)
  cpu.hook_add(UC_HOOK_CODE,code)
  cpu.reg_write(UC_ARM64_REG_SP,0x50ffe000);cpu.reg_write(UC_ARM64_REG_LR,STOP)
  cpu.reg_write(UC_ARM64_REG_X0,36 if case=='dispatch' else 0x80000000)
  cpu.emu_start(dispatch if case=='dispatch' else route,STOP,count=50000)
  assert cpu.reg_read(UC_ARM64_REG_PC)==STOP,'Instruction limit reached without return'
  if case=='dispatch':assert calls==[handler] and not writes
  elif case in blocked:assert cpu.reg_read(UC_ARM64_REG_X0)==0 and not writes
  else:
   assert [(a,v) for a,size,v in writes]==list(EXPECTED.items()),writes
   assert [size for a,size,v in writes]==[4,4,4,4,8]
   assert cpu.reg_read(UC_ARM64_REG_X0)==(1 if case=='nominal' else 0)
  result={'case':case,'passed':True,'image_sha256':digest,'writes':writes,'physical_access':False,
          'scope':'Actual linked route helper and UART dispatch branch; GIC registers modeled, ISR body substituted only for dispatch.'}
  (args.output/(case+'.json')).write_text(json.dumps(result,indent=2)+'\n');print(case,'PASS')
if __name__=='__main__':main()
