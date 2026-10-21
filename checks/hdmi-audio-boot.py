#!/usr/bin/env python3
"""Check HDMI audio quiet-interrupt admission in the linked application."""
from pathlib import Path
import argparse,importlib.util,json,struct,hashlib,re,subprocess
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
import unicorn.arm64_const as arm
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--session',type=Path,required=True)
p.add_argument('--contract',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--usb',action='store_true',help='Check only the copied-USB profile and its new interrupt guard')
a=p.parse_args();P=a.image;O=a.output;O.mkdir(parents=True,exist_ok=False)
session=a.session.read_bytes();contract=a.contract.read_bytes()
assert len(contract) in (224,4096) and not any(contract[224:])
contract=contract[:224]
expected=(2,816) if a.usb else (1,656)
assert len(contract)==224 and len(session)==expected[1] and struct.unpack_from('<3Q',session)==(0x4153373348444131,*expected)
spec=importlib.util.spec_from_file_location('hdmi_audio_irq_image',R/'checks/storage-read-native.py');n=importlib.util.module_from_spec(spec);spec.loader.exec_module(n);e=n.Image(P,layout_names=('a733_smp_contract',))
if a.usb:
 address=e.address('a733_hdmi_audio_geometry')
 geometry=next(data[address-base:address-base+96] for base,data in e.parts if base<=address and address+96<=base+len(data))
 assert struct.unpack('<12Q',geometry)==(2,816,440,216,0x41f10000,192000,48000,2,16,32,656,160)
# The producer leaves magic/timestamps for the SRAM launcher. Supply that
# arming step once; native model continuations reuse the saved exact contract.
contract=bytearray(contract)
assert struct.unpack_from('<Q',contract,e.layouts['a733_smp_contract']['magic'])[0]==0
struct.pack_into('<Q',contract,e.layouts['a733_smp_contract']['magic'],0x4153373348444331)
for field,value in dict(armed_at=100,startup_deadline=24000100,diagnostic_deadline=480000100,expires_at=480000100).items():
 assert struct.unpack_from('<Q',contract,e.layouts['a733_smp_contract'][field])[0]==0, field
 struct.pack_into('<Q',contract,e.layouts['a733_smp_contract'][field],value)
contract=bytes(contract)
(O/'contract.bin').write_bytes(contract)
ops={}
for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(P)],text=True).splitlines():
 m=re.match(r'^\s*([0-9a-f]+):\s+mrs\s+x(\d+),\s*(CurrentEL|MPIDR_EL1|SCTLR_EL3|CNTPCT_EL0)\s*$',line,re.I)
 if m:ops[int(m[1],16)]=(int(m[2]),m[3].lower())
assert ops
results={}
cases=('usb-quiet-with-neighboring-interrupts','usb-enabled','usb-pending','usb-active') if a.usb else ('quiet-with-neighboring-interrupts','dma-enabled','dma-pending','dma-active')
for case in cases:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.reg_write(UC_ARM64_REG_PSTATE,13);u.reg_write(UC_ARM64_REG_TPIDR_EL1,0)
 for a,z in [(0x41000000,0x1200000),(0x90000000,0x200000),(0x03400000,0x10000),(0x02050000,0x1000),(0x07050000,0x1000)]:u.mem_map(a,z)
 for a,b in e.parts:
  if b:u.mem_write(a,b)
 u.mem_write(0x42004000,contract);u.mem_write(0x41f00000,session)
 regs={0x03400000:0x30,0x03400004:7,0x03400088:0x55aa55aa,0x03400d08:0xffffffff,0x03400c14:0xaaaaaaaa,0x0340045c:0x12345678,0x034062f8:0x10000}
 for which,ofs,bit in [('dma-enabled',0x110,15),('dma-pending',0x210,15),('dma-active',0x310,15)]:
  if case==which:regs[0x03400000+ofs]=1<<bit
 for which,ofs,bit in [('usb-enabled',0x114,28),('usb-pending',0x214,29),('usb-active',0x314,30)]:
  if case==which:regs[0x03400000+ofs]=1<<bit
 if case in ('quiet-with-neighboring-interrupts','usb-quiet-with-neighboring-interrupts'):
  for ofs in (0x114,0x214,0x314):regs[0x03400000+ofs]=(1<<27)|(1<<31)
  for ofs in (0x110,0x210,0x310):regs[0x03400000+ofs]=(1<<14)|(1<<16)
 writes=[];terminal=[]
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def hook(cpu,a,size,unused):
  name=hooks.get(a)
  if a in ops and name is None:
   reg,kind=ops[a];u.reg_write(getattr(arm,"UC_ARM64_REG_X"+str(reg)),12 if kind=='currentel' else 200 if kind=='cntpct_el0' else 0);u.reg_write(UC_ARM64_REG_PC,a+4)
  elif name=='a733_smp_counter':ret(200)
  elif name in ('a733_smp_contract_valid','hdmi_session_prepare','a733_cpu_coherence_publish'):ret(1)
  elif name=='a733_cached_prepare':ret()
  else:terminal.append(name);u.emu_stop()
 hooks={e.address(name):name for name in ('a733_smp_counter','a733_smp_contract_valid','hdmi_session_prepare','a733_cpu_coherence_publish','a733_cached_prepare','a733_cpu_start_init','a733_smp_stop')}
 hooks[0x901ff000]='return'
 for a in set(hooks)|set(ops):u.hook_add(UC_HOOK_CODE,hook,begin=a,end=a)
 def read(cpu,access,a,size,value,unused):cpu.mem_write(a,regs.get(a,0).to_bytes(size,'little'))
 def write(cpu,access,a,size,value,unused):
  writes.append((a,value))
  if 0x03400180<=a<0x034001a0:regs[a-0x80]=regs.get(a-0x80,0)&~value
  else:regs[a]=value
 u.hook_add(UC_HOOK_MEM_READ,read,begin=0x03400000,end=0x0340ffff)
 u.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x03400000,end=0x0340ffff)
 u.reg_write(UC_ARM64_REG_SP,0x901fe000);u.reg_write(UC_ARM64_REG_LR,0x901ff000)
 name='a733_smp_boot'
 u.emu_start(e.address(name),0x901ff004,count=50000)
 if case in ('quiet-with-neighboring-interrupts','usb-quiet-with-neighboring-interrupts'):
  assert terminal==['a733_cpu_start_init'],(case,terminal,hex(u.reg_read(UC_ARM64_REG_PC)),writes)
 else:
  assert terminal==['a733_smp_stop'] and writes==[],(case,terminal,writes)
  assert (u.reg_read(UC_ARM64_REG_X0),u.reg_read(UC_ARM64_REG_X1))==(3,0x30b)
 results[case]=dict(passed=True,terminal=terminal,register_writes=len(writes));print('PASS',case,flush=True)
(O/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,usb_profile=a.usb,image_sha256=hashlib.sha256(P.read_bytes()).hexdigest(),cases=results,scope='Actual HDMI audio inherited-interrupt refusals before GIC writes; neighboring bits do not cause refusal. USB selection is bound to linked profile metadata. Session validation, cache prep, system registers and GIC effects modeled. Stops before CPU start constructor; no secondary, USB controller or physical execution. Existing HDMI startup and video-only evidence remain separately scoped without replay.'),indent=2)+'\n')
