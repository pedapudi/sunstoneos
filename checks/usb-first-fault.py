#!/usr/bin/env python3
"""Focused native USB first-fault and no-access BUSY cases; no hardware access."""
import argparse,hashlib,importlib.util,json
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_CPACR_EL1
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/usb-returning-20261018/physical');a=p.parse_args();E=a.evidence;B=a.baseline
spec=importlib.util.spec_from_file_location('layout',E/'layout-helper.py');layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout)
image=layout.Image(E/'build/probe.elf');payload=(B/'input.bin').read_bytes();captured=json.loads((B/'frozen/observations/fabric-session.json').read_text())['words'];base_regs={int(k,0):v for k,v in captured.items()}
class Machine:
 def __init__(self,variant):
  self.variant=variant;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u=self.u;u.mem_map(0x02000000,0x06000000);u.mem_map(0x40000000,0x04000000)
  for addr,data in image.parts:
   if data:u.mem_write(addr,data)
  u.mem_write(0x43000000,payload);u.reg_write(UC_ARM64_REG_PSTATE,5);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
  self.time=1000000;self.task=0;self.mutexes=0;self.tasks=[];self.scheduler=False;self.phase='constructors';self.reads=[];self.writes=[];self.exit=None
  self.regs={**base_regs,0x03400114:0,0x03400214:0,0x03400314:0,0x04100040:0,0x04100048:1,0x04100050:0,0x04100500:0,0x04100400:0,0x02003300:0x40000000,0x02003304:0x01000100,0x02003340:0x80000000}
  self.regs.update({0x04100540+i*16:0 for i in range(16)})
  names=['platform_init','xQueueCreateMutexStatic','xTaskCreateStatic','vTaskStartScheduler','platform_exit','platform_panic','xTaskGetCurrentTaskHandle','xQueueGetMutexHolder','a733_clock_token_status','a733_rail_status','a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','a733_clock_retain']
  self.hooks={image.address(n):n for n in names};u.hook_add(UC_HOOK_CODE,self.code);u.hook_add(UC_HOOK_MEM_READ,self.read_mmio,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_WRITE,self.write_mmio,begin=0x02000000,end=0x07ffffff)
  self.call('kernel_main');assert self.scheduler and len(self.tasks)==2 and self.mutexes==2 and not self.writes
  self.reads=[];self.phase='provider';self.shared=image.address('shared');self.nl=image.layouts['a733_native_usb'];self.ul=image.layouts['a733_usb_platform'];self.pl=image.layouts['a733_pmic'];self.tl=image.layouts['a733_clock_token'];self.usb=self.shared+self.nl['usb'];self.pmic=self.shared+self.nl['pmic'];self.task=self.get(self.shared+self.nl['usb_task'],8)
  self.put(self.shared+self.nl['usb_rail_index'],4);self.put(self.usb+self.ul['token']+self.tl['id'],42,8);self.put(self.usb+self.ul['token']+self.tl['owner'],3,8);self.put(self.shared+self.nl['usb_rail']+image.layouts['a733_rail_token']['owner'],3,8)
  self.put(self.pmic+self.pl['stage'],4)
  clock=self.pmic+self.pl['clock'];self.put(clock+self.tl['owner'],2,8);self.put(clock+self.tl['shared'],(1<<69)|(1<<32)|3,16);self.put(clock+self.tl['exclusive'],1<<55,16)
  lifetime=self.shared+self.nl['usb_power'];ll=image.layouts['a733_pmic_lifetime'];self.put(lifetime+ll['provider'],self.pmic,8);self.put(lifetime+ll['owner'],2,8);self.put(lifetime+ll['identity'],int.from_bytes(payload[24:32],'little'),8)
 def put(self,addr,v,n=4):self.u.mem_write(addr,int(v).to_bytes(n,'little'))
 def get(self,addr,n=4):return int.from_bytes(self.u.mem_read(addr,n),'little')
 def field(self,n):return self.get(self.usb+self.ul[n])
 def fault(self,n):return self.get(self.usb+self.ul['failure']+image.layouts['a733_usb_failure'][n],8 if n in ('address','last_us','sample_us','deadline_us') else 4)
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def call(self,name,args=(),unit=None):
  u=self.u
  for i,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,v)
  u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000);u.emu_start(image.address(name,unit=unit),0x43fff000,count=1000000)
  return u.reg_read(UC_ARM64_REG_X0)
 def code(self,cpu,addr,size,unused):
  word=self.get(addr);kind=word&0xffffffe0;reg=word&31
  if kind in (0xd5384240,0xd53be000,0xd53be020):
   value=4 if kind==0xd5384240 else 24000000 if kind==0xd53be000 else self.time*24
   if kind==0xd53be020:self.time+=1
   self.u.reg_write(UC_ARM64_REG_X0+reg,value);self.u.reg_write(UC_ARM64_REG_PC,addr+4);return
  if self.phase=='provider' and addr==image.address('a733_usb_platform_native_read') and self.variant=='read-error':self.ret(0);return
  if self.phase=='provider' and self.variant=='authority-and-deadline' and addr==image.address('usb_authorize',unit='/platform/a733/native_usb.c'):
   self.time=3000000;self.ret((1<<64)-1);return
  n=self.hooks.get(addr)
  if n is None:return
  args=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if n=='platform_init':self.ret()
  elif n=='xQueueCreateMutexStatic':self.mutexes+=1;self.ret(0x43010000+self.mutexes*256)
  elif n=='xTaskCreateStatic':self.tasks.append(args[0]);self.ret(0x43020000+len(self.tasks)*256)
  elif n=='vTaskStartScheduler':self.scheduler=True;self.u.emu_stop()
  elif n in ('xTaskGetCurrentTaskHandle','xQueueGetMutexHolder'):self.ret(0 if self.variant=='wrong-task' and n=='xTaskGetCurrentTaskHandle' else self.task)
  elif n in ('a733_platform_lock_enter','a733_platform_lock_retain'):self.ret(1)
  elif n in ('a733_clock_token_status','a733_rail_status','a733_platform_lock_leave','a733_clock_retain'):self.ret(3 if n=='a733_rail_status' and self.variant=='rail-fault' else 0)
  elif n=='platform_exit':self.exit=args[0];self.u.emu_stop()
  else:raise AssertionError('Unexpected panic')
 def read_mmio(self,cpu,access,addr,size,value,unused):
  assert size==4 and addr in self.regs,(hex(addr),size)
  if self.phase=='provider':self.reads.append(addr)
  self.put(addr,self.regs[addr])
  if self.phase=='provider' and self.variant=='busy-before-first-write' and addr==0x04100048:self.put(self.pmic+self.pl['busy'],1)
 def write_mmio(self,cpu,access,addr,size,value,unused):
  assert self.phase=='provider' and size==4 and addr in self.regs,(hex(addr),size)
  self.writes.append([addr,value])
  if self.variant!='write-ineffective':self.regs[addr]=value
  if self.variant=='busy-after-write':self.put(self.pmic+self.pl['busy'],1)
