#!/usr/bin/env python3
"""Linked ADC application boundaries; controller returns/shared manager state modeled."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ
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
     if d.tag=='DW_TAG_structure_type' and name in ('adc_application','board_application','board_provider','a733_native_usb','a733_clock_leases','a733_clock_token','a733_adc_native','a733_adc_thresholds','a733_adc_ops','a733_adc_config','a733_adc_sample','a733_adc','a733_pmic_snapshot','a733_platform_locks','a733_pmic','a733_pmic_native','a733_pmic_ops','a733_rail_leases','a733_rail_binding','a733_pins','a733_gpu_resource_manager'):
      fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name'in c.attributes and 'DW_AT_data_member_location'in c.attributes}
      if fields:
       if name in self.layouts:assert self.layouts[name]==fields,name
       self.layouts[name]=fields;self.sizes[name]=int(d.attributes['DW_AT_byte_size'].value)
 def address(self,name,unit=None):
  values=self.symbols.get(name,set())-{0}
  if unit:values=set().union(*(v for (u,n),v in self.functions.items() if n==name and u.endswith(unit)))
  assert len(values)==1,(name,unit,values);return next(iter(values))
class Machine:
 def __init__(self,image,session,variant,nominal):
  self.image=image;self.session=session;self.variant=variant;self.nominal=nominal;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  for base,n in [(0x02521000,0x1000),(0x40000000,0x04000000),(0x50000000,0x01000000)]:self.cpu.mem_map(base,n)
  self.cpu.reg_write(UC_ARM64_REG_PSTATE,5);self.cpu.reg_write(UC_ARM64_REG_TPIDR_EL1,0)
  for a,data in image.segments:self.cpu.mem_write(a,data)
  self.base=image.address('adc');self.layout=image.layouts['adc_application'];self.board=image.address('board');self.provider=image.address('providers');self.time=1000000;self.live=True;self.held=False;self.finished=False;self.calls=[];self.tx=[];self.retain_calls={};self.revoke_read=False;self.reads=0;self.snapshot=False;self.output_checks=0;self.construct_calls=0;self.samples=0;self.rearms=0;self.critical=0;self.inject_irq=False;self.interrupt_context=None;self.task=1;self.override_el=None;self.modeled_level_checks=0;self.level_instruction=image.address('allowed','/drivers/adc/a733_adc_native.c')
  # Resolve the architectural instruction within the defining function; its
  # admission prologue can change when native state changes.
  instructions=[self.level_instruction+i for i in range(0,128,4) if int.from_bytes(self.cpu.mem_read(self.level_instruction+i,4),'little')&0xffffffe0==0xd5384240]
  assert len(instructions)==1;self.level_instruction=instructions[0]
  self.cpu.mem_write(0x41f00000,session);self.cpu.mem_write(self.base,session);self.cpu.mem_write(self.board,session[:592]);self.cpu.mem_write(self.board+160,struct.pack('<Q',61000000));self.cpu.mem_write(self.board+168,struct.pack('<Q',24000000));self.field('handle',1,8)
  shared=self.provider+image.layouts['board_provider']['shared'];self.shared=shared;self.objects={name:shared+image.layouts['a733_native_usb'][name] for name in ('locks','clocks','pins','rails','pmic','counter')}
  def setv(obj,kind,field,value,size=8):self.cpu.mem_write(obj+image.layouts[kind][field],value.to_bytes(size,'little'))
  setv(self.objects['locks'],'a733_platform_locks','initialized',1,4);setv(self.objects['locks'],'a733_platform_locks','now',image.address('a733_counter_clock_now'));setv(self.objects['locks'],'a733_platform_locks','context',self.objects['counter']);setv(self.objects['locks'],'a733_platform_locks','mutex',1)
  setv(self.objects['clocks'],'a733_clock_leases','initialized',1,4);setv(self.objects['pins'],'a733_pins','initialized',1,4)
  self.cpu.mem_write(shared+image.layouts['a733_native_usb']['pmic_ready'],struct.pack('<I',1))
  self.driver=self.base+self.layout['driver'];self.native=self.base+self.layout['native'];self.hooks={}
  for name in ['a733_smp_counter','a733_counter_clock_now','board_live','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState','xQueueGetMutexHolder','aarch64_device_translate','a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','a733_adc_poll','a733_adc_read','a733_clock_acquire','a733_clock_retain','a733_rail_acquire','a733_rail_poll','a733_rail_status','a733_rail_retain','a733_pmic_identity','a733_pmic_live','a733_pmic_submit','a733_pmic_result','a733_cpu_coherence_publish','gicv3_spi_mask','gicv3_spi_rearm','vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush']:
   self.hooks[image.address(name)]=name
  self.cpu.hook_add(UC_HOOK_CODE,self.code);self.cpu.hook_add(UC_HOOK_MEM_READ,self.memory,begin=0x02521000,end=0x02521fff)
 def clock_token(self,address):
  raw=bytearray(self.image.sizes['a733_clock_token']);fields=self.image.layouts['a733_clock_token']
  for field,value,size in [('id',1,8),('owner',23,8),('shared',int(self.nominal['shared_hex'],16),16),('exclusive',int(self.nominal['exclusive_hex'],16),16)]:raw[fields[field]:fields[field]+size]=value.to_bytes(size,'little')
  self.cpu.mem_write(address,bytes(raw))
 def field(self,name,value=None,size=4):
  if value is not None:self.cpu.mem_write(self.base+self.layout[name],value.to_bytes(size,'little'))
  return int.from_bytes(self.cpu.mem_read(self.base+self.layout[name],size),'little')
 def returned(self,n):self.cpu.reg_write(UC_ARM64_REG_X0,n&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 def memory(self,cpu,access,a,size,value,unused):
  self.reads+=1
  if self.revoke_read:self.field('failed',1)
 def code(self,cpu,a,size,unused):
  if a==self.level_instruction and self.override_el is not None:
   instruction=int.from_bytes(cpu.mem_read(a,4),'little');destination=instruction&31;assert destination<29
   cpu.reg_write(UC_ARM64_REG_X0+destination,self.override_el);cpu.reg_write(UC_ARM64_REG_PC,a+4);self.modeled_level_checks+=1;return
  if a==0x50fff100:
   assert self.interrupt_context is not None;cpu.context_restore(self.interrupt_context);self.interrupt_context=None;cpu.reg_write(UC_ARM64_REG_PC,self.image.address('a733_adc_read'));return
  if a==self.image.address('a733_adc_read') and self.inject_irq:
   self.inject_irq=False;self.interrupt_context=cpu.context_save();cpu.reg_write(UC_ARM64_REG_LR,0x50fff100);cpu.reg_write(UC_ARM64_REG_PC,self.image.address('adc_application_irq'));return
  if a==self.image.address('adc_construct'):self.construct_calls+=1
  if a==0x50fff000:self.finished=True;cpu.emu_stop();return
  if a==self.image.address('storage_digest_bytes') and self.variant=='publish-revokes' and cpu.reg_read(UC_ARM64_REG_X0)==self.base+self.layout['payload']:self.field('failed',1)
  if a==self.image.address('adc_output_ready') and self.snapshot:
   self.output_checks+=1
   if self.output_checks==2:self.field('failed',1)
  name=self.hooks.get(a)
  if not name:return
  args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='a733_smp_counter':self.returned(self.time*24)
  elif name=='a733_counter_clock_now':cpu.mem_write(args[1],struct.pack('<Q',self.time));self.returned(1)
  elif name=='board_live':self.returned(self.live)
  elif name=='xTaskGetCurrentTaskHandle':self.returned(self.task)
  elif name=='xTaskGetSchedulerState':self.returned(2)
  elif name=='xQueueGetMutexHolder':self.returned(1 if self.held else 0)
  elif name=='aarch64_device_translate':cpu.mem_write(args[2],struct.pack('<Q2I',args[0],0,2));self.returned(1)
  elif name=='a733_platform_lock_enter':assert not self.held;self.held=True;self.returned(1)
  elif name=='a733_platform_lock_leave':assert self.held;self.held=False;self.returned(0)
  elif name=='a733_platform_lock_retain':assert self.held;self.held=False;self.returned(1)
  elif name=='a733_cpu_coherence_publish':assert args[1]==792 and bytes(cpu.mem_read(args[0],784))==self.session and bytes(cpu.mem_read(args[0]+784,8))==struct.pack('<2I',1,1);self.returned(1)
  elif name.startswith('vTask') and name!='vTaskDelay':self.critical+=1 if 'Enter' in name else -1;assert self.critical in (0,1);self.returned(0)
  elif name=='gicv3_spi_mask':assert self.critical;self.calls.append([name]);self.returned(0 if self.variant=='mask-failure' else 1)
  elif name=='gicv3_spi_rearm':
   assert self.critical;self.rearms+=1;self.calls.append([name]);self.returned(0 if self.variant=='rearm-busy' else 1)
  elif name in ('a733_clock_retain','a733_rail_retain'):
   self.retain_calls[name]=self.retain_calls.get(name,0)+1;self.returned((1 if name=='a733_clock_retain' else 2) if self.retain_calls[name]==1 else (4 if name=='a733_clock_retain' else 5))
  elif name=='a733_clock_acquire':
   self.calls.append([name]);assert args[1]==23 and (args[2]|args[3]<<64)==int(self.nominal['shared_hex'],16) and (args[4]|args[5]<<64)==int(self.nominal['exclusive_hex'],16);self.clock_token(args[7]);self.returned(0)
  elif name.startswith('a733_rail_'):
   self.calls.append([name])
   if name=='a733_rail_acquire':assert args[1:4]==[23,1<<self.nominal['rail_index'],0];cpu.mem_write(args[5],struct.pack('<4Q',2,23,1<<self.nominal['rail_index'],0));self.returned(1)
   else:self.returned(0)
  elif name.startswith('a733_pmic_'):
   self.calls.append([name]);assert args[0]==self.objects['pmic']
   lifetime=struct.pack('<3Q',self.objects['pmic'],struct.unpack_from('<Q',self.session,456)[0],struct.unpack_from('<Q',self.session,32)[0])
   if name=='a733_pmic_identity':cpu.mem_write(args[1],lifetime);self.returned(1)
   elif name=='a733_pmic_live':assert bytes(cpu.mem_read(args[1],24))==lifetime;self.returned(1)
   elif name=='a733_pmic_submit':assert args[1]==23 and args[3]==self.nominal['rail_index'];cpu.mem_write(args[5],lifetime+struct.pack('<2Q',1,23));self.returned(0)
   elif name=='a733_pmic_result':
    n=self.image.sizes['a733_pmic_snapshot'];raw=bytearray(n);fields=self.image.layouts['a733_pmic_snapshot']
    for f,v,size in [('rail',self.nominal['rail_index'],4),('chip',3,1),('enable',32,1),('selector',13,1),('enabled',1,4),('decoded',1,4),('programmed_uv',3300000 if self.variant=='wrong-rail' else 1800000,4)]:raw[fields[f]:fields[f]+size]=v.to_bytes(size,'little')
    cpu.mem_write(args[2],bytes(raw));self.returned(2)
  elif name=='a733_adc_poll':
   self.calls.append([name]);assert args[0]==self.driver
   if self.variant=='busy-fault':cpu.mem_write(self.driver+self.image.layouts['a733_adc']['fault'],struct.pack('<I',1));self.returned(1)
   else:self.returned(2)
  elif name=='a733_adc_read':
   self.calls.append([name]);assert args[0]==self.driver;self.samples+=1
   if self.variant=='partial-failure':cpu.mem_write(self.driver+self.image.layouts['a733_adc']['fault'],struct.pack('<I',1));self.returned(5);return
   if self.variant=='rearm-busy':self.returned(3);return
   assert self.samples<=3;valid=[1,14,15][self.samples-1];raw=bytearray(self.image.sizes['a733_adc_sample']);fields=self.image.layouts['a733_adc_sample']
   for i in range(4):
    if valid&(1<<i):struct.pack_into('<Q',raw,fields['observed_us']+8*i,self.time+i);struct.pack_into('<H',raw,fields['code']+2*i,i*1024)
   struct.pack_into('<2I',raw,fields['sequence'],1 if self.variant=='duplicate-sequence' else self.samples,valid);cpu.mem_write(args[2],bytes(raw));self.returned(4)
  elif name=='vTaskDelay':self.finished=True;cpu.emu_stop()
  elif name in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext'):self.returned(1)
  elif name=='tud_vendor_n_available':self.returned(1)
  elif name=='tud_vendor_n_read':cpu.mem_write(args[1],bytes(1));self.returned(1)
  elif name=='tud_vendor_n_write':self.tx.append(bytes(cpu.mem_read(args[1],args[2])));self.returned(args[2])
  elif name=='tud_vendor_n_write_flush':self.returned(0)
 def call(self,name,*args,unit=None):
  self.finished=False
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
  try:self.cpu.emu_start(self.image.address(name,unit),0,count=1000000)
  except Exception as error:raise RuntimeError((self.variant,name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)),str(error))) from error
  assert self.finished,(self.variant,name);return self.cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff

def run(image,session_path,out,nominal_path):
 out.mkdir(parents=True,exist_ok=True);elf=Image(image);session=session_path.read_bytes();decoder=module('adc_native_decoder',ROOT/'apps/adc/observe.py');results=json.loads((out/'progress.json').read_text())['cases'] if (out/'progress.json').exists() else {};nominal=json.loads(nominal_path.read_text())
 for name in ('read_register','write_register'):elf.address(name,'/apps/adc/native.c')
 elf.address('current','/drivers/adc/a733_adc_native.c')
 if not (out/'nominal-provider-preflight.json').exists():
  setup=Machine(elf,session,'setup',nominal);assert setup.call('adc_construct')==1
  assert setup.call('current',setup.native,unit='/drivers/adc/a733_adc_native.c')==1
  # Exercise actual high-bit pure core init and retained-token admission first.
  while setup.field('stage')<7:assert setup.call('adc_step')==0
  assert setup.call('admitted',setup.base,setup.driver+elf.layouts['a733_adc']['config'],setup.driver+elf.layouts['a733_adc']['token'],unit='/apps/adc/native.c')==1
  (out/'nominal-provider-preflight.json').write_text(json.dumps(dict(actual_core_init=True,actual_native_current=True,actual_native_admission=True,physical_access=False),indent=2)+'\n')
 variants=('unsealed-consumer','prefix-mismatch-consumer','success','wrapper-delegation','cancel-before-work','wrong-rail','busy-fault','partial-failure','duplicate-sequence','rearm-busy','pending-during-read','mask-failure','publish-revokes','output-cutoff','snapshot-revokes','queued-revokes')
 for variant in variants:
  if results.get(variant,{}).get('passed'):continue
  m=Machine(elf,session,variant,nominal)
  if variant in ('unsealed-consumer','prefix-mismatch-consumer'):
   if variant=='prefix-mismatch-consumer':
    prep=module('adc_binding_prepare',ROOT/'apps/adc/prepare.py');parts,entry,identity=prep.base.image_parts(image);epoch,generation,lease=struct.unpack_from('<3Q',session,24);_,contract=prep.request_bytes(entry,identity,session,epoch,generation,lease);contract=bytearray(contract[:224]);struct.pack_into('<Q',contract,0,prep.CONTRACT_MAGIC);struct.pack_into('<4Q',contract,80,100,24000100,1440000100,1440000100);m.cpu.mem_write(0x50001000,bytes(contract));assert m.call('adc_session_prepare',0x50001000)==1
    prefix=bytearray(session[:592]);prefix[456]^=1;m.cpu.mem_write(m.board+elf.layouts['board_application']['admitted_session'],bytes(prefix))
   assert m.call('board_application_pins',0x50002000)==0 and m.call('board_application_start')==0 and not m.calls and m.construct_calls==0
   results[variant]=dict(passed=True,component_calls=0)
  else:
   if variant=='wrapper-delegation':m.cpu.mem_write(m.native+elf.layouts['a733_adc_native']['thresholds'],bytes([255])*elf.sizes['a733_adc_thresholds'])
   assert m.call('adc_construct')==1
   if variant=='wrapper-delegation':
    assert bytes(m.cpu.mem_read(m.native+elf.layouts['a733_adc_native']['thresholds'],elf.sizes['a733_adc_thresholds']))==bytes(elf.sizes['a733_adc_thresholds'])
    m.held=True;m.cpu.mem_write(0x02521080,bytes(4));args=(m.base,0x02521080,0x50002000)
    assert m.call('read_register',*args,unit='/apps/adc/native.c')==1
    assert m.call('read_register',m.base,0x02521090,0x50002000,unit='/apps/adc/native.c')==0
    assert m.call('write_register',m.base,0x025210fc,0,unit='/apps/adc/native.c')==0
    for address,value in ((0x02521040,0),(0x02521020,1),(0x02521024,1),(0x02521030,1),(0x02521034,1)):
     assert m.call('write_register',m.base,address,value,unit='/apps/adc/native.c')==0
    m.task=2;assert m.call('read_register',*args,unit='/apps/adc/native.c')==0;m.task=1
    before=m.reads;m.override_el=8;assert m.call('read_register',*args,unit='/apps/adc/native.c')==0 and m.reads==before and m.modeled_level_checks==1;m.override_el=None
    m.revoke_read=True;assert m.call('read_register',*args,unit='/apps/adc/native.c')==0;before=m.reads;assert m.call('read_register',*args,unit='/apps/adc/native.c')==0 and m.reads==before
    results[variant]=dict(passed=True,actual_native_reads=m.reads,wrong_channel_reference_task_refused=True,modeled_exception_level_guard=True)
   else:
    if variant=='cancel-before-work':m.live=False
    for i in range(120):
     m.time+=25000
     if variant=='pending-during-read' and m.field('stage')==8 and not m.samples:m.inject_irq=True
     if variant=='mask-failure' and m.field('stage')==8 and not m.samples:m.field('masked',0);m.call('adc_application_irq')
     before=m.rearms;result=m.call('adc_step')
     if variant=='pending-during-read' and m.samples==1:assert m.field('pending')==1 and m.field('masked')==1 and m.rearms==before
     if result in (1,0xffffffff):break
    else:raise AssertionError('Unbounded ADC application stage')
    failure=variant in ('cancel-before-work','wrong-rail','busy-fault','partial-failure','duplicate-sequence','rearm-busy','mask-failure','publish-revokes')
    assert result==(0xffffffff if failure else 1) and m.field('result')==(2 if failure else 1),(variant,result,m.field('result'))
    if failure:
     before=len(m.calls);m.call('adc_step');m.call('adc_step');m.call('adc_step');assert len(m.calls)==before and all(n==2 for n in m.retain_calls.values())
     if variant=='rearm-busy':assert m.rearms==100
    else:
     assert m.field('sets')==2 and m.samples==3 and m.field('masked')==1
     if variant=='output-cutoff':m.time=60990000;assert m.call('adc_output_ready')==0
     m.snapshot=variant=='snapshot-revokes';m.call('board_usb0_task');count=128 if variant in ('output-cutoff','snapshot-revokes') else 288
     frame=bytes(m.cpu.mem_read(elf.address('adc_response'),count));decoded=decoder.decode(frame,session);assert decoded['status']==(2 if count==128 else 1)
     if variant=='success':(out/'native-frame.bin').write_bytes(frame)
     if variant=='queued-revokes':m.field('failed',1);m.call('board_usb0_task');assert not m.tx
    results[variant]=dict(passed=True,component_calls=len(m.calls),retention_calls=m.retain_calls,status=m.field('result'))
  (out/'progress.json').write_text(json.dumps(dict(cases=results,physical_access=False),indent=2)+'\n')
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=results,physical_access=False,scope='Actual linked application/seal/pure core init/native callbacks/ISR/copy instructions. Core/PMIC/rail/clock returns, GIC primitive outcomes, shared initial state, translations/time/RTOS/USB transport and wrong-EL architectural register modeled; new core validated separately.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('image','session','output','nominal'):p.add_argument('--'+key,type=Path,required=True)
 a=p.parse_args();run(a.image,a.session,a.output,a.nominal)
