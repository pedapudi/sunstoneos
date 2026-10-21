#!/usr/bin/env python3
"""Linked thermal composition with modeled clocks/MMIO, RTOS and USB boundaries."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class Image:
 def __init__(self,image):
  with image.open('rb') as f:
   e=ELFFile(f);self.segments=[(int(p['p_vaddr']),p.data()) for p in e.iter_segments() if p['p_type']=='PT_LOAD'];self.symbols={};self.functions={};self.layouts={}
   for s in e.get_section_by_name('.symtab').iter_symbols():self.symbols.setdefault(s.name,set()).add(int(s['st_value']))
   for cu in e.get_dwarf_info().iter_CUs():
    unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
    for d in cu.iter_DIEs():
     name=d.attributes.get('DW_AT_name')
     if not name:continue
     name=name.value.decode()
     if d.tag=='DW_TAG_subprogram' and 'DW_AT_low_pc'in d.attributes:self.functions.setdefault((unit,name),set()).add(int(d.attributes['DW_AT_low_pc'].value))
     if d.tag=='DW_TAG_structure_type' and name in ('thermal_application','board_application','board_provider','a733_native_usb','a733_clock_leases'):
      fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name'in c.attributes and 'DW_AT_data_member_location'in c.attributes}
      if fields:
       if name in self.layouts:assert self.layouts[name]==fields,name
       self.layouts[name]=fields
 def address(self,name,unit=None):
  values=self.symbols.get(name,set())-{0}
  if unit:values=set().union(*(v for (u,n),v in self.functions.items() if n==name and u.endswith(unit)))
  assert len(values)==1,(name,unit,values);return next(iter(values))
class Machine:
 def __init__(self,image,session,variant):
  self.image=image;self.variant=variant;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.mem_map(0x02000000,0x06000000);self.cpu.mem_map(0x40000000,0x04000000);self.cpu.mem_map(0x50000000,0x01000000)
  for a,data in image.segments:self.cpu.mem_write(a,data)
  self.base=image.address('thermal');self.layout=image.layouts['thermal_application'];self.board=image.address('board');self.provider=image.address('providers');self.time=1000000;self.live=True;self.held=False;self.retained=False;self.finished=False;self.trace=[];self.regs={};self.tx=[];self.read_irq=False;self.critical=0;self.inject_irq=False;self.interrupt_context=None;self.usb_snapshot=False;self.output_checks=0
  self.session=session;self.cpu.mem_write(0x41f00000,session);self.cpu.mem_write(self.base,session);self.cpu.mem_write(self.board,session[:592]);self.cpu.mem_write(self.board+160,struct.pack('<Q',61000000));self.write_field('handle',1,8)
  self.registry=self.provider+image.layouts['board_provider']['shared']+image.layouts['a733_native_usb']['clocks'];rl=image.layouts['a733_clock_leases']
  for field,value in [('next_id',1),('initialized',1),('now',image.address('now','/apps/thermal/native.c')),('context',self.base)]:self.cpu.mem_write(self.registry+rl[field],value.to_bytes(4 if field=='initialized' else 8,'little'))
  self.regs.update({0x07090160:0x10000002,0x02002000:0xf8000000|(99<<8)|(99<<16),0x02002518:1,0x02002fc0:0x40,0x02002fe4:0x20})
  codes=[2420,2400,2500,1769,1770];bits=250
  for i,code in enumerate(codes):bits|=code<<(12+12*i)
  for i in range(3):self.regs[0x03006244+4*i]=(bits>>(32*i))&0xffffffff
  self.hooks={}
  for name in ['a733_smp_counter','a733_counter_clock_now','xTaskGetCurrentTaskHandle','aarch64_device_translate','a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','board_live','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','a733_cpu_coherence_publish']:
   if name in image.symbols:self.hooks[image.address(name)]=name
  for name in ['vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR']:
   if name in image.symbols:self.hooks[image.address(name)]=name
  self.cpu.hook_add(UC_HOOK_CODE,self.code);self.cpu.hook_add(UC_HOOK_MEM_READ,self.mmio_read,begin=0x02000000,end=0x07ffffff);self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.mmio_write,begin=0x02000000,end=0x07ffffff)
 def write_field(self,name,value,size=4):self.cpu.mem_write(self.base+self.layout[name],value.to_bytes(size,'little'))
 def field(self,name,size=4):return int.from_bytes(self.cpu.mem_read(self.base+self.layout[name],size),'little')
 def returned(self,value):self.cpu.reg_write(UC_ARM64_REG_X0,value&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 def code(self,cpu,a,size,unused):
  if a==0x50fff100:
   assert self.interrupt_context is not None;cpu.context_restore(self.interrupt_context);self.interrupt_context=None;cpu.reg_write(UC_ARM64_REG_PC,self.image.address('a733_thermal_native_read'));return
  if a==self.image.address('a733_thermal_native_read') and self.inject_irq:
   self.inject_irq=False;self.interrupt_context=cpu.context_save();cpu.reg_write(UC_ARM64_REG_LR,0x50fff100);cpu.reg_write(UC_ARM64_REG_PC,self.image.address('thermal_application_irq'));return
  if a==self.image.address('thermal_output_ready') and self.usb_snapshot:
   self.output_checks+=1
   if self.output_checks==2:self.write_field('failed',1)
  if a==0x50fff000:self.finished=True;cpu.emu_stop();return
  name=self.hooks.get(a)
  if not name:return
  args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(4)]
  if name=='a733_smp_counter':self.returned(self.time*24)
  elif name=='a733_counter_clock_now':cpu.mem_write(args[1],struct.pack('<Q',self.time));self.returned(1)
  elif name=='a733_cpu_coherence_publish':
   assert args[1]==816 and bytes(cpu.mem_read(args[0],808))==self.session and bytes(cpu.mem_read(args[0]+808,8))==struct.pack('<2I',1,1);self.returned(1)
  elif name=='xTaskGetCurrentTaskHandle':self.returned(1)
  elif name=='board_live':self.returned(self.live)
  elif name=='aarch64_device_translate':cpu.mem_write(args[2],struct.pack('<Q2I',args[0],0,2));self.returned(1)
  elif name=='a733_platform_lock_enter':assert not self.held;self.held=True;self.returned(1)
  elif name=='a733_platform_lock_leave':assert self.held;self.held=False;self.returned(0)
  elif name=='a733_platform_lock_retain':assert self.held;self.held=False;self.retained=True;self.returned(1)
  elif name.startswith('vTask') and name!='vTaskDelay':
   self.critical+=1 if 'Enter'in name else -1;assert self.critical in (0,1);self.returned(0)
  elif name=='vTaskDelay':self.finished=True;cpu.emu_stop()
  elif name in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext'):self.returned(1)
  elif name=='tud_vendor_n_available':self.returned(1)
  elif name=='tud_vendor_n_read':cpu.mem_write(args[1],b'\0');self.returned(1)
  elif name=='tud_vendor_n_write':self.tx.append(bytes(cpu.mem_read(args[1],args[2])));self.returned(args[2])
  elif name=='tud_vendor_n_write_flush':self.returned(0)
 def mmio_read(self,cpu,access,a,size,value,unused):
  assert size==4,(hex(a),size);assert self.held or a>=0x03400000 and a<0x03500000,(hex(a),self.held)
  value=self.regs.get(a,0)
  if self.variant=='rearm-fault' and a==0x03400108:value=0
  cpu.mem_write(a,struct.pack('<I',value));self.trace.append(['read',a])
  if self.variant=='read-busy-fault' and self.read_irq and a==0x02522020:
   self.live=False;self.cpu.mem_write(self.registry+self.image.layouts['a733_clock_leases']['busy'],struct.pack('<I',1))
 def mmio_write(self,cpu,access,a,size,value,unused):
  assert size==4,(hex(a),size);assert self.held or a>=0x03400000 and a<0x03500000 and self.critical,(hex(a),self.held,self.critical);self.trace.append(['write',a,value])
  if 0x02522020<=a<=0x0252202c:self.regs[a]=self.regs.get(a,0)&~value
  elif a==0x03400188:self.regs[0x03400108]=self.regs.get(0x03400108,0)&~value
  else:self.regs[a]=value
  if self.variant in ('uncertain-start','startup-busy-fault') and a==0x02002fc0:
   self.live=False
   if self.variant=='startup-busy-fault':self.cpu.mem_write(self.registry+self.image.layouts['a733_clock_leases']['busy'],struct.pack('<I',1))
 def call(self,name,*args,unit=None):
  self.finished=False
  for i,x in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,x)
  self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000);self.cpu.emu_start(self.image.address(name,unit),0,count=1000000);assert self.finished,(self.variant,name);return self.cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
 def conversions(self,mask,delta=0):
  self.regs[0x02522020]=mask
  for i in range(5):self.regs[0x025220c0+4*i]=2400+11*i+delta

def run(image,session_path,out,resume=None):
 out.mkdir(parents=True,exist_ok=resume is not None)
 class Results(dict):
  def __setitem__(self,key,value):
   super().__setitem__(key,value);(out/'progress.json').write_text(json.dumps(dict(cases=self,physical_access=False),indent=2)+'\n')
 results=Results()
 if resume:
  for key,value in json.loads(resume.read_text())['cases'].items():results[key]=value
 session=session_path.read_bytes();elf=Image(image);decoder=module('thermal_native_decoder',ROOT/'apps/thermal/observe.py')
 # Resolve all case hooks/CUs before starting any behavior.
 for name in ('thermal_construct','thermal_step','thermal_application_irq','thermal_response','thermal_output_ready','board_usb0_task'):elf.address(name)
 for variant in ('unsealed-consumer','prefix-mismatch-consumer','success-partial-irq','cancel-before-work','uncertain-start','startup-busy-fault','read-busy-fault','pending-during-read','rearm-busy','rearm-fault','deadline-revokes','snapshot-revokes','queued-revokes','critical-stops'):
  if variant in results:continue
  m=Machine(elf,session,variant)
  if variant in ('unsealed-consumer','prefix-mismatch-consumer'):
   if variant=='prefix-mismatch-consumer':
    prep=module('thermal_binding_prepare',ROOT/'apps/thermal/prepare.py');_,entry,identity=prep.base.image_parts(image);epoch,generation,lease=struct.unpack_from('<3Q',session,24)
    _,contract=prep.request_bytes(entry,identity,session,epoch,generation,lease);contract=bytearray(contract[:224]);struct.pack_into('<Q',contract,0,prep.CONTRACT_MAGIC);struct.pack_into('<4Q',contract,80,100,24000100,1440000100,1440000100)
    m.cpu.mem_write(0x50001000,bytes(contract));assert m.call('thermal_session_prepare',0x50001000)==1
    prefix=bytearray(session[:592]);prefix[456]^=1;m.cpu.mem_write(m.board+elf.layouts['board_application']['admitted_session'],bytes(prefix))
   assert m.call('board_application_pins',0x50002000)==0 and m.call('board_application_start')==0 and not m.trace
   results[variant]={'pass':True,'provider_accesses':0};continue
  assert m.call('thermal_construct')==1
  if variant=='cancel-before-work':
   m.live=False;assert m.call('thermal_step')==0xffffffff and not m.trace;results[variant]={'pass':True};continue
  assert m.call('thermal_step')==0
  if variant=='rearm-busy':m.regs[0x03400000]=1<<31
  r=m.call('thermal_step')
  if variant in ('uncertain-start','startup-busy-fault'):
   assert r==0xffffffff and m.retained;before=len(m.trace);m.call('thermal_step');m.call('thermal_step');assert len(m.trace)==before and m.field('result')==2;results[variant]={'pass':True,'retained_common_mutex':True};continue
  if variant=='rearm-fault':
   assert r==0xffffffff and m.field('result')==2;before=len(m.trace);m.call('thermal_step');assert len(m.trace)==before;results[variant]={'pass':True};continue
  if variant=='rearm-busy':
   for _ in range(99):r=m.call('thermal_step')
   assert r==0xffffffff and m.field('result')==2 and m.field('masked')==1
   assert not any(x[0]=='write' and x[1]==0x03400108 for x in m.trace);results[variant]={'pass':True};continue
  assert r==0 and m.field('active')==1
  if variant=='critical-stops':
   # Channel 0 reads code 970, 110.035 C: the policy's critical trip ends the observation.
   m.time+=25000;m.conversions(31,-1430);assert m.call('thermal_step')==0xffffffff and m.field('result')==2 and m.field('masked')==1 and not m.held
   before=len(m.trace);m.call('thermal_step');assert len(m.trace)==before and not m.retained;results[variant]={'pass':True};continue
  if variant=='read-busy-fault':
   m.time+=25000;m.read_irq=True;m.conversions(31);assert m.call('thermal_step')==0xffffffff and m.field('result')==2 and m.field('masked')==1
   before=len(m.trace);m.call('thermal_step');assert len(m.trace)==before;results[variant]={'pass':True};continue
  for interval in range(2):
   m.time+=25000;m.conversions(3,interval);m.regs[0x0252202c]=1
   m.call('thermal_application_irq')
   assert m.field('pending')==1 and not m.held
   before=len(m.trace)
   if variant=='pending-during-read' and interval==0:m.inject_irq=True
   assert m.call('thermal_step')==0;assert m.field('sets')==interval
   if variant=='pending-during-read' and interval==0:
    assert m.field('pending')==1 and m.field('masked')==1 and not any(x[0]=='write' and x[1]==0x03400108 for x in m.trace[before:])
   m.time+=25000;m.conversions(28,interval);m.regs[0x02522028]=2;m.regs[0x02522024]=4
   assert m.call('thermal_step')==(1 if interval==1 else 0)
  assert m.field('result')==1 and m.field('masked')==1 and not m.held
  assert m.call('thermal_output_ready')==1
  if variant=='deadline-revokes':m.time=60990000;assert m.call('thermal_output_ready')==0
  m.usb_snapshot=variant=='snapshot-revokes'
  m.call('board_usb0_task');frame=bytes(m.cpu.mem_read(elf.address('thermal_response'),128 if variant in ('deadline-revokes','snapshot-revokes') else 352));decoded=decoder.decode(frame,session)
  assert decoded['status']==(2 if variant in ('deadline-revokes','snapshot-revokes') else 1)
  if variant=='success-partial-irq':(out/'native-frame.bin').write_bytes(frame)
  if variant=='queued-revokes':
   m.write_field('failed',1);m.call('board_usb0_task');assert not m.tx and int.from_bytes(m.cpu.mem_read(m.board+elf.layouts['board_application']['fatal'],4),'little')==1
  results[variant]=dict(pass_case=True,mmio_accesses=len(m.trace),sets=m.field('sets'))
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=results,physical_access=False,scope='Actual linked application, thermal core/native callbacks, clock registry, copied frame; MMIO values, RTOS synchronization/time and USB boundaries modeled.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('image','session','output'):p.add_argument('--'+key,type=Path,required=True)
 p.add_argument('--resume',type=Path)
 a=p.parse_args();run(a.image,a.session,a.output,a.resume)
