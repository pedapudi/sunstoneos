from pathlib import Path
import importlib.util,types,struct,json,re,subprocess,hashlib
from unittest.mock import patch
from unicorn import UC_HOOK_MEM_READ
from unicorn.arm64_const import *
import argparse
parser=argparse.ArgumentParser(description="Actual NPU single-core admission and startup; hardware responses remain modeled.")
for name in ('image','session','output'):parser.add_argument('--'+name,type=Path,required=True)
parser.add_argument('--usb-terminal-only',action='store_true')
parser.add_argument('--retention-only',action='store_true')
parser.add_argument('--state',type=Path)
parser.add_argument('--resume',action='store_true')
args=parser.parse_args();r=Path(__file__).resolve().parents[1];out=args.output;out.mkdir(parents=True,exist_ok=args.resume)
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
TCR=mod('a733_tcr',Path(__file__).resolve().parents[1]/'tools/a733_tcr.py').SINGLE_CORE
shared=mod('shared',r/'checks/storage-read-single-core.py');native=mod('im',r/'checks/storage-read-native.py');prep=mod('prep',r/'apps/npu-convolution/prepare-supervised.py')
# Only the structures this check reads; other units may reuse a tag such as profile.
LAYOUTS=('npu_session','a733_npu_application_boot','a733_npu_application_profile','a733_npu_reserved',
 'a733_observe_identity','npu_convolution','a733_counter_clock','a733_clock_leases','a733_platform_locks','a733_usb_platform')
image=args.image;e=native.Image(image,layout_names=LAYOUTS);e.path=image;e.ops={}
original_address=e.address
def address(n):
 if n=='a733_cpu_coherence_publish':return 0
 return original_address(n)
e.address=address
for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(image)],text=True).splitlines():
 m=re.match(r'^\s*([0-9a-f]+):\s+(.+)$',line)
 if m:e.ops[int(m[1],16)]=m[2].strip()
