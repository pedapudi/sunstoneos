#!/usr/bin/env python3
"""Execute the linked PCIe NVMe application, function client and NVMe driver.

The port starts from the register words of the C host model at READY
(checks/pcie-nvme-registers.c). Unicorn then runs the actual application
step, the general function client, the NVMe driver and the native write
filter against a modeled NVMe controller that reads its queues from the
image's non-cacheable pool. Port startup and a733_pcie_stop are replaced by
their recorded results, and provider status, translations, the RTOS and USB
transport are modeled. Nothing here establishes physical behavior.
"""
import argparse,hashlib,importlib.util,json,re,shutil,struct,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_TPIDR_EL1
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
base=module('pcie_nvme_native_base',ROOT/'checks/pcie-application-native.py')
POOL=(0x41c00000,0x41e00000)
BAR0=0x22000000
STRUCTS=('a733_pcie_nvme','a733_pcie_function','device_memory_pool','device_memory_allocation','device_memory_lease')
# Host-model C units, as in checks/a733-pcie-trace.py.
UNITS=['drivers/pci/a733_pcie.c','drivers/pci/a733_pcie_phy.c','drivers/pci/a733_pcie_epf_test.c',
 'drivers/power/a733_pmic.c','drivers/power/a733_rail_leases.c','drivers/clock/a733_clock_leases.c',
 'drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','drivers/gpu/a733_gpu_resources.c',
 'runtime/device_memory_pool.c','drivers/pci/a733_pcie_function.c','drivers/pci/a733_pcie_nvme.c']

