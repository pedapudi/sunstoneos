from pathlib import Path
import importlib.util,types,struct,json,re,subprocess,hashlib
from unittest.mock import patch
from unicorn import UC_HOOK_MEM_READ
from unicorn.arm64_const import *
import argparse
parser=argparse.ArgumentParser(description="Actual Board single-core admission and startup; hardware responses remain modeled.")
for name in ('image','session','profile','output'):parser.add_argument('--'+name,type=Path,required=True)
parser.add_argument('--binding-only',action='store_true')
parser.add_argument('--host-only-cases',action='store_true')
args=parser.parse_args();r=Path(__file__).resolve().parents[1];out=args.output;out.mkdir(parents=True,exist_ok=False)
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
shared=mod('shared',r/'checks/storage-read-single-core.py');native=mod('im',r/'checks/storage-read-native.py');prep=mod('prep',r/'apps/board/prepare.py')
image=args.image;e=native.Image(image);e.path=image;e.ops={}
for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(image)],text=True).splitlines():
 m=re.match(r'^\s*([0-9a-f]+):\s+(.+)$',line)
 if m:e.ops[int(m[1],16)]=m[2].strip()
events={e.address('a733_cpu_coherence_publish')}|{a for a,op in e.ops.items() if op.split()[0].lower() in ('mrs','msr','at','wfe')}
prefix=bytearray(args.session.read_bytes()[:592]);_,_,build=prep.base.image_parts(image)
prefix[48:80]=build
layout=e.layouts['a733_board_session']
def put(raw,name,value,width=8):raw[layout[name]:layout[name]+width]=value.to_bytes(width,'little')
profile=args.profile.read_bytes()
put(prefix,'workload',3,4);put(prefix,'topology',201);put(prefix,'sid_authority',51)
prefix[layout['sid_evidence']:layout['sid_evidence']+32]=bytes([5])*32
put(prefix,'latch_m_identity',61);put(prefix,'latch_m_owner',62);put(prefix,'latch_m_known_mask',0x3f,4);put(prefix,'latch_m_value',0,4)
put(prefix,'radio_profile_address',0x43000000);put(prefix,'radio_profile_bytes',len(profile));prefix[368:400]=hashlib.sha256(profile).digest()
raw=bytes(prefix);(out/'MODEL-radio-session.bin').write_bytes(raw)
class Machine(shared.Machine):
 def __init__(self,variant='nominal',session=raw):
  compat=types.SimpleNamespace(session_check=prep.session_check,embedded_reference=lambda *a:None,request_bytes=lambda address,identity,raw,*ids:prep.request_bytes(address,build,*ids,raw))
  self.el=12
  with patch.object(shared,'module',lambda *a:compat):super().__init__(e,session,variant)
  self.cpu.mem_write(0x43000000,profile)
  for i,value in enumerate((0x11223344,0x55667788,0x99aabbcc,0xddeeff00)):self.write(0x03006200+4*i,value)
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
  text=e.ops.get(a,'');m=re.fullmatch(r'mrs\s+x(\d+),\s*(CurrentEL|sctlr_el3|ctr_el0)',text,re.I)
  if m:cpu.reg_write(UC_ARM64_REG_X0+int(m[1]),self.el if m[2].lower()=='currentel' else 0x04040004 if m[2].lower()=='ctr_el0' else 0);cpu.reg_write(UC_ARM64_REG_PC,a+4);return
  super().hook(cpu,a,size,u)
 def call(self,name,*args):
  self.finished=False;self.parked=False
  self.cpu.reg_write(UC_ARM64_REG_SP,0x43ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.emu_start(e.address(name),0,count=50000000)
  assert self.finished or self.parked,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
  return self.cpu.reg_read(UC_ARM64_REG_X0)
 def translate(self,va,write):
  assert self.sysregs['tcr_el1']==0x200803019
  self.sysregs['tcr_el1']=0x200803519
  try:return super().translate(va,write)
  finally:self.sysregs['tcr_el1']=0x200803019
if args.host_only_cases:
 cases={}
 for workload in (0,1):
  selected=bytearray(raw);put(selected,'workload',workload,4)
  if not workload:
   for name in ('sid_authority','radio_profile_address','radio_profile_bytes','latch_m_identity','latch_m_owner'):put(selected,name,0)
   put(selected,'latch_m_known_mask',0,4);put(selected,'latch_m_value',0,4);selected[368:400]=bytes(32);selected[layout['sid_evidence']:layout['sid_evidence']+32]=bytes(32)
  for name,value in (('latch_identity',20),('latch_owner',21),('typec_owner',72),('usb2_owner',73)):put(selected,name,value)
  for name,value in (('latch_known_mask',0x3fff),('source_min_mv',4750),('source_max_mv',5500),('source_ma',500),('zero_threshold_max_mv',800),('fusb_device_id',0x80),('usb2_revision',0x3137302a)):put(selected,name,value,4)
  struct.pack_into('<I',selected,layout['usb_supply_uv']+16,1800000)
  h=Machine(session=bytes(selected));h.call('sd_single_core_monitor_init');assert not h.parked
  h.el=4;h.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3c5);h.stop_at=e.address('kernel_main');h.finished=False;h.cpu.emu_start(e.address('_start'),0,count=3000000)
  h.stop_at=e.address('vTaskStartScheduler');h.call('kernel_main');assert not h.parked
  cases['actual-TypeC-USB2-and-'+('radio' if workload else 'USB-only')+'-construction']=True
  (out/'host-startup-result.json').write_text(json.dumps({'physical_access':False,'cases':cases,'limits':'Constructor execution only; hardware/counter/GIC/SID modeled. No host, network, RF or entropy polling.'},indent=2)+'\n')
 raise SystemExit(0)