results=[]
for variant in ['rail-fault','pmic-fault','authority-and-deadline','read-busy','write-busy','baseline-busy-resumes','read-error','wrong-task','value-mismatch','busy-before-first-write','busy-after-write','write-ineffective','deadline']:
 m=Machine(variant)
 if variant in ('read-busy','write-busy','baseline-busy-resumes'):m.put(m.pmic+m.pl['busy'],1)
 if variant=='pmic-fault':m.put(m.pmic+m.pl['failed'],1)
 if variant in ('read-busy','write-busy'):
  name='usb_read' if variant=='read-busy' else 'usb_write';args=[m.shared,0x07090160,0x43030000] if variant=='read-busy' else [m.shared,0x04100048,0]
  result=m.call(name,args,'/platform/a733/native_usb.c');assert result==2 and not m.reads and not m.writes and m.get(m.shared+m.nl['usb_io_denial'])==0
 else:
  stage=21 if variant in ('busy-before-first-write','busy-after-write','write-ineffective') else 1 if variant=='authority-and-deadline' else 2;m.put(m.usb+m.ul['stage'],stage)
  if variant=='value-mismatch':m.regs[0x070101ac]=0
  if variant=='deadline':m.time=3000000
  result=m.call('a733_usb_platform_poll',[m.usb])
  if variant in ('baseline-busy-resumes','busy-before-first-write'):
   assert result==1 and m.field('stage')==stage and not m.field('fault') and not m.fault('kind') and not m.writes
   if variant=='busy-before-first-write':assert m.field('writes_attempted')==1 and m.field('writes_completed')==0
   m.put(m.pmic+m.pl['busy'],0);m.variant='resumed'
   result=m.call('a733_usb_platform_poll',[m.usb]);assert result==0 and m.field('stage')==(20 if stage==2 else 22) and not m.field('fault')
  else:
   assert result==3 and m.field('stage')==19 and m.field('fault')==1 and m.fault('stage')==stage
   expected={'rail-fault':2,'pmic-fault':2,'authority-and-deadline':7,'read-error':2,'wrong-task':2,'value-mismatch':4,'busy-after-write':15,'write-ineffective':4,'deadline':1}[variant];assert m.fault('kind')==expected,(variant,m.fault('kind'))
   if variant in ('rail-fault','pmic-fault'):assert m.get(m.shared+m.nl['usb_provider_denial'])==(8 if variant=='rail-fault' else 9) and m.get(m.shared+m.nl['usb_provider_result'])==(3 if variant=='rail-fault' else 6) and not m.reads and not m.writes
   if variant=='deadline':assert m.fault('sample_us')>=m.fault('deadline_us') and m.fault('callback_result')==1
   if variant=='busy-after-write':assert m.fault('writes_completed')==1 and m.fault('callback_result')==2 and len(m.writes)==1
   if variant=='value-mismatch':assert m.fault('address')==0x070101ac and m.fault('mask')==1 and m.fault('observed')==0 and m.fault('observed_valid')==1
   fault_bytes=(image.layouts['a733_usb_failure']['callback_result']+4+7)&~7
   before=bytes(m.u.mem_read(m.usb+m.ul['failure'],fault_bytes));io=[len(m.reads),len(m.writes)]
   assert m.call('a733_usb_platform_poll',[m.usb])==3 and before==bytes(m.u.mem_read(m.usb+m.ul['failure'],fault_bytes)) and io==[len(m.reads),len(m.writes)]
   m.u.mem_write(0x43030100,b'modeled retained diagnosis\0')
   m.call('retained',[0x43030100],unit='/apps/usb-returning/main.c');assert m.exit==1
   length=m.get(0x42000018,8);log=bytes(m.u.mem_read(0x42000100,length)).decode();assert f'USB_FAULT_KIND=0x{expected:016x}' in log and f'USB_FAULT_STAGE=0x{stage:016x}' in log and 'COMPLETE native USB' not in log
   (E/(variant+'.log')).write_text(log)
 results.append({'name':variant,'passed':True,'result':result,'stage':m.field('stage'),'first_fault_kind':m.fault('kind'),'mmio_reads':len(m.reads),'writes':m.writes})
result={'passed':True,'physical_access':False,'elf_sha256':hashlib.sha256((E/'build/probe.elf').read_bytes()).hexdigest(),'input_sha256':hashlib.sha256(payload).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'scope':'Actual constructors, native USB context checks and callbacks, actual PMIC live/busy handling, actual USB provider poll, native MMIO guards and fault serializer. Scheduler/task/mutex and clock/rail health modeled; captured clock data and modeled USB register effects. No physical cause or operation claimed.','cases':results}
(E/'first-fault-check.json').write_text(json.dumps(result,indent=2)+'\n');print(f'{len(results)} focused native callback/first-fault cases passed')