def registers(out):
 """Build and run the host model once; return its READY register words."""
 gen=out/'registers-generated';gen.mkdir(parents=True,exist_ok=True)
 native=(ROOT/'drivers/pci/a733_pcie_native.c').read_text()
 start=re.search(r'int a733_pcie_native_address\(',native).start();depth,end=1,native.index('{',start)+1
 while depth:depth+=(native[end]=='{')-(native[end]=='}');end+=1
 (gen/'address.c').write_text('#include "a733_pcie_native.h"\n'+native[start:end]+'\n')
 model=(ROOT/'checks/a733-pcie-nvme.c').read_text();assert model.count('int main(int argc,char **argv)')==1
 (gen/'a733-pcie-nvme-entry.c').write_text(model.replace('int main(int argc,char **argv)','int nvme_main(int argc,char **argv)'))
 command=[shutil.which('clang-21'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
  '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'include'),'-I'+str(gen),'-I'+str(ROOT/'checks'),
  str(ROOT/'checks/pcie-nvme-registers.c'),*[str(ROOT/u) for u in UNITS],str(gen/'address.c'),'-o',str(gen/'registers')]
 subprocess.run(command,check=True)
 text=subprocess.run([str(gen/'registers')],check=True,capture_output=True,text=True).stdout
 (out/'registers.txt').write_text(text);words={};fields={}
 for line in text.splitlines():
  key,value=line.split()
  if key in ('root_cap','phase'):fields[key]=int(value,16 if key=='root_cap' else 10)
  else:words[int(key,16)]=int(value,16)
 assert fields=={'root_cap':0x70,'phase':16},fields
 return words,fields

class Image(base.Image):
 def __init__(self,path):
  super().__init__(path);self.enums={}
  with path.open('rb') as f:
   for cu in ELFFile(f).get_dwarf_info().iter_CUs():
    for d in cu.iter_DIEs():
     if d.tag=='DW_TAG_enumerator':
      name=d.attributes['DW_AT_name'].value.decode()
      if name.startswith(('A733_','DEVICE_MEMORY_')):self.enums[name]=int(d.attributes['DW_AT_const_value'].value)
     name=d.attributes.get('DW_AT_name')
     if d.tag!='DW_TAG_structure_type' or not name or name.value.decode() not in STRUCTS:continue
     fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children()
      if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
     if fields:self.layouts[name.value.decode()]=fields;self.sizes[name.value.decode()]=int(d.attributes['DW_AT_byte_size'].value)

def pattern(at):return ((at*131+(at>>9)*7)^0x3c)&255

class Machine:
 """One linked-image run; variant selects the modeled fault."""
 HOOKS=['a733_smp_counter','a733_counter_clock_now','board_live','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState',
  'xQueueGetMutexHolder','aarch64_device_translate','a733_platform_lock_enter','a733_platform_lock_leave',
  'a733_platform_lock_retain','a733_pcie_poll','a733_pcie_stop','a733_clock_token_status','a733_rail_status',
  'a733_pmic_live','a733_gpu_resources_status','a733_clock_retain','a733_rail_retain','vTaskDelay',
  'a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available',
  'tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush']
 def __init__(self,image,session,words,fields,variant):
  self.image=image;self.session=session;self.variant=variant;self.fields=fields
  cpu=self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  for start,size in ((0x02000000,0x06000000),(0x20000000,0x1000),(BAR0,0x100000),(0x40000000,0x04000000),(0x50000000,0x01000000)):cpu.mem_map(start,size)
  cpu.reg_write(UC_ARM64_REG_PSTATE,5);cpu.reg_write(UC_ARM64_REG_TPIDR_EL1,0)
  for a,data in image.segments:cpu.mem_write(a,data)
  for a,v in words.items():cpu.mem_write(a,struct.pack('<I',v))
  vendor,device=struct.unpack_from('<2H',session,788);cpu.mem_write(0x20000000,struct.pack('<2H',vendor,device))
  self.vendor_id=vendor
  # NVMe BAR0: CAP (MQES 255, CQR, TO 10 s, NVM set, DSTRD 0), VS 1.4, CC and CSTS clear.
  cpu.mem_write(BAR0,struct.pack('<3I',0x140100ff,0x20,0x00010400))
  self.layout=image.layouts['pcie_application'];self.base=image.address('pcie');self.board=image.address('board');self.provider=image.address('providers')
  self.port=self.base+self.layout['port'];self.native=self.base+self.layout['native'];self.nvme=self.base+self.layout['nvme']
  self.time=1000000;self.live=True;self.held=False;self.scheduler=1;self.finished=False
  self.retain_calls={};self.stops=0;self.bar_accesses=0;self.dma=0;self.vendor_bytes=bytearray()
  self.ctl=dict(cc=0,csts=0,ready_at=None,shutdown_at=None,sq=[0,0],cq=[0,0],sq_head=[0,0],cq_tail=[0,0],phase=[1,1],created=0,commands=[])
  cpu.mem_write(0x41f00000,session);cpu.mem_write(self.base,session);cpu.mem_write(self.board,session[:592])
  cpu.mem_write(self.board+160,struct.pack('<Q',61000000));cpu.mem_write(self.board+168,struct.pack('<Q',24000000));self.field('handle',1,8)
  L=image.layouts;shared=self.provider+L['board_provider']['shared']
  objects={name:shared+L['a733_native_usb'][name] for name in ('locks','clocks','pins','rails','pmic','pmic_native','gpu','counter')};self.objects=objects
  def setv(obj,kind,field,value,size=8):cpu.mem_write(obj+L[kind][field],value.to_bytes(size,'little'))
  setv(objects['locks'],'a733_platform_locks','initialized',1,4);setv(objects['locks'],'a733_platform_locks','now',image.address('a733_counter_clock_now'))
  setv(objects['locks'],'a733_platform_locks','context',objects['counter']);setv(objects['locks'],'a733_platform_locks','mutex',1)
  setv(objects['clocks'],'a733_clock_leases','initialized',1,4);setv(objects['pins'],'a733_pins','initialized',1,4);setv(objects['gpu'],'a733_gpu_resource_manager','initialized',1,4)
  setv(objects['rails'],'a733_rail_leases','self',objects['rails']);binding=objects['rails']+L['a733_rail_leases']['binding']
  setv(binding,'a733_rail_binding','clocks',objects['clocks']);setv(binding,'a733_rail_binding','gpu',objects['gpu'])
  for field in ('clocks','pins','rails'):setv(objects['pmic'],'a733_pmic',field,objects[field])
  setv(objects['pmic'],'a733_pmic','self',objects['pmic']);setv(objects['pmic'],'a733_pmic','cookie',objects['pmic_native'])
  cpu.mem_write(shared+L['a733_native_usb']['pmic_ready'],struct.pack('<I',1))
  self.hooks={image.address(name):name for name in self.HOOKS}
  self.msix='pcie_nvme_irq_bind' in image.symbols
  self.msi_status=0;self.gic_enabled=False;self.irqs=0;self.notifications=0
  for n in ('vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR'):
   self.hooks[image.address(n)]=n
  if self.msix:
   for n in ('vTaskGenericNotifyGiveFromISR','ulTaskGenericNotifyTake'):
    self.hooks[image.address(n)]=n
   for i in range(8):cpu.mem_write(BAR0+0x2000+16*i+12,struct.pack('<I',1))
   cpu.hook_add(UC_HOOK_MEM_READ,self.irq_read,begin=0x06000830,end=0x06000830)
   cpu.hook_add(UC_HOOK_MEM_WRITE,self.irq_write,begin=0x03400000,end=0x06000830)
  cpu.hook_add(UC_HOOK_CODE,self.code)
  cpu.hook_add(UC_HOOK_MEM_READ,self.bar_size,begin=0x20000010,end=0x20000027)
  cpu.hook_add(UC_HOOK_MEM_READ,self.bar_read,begin=BAR0,end=BAR0+0xfffff)
  cpu.hook_add(UC_HOOK_MEM_WRITE,self.bar_write,begin=BAR0,end=BAR0+0xfffff)
  assert self.call('a733_pmic_native_init',objects['pmic_native'],objects['locks'],2)==1
  assert self.call('a733_memory_uncached_prepare')==1
  data=bytes(cpu.mem_read(objects['pmic_native']+L['a733_pmic_native']['pmic_bus_ops'],image.sizes['a733_pmic_ops']));cpu.mem_write(objects['pmic']+L['a733_pmic']['pmic_bus_ops'],data)
 def irq_read(self,cpu,access,a,size,value,unused):
  cpu.mem_write(a,struct.pack('<I',self.msi_status))
 def irq_write(self,cpu,access,a,size,value,unused):
  if a==0x06000830:self.msi_status&=~value
  elif a==0x03400114:self.gic_enabled=bool(value&(1<<25))
  elif a==0x03400194:self.gic_enabled=False;cpu.mem_write(0x03400114,struct.pack('<I',0))
 def deliver(self):
  if self.msix and self.msi_status and self.gic_enabled and self.variant!='lost-irq':
   self.irqs+=1;self.call('pcie_nvme_application_irq')
   if self.variant=='repeat-irq':self.call('pcie_nvme_application_irq')
 def bind(self):
  if self.msix:assert self.call('pcie_nvme_irq_bind',1)==1
 def field(self,name,value=None,size=4):
  if value is not None:self.cpu.mem_write(self.base+self.layout[name],value.to_bytes(size,'little'))
  return int.from_bytes(self.cpu.mem_read(self.base+self.layout[name],size),'little')
 def u32(self,a):return struct.unpack('<I',bytes(self.cpu.mem_read(a,4)))[0]
 def nvme_field(self,name,size=4):return int.from_bytes(self.cpu.mem_read(self.nvme+self.image.layouts['a733_pcie_nvme'][name],size),'little')
 def lease_state(self):
  L=self.image.layouts;lease=self.nvme+L['a733_pcie_nvme']['lease'];slot=self.u32(lease+L['device_memory_lease']['slot'])
  pool=self.image.address('uncached_pool');at=pool+L['device_memory_pool']['allocations']+slot*self.image.sizes['device_memory_allocation']
  return self.u32(at+L['device_memory_allocation']['state'])
 def msi_retained(self):
  L=self.image.layouts;lease=self.nvme+L['a733_pcie_nvme']['function']+L['a733_pcie_function']['msi_lease'];slot=self.u32(lease+L['device_memory_lease']['slot'])
  pool=self.image.address('uncached_pool');at=pool+L['device_memory_pool']['allocations']+slot*self.image.sizes['device_memory_allocation']
  return self.u32(at+L['device_memory_allocation']['state'])==self.image.enums['DEVICE_MEMORY_RETAINED']
 def returned(self,n):self.cpu.reg_write(UC_ARM64_REG_X0,n&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 # Modeled endpoint: BAR sizing, the NVMe controller and its queue DMA.
 def bar_size(self,cpu,access,a,size,value,unused):
  sizes={0x20000010:0xffffc004,0x20000014:0xffffffff,0x20000018:0xfffff000,0x2000001c:0,0x20000020:1,0x20000024:0}
  if a in sizes and self.u32(a)==0xffffffff:cpu.mem_write(a,struct.pack('<I',sizes[a]))
 def bar0(self):return self.u32(0x20000010)&~15
 def bar_read(self,cpu,access,a,size,value,unused):
  self.bar_accesses+=1;assert self.u32(0x20000004)&2 and a-self.bar0()<0x4000,hex(a)
  if a-self.bar0()==0x1c:
   c=self.ctl
   if c['cc']&1 and c['ready_at'] is not None and self.time>=c['ready_at']:c['csts']|=1
   if (c['cc']>>14)&3==1 and self.variant!='shutdown-stuck' and self.time>=c['shutdown_at']:c['csts']=(c['csts']&~0xc)|8
   cpu.mem_write(a,struct.pack('<I',c['csts']))
 def owned_dma(self):
  # Every modeled DMA needs bus mastering and device-owned queue memory.
  assert self.u32(0x20000004)&4 and self.lease_state()==self.image.enums['DEVICE_MEMORY_DEVICE'];self.dma+=1
 def host(self,physical,n):assert POOL[0]<=physical and physical+n<=POOL[1],hex(physical);return physical
 def post(self,q,cid,status=0):
  c=self.ctl;at=self.host(c['cq'][q]+16*c['cq_tail'][q],16);self.owned_dma()
  self.cpu.mem_write(at,struct.pack('<4I',0,0,c['sq_head'][q]|q<<16,cid|c['phase'][q]<<16|status<<17))
  if self.msix:
   assert self.u32(0x20000090)>>16&0xc000==0x8000
   assert self.u32(BAR0+0x2000)==self.u32(0x06000820) and not self.u32(BAR0+0x2004) and not self.u32(BAR0+0x2008) and not self.u32(BAR0+0x200c)&1
   assert self.u32(0x06000828)==1 and not self.u32(0x0600082c)&1
   self.msi_status|=1
  c['cq_tail'][q]+=1
  if c['cq_tail'][q]==2:c['cq_tail'][q]=0;c['phase'][q]^=1
 def execute(self,q,tail):
  c=self.ctl
  while c['sq_head'][q]!=tail:
   at=self.host(c['sq'][q]+64*c['sq_head'][q],64);self.owned_dma();w=struct.unpack('<16I',bytes(self.cpu.mem_read(at,64)))
   c['sq_head'][q]=(c['sq_head'][q]+1)%2;opcode,cid=w[0]&255,w[0]>>16;c['commands'].append((q,opcode))
   if q:
    # Read only: no other I/O opcode reaches the model.
    assert opcode==2 and w[1]==1 and not w[8] and not w[9],w
    if self.variant=='timeout':continue
    lba=w[11]<<32|w[10];n=(w[12]+1)*512;assert n<=4096
    data=self.host(w[7]<<32|w[6],n);self.owned_dma();self.cpu.mem_write(data,bytes(pattern(lba*512+i) for i in range(n)));self.post(1,cid);continue
   if opcode==6:
    data=self.host(w[7]<<32|w[6],4096);self.owned_dma();d=bytearray(4096)
    if w[10]==1:
     struct.pack_into('<2H',d,0,self.vendor_id,self.vendor_id);d[4:24]=b'S4EWNX0R123456      ';d[24:64]=b'MODEL NVME READ-ONLY FIXTURE            ';d[64:72]=b'2B2QEXM7'
     d[77]=5;struct.pack_into('<I',d,80,0x00010400);struct.pack_into('<I',d,516,1)
    else:
     assert w[10]==0 and w[1]==1;struct.pack_into('<Q',d,0,0x100000);d[130]=9
    self.cpu.mem_write(data,bytes(d));self.post(0,cid)
   elif opcode==9:assert w[10]==7 and not w[11];self.post(0,cid)
   elif opcode==5:
    assert w[11]==(3 if self.msix else 1);c['cq'][1]=w[7]<<32|w[6];c['created']|=1;self.post(0,cid)
   elif opcode==1:assert c['created']&1;c['sq'][1]=w[7]<<32|w[6];c['created']|=2;self.post(0,cid)
   elif opcode==0:c['created']&=~2;self.post(0,cid)
   elif opcode==4:assert not c['created']&2;c['created']&=~1;self.post(0,cid)
   else:raise AssertionError(('admin opcode',opcode))
 def bar_write(self,cpu,access,a,size,value,unused):
  self.bar_accesses+=1;off=a-self.bar0();assert self.u32(0x20000004)&2 and size==4 and 0<=off<0x4000,hex(a);c=self.ctl
  if off==0x14:
   if value&1 and not c['cc']&1:
    aqa,asq,acq=self.u32(a+0x10),struct.unpack('<Q',bytes(cpu.mem_read(a+0x14,8)))[0],struct.unpack('<Q',bytes(cpu.mem_read(a+0x1c,8)))[0]
    assert aqa==0x00010001 and asq and acq;c['sq'][0],c['cq'][0]=asq,acq;c['ready_at']=self.time+2000
   if (value>>14)&3==1 and not (c['cc']>>14)&3:c['shutdown_at']=self.time+500
   c['cc']=value
  elif off in (0x1000,0x1008):self.execute((off-0x1000)//8,value)
  elif off in (0x1004,0x100c):pass
  else:assert off in (0x24,0x28,0x2c,0x30,0x34) or 0x2000<=off<0x2080,hex(off)
 def code(self,cpu,a,size,unused):
  if a==0x50fff000:self.finished=True;cpu.emu_stop();return
  name=self.hooks.get(a)
  if not name:return
  args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(6)];E=self.image.enums;P=self.image.layouts['a733_pcie']
  if name=='a733_smp_counter':self.returned(self.time*24)
  elif name=='a733_counter_clock_now':cpu.mem_write(args[1],struct.pack('<Q',self.time));self.returned(1)
  elif name=='board_live':self.returned(self.live)
  elif name=='xTaskGetCurrentTaskHandle':self.returned(1)
  elif name=='xTaskGetSchedulerState':self.returned(self.scheduler)
  elif name=='xQueueGetMutexHolder':self.returned(1 if self.held else 0)
  elif name=='aarch64_device_translate':
   nc=POOL[0]<=args[0]<POOL[1];normal=self.variant=='unmapped-window' and BAR0<=args[0]<0x28000000
   cpu.mem_write(args[2],struct.pack('<Q2I',args[0],0x44 if nc else 0xff if normal else 0,3 if nc or normal else 2));self.returned(1)
  elif name=='a733_platform_lock_enter':assert not self.held;self.held=True;self.returned(1)
  elif name=='a733_platform_lock_leave':assert self.held;self.held=False;self.returned(0)
  elif name=='a733_platform_lock_retain':assert self.held;self.held=False;self.returned(1)
  elif name=='a733_pcie_poll':
   # Startup reached READY in the C host model whose registers seeded this run.
   assert args[0]==self.port
   for field,identity in (('clock',1),('rail',2)):cpu.mem_write(self.port+P[field],struct.pack('<2Q',identity,23))
   cpu.mem_write(self.port+P['phase'],struct.pack('<I',self.fields['phase']));cpu.mem_write(self.port+P['root_cap'],bytes([self.fields['root_cap']]))
   self.returned(E['A733_PCIE_READY'])
  elif name=='a733_pcie_stop':
   # The core stop is covered by the host model; require its preconditions here.
   assert args[0]==self.port and self.u32(self.port+P['client_stopped'])==1 and not self.u32(0x20000004)&7 and not self.u32(0x06300204)>>31
   self.stops+=1;cpu.mem_write(self.port+P['phase'],struct.pack('<I',19));self.returned(E['A733_PCIE_STOPPED'])
  elif name=='a733_clock_token_status':self.returned(E['A733_CLOCK_OK'])
  elif name=='a733_rail_status':self.returned(E['A733_RAIL_READY'])
  elif name=='a733_pmic_live':self.returned(E['A733_PMIC_READY'])
  elif name=='a733_gpu_resources_status':self.returned(E['A733_GPU_RESOURCE_OK'])
  elif name in ('a733_clock_retain','a733_rail_retain'):
   self.retain_calls[name]=self.retain_calls.get(name,0)+1;self.returned(E['A733_CLOCK_OK'] if name=='a733_clock_retain' else E['A733_RAIL_READY'])
  elif name in ('vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR'):self.returned(0)
  elif name=='vTaskGenericNotifyGiveFromISR':self.notifications+=1;self.returned(0)
  elif name=='ulTaskGenericNotifyTake':self.returned(self.notifications);self.notifications=0
  elif name=='vTaskDelay':self.finished=True;cpu.emu_stop()
  elif name in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available'):self.returned(1)
  elif name=='tud_vendor_n_read':cpu.mem_write(args[1],bytes(1));self.returned(1)
  elif name=='tud_vendor_n_write':
   # Bounded FIFO acceptance exercises the fragmented serializer.
   count=min(113,args[2]);self.vendor_bytes.extend(cpu.mem_read(args[1],count));self.returned(count)
  elif name=='tud_vendor_n_write_flush':self.returned(0)
 def call(self,name,*args,unit=None):
  self.finished=False
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
  try:self.cpu.emu_start(self.image.address(name,unit),0,count=5000000)
  except Exception as error:raise RuntimeError((self.variant,name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)),str(error))) from error
  assert self.finished,(self.variant,name);return self.cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
 def tick_usb(self):
  if not hasattr(self,'usb_context'):
   self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_X0,0);pc=self.image.address('board_usb0_task')
  else:self.cpu.context_restore(self.usb_context);pc=self.usb_pc
  self.finished=False;self.cpu.emu_start(pc,0,count=1000000);assert self.finished
  self.usb_pc=self.cpu.reg_read(UC_ARM64_REG_LR);self.usb_context=self.cpu.context_save()
 def native_write(self,address,width,value):return self.call('write',self.native,address,width,value,unit='/drivers/pci/a733_pcie_native.c')
 def run(self,limit=400,step_us=1000):
  assert self.call('pcie_session_valid',0x41f00000)==1 and self.call('pcie_construct')==1
  self.bind();self.scheduler=2
  for _ in range(limit):
   if self.variant=='irq-unconfigured' and self.field('stage')==2:self.msi_status|=2
   self.deliver();result=self.call('pcie_step');self.time+=step_us
   if result in (1,0xffffffff):return result
  raise AssertionError((self.variant,'unbounded application stage',self.field('stage')))

