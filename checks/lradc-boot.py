#!/usr/bin/env python3
"""Check LRADC quiet-interrupt admission in the linked application."""
from pathlib import Path
import argparse,importlib.util,json,struct,hashlib,re,subprocess,gzip,pickle
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
import unicorn.arm64_const as arm
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--session',type=Path,required=True)
p.add_argument('--contract',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--runtime-contract',type=Path,help='Reuse the already modeled launcher output byte-for-byte.')
a=p.parse_args();P=a.image;O=a.output;O.mkdir(parents=True,exist_ok=False)
session=a.session.read_bytes();contract=a.contract.read_bytes()
assert len(contract) in (224,4096) and not any(contract[224:])
contract=contract[:224]
assert len(contract)==224 and len(session)==784 and struct.unpack_from('<3Q',session,592)==(0x4153373341444131,2,784)
spec=importlib.util.spec_from_file_location('lradc_irq_image',R/'checks/storage-read-native.py');n=importlib.util.module_from_spec(spec);spec.loader.exec_module(n);e=n.Image(P,layout_names=('a733_smp_contract',))
# The producer leaves magic/timestamps for the SRAM launcher. Supply that
# arming step once; native model continuations reuse the saved exact contract.
template=contract
if a.runtime_contract:
 contract=a.runtime_contract.read_bytes()
 assert len(contract)==224 and contract[8:80]==template[8:80] and contract[112:]==template[112:]
 assert struct.unpack_from('<Q',contract,0)[0]==0x4153373341444331
 armed,startup,diagnostic,expires=struct.unpack_from('<4Q',contract,80)
 assert armed>0 and (startup,diagnostic,expires)==(armed+24000000,armed+1440000000,armed+1440000000)
else:
 contract=bytearray(template)
 assert struct.unpack_from('<Q',contract,0)[0]==0
 struct.pack_into('<Q',contract,0,0x4153373341444331)
 struct.pack_into('<4Q',contract,80,100,24000100,1440000100,1440000100)
 contract=bytes(contract)
counter=struct.unpack_from('<Q',contract,80)[0]+100
(O/'contract.bin').write_bytes(contract)
ops={}
for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(P)],text=True).splitlines():
 m=re.match(r'^\s*([0-9a-f]+):\s+mrs\s+x(\d+),\s*(CurrentEL|MPIDR_EL1|SCTLR_EL3|CNTPCT_EL0)\s*$',line,re.I)
 if m:ops[int(m[1],16)]=(int(m[2]),m[3].lower())
assert ops
results={}
cases=[('quiet-with-neighboring-interrupts',None,None),('legacy-profile',0x108,1<<5)]
for bank,mask in ((2,1<<5),):
 for kind,offset in (('enabled',0x100),('pending',0x200),('active',0x300)):
  cases.append((f'bank{bank}-{kind}',offset+bank*4,mask))
for case,case_offset,case_mask in cases:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.reg_write(UC_ARM64_REG_PSTATE,13);u.reg_write(UC_ARM64_REG_TPIDR_EL1,0)
 for a,z in [(0x41000000,0x1200000),(0x90000000,0x200000),(0x03400000,0x10000),(0x02050000,0x1000),(0x07050000,0x1000)]:u.mem_map(a,z)
 for a,b in e.parts:
  if b:u.mem_write(a,b)
 u.mem_write(0x42004000,contract);u.mem_write(0x41f00000,session)
 regs={0x03400000:0x30,0x03400004:7,0x03400088:0x55aa55aa,0x03400d08:0xffffffff,0x03400c10:0xaaaaaaaa,0x03400444:0x12345678,0x03406230:0x10000}
 if case_offset is not None:regs[0x03400000+case_offset]=case_mask
 if case=='quiet-with-neighboring-interrupts':
  for base in (0x100,0x200,0x300):
   regs[0x03400000+base+8]=(1<<4)|(1<<6)
 writes=[];terminal=[]
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def hook(cpu,a,size,unused):
  name=hooks.get(a)
  if a in ops and name is None:
   reg,kind=ops[a];u.reg_write(getattr(arm,"UC_ARM64_REG_X"+str(reg)),12 if kind=='currentel' else counter if kind=='cntpct_el0' else 0);u.reg_write(UC_ARM64_REG_PC,a+4)
  elif name=='a733_smp_counter':ret(counter)
  elif name in ('a733_smp_contract_valid','board_session_prepare','adc_session_prepare','a733_cpu_coherence_publish'):ret(1)
  elif name=='adc_lradc_selected':ret(case!='legacy-profile')
  elif name=='a733_cached_prepare':ret()
  else:terminal.append(name);u.emu_stop()
 hooks={e.address(name):name for name in ('a733_smp_counter','a733_smp_contract_valid','board_session_prepare','adc_session_prepare','a733_cpu_coherence_publish','a733_cached_prepare','adc_lradc_selected','a733_cpu_start_init','a733_smp_stop')}
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
 try:
  u.emu_start(e.address(name),0x901ff004,count=50000)
  if case in ('quiet-with-neighboring-interrupts','legacy-profile'):
   assert terminal==['a733_cpu_start_init'],(case,terminal,hex(u.reg_read(UC_ARM64_REG_PC)),writes)
  else:
   assert terminal==['a733_smp_stop'] and writes==[],(case,terminal,writes)
   assert (u.reg_read(UC_ARM64_REG_X0),u.reg_read(UC_ARM64_REG_X1))==(3,0x30d)
 except BaseException:
  state=dict(case=case,cpu=u.context_save(),registers=regs,writes=writes,terminal=terminal,
             image_sha256=hashlib.sha256(P.read_bytes()).hexdigest(),
             memory=[(base,bytes(u.mem_read(base,length))) for base,length in
                     ((0x41000000,0x1200000),(0x90000000,0x200000))])
  (O/(case+'-failure.pkl.gz')).write_bytes(gzip.compress(pickle.dumps(state),compresslevel=1))
  raise
 results[case]=dict(passed=True,terminal=terminal,register_writes=len(writes));print('PASS',case,flush=True)
(O/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(P.read_bytes()).hexdigest(),cases=results,scope='Actual LRADC inherited-interrupt refusals before GIC writes; neighboring bits do not cause refusal. Board/ADC session preparation, validated-profile selection, cache prep, system registers and GIC effects modeled. Stops before CPU start constructor; no secondary, USB or physical execution. INTID69 enabled, pending and active states refuse; neighboring interrupts and the modeled legacy-profile selection retain their original behavior.'),indent=2)+'\n')
