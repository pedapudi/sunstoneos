from pathlib import Path
import importlib.util,types,struct,json,re,subprocess,hashlib
from unittest.mock import patch
from unicorn import UC_HOOK_MEM_READ
from unicorn.arm64_const import *
import argparse
parser=argparse.ArgumentParser(description="Actual Thermal single-core admission and startup; hardware responses remain modeled.")
for name in ('image','output'):parser.add_argument('--'+name,type=Path,required=True)
parser.add_argument('--session',type=Path)
parser.add_argument('--profile-only',action='store_true',help='Check changed native profile packaging without replaying startup.')
args=parser.parse_args()
if not args.profile_only and args.session is None:parser.error('Startup checks require --session')
r=Path(__file__).resolve().parents[1];out=args.output;out.mkdir(parents=True,exist_ok=False)
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
TCR=mod('a733_tcr',Path(__file__).resolve().parents[1]/'tools/a733_tcr.py').SINGLE_CORE
shared=mod('shared',r/'checks/storage-read-single-core.py');native=mod('im',r/'checks/storage-read-native.py');prep=mod('prep',r/'apps/thermal/prepare.py')

def profile_cases():
 # The native ELF selects its profile; an external manifest is optional metadata.
 image_raw=args.image.read_bytes()
 assert prep.image_manifest_check({},image_raw)=='thermal-single-core-supervised-v1'
 assert prep.image_manifest_check({'target':'stale-target','execution_profile':'stale-profile','final_elf':'0'*64},image_raw)=='thermal-single-core-supervised-v1'
 # Keep the actual immutable native geometry rejection.
 import io
 elf=prep.ELFFile(io.BytesIO(image_raw))
 symbol=next(v for v in elf.get_section_by_name('.symtab').iter_symbols() if v.name=='thermal_single_core_geometry')
 section=elf.get_section(symbol['st_shndx']);offset=section['sh_offset']+symbol['st_value']-section['sh_addr']
 bad=bytearray(image_raw);bad[offset]^=1
 try:prep.image_manifest_check({},bytes(bad))
 except ValueError:pass
 else:raise AssertionError('Corrupt native geometry accepted')

if args.profile_only:
 profile_cases()
 (out/'result.json').write_text(json.dumps({'physical_access':False,'cases':{'native-profile-without-review-records':True,'corrupt-native-geometry-refused':True},'limits':'ELF profile and immutable geometry checks only; no native startup replay.'},indent=2)+'\n')
 print('Native profile and geometry checks passed.');raise SystemExit(0)
image=args.image;e=native.Image(image);e.path=image;e.ops={}
for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(image)],text=True).splitlines():
 m=re.match(r'^\s*([0-9a-f]+):\s+(.+)$',line)
 if m:e.ops[int(m[1],16)]=m[2].strip()