def nominal(m,out,session,decoder):
 E=m.image.enums
 assert m.call('pcie_session_valid',0x41f00000)==1 and m.call('pcie_construct')==1;m.bind();m.scheduler=2
 refusals={}
 for _ in range(400):
  m.deliver();result=m.call('pcie_step');m.time+=1000
  if m.field('stage')==2 and not refusals:
   # At READY with the read queued: the native filter keeps the exact rules.
   m.held=True
   command=m.u32(0x20000004)&0xffff;assert command==0x406,hex(command)
   assert m.native_write(0x20000004,2,command)==1 # Positive control: the current exact command value.
   refusals=dict(cc_without_session=m.native_write(BAR0+0x14,4,0x00460000)==0,
    io_doorbell_without_session=m.native_write(BAR0+0x1008,4,1)==0,
    bar_reassignment=m.native_write(0x20000010,4,0x22100004)==0,
    command_master_io=m.native_write(0x20000004,2,7)==0,
    region_one_retarget=m.native_write(0x06300214,4,0x41000000)==0,
    root_window=m.native_write(0x06000020,4,0x22102200)==0)
   m.held=False
  if result in (1,0xffffffff):break
 assert result==1 and m.field('result')==1,(result,m.field('stage'),m.field('result'))
 assert all(refusals.values()) and refusals,refusals
 # Read only: the I/O queue saw exactly one READ; admin commands in order.
 assert m.ctl['commands']==[(0,6),(0,6),(0,9),(0,5),(0,1),(1,2),(0,0),(0,4)],m.ctl['commands']
 if m.msix:assert m.msi_retained() and not m.gic_enabled and not m.u32(0x06000828) and m.u32(0x20000090)>>16&0xc000==0x4000
 assert m.stops==1 and m.lease_state()==E['DEVICE_MEMORY_RETAINED'] and not m.u32(0x20000004)&7 and (m.ctl['csts']>>2)&3==2
 payload=bytes(m.cpu.mem_read(m.base+m.layout['payload'],4288));(out/'native-payload.bin').write_bytes(payload)
 assert payload[192:]==bytes(pattern(8*512+i) for i in range(4096))
 for _ in range(60):
  m.tick_usb()
  if len(m.vendor_bytes)==128+4288:break
 frame=bytes(m.vendor_bytes);assert len(frame)==128+4288 and frame[128:]==payload
 (out/'native-frame.bin').write_bytes(frame);decoded=decoder.decode(frame,session)
 assert decoded['serial']=='S4EWNX0R123456' and decoded['lba']==8 and decoded['bar0']==BAR0 and decoded['bar0_bytes']==0x4000
 return dict(passed=True,irqs=m.irqs,notifications=m.notifications,decoded=decoded,native_refusals=refusals,dma=m.dma,admin_and_io_commands=m.ctl['commands'],lease='RETAINED',port_stop_calls=m.stops)