private_digest=original_address('digest',unit='drivers/npu/npu_convolution.c')
events={private_digest}|{a for a,op in e.ops.items() if op.split()[0].lower() in ('mrs','msr','at','wfe')}
raw=bytearray(args.session.read_bytes());_,_,build,layout=prep.image_data(image.read_bytes());raw[48:80]=build;struct.pack_into('<Q',raw,8,2);raw=bytes(raw)
class Machine(shared.Machine):
 def __init__(self,variant='nominal'):
  compat=types.SimpleNamespace(session_check=lambda raw:prep.session_profile(raw,layout),embedded_reference=lambda *a:None,request_bytes=lambda address,identity,raw,*ids:prep.request_bytes(address,identity,raw,prep.session_profile(raw,layout)))
  self.el=12
  with patch.object(shared,'module',lambda *a:compat):super().__init__(e,raw,variant)
  contract=bytearray(self.contract);struct.pack_into('<Q',contract,0,prep.CONTRACT_MAGIC);struct.pack_into('<4Q',contract,80,self.time-100,self.time-100+24000000,self.time-100+480000000,self.time-100+480000000);self.contract=bytes(contract);self.cpu.mem_write(0x42004000,self.contract)
  if variant=='tampered-session':self.cpu.mem_write(0x41f00000+216,bytes([raw[216]^1]))
  if variant=='tampered-contract':self.cpu.mem_write(0x42004000+192,bytes([contract[192]^1]))
  self.cpu.hook_add(UC_HOOK_MEM_READ,self.monitor_read,begin=0x42000000,end=0x421fffff)
 def monitor_read(self,cpu,access,address,size,value,unused):
  if self.el==4 and self.sysregs['ttbr0_el1']:raise AssertionError(('Protected EL1 monitor access',hex(address)))
 def hook(self,cpu,a,size,u):
  if a==private_digest:
   address,n,dest=(cpu.reg_read(reg) for reg in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2))
   assert 0<n<=524288 and 0x41000000<=address<address+n<=0x42000000
   cpu.mem_write(dest,hashlib.sha256(bytes(cpu.mem_read(address,n))).digest());self.ret();return
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
assert not args.resume or args.usb_terminal_only
results=json.loads((out/'completed.json').read_text()) if args.resume else {}
def passed(n):results[n]=True;(out/'completed.json').write_text(json.dumps(results,indent=2)+'\n')
if args.usb_terminal_only:
 # Reuse accepted startup solely to construct the real application/provider
 # fixture. Only changed terminal-USB branches are cases in this invocation.
 from elftools.elf.elffile import ELFFile
 from elftools.dwarf.dwarf_expr import DWARFExprParser
 assert args.state, '--state is required for the actual attempted baseline'
 state=json.loads(args.state.read_text());registers={int(x['address'],0):int(x['value'],0) for x in state['registers']}
 with image.open('rb') as stream:
  dwarf=ELFFile(stream).get_dwarf_info();objects={}
  for cu in dwarf.iter_CUs():
   unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
   for die in cu.iter_DIEs():
    if die.tag!='DW_TAG_variable' or 'DW_AT_location' not in die.attributes or 'DW_AT_name' not in die.attributes:continue
    name=die.attributes['DW_AT_name'].value.decode()
    if not ((unit.endswith('/apps/npu-convolution/native.c') and name=='app') or (unit.endswith('/npu-single-core/usb_service.c') and name=='task_state')):continue
    ops=DWARFExprParser(dwarf.structs).parse_expr(die.attributes['DW_AT_location'].value);assert len(ops)==1
    address=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dwarf.get_addr(cu,ops[0].args[0])
    fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in die.get_DIE_from_attribute('DW_AT_type').iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes}
    objects[name]=(address,fields)
 assert set(objects)=={'app','task_state'}
 app,fields=objects['app'];usb_task=objects['task_state'][0]
 def constructed():
  m=Machine();m.call('sd_single_core_monitor_init');assert not m.parked
  m.el=4;m.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3c5);m.stop_at=e.address('kernel_main');m.finished=False;m.cpu.emu_start(e.address('_start'),0,count=3000000);assert m.finished
  m.stop_at=e.address('a733_smp_diagnostic_start');m.call('kernel_main')
  m.stop_at=e.address('vTaskStartScheduler');m.call('a733_smp_diagnostic_start');assert not m.parked
  m.stop_at=None;m.write(e.address('xSchedulerRunning'),1,8);m.write(e.address('ullCriticalNesting'),0,8);m.write(e.address('ullPortInterruptNesting'),0,8)
  m.write(e.address('pxCurrentTCB'),usb_task,8);m.cpu.reg_write(UC_ARM64_REG_PSTATE,0x345)
  for address,value in registers.items():m.write(address,value)
  m.write(0x04100040,0x40)
  return m
 def prepared(m):
  for polls in range(512):
   ready=m.call('npu_single_core_usb_poll');assert ready in (0,1),(polls,ready)
   if ready:return
   m.time+=24000
  raise AssertionError('USB ROM takeover remained pending')
 def frame(m,reason):
  buffer,challenge=0x43010000,0x43011000;m.cpu.mem_write(challenge,b'x'*32)
  assert m.call('a733_npu_application_frame',buffer,e.address('a733_observe_identity'),challenge,e.address('a733_smp_build_id'))==1
  body=bytes(m.cpu.mem_read(buffer,1024));assert struct.unpack_from('<Q',body,16)[0]==3 and struct.unpack_from('<Q',body,832)[0]==reason
  assert body[992:]==hashlib.sha256(body[:992]).digest()
 if 'preparation-after-client-failure-delivers-terminal-frame' not in results:
  m=constructed();m.write(e.address('a733_npu_application_failed'),8);prepared(m);frame(m,8);passed('preparation-after-client-failure-delivers-terminal-frame')
 if 'prepared-usb-delivers-convolution-failure-frame' not in results:
  m=constructed();prepared(m)
  m.write(app+fields['reserved']+e.layouts['a733_npu_reserved']['convolution']+e.layouts['npu_convolution']['failed'],1)
  assert m.call('npu_single_core_usb_poll')==1;frame(m,17);passed('prepared-usb-delivers-convolution-failure-frame')
 # Shared transport uncertainty must refuse without another peripheral write.
 for name,field,struct_name,member,value in [
  ('counter-failed','clock','a733_counter_clock','failed',1),
  ('registry-failed','registry','a733_clock_leases','failed',1),
  ('register-lock-retained','locks','a733_platform_locks','retained',1),
  ('i2c-lock-retained','locks','a733_platform_locks','retained',2),
  ('usb-provider-fault','usb_platform','a733_usb_platform','fault',1)]:
  if name+'-refuses-without-mmio' in results:continue
  m=constructed();prepared(m);m.write(e.address('a733_npu_application_failed'),8)
  m.write(app+fields[field]+e.layouts[struct_name][member],value);writes=list(m.writes)
  assert m.call('npu_single_core_usb_poll')&0xffffffff==0xffffffff and m.writes==writes,name
  passed(name+'-refuses-without-mmio')
 for name in ('wrong-task','expired-session','bad-usb-evidence'):
  if name+'-refuses-without-mmio' in results:continue
  m=constructed();prepared(m);m.write(e.address('a733_npu_application_failed'),8)
  if name=='wrong-task':m.write(e.address('pxCurrentTCB'),usb_task+16,8)
  elif name=='expired-session':m.time=m.read(e.address('a733_observe_identity')+e.layouts['a733_observe_identity']['deadline'])
  else:
   sealed=m.call('npu_session_get');m.write(sealed+e.layouts['npu_session']['boot']+e.layouts['a733_npu_application_boot']['profile']+e.layouts['a733_npu_application_profile']['usb_evidence'],0,8)
  writes=list(m.writes);assert m.call('npu_single_core_usb_poll')&0xffffffff==0xffffffff and m.writes==writes,name
  passed(name+'-refuses-without-mmio')
 (out/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'cases':results,'scope':'Changed terminal-USB branches only. Actual ELF application/provider/result frame, registry/locks and SHA. Accepted startup used only for fixture construction. CPU/system registers,counter,task and MMIO hardware effects modeled; no inference, PLL work or physical operation.'},indent=2)+'\n')
 print('PASS terminal USB',len(results));raise SystemExit(0)
