#!/usr/bin/env python3
"""Check G2D and USB quiet-interrupt admission in the linked application."""
from pathlib import Path
import argparse,importlib.util,json,struct,hashlib,re,subprocess
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--session',type=Path,required=True)
p.add_argument('--contract',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();P=a.image;O=a.output;O.mkdir(parents=True,exist_ok=False)
session=a.session.read_bytes();contract=a.contract.read_bytes()
assert len(contract)==288 and len(session)==904 and struct.unpack_from('<2Q',session,592)==(0x4153373347324131,1)
spec=importlib.util.spec_from_file_location('g2d_irq_image',R/'checks/storage-read-native.py');n=importlib.util.module_from_spec(spec);spec.loader.exec_module(n);e=n.Image(P,layout_names=())
ops={}
for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(P)],text=True).splitlines():
 m=re.match(r'^\s*([0-9a-f]+):\s+mrs\s+x(\d+),\s*(CurrentEL|MPIDR_EL1|SCTLR_EL3|CNTPCT_EL0)\s*$',line,re.I)
 if m:ops[int(m[1],16)]=(int(m[2]),m[3].lower())
assert ops
results={}
for case in ('quiet-with-neighboring-interrupts','g2d-enabled','g2d-pending','g2d-active','usb-enabled'):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.reg_write(UC_ARM64_REG_PSTATE,13);u.reg_write(UC_ARM64_REG_TPIDR_EL1,0)
 for a,z in [(0x41000000,0x1200000),(0x50000000,0x200000),(0x03400000,0x10000),(0x02050000,0x1000),(0x07050000,0x1000)]:u.mem_map(a,z)
 for a,b in e.parts:
  if b:u.mem_write(a,b)
 u.mem_write(0x42004000,contract);u.mem_write(0x41f00000,session)
 regs={0x03400000:0x30,0x03400004:7,0x03400088:0x55aa55aa,0x03400d08:0xffffffff,0x03400c14:0xaaaaaaaa,0x0340045c:0x12345678,0x034062f8:0x10000}
 for which,ofs,bit in [('g2d-enabled',0x108,19),('g2d-pending',0x208,19),('g2d-active',0x308,19),('usb-enabled',0x114,28)]:
  if case==which:regs[0x03400000+ofs]=1<<bit
 if case=='quiet-with-neighboring-interrupts':
  for ofs in (0x114,0x214,0x314):regs[0x03400000+ofs]=(1<<27)|(1<<31)
  for ofs in (0x108,0x208,0x308):regs[0x03400000+ofs]=(1<<18)|(1<<20)
 writes=[];terminal=[]
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def hook(cpu,a,size,unused):
  name=hooks.get(a)
  if a in ops and name is None:
   reg,kind=ops[a];u.reg_write(UC_ARM64_REG_X0+reg,12 if kind=='currentel' else 200 if kind=='cntpct_el0' else 0);u.reg_write(UC_ARM64_REG_PC,a+4)
  elif name=='a733_smp_counter':ret(200)
  elif name in ('a733_smp_contract_valid','a733_device_memory_boot_admit','g2d_session_prepare','g2d_board_prepare','a733_cpu_coherence_publish'):ret(1)
  elif name=='a733_cached_prepare':ret()
  else:terminal.append(name);u.emu_stop()
 hooks={e.address(name):name for name in ('a733_smp_counter','a733_smp_contract_valid','a733_device_memory_boot_admit','g2d_session_prepare','g2d_board_prepare','a733_cpu_coherence_publish','a733_cached_prepare','a733_cpu_start_init','a733_smp_stop')}
 hooks[0x501ff000]='return'
 for a in set(hooks)|set(ops):u.hook_add(UC_HOOK_CODE,hook,begin=a,end=a)
 def read(cpu,access,a,size,value,unused):cpu.mem_write(a,regs.get(a,0).to_bytes(size,'little'))
 def write(cpu,access,a,size,value,unused):
  writes.append((a,value))
  if 0x03400180<=a<0x034001a0:regs[a-0x80]=regs.get(a-0x80,0)&~value
  else:regs[a]=value
 u.hook_add(UC_HOOK_MEM_READ,read,begin=0x03400000,end=0x0340ffff)
 u.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x03400000,end=0x0340ffff)
 u.reg_write(UC_ARM64_REG_SP,0x501fe000);u.reg_write(UC_ARM64_REG_LR,0x501ff000)
 name='a733_smp_boot'
 u.emu_start(e.address(name),0x501ff004,count=50000)
 if case=='quiet-with-neighboring-interrupts':
  assert terminal==['a733_cpu_start_init'],(case,terminal,hex(u.reg_read(UC_ARM64_REG_PC)),writes)
 else:
  assert terminal==['a733_smp_stop'] and writes==[],(case,terminal,writes)
  assert (u.reg_read(UC_ARM64_REG_X0),u.reg_read(UC_ARM64_REG_X1))==(3,0x30b)
 results[case]=dict(passed=True,terminal=terminal,register_writes=len(writes));print('PASS',case,flush=True)
 (O/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(P.read_bytes()).hexdigest(),cases=results,scope='Actual G2D enable/pending/active and USB enable refusals before GIC writes; unrelated adjacent bits do not cause refusal. Session validation, cache prep, system registers and GIC effects modeled. Stops before CPU start constructor; no secondary, USB or physical execution. Earlier GPU/display USB pending/active cases are not replayed.'),indent=2)+'\n')