def failure(m,variant):
 # Ten-millisecond steps let the one-second command bound expire.
 E=m.image.enums;result=m.run(step_us=10000)
 assert result==0xffffffff and m.field('result')==2 and m.field('retaining')==1,(variant,result,m.field('result'))
 calls=dict(m.retain_calls);m.call('pcie_step');m.call('pcie_step')
 assert m.stops==0 and all(n>=1 for n in m.retain_calls.values()) and len(m.retain_calls)==2,(variant,m.retain_calls)
 if m.msix:assert m.msi_retained() and not m.gic_enabled
 detail=dict(passed=True,commands=m.ctl['commands'],retention_calls=calls,bar_accesses=m.bar_accesses)
 # The queue memory is retained whether the driver or the application failed.
 assert m.lease_state()==E['DEVICE_MEMORY_RETAINED'] and m.nvme_field('failed')==(variant in ('timeout','shutdown-stuck','irq-unconfigured'))
 if variant=='unmapped-window':
  # Refused after BAR assignment and before any BAR access or bus mastering.
  assert not m.bar_accesses and not m.ctl['commands'] and not m.u32(0x20000004)&4
 if variant=='timeout':
  # No completion proof: bus mastering stays set and no stop or delete follows.
  assert m.ctl['commands'][-1]==(1,2) and m.u32(0x20000004)&4
 if variant=='shutdown-stuck':assert m.ctl['commands'][-2:]==[(0,0),(0,4)] and m.u32(0x20000004)&4
 if variant=='lba-outside':assert (1,2) not in m.ctl['commands'] and m.u32(0x20000004)&4
 return detail

