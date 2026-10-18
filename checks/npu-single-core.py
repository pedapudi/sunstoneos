from pathlib import Path
import importlib.util,types,struct,json,re,subprocess,hashlib
from unittest.mock import patch
from unicorn import UC_HOOK_MEM_READ
from unicorn.arm64_const import *
import argparse
parser=argparse.ArgumentParser(description="Actual NPU single-core admission and startup; hardware responses remain modeled.")
for name in ('image','session','output'):parser.add_argument('--'+name,type=Path,required=True)
args=parser.parse_args();r=Path(__file__).resolve().parents[1];out=args.output;out.mkdir(parents=True,exist_ok=False)
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
shared=mod('shared',r/'checks/storage-read-single-core.py');native=mod('im',r/'checks/storage-read-native.py');prep=mod('prep',r/'apps/npu-convolution/prepare-supervised.py')
image=args.image;e=native.Image(image);e.path=image;e.ops={}
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
  assert self.sysregs['tcr_el1']==0x200803019
  self.sysregs['tcr_el1']=0x200803519
  try:return super().translate(va,write)
  finally:self.sysregs['tcr_el1']=0x200803019
results={}
def passed(n):results[n]=True;(out/'completed.json').write_text(json.dumps(results,indent=2)+'\n')
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