if args.binding_only:
 nominal=Machine();assert nominal.call('board_session_prepare',0x42004000)==1
 wrong=bytearray(raw);wrong[48:80]=hashlib.sha256(image.read_bytes()).digest()
 rejected=Machine(session=bytes(wrong))
 assert rejected.call('a733_smp_contract_valid',0x42004000,rejected.time)==1
 assert rejected.call('board_session_prepare',0x42004000)==0
 (out/'binding-result.json').write_text(json.dumps({'physical_access':False,'valid-linked-id-accepted':True,'elf-hash-refused-by-actual-retained-session-consumer':True},indent=2)+'\n')
 raise SystemExit(0)
results={}
def passed(n):results[n]=True;(out/'completed.json').write_text(json.dumps(results,indent=2)+'\n')
def admitted(session=raw):
 m=Machine(session=session);m.call('sd_single_core_monitor_init');assert not m.parked
 return m
m=admitted();sealed=m.call('board_session_snapshot');identity=m.call('board_radio_chip_id')
lo,hi=e.address('__board_retained_start'),e.address('__board_retained_end')
assert lo<=sealed<sealed+592<=hi and lo<=identity<identity+24<=hi
assert bytes(m.cpu.mem_read(sealed,592))==raw and m.read(identity)==51
assert bytes(m.cpu.mem_read(identity+8,16))==struct.pack('<4I',0x11223344,0x55667788,0x99aabbcc,0xddeeff00)
runtime=m.call('sd_single_core_contract');assert lo<=runtime<runtime+224<=hi
passed('authenticated-session-and-sid-runtime-contract-retained')
before=bytes(m.cpu.mem_read(lo,hi-lo));m.cpu.mem_write(0x41f00000,bytes(592));m.el=4
m.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3c5);m.stop_at=e.address('kernel_main');m.finished=False
m.cpu.emu_start(e.address('_start'),0,count=3000000);assert m.finished
assert bytes(m.cpu.mem_read(lo,hi-lo))==before;passed('session-and-sid-survive-upload-mutation-and-bss-clear')
m.stop_at=e.address('a733_smp_diagnostic_start');m.call('kernel_main');assert not m.parked
assert m.call('a733_smp_device_preparation_admitted')==1
for va,attr,share in [(0x04021000,0,2),(0x04603000,0,2),(0x03400000,0,2),(sealed,255,3),(0x43000000,255,3),(e.address('__device_pool_start'),255,3),(0x41c00000,68,3)]:
 assert m.call('aarch64_device_translate',va,1,0x43300000)==1
 assert struct.unpack('<QII',m.cpu.mem_read(0x43300000,16))==(va,attr,share)
for va in (0x42000000,0x42005000,e.address('a733_cached_el1_l1')):
 assert m.call('aarch64_device_translate',va,1,0x43300000)==0
passed('actual-descriptors-cover-radio-ce-profiles-pools-and-protected-monitor')
m.stop_at=e.address('vTaskStartScheduler');m.call('a733_smp_diagnostic_start');assert not m.parked
assert m.cpu.reg_read(UC_ARM64_REG_PSTATE)&0x3c0==0x3c0
passed('actual-radio-provider-entropy-session-and-static-task-construction')
for variant in ('tampered-session','tampered-contract','secondary-active','secondary-control','secondary-mode','watchdog-active','publication-failure'):
 q=Machine(variant);q.call('sd_single_core_monitor_init');assert q.parked,variant
 passed('refuses-'+variant)
# The same target also constructs the provider/USB-only selection using its admitted prefix.
usb=bytearray(raw);put(usb,'workload',2,4)
for name in ('sid_authority','radio_profile_address','radio_profile_bytes','latch_m_identity','latch_m_owner'):put(usb,name,0)
put(usb,'latch_m_known_mask',0,4);put(usb,'latch_m_value',0,4);usb[368:400]=bytes(32);usb[layout['sid_evidence']:layout['sid_evidence']+32]=bytes(32)
u=admitted(bytes(usb));assert u.call('board_radio_chip_id')==0
u.el=4;u.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3c5);u.stop_at=e.address('kernel_main');u.finished=False;u.cpu.emu_start(e.address('_start'),0,count=3000000)
u.stop_at=e.address('vTaskStartScheduler');u.call('kernel_main');assert not u.parked
passed('actual-usb-only-construction-without-sid-read')
manifest=json.loads((image.parent/'board-single-core/manifest.json').read_text())
assert prep.image_manifest_check(manifest,image.read_bytes())=='board-single-core-supervised-v1'
for key,value in [('target','sunstone-a733-board'),('execution_profile','board-supervised-v2')]:
 try:prep.image_manifest_check({**manifest,key:value},image.read_bytes())
 except ValueError:pass
 else:raise AssertionError(key)
passed('elf-manifest-profile-binding')
(out/'result.json').write_text(json.dumps({'physical_access':False,'cases':results,'limits':'System registers/counter/GIC/SID modeled. Actual native constructors, descriptors, FreeRTOS task/queue and provider code execute. No radio firmware/association/entropy output is injected or replayed.'},indent=2)+'\n')