def irq_lifecycle(m):
 # Execute the linked ISR, GIC adapter and callbacks. Only RTOS scheduling
 # primitives and register side effects are modeled.
 m.bind();m.scheduler=2;unit='/apps/pcie-nvme/irq.c';out=0x50001000
 assert m.call('arm',0,unit=unit)==1
 m.call('pcie_nvme_application_irq');m.call('pcie_nvme_application_irq')
 assert m.notifications==2
 # Notification arrived before the wait, and remains consumable.
 assert m.call('ulTaskGenericNotifyTake',0,1,1)==2
 m.cpu.mem_write(0x03400000,struct.pack('<I',1<<31))
 assert m.call('event',0,out,unit=unit)==0 and m.u32(out)==1
 for _ in range(3):
  m.call('pcie_nvme_application_irq')
  assert m.call('pcie_nvme_irq_health')==1
  assert m.call('event',0,out,unit=unit)==0 and m.u32(out)==0
 m.cpu.mem_write(0x03400000,struct.pack('<I',0))
 assert m.call('event',0,out,unit=unit)==1 and m.u32(out)==0
 # Fresh delivery immediately after rearm is a new consumable hint.
 m.call('pcie_nvme_application_irq')
 assert m.call('event',0,out,unit=unit)==1 and m.u32(out)==1
 m.call('stop',0,unit=unit);m.call('pcie_nvme_application_irq')
 assert m.call('pcie_nvme_irq_health')==0 and m.field('result')==2
 assert m.call('event',0,out,unit=unit)==0xffffffff
 assert m.call('arm',0,unit=unit)==0xffffffff
 return dict(passed=True,scope='Linked mask/rearm, RWP-deferred count consumption, repeat delivery, notification-before-wait and permanent CLOSED fault; scheduler modeled')

