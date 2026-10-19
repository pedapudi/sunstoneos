#!/usr/bin/env python3
"""Execute linked client/application/native-access instructions, without startup.

Reuse nominal actual-provider register values from a733-pcie-epf.c. Initial
acquired manager metadata, MMIO responses, endpoint DMA/MSI, translations, RTOS
and USB FIFOs are modeled. Boot, route, dispatch and IRQ code are not executed.
"""
import argparse,importlib.util,json,struct,inspect,textwrap
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
old=module('pcie_test_native_base',ROOT/'checks/pcie-application-native.py')
# The selected image garbage-collects unused configuration-only getters. Remove
# those two unused hooks from the reused constructor before resolving symbols.
constructor=textwrap.dedent(inspect.getsource(old.Machine.__init__))
constructor=constructor.replace("'a733_pcie_identity','a733_pcie_config_read',",'')
namespace={};exec(compile(constructor,str(ROOT/'checks/pcie-application-native.py'),'exec'),old.__dict__,namespace)
old.Machine.__init__=namespace['__init__']
class Image(old.Image):
 def __init__(self,path):
  super().__init__(path);self.enums={}
  with path.open('rb') as f:
   for cu in ELFFile(f).get_dwarf_info().iter_CUs():
    for d in cu.iter_DIEs():
     if d.tag=='DW_TAG_enumerator':
      name=d.attributes['DW_AT_name'].value.decode();value=int(d.attributes['DW_AT_const_value'].value)
      if name.startswith('A733_'):self.enums[name]=value
     name=d.attributes.get('DW_AT_name')
     if d.tag!='DW_TAG_structure_type' or not name or name.value.decode() not in ('a733_pcie_epf_test','device_memory_extent','device_memory_pool'):continue
     fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name'in c.attributes and 'DW_AT_data_member_location'in c.attributes}
     if fields:self.layouts[name.value.decode()]=fields;self.sizes[name.value.decode()]=int(d.attributes['DW_AT_byte_size'].value)
