#!/usr/bin/env python3
"""Linked SPI application boundaries; controller returns/shared manager state modeled."""
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
     if d.tag=='DW_TAG_structure_type' and name in ('spi_application','board_application','board_provider','a733_native_usb','a733_clock_leases','a733_clock_token','a733_spi_platform','a733_spi_platform_native','a733_spi_platform_ops','a733_spi_platform_config','a733_spi','a733_pmic_snapshot','a733_platform_locks','a733_pmic','a733_pmic_native','a733_pmic_ops','a733_rail_leases','a733_rail_binding','a733_pins','a733_gpu_resource_manager','a733_counter_clock','spi_dma_application','a733_dma_startup','a733_dma_platform','a733_dma_controller','a733_dma_channel','a733_dma_transfer','a733_dma_mapping','device_memory_pool','device_memory_allocation','a733_dma_native'):
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
  for base,n in [(0x02543000,0x1000),(0x40000000,0x04000000),(0x50000000,0x01000000)]:self.cpu.mem_map(base,n)
  self.cpu.reg_write(UC_ARM64_REG_PSTATE,5);self.cpu.reg_write(UC_ARM64_REG_TPIDR_EL1,0)
  for a,data in image.segments:self.cpu.mem_write(a,data)
  self.base=image.address('spi');self.layout=image.layouts['spi_application'];self.board=image.address('board');self.provider=image.address('providers');self.time=1000000;self.live=True;self.held=False;self.finished=False;self.calls=[];self.tx=[];self.retain_calls={};self.revoke_read=False;self.reads=0;self.snapshot=False;self.output_checks=0;self.construct_calls=0;self.polls=0;self.reaped=False
  self.cpu.mem_write(0x41f00000,session);self.cpu.mem_write(self.base,session);self.cpu.mem_write(self.board,session[:592]);self.cpu.mem_write(self.board+160,struct.pack('<Q',61000000));self.cpu.mem_write(self.board+168,struct.pack('<Q',24000000));self.field('handle',1,8)
  shared=self.provider+image.layouts['board_provider']['shared'];self.shared=shared;self.objects={name:shared+image.layouts['a733_native_usb'][name] for name in ('locks','clocks','pins','rails','pmic','counter')}
  def setv(obj,kind,field,value,size=8):self.cpu.mem_write(obj+image.layouts[kind][field],value.to_bytes(size,'little'))
  setv(self.objects['locks'],'a733_platform_locks','initialized',1,4);setv(self.objects['locks'],'a733_platform_locks','now',image.address('a733_counter_clock_now'));setv(self.objects['locks'],'a733_platform_locks','context',self.objects['counter']);setv(self.objects['locks'],'a733_platform_locks','mutex',1)
  setv(self.objects['clocks'],'a733_clock_leases','initialized',1,4);setv(self.objects['pins'],'a733_pins','initialized',1,4)
  self.cpu.mem_write(shared+image.layouts['a733_native_usb']['pmic_ready'],struct.pack('<I',1))
  self.platform=self.base+self.layout['platform'];self.native=self.base+self.layout['native'];self.hooks={}
  for name in ['a733_smp_counter','a733_counter_clock_now','board_live','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState','xQueueGetMutexHolder','aarch64_device_translate','a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','a733_spi_platform_poll','a733_spi_platform_submit','a733_spi_platform_reap','a733_clock_retain','a733_rail_acquire','a733_rail_poll','a733_rail_status','a733_rail_retain','a733_pmic_identity','a733_pmic_live','a733_pmic_submit','a733_pmic_result','a733_cpu_coherence_publish','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush']:
   self.hooks[image.address(name)]=name
  self.cpu.hook_add(UC_HOOK_CODE,self.code);self.cpu.hook_add(UC_HOOK_MEM_READ,self.memory,begin=0x02543000,end=0x02543fff)
 def clock_token(self,address):
  raw=bytearray(self.image.sizes['a733_clock_token']);fields=self.image.layouts['a733_clock_token']
  for field,value,size in [('id',1,8),('owner',23,8),('shared',self.nominal['parent_scope'],16),('exclusive',self.nominal['spi_scope'],16)]:raw[fields[field]:fields[field]+size]=value.to_bytes(size,'little')
  self.cpu.mem_write(address,bytes(raw))
 def field(self,name,value=None,size=4):
  if value is not None:self.cpu.mem_write(self.base+self.layout[name],value.to_bytes(size,'little'))
  return int.from_bytes(self.cpu.mem_read(self.base+self.layout[name],size),'little')
 def returned(self,n):self.cpu.reg_write(UC_ARM64_REG_X0,n&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 def memory(self,cpu,access,a,size,value,unused):
  self.reads+=1
  if self.revoke_read:self.field('failed',1)
 def code(self,cpu,a,size,unused):
  if a==self.image.address('spi_construct'):self.construct_calls+=1
  if a==0x50fff000:self.finished=True;cpu.emu_stop();return
  if a==self.image.address('storage_digest_bytes') and self.variant=='publish-revokes' and cpu.reg_read(UC_ARM64_REG_X0)==self.base+self.layout['payload']:self.field('failed',1)
  if a==self.image.address('spi_output_ready') and self.snapshot:
   self.output_checks+=1
   if self.output_checks==2:self.field('failed',1)
  name=self.hooks.get(a)
  if not name:return
  args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(6)]
  if name=='a733_smp_counter':self.returned(self.time*24)
  elif name=='a733_counter_clock_now':cpu.mem_write(args[1],struct.pack('<Q',self.time));self.returned(1)
  elif name=='board_live':self.returned(self.live)
  elif name=='xTaskGetCurrentTaskHandle':self.returned(1)
  elif name=='xTaskGetSchedulerState':self.returned(2)
  elif name=='xQueueGetMutexHolder':self.returned(1 if self.held else 0)
  elif name=='aarch64_device_translate':cpu.mem_write(args[2],struct.pack('<Q2I',args[0],0,2));self.returned(1)
  elif name=='a733_platform_lock_enter':assert not self.held;self.held=True;self.returned(1)
  elif name=='a733_platform_lock_leave':assert self.held;self.held=False;self.returned(0)
  elif name=='a733_platform_lock_retain':assert self.held;self.held=False;self.returned(1)
  elif name=='a733_cpu_coherence_publish':assert args[1]==840 and bytes(cpu.mem_read(args[0],832))==self.session and bytes(cpu.mem_read(args[0]+832,8))==struct.pack('<2I',1,1);self.returned(1)
  elif name in ('a733_clock_retain','a733_rail_retain'):
   self.retain_calls[name]=self.retain_calls.get(name,0)+1;self.returned((1 if name=='a733_clock_retain' else 2) if self.retain_calls[name]==1 else (4 if name=='a733_clock_retain' else 5))
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
    for f,v,size in [('rail',self.nominal['rail_index'],4),('chip',3,1),('enable',64,1),('selector',13,1),('enabled',1,4),('decoded',1,4),('programmed_uv',3300000 if self.variant=='wrong-rail' else 1800000,4)]:raw[fields[f]:fields[f]+size]=v.to_bytes(size,'little')
    cpu.mem_write(args[2],bytes(raw));self.returned(2)
  elif name.startswith('a733_spi_platform_'):
   self.calls.append([name]);assert args[0]==self.platform
   if name=='a733_spi_platform_poll':
    self.polls+=1
    if self.polls==1:
     self.clock_token(self.platform+self.image.layouts['a733_spi_platform']['clock'])
     if self.variant=='busy-fault':cpu.mem_write(self.platform+self.image.layouts['a733_spi_platform']['fault'],struct.pack('<I',1));self.returned(1);return
     self.returned(2)
    else:
     assert self.polls==2
     if self.variant=='partial-failure':cpu.mem_write(self.platform+self.image.layouts['a733_spi_platform']['fault'],struct.pack('<I',1));self.returned(5);return
     rx=struct.unpack_from('<I',self.session,796)[0];cpu.mem_write(self.base+self.layout['rx'],bytes(range(1,rx+1)));self.returned(3)
   elif name=='a733_spi_platform_submit':
    assert args[1]==self.base+self.layout['transfer'];self.returned(0)
   elif name=='a733_spi_platform_reap':
    if self.variant=='reap-deadline':self.time=6000000;self.returned(1)
    else:self.reaped=True;self.returned(2)
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
 out.mkdir(parents=True,exist_ok=False);elf=Image(image);session=session_path.read_bytes();decoder=module('spi_native_decoder',ROOT/'apps/spi/observe.py');results={};nominal=json.loads(nominal_path.read_text());assert nominal['format_valid']
 for name in ('read_register','write_register','read_byte','write_byte'):elf.address(name,'/apps/spi/native.c')
 elf.address('current','/drivers/spi/a733_spi_platform_native.c')
 setup=Machine(elf,session,'setup',nominal);assert setup.call('spi_construct')==1
 assert setup.call('current',setup.native,unit='/drivers/spi/a733_spi_platform_native.c')==1
 # The setup binary emits the actual C header scopes; no guessed bit indices.
 platform_clock=setup.platform+elf.layouts['a733_spi_platform']['clock']
 setup.clock_token(platform_clock)
 assert setup.call('admitted',setup.base,setup.platform+elf.layouts['a733_spi_platform']['config'],platform_clock,0,unit='/apps/spi/native.c')==1
 (out/'nominal-provider-preflight.json').write_text(json.dumps(dict(actual_platform_init=True,actual_native_current=True,physical_access=False),indent=2)+'\n')
 for variant in ('unsealed-consumer','prefix-mismatch-consumer','success','wrapper-delegation','cancel-before-work','wrong-rail','busy-fault','partial-failure','reap-deadline','publish-revokes','output-cutoff','snapshot-revokes','queued-revokes'):
  m=Machine(elf,session,variant,nominal)
  if variant in ('unsealed-consumer','prefix-mismatch-consumer'):
   if variant=='prefix-mismatch-consumer':
    prep=module('spi_binding_prepare',ROOT/'apps/spi/prepare.py');parts,entry,identity=prep.base.image_parts(image);epoch,generation,lease=struct.unpack_from('<3Q',session,24);_,contract=prep.request_bytes(entry,identity,session,epoch,generation,lease);contract=bytearray(contract[:224]);struct.pack_into('<Q',contract,0,prep.CONTRACT_MAGIC);struct.pack_into('<4Q',contract,80,100,24000100,1440000100,1440000100);m.cpu.mem_write(0x50001000,bytes(contract));assert m.call('spi_session_prepare',0x50001000)==1
    prefix=bytearray(session[:592]);prefix[456]^=1;m.cpu.mem_write(m.board+elf.layouts['board_application']['admitted_session'],bytes(prefix))
   assert m.call('board_application_pins',0x50002000)==0 and m.call('board_application_start')==0 and not m.calls and m.construct_calls==0
   results[variant]=dict(passed=True,component_calls=0)
  else:
   assert m.call('spi_construct')==1
   if variant=='wrapper-delegation':
    m.held=True;m.cpu.mem_write(0x02543004,struct.pack('<I',0x83));m.cpu.mem_write(0x02543300,bytes([0xa5]));args=(m.base,0x02543004,0x50002000)
    assert m.call('read_register',*args,unit='/apps/spi/native.c')==1 and bytes(m.cpu.mem_read(0x50002000,4))==struct.pack('<I',0x83)
    assert m.call('read_register',m.base,0x02543300,0x50002000,unit='/apps/spi/native.c')==0
    assert m.call('read_byte',m.base,0x02543300,0x50002000,unit='/apps/spi/native.c')==1 and bytes(m.cpu.mem_read(0x50002000,1))==bytes([0xa5])
    assert m.call('write_byte',m.base,0x02543200,0x5a,unit='/apps/spi/native.c')==1 and bytes(m.cpu.mem_read(0x02543200,1))==bytes([0x5a])
    assert m.call('write_register',m.base,0x02544004,0,unit='/apps/spi/native.c')==0
    m.revoke_read=True;assert m.call('read_register',*args,unit='/apps/spi/native.c')==0;before=m.reads;assert m.call('read_register',*args,unit='/apps/spi/native.c')==0 and m.reads==before
    results[variant]=dict(passed=True,actual_native_reads=m.reads,wrong_width_port_refused=True)
   else:
    if variant=='cancel-before-work':m.live=False
    for i in range(32):
     result=m.call('spi_step')
     if result in (1,0xffffffff):break
    else:raise AssertionError('Unbounded SPI application stage')
    failure=variant in ('cancel-before-work','wrong-rail','busy-fault','partial-failure','reap-deadline','publish-revokes')
    assert result==(0xffffffff if failure else 1) and m.field('result')==(2 if failure else 1),(variant,result,m.field('result'))
    if failure:
     before=len(m.calls);m.call('spi_step');m.call('spi_step');m.call('spi_step');assert len(m.calls)==before and all(n==2 for n in m.retain_calls.values())
    else:
     assert m.reaped
     if variant=='success':
      before=len(m.calls);m.time=6000001;assert m.call('spi_step')==1 and len(m.calls)==before and m.call('spi_output_ready')==1
     if variant=='output-cutoff':m.time=60990000;assert m.call('spi_output_ready')==0
     m.snapshot=variant=='snapshot-revokes';m.call('board_usb0_task');count=128 if variant in ('output-cutoff','snapshot-revokes') else 224
     frame=bytes(m.cpu.mem_read(elf.address('spi_response'),count));decoded=decoder.decode(frame,session);assert decoded['status']==(2 if count==128 else 1)
     if variant=='success':(out/'native-frame.bin').write_bytes(frame)
     if variant=='queued-revokes':m.field('failed',1);m.call('board_usb0_task');assert not m.tx
    results[variant]=dict(passed=True,component_calls=len(m.calls),retention_calls=m.retain_calls,status=m.field('result'))
  (out/'progress.json').write_text(json.dumps(dict(cases=results,physical_access=False),indent=2)+'\n')
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=results,physical_access=False,scope='Actual linked application/seal/pure platform init/native callbacks/copy instructions. Platform/controller/PMIC/rail returns, shared initial state, translations, time, RTOS and USB transport modeled; existing controller/platform/provider evidence reused.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('image','session','output','nominal'):p.add_argument('--'+key,type=Path,required=True)
 a=p.parse_args();run(a.image,a.session,a.output,a.nominal)