def early_irq(m):
 assert m.call('pcie_construct')==1;m.bind();m.scheduler=2
 # The timeout variant submits the real READ but withholds its CQ entry.
 for _ in range(400):
  m.deliver();assert m.call('pcie_step')==0;m.time+=1000
  if m.field('stage')==2 and m.nvme_field('pending')==1:break
 else:raise AssertionError('READ not submitted')
 assert m.nvme_field('pending')==1
 m.msi_status|=1;m.deliver()
 assert m.call('pcie_step')==0 and m.nvme_field('pending')==1 and m.field('result')==0
 # Actual phase-tagged completion after the early hint permits progress.
 m.post(1,m.nvme_field('command_id',2))
 for _ in range(100):
  m.deliver();r=m.call('pcie_step');m.time+=1000
  if r:break
 assert r==1 and m.stops==1
 # A closed source faults immediately and revokes the published result.
 m.call('pcie_nvme_application_irq')
 assert m.field('result')==2 and m.call('pcie_step')==0xffffffff
 m.call('pcie_step');assert len(m.retain_calls)==2 and m.msi_retained()
 return dict(passed=True,scope='IRQ before CQ cannot complete read; later CQ and shutdown succeed; CLOSED delivery revokes output')

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('image','session','output'):p.add_argument('--'+key,type=Path,required=True)
 p.add_argument('--case',action='append',choices=('nominal','unmapped-window','timeout','shutdown-stuck','lba-outside','repeat-irq','lost-irq','irq-lifecycle','irq-before-cq','irq-unconfigured'))
 a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=True)
 image=Image(a.image);session=a.session.read_bytes();words,fields=registers(out)
 decoder=module('pcie_nvme_native_decode',ROOT/'apps/pcie-nvme/observe.py')
 results={};msix='pcie_nvme_irq_bind' in image.symbols
 cases=a.case or ['nominal','unmapped-window','timeout','shutdown-stuck','lba-outside']+(['repeat-irq','lost-irq','irq-lifecycle','irq-before-cq','irq-unconfigured'] if msix else [])
 for variant in cases:
  s=bytearray(session)
  if variant=='lba-outside':struct.pack_into('<Q',s,800,0x100000-7)
  m=Machine(image,bytes(s),words,fields,'timeout' if variant=='irq-before-cq' else variant)
  if variant in ('nominal','repeat-irq','lost-irq'):
   target=out if variant=='nominal' else out/variant;target.mkdir(exist_ok=True)
   results[variant]=nominal(m,target,session,decoder)
   if variant=='lost-irq':assert m.irqs==0
   elif msix:assert m.irqs>0
  elif variant=='irq-lifecycle':results[variant]=irq_lifecycle(m)
  elif variant=='irq-before-cq':results[variant]=early_irq(m)
  else:results[variant]=failure(m,variant)
  (out/('case-'+variant+'.json')).write_text(json.dumps(results[variant],indent=2)+'\n')
  print(variant,'PASS',flush=True)
 record=dict(status='pass',image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),cases=results,physical_access=False,scope=__doc__)
 (out/'native-result.json').write_text(json.dumps(record,indent=2)+'\n')
 print(json.dumps({k:v['passed'] for k,v in results.items()}))
if __name__=='__main__':main()