m=Machine();m.call('sd_single_core_monitor_init');assert not m.parked
sealed=m.call('npu_session_get');assert bytes(m.cpu.mem_read(sealed,len(raw)))==raw
lo,hi=e.address('__npu_retained_start'),e.address('__npu_retained_end');assert lo<=sealed<sealed+len(raw)<=hi
runtime=m.call('sd_single_core_contract');assert lo<=runtime<runtime+224<=hi
assert not m.read(0x0340010c,4)&2
passed('sealed-session-runtime-contract-and-disabled-spi97')
before=bytes(m.cpu.mem_read(lo,hi-lo));m.cpu.mem_write(0x41f00000,bytes(len(raw)));m.el=4;m.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3c5);m.stop_at=e.address('kernel_main');m.finished=False;m.cpu.emu_start(e.address('_start'),0,count=3000000);assert m.finished
assert bytes(m.cpu.mem_read(lo,hi-lo))==before;passed('session-survives-upload-mutation-and-el1-bss-clear')
m.stop_at=e.address('a733_smp_diagnostic_start');m.call('kernel_main');assert not m.parked
assert m.call('a733_smp_device_preparation_admitted')==1 and m.call('npu_session_copy')==1
for va,attr,share in [(0x02002000,0,2),(0x03600000,0,2),(0x04100000,0,2),(sealed,255,3),(e.address('__device_pool_start'),255,3)]:
 assert m.call('aarch64_device_translate',va,1,0x43010000)==1
 assert struct.unpack('<QII',m.cpu.mem_read(0x43010000,16))==(va,attr,share)