class Machine(old.Machine):
 def __init__(self,image,session,registers):
  self.pending_w1c=False;self.scheduler=0;self.events=0;self.arm_calls=0;self.stop_calls=0;self.vendor=bytearray();self.command_seen=0
  super().__init__(image,session,'test-client')
  self.cpu.mem_map(0x02000000,0x06000000);self.cpu.mem_map(0x22000000,0x100000)
  for address,value in registers.items():self.cpu.mem_write(int(address,16),struct.pack('<I',value))
  self.test=self.base+self.layout['test'];self.tf=image.layouts['a733_pcie_epf_test']
  self.cpu.hook_add(UC_HOOK_MEM_READ,self.mmio_read,begin=0x20000010,end=0x20000013)
  self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.mmio_write,begin=0x06000830,end=0x06000833)
  self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.command,begin=0x22000004,end=0x22000007)
  self.pending_w1c=False
  self.cpu.mem_write(self.objects['locks']+image.layouts['a733_platform_locks']['retained'],bytes(4))
 def mmio_read(self,cpu,access,a,size,value,unused):
  if a==0x20000010 and bytes(cpu.mem_read(a,4))==b'\xff'*4:cpu.mem_write(a,struct.pack('<I',0xfffffe00))
 def mmio_write(self,cpu,access,a,size,value,unused):
  if a==0x06000830:self.pending_w1c=True
 def command(self,cpu,access,a,size,value,unused):
  if a==0x22000004 and value==32:self.command_seen+=1
 def code(self,cpu,address,size,unused):
  if self.pending_w1c:
   cpu.mem_write(0x06000830,bytes(4));self.pending_w1c=False
  arg=lambda n:cpu.reg_read(UC_ARM64_REG_X0+n)
  if address==self.image.address('xTaskGetSchedulerState'):self.returned(self.scheduler);return
  if address==self.image.address('aarch64_device_translate'):
   nc=0x41c00000<=arg(0)<0x41e00000;cpu.mem_write(arg(2),struct.pack('<Q2I',arg(0),0x44 if nc else 0,3 if nc else 2));self.returned(1);return
  if address==self.image.address('a733_pcie_poll'):
   super().code(cpu,address,size,unused)
   self.cpu.mem_write(self.port+self.image.layouts['a733_pcie']['phase'],struct.pack('<I',16))
   self.cpu.mem_write(self.port+self.image.layouts['a733_pcie']['root_cap'],bytes([0x70]));return
  for name,result in [('a733_clock_token_status',self.image.enums['A733_CLOCK_OK']),('a733_rail_status',self.image.enums['A733_RAIL_READY']),('a733_pmic_live',self.image.enums['A733_PMIC_READY']),('a733_gpu_resources_status',self.image.enums['A733_GPU_RESOURCE_OK'])]:
   if address==self.image.address(name):self.returned(result);return
  if address==self.image.address('pcie_test_irq_arm'):self.arm_calls+=1;self.returned(1);return
  if address==self.image.address('pcie_test_irq_event'):cpu.mem_write(arg(0),struct.pack('<I',self.events));self.returned(bool(self.events));return
  if address==self.image.address('pcie_test_irq_stop'):self.stop_calls+=1;self.returned(1);return
  if address==self.image.address('tud_vendor_n_write'):
   count=min(113,arg(2));self.vendor.extend(cpu.mem_read(arg(1),count));self.returned(count);return
  super().code(cpu,address,size,unused)
 def memory(self,*args):pass
 def extents(self):
  at=self.test+self.tf['memory'];return [struct.unpack('<3Q',bytes(self.cpu.mem_read(at+24*i,24))) for i in range(3)]
 def complete_dma(self):
  src,dst,msi=self.extents();self.cpu.mem_write(dst[0]+4096,bytes(self.cpu.mem_read(src[0]+4096,4096)))
  self.cpu.mem_write(0x22000004,bytes(4));self.cpu.mem_write(0x22000008,struct.pack('<I',0x50));self.cpu.mem_write(0x06000830,struct.pack('<I',1));self.events=1
 def tick_usb(self):
  if not hasattr(self,'usb_context'):
   self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_X0,0);pc=self.image.address('board_usb0_task')
  else:self.cpu.context_restore(self.usb_context);pc=self.usb_pc
  self.finished=False;self.cpu.emu_start(pc,0,count=1000000);assert self.finished
  self.usb_pc=self.cpu.reg_read(UC_ARM64_REG_LR);self.usb_context=self.cpu.context_save()
 def native_write(self,address,width,value):return self.call('write',self.native,address,width,value,unit='/drivers/pci/a733_pcie_native.c')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--session',type=Path,required=True);p.add_argument('--registers',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 registers={}
 for line in a.registers.read_text().splitlines():
  parts=line.split()
  if len(parts)==2:registers[parts[0]]=int(parts[1],16)
 image=Image(a.image);session=a.session.read_bytes();m=Machine(image,session,registers)
 assert m.call('pcie_session_valid',0x41f00000)==1
 assert m.call('pcie_construct')==1;m.scheduler=2
 assert m.call('admitted',m.base,m.port+image.layouts['a733_pcie']['config'],unit='/apps/pcie-test/native.c')==1
 for _ in range(20):
  m.call('pcie_step')
  if m.command_seen:break
 if not (m.command_seen==1 and m.arm_calls==1 and not m.field('failed')):
  diagnostic=dict(app_stage=m.field('stage'),app_failed=m.field('failed'),port_failed=int.from_bytes(m.cpu.mem_read(m.port+image.layouts['a733_pcie']['failed'],4),'little'),test_stage=int.from_bytes(m.cpu.mem_read(m.test+m.tf['stage'],4),'little'),last_value=hex(int.from_bytes(m.cpu.mem_read(m.port+image.layouts['a733_pcie']['last_value'],4),'little')),calls=m.calls,retains=m.retain_calls)
  (a.output/'native-setup-diagnostic.json').write_text(json.dumps(diagnostic,indent=2)+'\n')
 assert m.command_seen==1 and m.arm_calls==1 and not m.field('failed')
 # Execute the actual native write/value/mutex/owner chain, preserving no write on refusal.
 m.held=True
 assert m.native_write(0x06000828,4,UINT32_MAX:=0xffffffff)==0
 assert m.native_write(0x0600082c,4,0)==0
 assert m.native_write(0x06300214,4,0x41000000)==0
 assert m.native_write(0x20000018,4,0x22000000)==0
 assert m.native_write(0x22000004,4,32)==0 # one command already submitted
 m.held=False
 assert m.native_write(0x06000828,4,1)==0 # no register-mutex ownership
 m.complete_dma()
 for _ in range(5):
  m.call('pcie_step')
  if m.field('result')==1:break
 assert m.field('result')==1 and m.command_seen==1
 payload=bytes(m.cpu.mem_read(m.base+m.layout['payload'],4224));(a.output/'native-payload.bin').write_bytes(payload)
 # Source bindings, owner-task checked copy and continued healthy observation.
 before=len(m.calls);assert m.call('pcie_step')==1 and m.command_seen==1
 assert payload==bytes(m.cpu.mem_read(m.base+m.layout['payload'],4224))
 # Exercise the actual result serializer through bounded FIFO acceptance.
 for _ in range(50):
  m.tick_usb()
  if len(m.vendor)==4352:break
 assert len(m.vendor)==4352 and bytes(m.vendor[128:])==payload
 frame=bytes(m.vendor);(a.output/'native-frame.bin').write_bytes(frame)
 decode=module('pcie_test_native_decode',ROOT/'apps/pcie-test/observe.py');decoded=decode.decode(frame,session)
 m.time+=11000;assert m.call('pcie_output_ready')==0
 result=dict(actual_client=True,actual_native_value_mutex_policy=True,actual_checked_copy=True,actual_fragmented_serializer=True,continued_observation=True,stale_export_refused=True,decoded=decoded,boundaries=__doc__)
 (a.output/'native-result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k not in ('boundaries','decoded')}))
if __name__=='__main__':main()
