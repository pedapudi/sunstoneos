#!/usr/bin/env python3
"""Linked repeated thermal/ADC composition; controller/GIC/PMIC outcomes modeled."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_TPIDR_EL1
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class Image:
 def __init__(self,image):
  with image.open('rb') as f:
   e=ELFFile(f);self.segments=[(int(p['p_vaddr']),p.data()) for p in e.iter_segments() if p['p_type']=='PT_LOAD'];self.symbols={};self.functions={};self.layouts={};self.sizes={}
   for s in e.get_section_by_name('.symtab').iter_symbols():self.symbols.setdefault(s.name,set()).add(int(s['st_value']))
   for cu in e.get_dwarf_info().iter_CUs():
    unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
    for d in cu.iter_DIEs():
     name=d.attributes.get('DW_AT_name')
     if not name and 'DW_AT_abstract_origin' in d.attributes:name=d.get_DIE_from_attribute('DW_AT_abstract_origin').attributes.get('DW_AT_name')
     if not name:continue
     name=name.value.decode()
     if d.tag=='DW_TAG_subprogram' and 'DW_AT_low_pc'in d.attributes:self.functions.setdefault((unit,name),set()).add(int(d.attributes['DW_AT_low_pc'].value))
     if d.tag=='DW_TAG_structure_type' and name in ('gicv3_spi','monitor_application','monitor_owner','monitor_snapshot','board_application','board_provider','a733_native_usb','a733_clock_leases','a733_clock_token','a733_adc_native','a733_adc_ops','a733_adc_config','a733_adc_sample','a733_adc','a733_thermal','a733_thermal_config','a733_thermal_sample','a733_thermal_calibration','a733_pmic_snapshot','a733_platform_locks','a733_pmic','a733_pmic_native','a733_pmic_ops','a733_rail_leases','a733_rail_binding','a733_pins','a733_gpu_resource_manager'):
      fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name'in c.attributes and 'DW_AT_data_member_location'in c.attributes}
      if fields:
       if name in self.layouts:assert self.layouts[name]==fields,name
       self.layouts[name]=fields;self.sizes[name]=int(d.attributes['DW_AT_byte_size'].value)
 def address(self,name,unit=None):
  values=self.symbols.get(name,set())-{0}
  if unit:values=set().union(*(v for (u,n),v in self.functions.items() if n==name and u.endswith(unit)))
  assert len(values)==1,(name,unit,values);return next(iter(values))
class Machine:
 def __init__(self,image,session,nominal,variant):
  self.image=image;self.session=session;self.nominal=nominal;self.variant=variant;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  for a,n in [(0x02000000,0x02000000),(0x07010000,0x100000),(0x40000000,0x04000000),(0x50000000,0x1000000)]:self.cpu.mem_map(a,n)
  self.cpu.reg_write(UC_ARM64_REG_PSTATE,5);self.cpu.reg_write(UC_ARM64_REG_TPIDR_EL1,0)
  for a,data in image.segments:self.cpu.mem_write(a,data)
  self.base=image.address('monitor');self.layout=image.layouts['monitor_application'];self.board=image.address('board');self.provider=image.address('providers')
  self.owners=[self.base+self.layout['owners']+i*image.sizes['monitor_owner'] for i in range(2)]
  self.shared=self.provider+image.layouts['board_provider']['shared']
  self.objects={n:self.shared+image.layouts['a733_native_usb'][n] for n in ('locks','clocks','rails','pins','pmic','counter')}
  self.time=1000000;self.task=1;self.live=True;self.held=0;self.critical=0;self.finished=False;self.calls=[];self.parent_writes=[];self.samples=[0,0];self.stops=[0,0];self.rearms=[0,0];self.pending_inject=False;self.interrupt_context=None;self.transition=None;self.state_reads=0;self.tx=[];self.packet=b'';self.frame_revoke=False;self.output_checks=0
  self.cpu.mem_write(self.base,session);self.cpu.mem_write(0x41f00000,session);self.cpu.mem_write(self.board,session[:592]);self.cpu.mem_write(self.board+160,struct.pack('<Q',61000000))
  for i,o in enumerate(self.owners):self.set(o,'monitor_owner','handle',i+1,8);self.set(o,'monitor_owner','kind',i)
  self.set(self.objects['locks'],'a733_platform_locks','initialized',1);self.set(self.objects['locks'],'a733_platform_locks','now',image.address('a733_counter_clock_now'),8);self.set(self.objects['locks'],'a733_platform_locks','context',self.objects['counter'],8);self.set(self.objects['locks'],'a733_platform_locks','mutex',1,8)
  self.cpu.mem_write(self.shared+image.layouts['a733_native_usb']['pmic_ready'],struct.pack('<I',1))
  for a,v in [(0x07090160,0x10000002),(0x02002000,0xf8636300),(0x02002518,0),(0x02002fc0,0)]:self.cpu.mem_write(a,struct.pack('<I',v))
  names=['a733_smp_counter','a733_counter_clock_now','board_live','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState','xQueueGetMutexHolder','aarch64_device_translate','a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','a733_adc_poll','a733_adc_read','a733_thermal_poll','a733_thermal_read','a733_adc_stop','a733_thermal_stop','a733_rail_acquire','a733_rail_poll','a733_rail_status','a733_rail_retain','a733_pmic_identity','a733_pmic_live','a733_pmic_submit','a733_pmic_result','a733_cpu_coherence_publish','gicv3_spi_mask','gicv3_spi_rearm','vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush']
  self.hooks={image.address(n):n for n in names}
  image.address('current','/drivers/adc/a733_adc_native.c')
  self.cpu.hook_add(UC_HOOK_CODE,self.code);self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.write_memory);self.cpu.hook_add(UC_HOOK_MEM_READ,self.read_memory)
  assert self.call('a733_clock_leases_init',self.objects['clocks'],image.address('a733_counter_clock_now'),self.objects['counter'])==1
 def set(self,obj,kind,field,value,n=4):self.cpu.mem_write(obj+self.image.layouts[kind][field],value.to_bytes(n,'little'))
 def field(self,field,value=None,n=4):
  a=self.base+self.layout[field]
  if value is not None:self.cpu.mem_write(a,value.to_bytes(n,'little'))
  return int.from_bytes(self.cpu.mem_read(a,n),'little')
 def state(self,value=None):return self.field('state',value)
 def returned(self,n):self.cpu.reg_write(UC_ARM64_REG_X0,n&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 def write_memory(self,cpu,access,a,size,value,unused):
  if a==0x02002fc0:self.parent_writes.append(value)
 def read_memory(self,cpu,access,a,size,value,unused):
  if a==self.base+self.layout['state'] and self.transition in ('start','halt') and self.critical:
   self.state_reads+=1
   if (self.transition=='start' and self.state_reads==1) or (self.transition=='halt' and self.state_reads==4):self.state(4);self.transition=None
 def code(self,cpu,a,size,unused):
  if a==0x50fff000:self.finished=True;cpu.emu_stop();return
  if a==0x50fff100:
   assert self.interrupt_context is not None
   context,target=self.interrupt_context;cpu.context_restore(context);cpu.reg_write(UC_ARM64_REG_PC,target);self.interrupt_context=None;return
  name=self.hooks.get(a)
  if name in ('a733_adc_read','a733_thermal_read') and self.pending_inject:
   self.pending_inject=False;self.interrupt_context=(cpu.context_save(),a);cpu.reg_write(UC_ARM64_REG_LR,0x50fff100)
   if self.variant=='stop-during-read':cpu.reg_write(UC_ARM64_REG_PC,self.image.address('monitor_request_stop'))
   else:cpu.reg_write(UC_ARM64_REG_X0,0 if name=='a733_adc_read' else 1);cpu.reg_write(UC_ARM64_REG_PC,self.image.address('monitor_application_irq'))
   return
  if a==self.image.address('monitor_output_eligible') and self.frame_revoke:
   self.output_checks+=1
   if self.output_checks==1:self.state(4)
  if not name:return
  args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='a733_smp_counter':
   if self.transition=='stop' and self.critical:self.state(4);self.transition=None
   self.returned(self.time*24)
  elif name=='a733_counter_clock_now':cpu.mem_write(args[1],struct.pack('<Q',self.time));self.returned(1)
  elif name=='board_live':self.returned(self.live)
  elif name=='xTaskGetCurrentTaskHandle':self.returned(self.task)
  elif name=='xTaskGetSchedulerState':self.returned(2)
  elif name=='xQueueGetMutexHolder':self.returned(self.held)
  elif name=='aarch64_device_translate':cpu.mem_write(args[2],struct.pack('<Q2I',args[0],0,2));self.returned(1)
  elif name=='a733_platform_lock_enter':assert not self.held;self.held=self.task;self.returned(1)
  elif name=='a733_platform_lock_leave':assert self.held==self.task;self.held=0;self.returned(0)
  elif name=='a733_platform_lock_retain':assert self.held==self.task;self.held=0;self.set(self.objects['locks'],'a733_platform_locks','retained',1);self.returned(1)
  elif name=='a733_cpu_coherence_publish':assert args[1]==840 and bytes(cpu.mem_read(args[0],832))==self.session;self.returned(1)
  elif name.startswith('vTask') and name!='vTaskDelay':
   self.critical+=1 if 'Enter' in name else -1;assert self.critical in (0,1);self.returned(0)
  elif name.startswith('gicv3_spi_'):
   assert self.critical;kind=int.from_bytes(cpu.mem_read(args[0]+self.image.layouts['gicv3_spi']['intid'],4),'little')-70;assert kind in (0,1)
   self.calls.append((name,kind))
   if name.endswith('rearm'):self.rearms[kind]+=1
   self.returned(0 if self.variant=='rearm-busy' and name.endswith('rearm') else 1)
  elif name.startswith('a733_rail_'):
   self.calls.append((name,self.task))
   if name=='a733_rail_acquire':assert args[1]==23 and args[2]==1<<self.nominal['rail_index'];cpu.mem_write(args[5],struct.pack('<4Q',2,23,args[2],0));self.returned(1)
   else:self.returned(5 if name=='a733_rail_retain' else 0)
  elif name.startswith('a733_pmic_'):
   self.calls.append((name,self.task));lifetime=struct.pack('<3Q',self.objects['pmic'],struct.unpack_from('<Q',self.session,456)[0],struct.unpack_from('<Q',self.session,32)[0])
   if name=='a733_pmic_identity':cpu.mem_write(args[1],lifetime);self.returned(1)
   elif name=='a733_pmic_live':self.returned(5 if self.variant=='supply-failure' and self.samples[0]>=3 else 1)
   elif name=='a733_pmic_submit':cpu.mem_write(args[5],lifetime+struct.pack('<2Q',1,23));self.returned(0)
   elif name=='a733_pmic_result':
    raw=bytearray(self.image.sizes['a733_pmic_snapshot']);fields=self.image.layouts['a733_pmic_snapshot']
    for f,v,n in [('rail',self.nominal['rail_index'],4),('chip',3,1),('enable',32,1),('selector',13,1),('enabled',1,4),('decoded',1,4),('programmed_uv',1800000,4)]:raw[fields[f]:fields[f]+n]=v.to_bytes(n,'little')
    cpu.mem_write(args[2],bytes(raw));self.returned(2)
  elif name in ('a733_adc_poll','a733_thermal_poll'):
   kind=int('thermal' in name);self.calls.append((name,kind));assert self.field('parent_ready')==1 and len(self.parent_writes)==2
   if kind:
    cal=self.base+self.layout['thermal']+self.image.layouts['a733_thermal']['calibration'];cpu.mem_write(cal,struct.pack('<11H',self.nominal['factory_tenth_c'],*self.nominal['factory_codes'],*self.nominal['corrections']))
   if self.variant=='failure-at-start-transition' and kind and self.field('active_mask')==1:self.transition='start';self.state_reads=0
   self.returned(2)
  elif name in ('a733_adc_read','a733_thermal_read'):
   kind=int('thermal' in name);self.calls.append((name,kind));self.samples[kind]+=1
   driver='a733_thermal' if kind else 'a733_adc'
   if self.variant=='busy-fault' and kind==0:self.set(args[0],driver,'fault',1);self.returned(1);return
   sample='a733_thermal_sample' if kind else 'a733_adc_sample';fields=self.image.layouts[sample];raw=bytearray(self.image.sizes[sample])
   if kind:
    struct.pack_into('<Q',raw,fields['observed_us'],self.time);raw[fields['valid']]=31;raw[fields['alarm']]=1
    for i in range(5):struct.pack_into('<H',raw,fields['code']+2*i,2400+i);struct.pack_into('<i',raw,fields['millicelsius']+4*i,(2400+i-2822)*-62)
   else:
    struct.pack_into('<I',raw,fields['valid'],15)
    for i in range(4):struct.pack_into('<Q',raw,fields['observed_us']+8*i,self.time);struct.pack_into('<H',raw,fields['code']+2*i,i*1024)
   struct.pack_into('<I',raw,fields['sequence'],self.samples[kind]);cpu.mem_write(args[2],bytes(raw));self.returned(4)
  elif name in ('a733_adc_stop','a733_thermal_stop'):
   kind=int('thermal' in name);self.calls.append((name,kind));self.stops[kind]+=1
   if self.variant in ('stop-busy','stop-deadline') and kind and (self.stops[kind]==1 or self.variant=='stop-deadline'):self.returned(1)
   elif self.variant=='stop-fault' and kind:self.set(args[0],'a733_thermal','fault',1);self.returned(1)
   else:self.returned(7)
  elif name=='vTaskDelay':self.delay_return=cpu.reg_read(UC_ARM64_REG_LR);self.finished=True;cpu.emu_stop()
  elif name in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext'):self.returned(1)
  elif name=='tud_vendor_n_available':self.returned(len(self.packet))
  elif name=='tud_vendor_n_read':n=min(args[2],len(self.packet));cpu.mem_write(args[1],self.packet[:n]);self.packet=self.packet[n:];self.returned(n)
  elif name=='tud_vendor_n_write':self.tx.append(bytes(cpu.mem_read(args[1],args[2])));self.returned(args[2])
  elif name=='tud_vendor_n_write_flush':self.returned(0)
 def call(self,name,*args,unit=None):
  self.finished=False
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
  try:self.cpu.emu_start(self.image.address(name,unit),0,count=1000000)
  except Exception as e:raise RuntimeError((self.variant,name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)),str(e))) from e
  assert self.finished,(self.variant,name);return self.cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
 def usb_cycle(self):
  if not hasattr(self,'delay_return'):return self.call('board_usb0_task',0,unit='/apps/monitor/usb0.c')
  self.finished=False;self.cpu.reg_write(UC_ARM64_REG_PC,self.delay_return)
  self.cpu.emu_start(self.delay_return,0,count=1000000);assert self.finished
 def step(self,kind):self.task=kind+1;self.time+=30000;return self.call('monitor_step',kind)
 def start(self):
  assert self.call('monitor_construct')==1
  for _ in range(80):
   self.step(0);self.step(1)
   if self.state()==4:return
   if min(self.samples)>=4:return
  raise AssertionError('Monitor did not start repeated acquisition')
 def snapshot(self):
  assert self.call('monitor_read_snapshot',0x50002000,0)==2
  return bytes(self.cpu.mem_read(0x50002000,192))
def run(image,session_path,nominal_path,out):
 out.mkdir(parents=True,exist_ok=True);elf=Image(image);session=session_path.read_bytes();nominal=json.loads(nominal_path.read_text())
 for name in ('read_register','write_register'):elf.address(name,'/apps/monitor/native.c')
 elf.address('current','/drivers/adc/a733_adc_native.c');elf.address('board_usb0_task','/apps/monitor/usb0.c')
 decoder=module('monitor_native_observe',ROOT/'apps/monitor/observe.py');results=json.loads((out/'progress.json').read_text()).get('cases',{}) if (out/'progress.json').exists() else {}
 variants=('unsealed-consumer','repeated-coexistence-stop','stop-busy','stop-fault','stop-deadline','stop-during-read','pending-during-read','busy-fault','supply-failure','shared-clock-failure','failure-at-stop-transition','failure-at-start-transition','failure-at-halt-transition','wrapper-owner-parent-refusal','rearm-busy','snapshot-revokes','usb-copied-frame','usb-snapshot-revokes','usb-queued-revokes','event-generation-immutable')
 for variant in variants:
  if results.get(variant,{}).get('passed'):continue
  m=Machine(elf,session,nominal,variant)
  if variant=='unsealed-consumer':assert m.call('board_application_pins',0x50002000)==0 and m.call('board_application_start')==0 and not m.calls
  elif variant=='failure-at-start-transition':
   assert m.call('monitor_construct')==1
   for _ in range(80):
    m.step(0)
    m.step(1)
    if m.state()==4:break
   assert m.state()==4 and not m.transition
  else:
   m.start()
   if variant in ('busy-fault','supply-failure'):assert m.state()==4
   elif variant=='rearm-busy':
    for _ in range(110):m.step(0);m.step(1)
    assert m.state()==4
   else:
    assert m.state()==1 and min(m.samples)>=4
    before=m.snapshot();g,_,ts,ads=struct.unpack_from('<4Q',before)
    assert ts>=3 and ads>=3 and len(m.parent_writes)==2
    if variant=='shared-clock-failure':
     calls=len(m.calls);m.set(m.objects['clocks'],'a733_clock_leases','failed',1);m.step(0);m.step(1);assert m.state()==4
     assert not any(x[0].endswith(('_read','_poll')) for x in m.calls[calls:])
    elif variant in ('pending-during-read','stop-during-read'):
     m.pending_inject=True;m.step(0)
     if variant=='pending-during-read':
      o=m.owners[0];assert int.from_bytes(m.cpu.mem_read(o+elf.layouts['monitor_owner']['pending'],4),'little')==1
      rearm=m.rearms[0];m.step(0);assert m.rearms[0]==rearm+1
     else:
      assert m.state()==2
      assert int.from_bytes(m.cpu.mem_read(m.base+m.layout['latest'],8),'little')==g
      m.step(0);assert m.state()==2;m.step(1);assert m.state()==3
    elif variant=='event-generation-immutable':
     sample='a733_thermal_sample';fields=elf.layouts[sample];raw=bytearray(elf.sizes[sample]);m.samples[1]+=1
     struct.pack_into('<Q',raw,fields['observed_us'],m.time);struct.pack_into('<I',raw,fields['sequence'],m.samples[1]);raw[fields['alarm']]=2
     m.cpu.mem_write(0x50003000,bytes(raw));assert m.call('monitor_collect_thermal',0x50003000)==1
     assert m.call('monitor_read_snapshot',0x50002000,g)==1 and m.snapshot()==before
     m.step(1);after=m.snapshot();assert struct.unpack_from('<Q',after)[0]>g and after[184]==3
    elif variant=='wrapper-owner-parent-refusal':
     o=m.owners[0];m.set(o,'monitor_owner','in_call',1);m.held=1;m.task=2
     assert m.call('read_register',o,0x02521080,0x50002000,unit='/apps/monitor/native.c')==0
     m.task=1;assert m.call('write_register',o,0x02002fc0,0x81000000,unit='/apps/monitor/native.c')==0;m.held=0
    elif variant in ('usb-copied-frame','usb-snapshot-revokes','usb-queued-revokes'):
     m.packet=bytes(9);m.frame_revoke=variant=='usb-snapshot-revokes'
     m.usb_cycle()
     if variant=='usb-queued-revokes':m.state(4)
     m.usb_cycle()
     if variant=='usb-queued-revokes':assert not m.tx
     else:
      assert len(m.tx)==1;result=decoder.decode(m.tx[0],session)
      assert result['status']==(5 if variant=='usb-snapshot-revokes' else 2)
      if variant=='usb-copied-frame':(out/'native-frame.bin').write_bytes(m.tx[0])
    elif variant=='snapshot-revokes':
     m.state(4);m.cpu.mem_write(0x50002000,bytes([0xa5])*192)
     assert m.call('monitor_read_snapshot',0x50002000,0)==5 and bytes(m.cpu.mem_read(0x50002000,192))==bytes([0xa5])*192
    elif variant=='failure-at-stop-transition':
     m.transition='stop';assert m.call('monitor_request_stop')==5 and m.state()==4 and not m.transition
    else:
     if variant=='repeated-coexistence-stop':
      m.step(0);m.step(1);after=m.snapshot();assert struct.unpack_from('<Q',after)[0]>g
      frame=struct.pack('<4I2Q',0x314e4f4d,1,2,192,*struct.unpack_from('<2Q',session,24))+hashlib.sha256(session).digest()+hashlib.sha256(after).digest()+bytes(32)+after
      decoder.decode(frame,session);(out/'native-frame.bin').write_bytes(frame)
     assert m.call('monitor_request_stop')==3 and m.state()==2
     m.step(0);assert m.state()==2
     if variant=='failure-at-halt-transition':m.transition='halt';m.state_reads=0
     m.step(1)
     if variant=='stop-busy':assert m.state()==2;m.step(1);assert m.state()==3
     elif variant=='stop-fault':assert m.state()==4
     elif variant=='stop-deadline':m.time+=1000001;m.step(0);assert m.state()==4
     elif variant=='failure-at-halt-transition':assert m.state()==4 and not m.transition
     else:assert m.state()==3
     if m.state()==3:
      calls=len(m.calls);m.step(0);m.step(1);assert len(m.calls)==calls and m.call('monitor_request_stop')==4
  results[variant]=dict(passed=True,samples=m.samples,stop_calls=m.stops,parent_writes=m.parent_writes)
  (out/'progress.json').write_text(json.dumps(dict(cases=results),indent=2)+'\n')
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=results,physical_access=False,scope='Actual linked service/copy/seal/native owner filters, parent MMIO and clock registry/publication; core acquisition/stop returns, PMIC/rail/GIC/RTOS/time/USB outcomes modeled. Actual core stop tested separately; no physical quiescence/drain qualification.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser();
 for n in ('image','session','nominal','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.image,a.session,a.nominal,a.output)