for va in (0x42000000,0x42005000,e.address('a733_cached_el1_l1')):assert m.call('aarch64_device_translate',va,1,0x43010000)==0
passed('descriptor-walk-enforces-device-wb-and-protected-monitor-mappings')
if args.retention_only:
 (out/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'cases':results,'scope':'New complete-image retention/layout composition only: actual EL3 validation/seal, retained session/runtime, EL1 BSS clear and descriptor checks. Existing software-only fixture adapted to new build identity; CPU/GIC/counter effects modeled. No task/device/quiescence replay or physical admission.'},indent=2)+'\n')
 print('PASS NPU retained-session composition',len(results));raise SystemExit(0)

m.stop_at=e.address('vTaskStartScheduler');m.call('a733_smp_diagnostic_start');assert not m.parked;passed('actual-static-task-and-provider-construction')
# Continue from actual construction into the USB provider with modeled task,
# counter and ROM hardware state. No NPU inference or peripheral probe runs.
m.stop_at=None
m.write(e.address('xSchedulerRunning'),1,8)
m.write(e.address('ullCriticalNesting'),0,8);m.write(e.address('ullPortInterruptNesting'),0,8)
# Local symbols repeat: find this variable through its defining compilation unit.
from elftools.elf.elffile import ELFFile
with image.open('rb') as stream:
 dwarf=ELFFile(stream).get_dwarf_info();matches=[]
 for cu in dwarf.iter_CUs():
  unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
  if not unit.endswith('/npu-single-core/usb_service.c'):continue
  for die in cu.iter_DIEs():
   if die.tag=='DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'task_state':
    from elftools.dwarf.dwarf_expr import DWARFExprParser
    ops=DWARFExprParser(dwarf.structs).parse_expr(die.attributes['DW_AT_location'].value)
    assert len(ops)==1 and ops[0].op_name in ('DW_OP_addr','DW_OP_addrx')
    matches.append(dwarf.get_addr(cu,ops[0].args[0]) if ops[0].op_name=='DW_OP_addrx' else ops[0].args[0])
 assert len(matches)==1,matches
 usb_task=matches[0]
m.write(e.address('pxCurrentTCB'),usb_task,8);m.cpu.reg_write(UC_ARM64_REG_PSTATE,0x345)
C=0x02002000;U=0x04100000
registers={0x07090160:0x10000002,0x070101ac:1,C:0xf8000000|(99<<8)|(99<<16),
 C+0xa0:0xfe000000|(99<<8)|(1<<20)|(2<<16)|(4<<2),C+0x1908:0x0e060e06,C+0x1988:0x0e060000,
 C+0x500:0x03000001,C+0x580:0x82000001,C+0x588:0x84000000,
 C+0x5a4:0x30001,C+0x5c0:0x200,C+0x5e0:0x80000000,
 C+0x1340:0x80000000,C+0x1300:0x40000000,C+0x1304:0x01000100,U+0x40:0x40}
for address,value in registers.items():m.write(address,value)
for polls in range(512):
 ready=m.call('npu_single_core_usb_poll');assert ready in (0,1),(polls,ready)
 if ready:break
 m.time+=24000
else:raise AssertionError('USB ROM takeover remained pending')
assert m.read(U+0x40,4)&0x40==0 and m.read(C+0x1304,4)==0x01000100
assert m.read(U+0x418,4)&0xf3f==0x806
passed('actual-usb-rom-provider-shares-registry-lock-and-device-mappings')
for v in ('tampered-session','tampered-contract','watchdog-active','secondary-active','secondary-control','secondary-mode'):
 f=Machine(v);f.call('sd_single_core_monitor_init');assert f.parked and f.read(0x03400000,4)==0x37;passed(v+'-retained-before-gic')
(out/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'cases':results,'scope':'Actual admission, SHA-256, retained copies, EL1 BSS clear, table construction, descriptor walk and static task/provider construction. Counter, system registers, GIC and private artifact SHA helper remain modeled. The SHA helper uses hashlib over actual bytes, reusing accepted model evidence. No inference replay.'},indent=2)+'\n');(out/'session-fixture.bin').write_bytes(raw)
print('PASS',len(results))
