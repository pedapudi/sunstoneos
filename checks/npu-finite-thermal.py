#!/usr/bin/env python3
"""Linked finite THS providers, freshness claims, bootstrap and IRQ admission.

Registers, translations, mutexes and scheduling are modeled. Counter/leases,
thermal native adapter, calibration/start/read/disable, fixed policy and gates
execute from the ELF. Bootstrap uses modeled NPU startup/activation/stop calls;
it does not establish engine operation or physical thermal qualification.
"""
import argparse,hashlib,importlib.util,json,struct,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'checks'));import harness
LAYOUTS=('npu_finite_thermal','a733_counter_clock','a733_clock_leases','a733_thermal','a733_thermal_policy',
 'a733_npu_startup','a733_npu_startup_config','a733_npu_reserved','npu_convolution','npu_service','a733_power_domain_row')
class Machine:
 def __init__(self,image,fields):
  self.e=image;self.f=fields;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(0x02000000,0x07000000);self.u.mem_map(0x40000000,0x04000000)
  self.u.reg_write(UC_ARM64_REG_PSTATE,0x3c5)
  for at,data in image.parts:
   if data:self.u.mem_write(at,data)
  self.app=image.address('app',unit='apps/npu-convolution/native.c');self.thermal=self.app+fields['thermal'];self.tl=image.layouts['npu_finite_thermal']
  self.counter=self.app+fields['clock'];self.registry=self.app+fields['registry'];self.task=0x43001000
  self.time=1000000;self.done=False;self.held=False;self.retained=False;self.trace=[];self.now_calls=0;self.jump=None;self.bad_disable=False
  self.startups=0;self.activations=0;self.stops=0;self.delays=0;self.mode=None;self.events=[]
  self.regs={0x07090160:0x10000002,0x02002000:0xf8000000|(99<<8)|(99<<16),0x02002518:1,0x02002fc0:0x40,0x02002fe4:0x20}
  bits=250
  for i,code in enumerate((2420,2400,2500,1769,1770)):bits|=code<<(12+12*i)
  for i in range(3):self.regs[0x03006244+4*i]=(bits>>(32*i))&0xffffffff
  self.startup=self.app+fields['startup'];self.sl=image.layouts['a733_npu_startup']
  self.conv=self.app+fields['reserved']+image.layouts['a733_npu_reserved']['convolution'];self.cl=image.layouts['npu_convolution']
  names=('xTaskGetCurrentTaskHandle','aarch64_device_translate','a733_platform_lock_enter','a733_platform_lock_leave',
   'a733_platform_lock_retain','a733_counter_clock_now','npu_convolution_startup_poll','npu_service_activate',
   'npu_convolution_stop_poll','vTaskDelay','vTaskSuspend','npu_finite_base_monitor_init','a733_smp_stop')
  self.hooks={image.address(n):n for n in names};self.hooks[image.address('parent',unit='apps/npu-convolution/native.c')]='parent'
  self.special={}
  for at,data in image.parts:
   for off in range(0,len(data)-3,4):
    word=int.from_bytes(data[off:off+4],'little')
    if word&0xffffffe0 in (0xd53be020,0xd53be000):self.special[at+off]=word
  for at in set(self.hooks)|set(self.special)|{0x43fff000}:self.u.hook_add(UC_HOOK_CODE,self.code,begin=at,end=at)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read,begin=0x02000000,end=0x08ffffff)
  self.u.hook_add(UC_HOOK_MEM_WRITE,self.write,begin=0x02000000,end=0x08ffffff)
  assert self.call('a733_counter_clock_init',self.counter,24000000)==1
  assert self.call('a733_clock_leases_init',self.registry,image.address('a733_counter_clock_now'),self.counter)==1
  self.put(image.address('npu_finite_thermal_irq_admitted'),1)
  boot=0x43e00000;self.u.mem_write(boot,struct.pack('<2Q',9,10)+bytes(128)+struct.pack('<2IB7x',1,12000000,1))
  assert self.call('npu_finite_thermal_init',self.thermal,boot,self.counter,self.registry,self.app+fields['locks'],self.task,20000000)==1
  self.put(self.app+fields['used'],1);self.put(self.app+fields['bootstrap_task'],self.task,8)
  self.put(self.startup+self.sl['config']+image.layouts['a733_npu_startup_config']['deadline_us'],20000000,8)
 def put(self,at,value,n=4):self.u.mem_write(at,int(value).to_bytes(n,'little'))
 def get(self,at,n=4):return int.from_bytes(self.u.mem_read(at,n),'little')
 def gate(self):return self.get(self.thermal+self.tl['gate'],8)
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def code(self,u,at,size,unused):
  if at==0x43fff000:self.done=True;u.emu_stop();return
  if at in self.special:
   word=self.special[at];u.reg_write(UC_ARM64_REG_X0+(word&31),self.time*24 if word&0xffffffe0==0xd53be020 else 24000000);u.reg_write(UC_ARM64_REG_PC,at+4);return
  name=self.hooks[at];r=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='a733_counter_clock_now':
   self.now_calls+=1
   if self.jump and self.now_calls==self.jump[0]:self.time=self.jump[1]
   return
  if name=='xTaskGetCurrentTaskHandle':self.ret(self.task)
  elif name=='aarch64_device_translate':u.mem_write(r[2],struct.pack('<QII',r[0],0,2));self.ret(1)
  elif name=='a733_platform_lock_enter':assert not self.held;self.held=True;self.ret(1)
  elif name=='a733_platform_lock_leave':assert self.held;self.held=False;self.ret()
  elif name=='a733_platform_lock_retain':self.retained|=self.held;self.held=False;self.ret(1)
  elif name=='parent':self.ret(1)
  elif name=='npu_convolution_startup_poll':
   self.startups+=1;self.events.append('startup');self.ret(0)
  elif name=='npu_service_activate':self.activations+=1;self.events.append('activate');self.ret(0)
  elif name=='npu_convolution_stop_poll':self.stops+=1;self.events.append('local-stop');self.ret(0)
  elif name=='vTaskDelay':
   self.delays+=1;self.time+=r[0]*1000
   if self.mode=='accepted-close' and self.activations:
    self.regs[0x02522020]=31
    for i in range(5):self.regs[0x025220c0+4*i]=970 if i==2 else 2400
    if self.gate()&4:self.put(self.conv+self.cl['closed'],1)
   self.ret()
  elif name=='vTaskSuspend':self.events.append('suspend');self.done=True;u.emu_stop()
  elif name=='npu_finite_base_monitor_init':self.ret() # Base admission already covered separately.
  elif name=='a733_smp_stop':self.retained=True;self.done=True;u.emu_stop()
 def read(self,u,access,at,size,value,unused):
  assert size==4 and (self.held or 0x03400000<=at<0x03500000),(hex(at),size,self.held)
  self.put(at,self.regs.get(at,0));self.trace.append(('read',at))
 def write(self,u,access,at,size,value,unused):
  assert size==4 and self.held,(hex(at),size,self.held)
  self.trace.append(('write',at,value))
  if self.bad_disable and at==0x02522004 and not value&31:return
  if 0x02522020<=at<=0x0252202c:self.regs[at]=self.regs.get(at,0)&~value
  else:self.regs[at]=value
  if at==0x02522004 and not value&31:self.events.append('sensor-disabled')
 def call(self,name,*args,unit=None):
  self.done=False;self.u.reg_write(UC_ARM64_REG_SP,0x43ffeff0);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  self.u.emu_start(self.e.address(name,unit=unit),0,count=3000000);assert self.done,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  return self.u.reg_read(UC_ARM64_REG_X0)&0xffffffffffffffff
 def invoke(self,name):return self.call('npu_finite_thermal_'+name,self.thermal)
 def sample(self,codes=None,mask=31):
  self.time+=25000;self.regs[0x02522020]=mask
  for i,code in enumerate(codes or [2400]*5):self.regs[0x025220c0+4*i]=code
  return self.invoke('step')
 def ready(self):
  assert self.invoke('step')==1 and self.invoke('step')==1 and not self.gate()&1
  assert self.sample()==1 and self.gate()&1
 def bootstrap(self):return self.call('bootstrap',self.app,unit='apps/npu-convolution/native.c')

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--group',choices=('provider','session','never'));a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
 e=harness.Image(a.image,layout_names=LAYOUTS);fields=[]
 with a.image.open('rb') as stream:
  for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
   if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/npu-convolution/native.c'):continue
   for d in cu.iter_DIEs():
    if d.tag=='DW_TAG_variable' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value==b'app':
     fields.append({x.attributes['DW_AT_name'].value.decode():int(x.attributes['DW_AT_data_member_location'].value) for x in d.get_DIE_from_attribute('DW_AT_type').iter_children() if x.tag=='DW_TAG_member' and 'DW_AT_name' in x.attributes})
 assert len(fields)==1;f=fields[0];results={}
 def machine():return Machine(e,f)
 def save(name,**details):
  results[name]={'passed':True,**details};(a.output/'result.json').write_text(json.dumps({'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'cases':results,'scope':__doc__,'physical_access':False},indent=2)+'\n')
 if a.group in (None,'session'):
  def module(name,path):
   spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
  prepare=module('finite_thermal_prepare',ROOT/'apps/npu-convolution/prepare-supervised.py')
  pages=module('finite_thermal_pages',ROOT/'checks/npu-image-checks.py')
  layout=prepare.image_data(a.image.read_bytes())[3];assert layout[:3]==(616,224,392)
  old=(Path.home()/'.cache/sunstoneos/evidence/npu-single-core-20261018/startup-usb-resume/session-fixture.bin').read_bytes()
  raw=bytearray(pages.npu_page(old,(456,224,232,*layout[3:]),3))+bytearray(160);struct.pack_into('<Q',raw,16,616)
  struct.pack_into('<2Q',raw,456,0x12345,0x23456);raw[472:600]=bytes([1])*128;struct.pack_into('<2IB',raw,600,1,12000000,1)
  changes={'valid':None,'short-size':(16,'Q',456),'extra-size':(16,'Q',624),
   'duplicate-owner':(456,'Q',struct.unpack_from('<Q',raw,224+40)[0]),'missing-authority':(464,'Q',0),
   'bad-APB-rate':(604,'I',1),'bad-configure':(608,'B',2),'nonzero-padding':(609,'B',1)}
  m=machine()
  for label,change in changes.items():
   value=bytearray(raw)
   if change:at,kind,v=change;struct.pack_into('<'+kind,value,at,v)
   m.u.mem_write(0x43e01000,bytes(value));assert m.call('npu_session_valid',0x43e01000)==int(change is None),label
   save('native-session-'+label)
  m=machine();m.ready();m.trace.clear();before=m.now_calls;assert m.sample()==1
  reads=sum(x[0]=='read' for x in m.trace);writes=sum(x[0]=='write' for x in m.trace);sample_counters=m.now_calls-before
  before=m.now_calls;assert m.invoke('claim')==1;claim_counters=m.now_calls-before;assert claim_counters==2
  save('measured-fresh-sample-and-claim',sample_MMIO_reads=reads,sample_MMIO_writes=writes,
   sample_counter_queries=sample_counters,claim_counter_queries=claim_counters,steady_interval_us=25000)
  if a.group=='session':print('PASS finite THS session/budget:',len(results),'cases');return
 if a.group in (None,'never'):
  for label in ('startup-token','row-request','worker-admitted'):
   m=machine();m.ready();m.sample([2400,2400,970,2400,2400])
   if label=='startup-token':m.put(m.startup+m.sl['clock'],1,8)
   elif label=='row-request':m.put(m.startup+m.sl['row']+e.layouts['a733_power_domain_row']['request'],1)
   else:m.put(m.app+f['worker_admitted'],1)
   m.bootstrap();assert not m.startups and not m.get(m.app+f['never_started']) and m.get(e.address('a733_npu_application_failed'))
   save('never-started-refuses-'+label)
  if a.group=='never':print('PASS finite never-started refusals:',len(results),'cases');return
 m=machine();assert m.invoke('claim')==0;m.ready();assert m.invoke('claim')==1;m.invoke('complete');save('actual-calibration-five-fresh-channels',accesses=len(m.trace))
 m=machine();m.invoke('step');m.invoke('step');assert m.sample(mask=15)==1 and m.invoke('claim')==0
 m.time+=1000000;assert m.invoke('claim')==0 and m.gate()&4;save('partial-channels-never-admit')
 m=machine();m.ready();until=m.gate()>>6;m.jump=(m.now_calls+2,until);assert m.invoke('claim')==0 and not m.gate()&2 and m.gate()&4;save('preempted-reservation-rechecks-expiry')
 m=machine();m.ready();assert m.invoke('claim')==1;assert m.sample([2400,2400,970,2400,2400])==1 and m.gate()&6==6
 deadline=m.gate()>>6;m.invoke('complete');assert m.gate()&4 and not m.gate()&2 and m.gate()>>6==deadline
 assert m.invoke('step')==1 and m.invoke('quiet')==1 and not m.regs[0x02522004]&31
 assert all(m.regs.get(a,0)==0 for a in (0x02522010,0x02522014,0x02522018));save('critical-preserves-accepted-and-stops-sensor',deadline=deadline)
 m=machine();m.ready();m.sample([1100,2400,2400,2400,2400]);assert m.invoke('reason')==1 and m.invoke('claim')==0
 assert m.sample()==1 and m.invoke('claim')==0;save('hot-refusal-terminal')
 m=machine();m.ready();m.time=m.gate()>>6;assert m.invoke('watch')==1 and m.gate()&4
 until=m.gate()>>6;m.time=until;assert m.invoke('watch')==0;save('independent-freeze-watch-fixed-drain',deadline=until)
 m=machine();m.ready();m.bad_disable=True;m.invoke('close');assert m.invoke('step')==0 and m.invoke('quiet')==0;save('disable-readback-failure-retains')
 m=machine();m.ready();m.invoke('close');assert m.invoke('step')==1;m.regs[0x03400208]=128;assert m.invoke('quiet')==0;save('late-IRQ71-refuses-cleanup')
 m=machine();m.ready();m.invoke('close');assert m.invoke('step')==1 and m.invoke('quiet')==1
 m.regs[0x03400208]=128;assert m.invoke('step')==0 and m.get(m.thermal+m.tl['fault'])==1 and m.invoke('quiet')==0
 save('late-fault-after-stopped-refuses-quiet')
 m=machine();m.ready();m.sample([2400,2400,970,2400,2400]);m.bootstrap()
 assert m.startups==m.activations==0 and m.get(m.app+f['never_started'])==1 and not m.get(m.app+f['stopped']) and not m.get(m.conv+m.cl['closed'])
 assert m.events[-1]=='suspend' and 'sensor-disabled' in m.events;save('actual-bootstrap-never-started-after-configured-stop',events=m.events)
 m=machine();m.ready();m.mode='accepted-close';m.bootstrap();assert m.startups==m.activations==m.stops==1 and m.get(m.app+f['stopped'])==1 and not m.get(e.address('a733_npu_application_failed')),(m.startups,m.activations,m.stops,m.events,m.time,m.get(e.address('a733_npu_application_failed')))
 assert m.events.index('sensor-disabled')<m.events.index('suspend');save('accepted-startup-then-critical-drain',events=m.events)
 m=machine();m.ready();m.bootstrap();assert m.get(e.address('a733_npu_application_failed')) and not m.get(m.app+f['stopped'])
 save('actual-bootstrap-refuses-stuck-client-drain',failed_at=m.time,startup_calls=m.startups)
 for condition,address in [('enabled',None),('pending',0x03400208),('active',0x03400308),('quiet',None)]:
  m=machine();m.put(e.address('npu_finite_thermal_irq_admitted'),0);saved=e.address('saved');m.u.mem_write(saved,struct.pack('<8I',1,4,0x33,0,0,0,0,0)+bytes(128))
  if condition=='enabled':m.put(saved+32+8,128)
  if address:m.regs[address]=128
  m.call('sd_single_core_monitor_init');assert m.get(e.address('npu_finite_thermal_irq_admitted'))==(condition=='quiet') and m.retained==(condition!='quiet')
  assert not any(x[0]=='write' for x in m.trace);save('inherited-IRQ71-'+condition)
 print('PASS finite THS provider/caller cases:',len(results))
if __name__=='__main__':main()