events={e.address('a733_cpu_coherence_publish')}|{a for a,op in e.ops.items() if op.split()[0].lower() in ('mrs','msr','at','wfe')}
raw=bytearray(args.session.read_bytes());_,_,build=prep.base.image_parts(image);raw[48:80]=build;raw=bytes(raw)
class Machine(shared.Machine):
 def __init__(self,variant='nominal'):
  compat=types.SimpleNamespace(session_check=prep.session_check,embedded_reference=lambda *a:None,request_bytes=prep.request_bytes)
  self.el=12
  with patch.object(shared,'module',lambda *a:compat):super().__init__(e,raw,variant)
  contract=bytearray(self.contract);struct.pack_into('<Q',contract,0,prep.CONTRACT_MAGIC);struct.pack_into('<4Q',contract,80,self.time-100,self.time-100+24000000,self.time-100+1440000000,self.time-100+1440000000);self.contract=bytes(contract);self.cpu.mem_write(0x42004000,self.contract)
  if variant=='tampered-session':self.cpu.mem_write(0x41f00000+216,bytes([raw[216]^1]))
  if variant=='tampered-contract':self.cpu.mem_write(0x42004000+192,bytes([contract[192]^1]))
  self.cpu.hook_add(UC_HOOK_MEM_READ,self.monitor_read,begin=0x42000000,end=0x421fffff)
 def monitor_read(self,cpu,access,address,size,value,unused):
  if self.el==4 and self.sysregs['ttbr0_el1']:raise AssertionError(('Protected EL1 monitor access',hex(address)))
 def hook(self,cpu,a,size,u):
  if a==e.address('a733_cpu_coherence_publish'):
   self.publications.append((cpu.reg_read(UC_ARM64_REG_X0),cpu.reg_read(UC_ARM64_REG_X1)))
   self.ret(0 if self.variant=='publication-failure' else 1);return
  if a not in events and a not in (0x43fff000,e.address('platform_panic'),getattr(self,'stop_at',None)):return
  text=e.ops.get(a,'');m=re.fullmatch(r'mrs\s+x(\d+),\s*(CurrentEL|sctlr_el3)',text,re.I)
  if m:cpu.reg_write(UC_ARM64_REG_X0+int(m[1]),self.el if m[2].lower()=='currentel' else 0);cpu.reg_write(UC_ARM64_REG_PC,a+4);return
  super().hook(cpu,a,size,u)
 def call(self,name,*args):
  self.finished=False;self.parked=False
  self.cpu.reg_write(UC_ARM64_REG_SP,0x43ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.emu_start(e.address(name),0,count=50000000)
  assert self.finished or self.parked,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
  return self.cpu.reg_read(UC_ARM64_REG_X0)
 def translate(self,va,write):
  assert self.sysregs['tcr_el1']==TCR
  self.sysregs['tcr_el1']=TCR
  try:return super().translate(va,write)
  finally:self.sysregs['tcr_el1']=TCR
results={}
def passed(n):results[n]=True;(out/'completed.json').write_text(json.dumps(results,indent=2)+'\n')
m=Machine();m.call('sd_single_core_monitor_init');assert not m.parked, (m.read(0x42000028),m.publications)
sealed=m.call('thermal_admitted_session');board=m.call('board_session_snapshot')
assert bytes(m.cpu.mem_read(sealed,len(raw)))==raw
lo,hi=e.address('__thermal_retained_start'),e.address('__thermal_retained_end')
assert lo<=sealed<sealed+len(raw)<=hi and lo<=board<board+592<=hi
runtime=m.call('sd_single_core_contract');assert lo<=runtime<runtime+224<=hi
assert m.read(0x03400088,4)&128 and not m.read(0x03400108,4)&128
assert m.read(0x03406238)==0 and m.read(0x03400444,4)>>24==0xd8
assert not m.read(0x03400d08,4)&128 and not m.read(0x03400c10,4)&(3<<14)
passed('retained-session-contract-and-disabled-routed-irq71')
before=bytes(m.cpu.mem_read(lo,hi-lo));m.cpu.mem_write(0x41f00000,bytes(len(raw)));m.el=4
m.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3c5);m.stop_at=e.address('kernel_main');m.finished=False
m.cpu.emu_start(e.address('_start'),0,count=3000000);assert m.finished
assert bytes(m.cpu.mem_read(lo,hi-lo))==before;passed('session-survives-upload-mutation-and-el1-bss-clear')
m.stop_at=e.address('a733_smp_diagnostic_start');m.call('kernel_main');assert not m.parked
assert m.call('a733_smp_device_preparation_admitted')==1
for va,attr,share in [(0x02522000,0,2),(0x03006244,0,2),(0x03400000,0,2),(sealed,255,3)]:
 assert m.call('aarch64_device_translate',va,1,0x43010000)==1
 assert struct.unpack('<QII',m.cpu.mem_read(0x43010000,16))==(va,attr,share)
for va in (0x42000000,0x42005000,e.address('a733_cached_el1_l1')):
 assert m.call('aarch64_device_translate',va,1,0x43010000)==0
passed('actual-descriptors-map-thermal-device-and-exclude-monitor')
m.stop_at=e.address('vTaskStartScheduler');m.call('a733_smp_diagnostic_start');assert not m.parked
assert m.cpu.reg_read(UC_ARM64_REG_PSTATE)&0x3c0==0x3c0
passed('actual-owner-client-provider-construction-restores-daif')
# The actual single-core dispatch/ISR masks only its GIC source and defers work.
fields=e.layouts['thermal_application'];thermal=e.address('thermal')
m.write(thermal+fields['masked'],0);m.write(thermal+fields['pending'],0)
accesses=[]
def forbidden(cpu,access,address,size,value,unused):accesses.append(address)
m.cpu.hook_add(UC_HOOK_MEM_READ,forbidden,begin=0x02522000,end=0x02522fff)
m.write(e.address('ullPortInterruptNesting'),1,8)
m.call('vApplicationFPUSafeIRQHandler',71)
assert m.read(thermal+fields['masked'],4)==1 and m.read(thermal+fields['pending'],4)==1
assert not accesses
passed('actual-single-core-irq71-dispatch-defers-without-sensor-read')
for variant in ('tampered-session','tampered-contract','secondary-active','secondary-control','secondary-mode','watchdog-active','publication-failure'):
 q=Machine(variant);q.call('sd_single_core_monitor_init');assert q.parked,variant
 passed('refuses-'+variant)
profile_cases()
passed('native-profile-without-review-records-and-corrupt-geometry-refusal')
(out/'result.json').write_text(json.dumps({'physical_access':False,'cases':results,'limits':'System registers, counter, GIC and RTC registers modeled. No service arithmetic or physical behavior replay.'},indent=2)+'\n')
