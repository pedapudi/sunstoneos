#!/usr/bin/env python3
"""Execute linked RTC operations and device EL3 hooks; model unchanged platform/time/MMIO."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_CPACR_EL1
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--operation',choices=('source','alarm'),required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence
spec=importlib.util.spec_from_file_location('rtc_layout',e/'layout-helper.py');layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout);image=layout.Image(e/'build/probe.elf')
D=0x03400000;R=0x07090000;END=0x43fff000;IRQ_RETURN=0x43fff100
class Machine:
 def __init__(self,variant):
  self.variant=variant;self.u=u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x06000000);u.mem_map(0x40000000,0x04000000)
  for address,data in image.parts:
   if data:u.mem_write(address,data)
  u.reg_write(UC_ARM64_REG_PSTATE,5);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
  self.el=4 if variant=='wrong-el' else 12;self.sctlr=1 if variant=='mmu-on' else 0;self.scr=0x400;self.pmr=0xf0;self.time=1000000;self.exit=None;self.forward=None;self.legacy=0;self.delays=0;self.irq_context=None;self.events=[];self.reads=[];self.writes=[];self.privileged=[]
  external=a.operation=='alarm' and variant!='internal-refusal'
  self.regs={D:0x37,D+4:0x18,D+0x11c:0x80000000,D+0x21c:0,D+0x31c:0,D+0x9c:0x12345660,D+0xd1c:0x12345010,D+0xc38:0xa5a50200,D+0x4e4:0x44332280,D+0x6720:0x100000001,0x0701020c:0x10001,
   R:0x401d if external else 0x401c,R+4:1 if external else 0,R+8:15,R+12:0x7a000000,R+0x160:0x983fb0f7,R+0x20:0x1234,R+0x24:0x010203,R+0x28:0,R+0x2c:0,R+0x30:0,R+0x50:0}
  if variant=='original-enabled':self.regs[D+0x11c]|=16
  if variant=='original-pending':self.regs[D+0x21c]|=16
  if variant=='original-active':self.regs[D+0x31c]|=16
  if variant=='ds-set':self.regs[D]|=0x40
  if variant=='bus-off':self.regs[0x0701020c]=0
  if variant=='inherited-rtc-event':self.regs[R+0x30]=1
  self.original=dict(self.regs);self.hooks={image.address(n):n for n in ('monitor_init','platform_init','xTaskCreateStatic','vTaskStartScheduler','vTaskDelay','platform_exit','platform_panic') if n in image.symbols and image.symbols[n]-{0}}
  if a.operation=='alarm':self.hooks[image.address('monitor_exception')]='monitor_exception'
  u.hook_add(UC_HOOK_CODE,self.code);u.hook_add(UC_HOOK_MEM_READ,self.read,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_WRITE,self.write,begin=0x02000000,end=0x07ffffff)
 def put(self,address,value,size=8):self.u.mem_write(address,int(value).to_bytes(size,'little'))
 def get(self,address,size=8):return int.from_bytes(self.u.mem_read(address,size),'little')
 def ret(self,value=0):self.u.reg_write(UC_ARM64_REG_X0,value);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def value(self,address):
  seconds=6316+(self.time-1000000+200000)//1000000
  if address==R+0x10:return 0 if self.variant=='invalid-calendar' else 1
  if address==R+0x14:return ((seconds//3600)<<16)|(((seconds%3600)//60)<<8)|(seconds%60)
  if a.operation=='alarm' and self.regs[R+0x28]&1:
   target=self.regs[R+0x24];target_seconds=((target>>16)&31)*3600+((target>>8)&63)*60+(target&63)
   if seconds>=target_seconds and self.variant not in ('clean-cancel','no-irq'):
    self.regs[R+0x30]|=1
    if self.regs[R+0x2c]&1:self.regs[D+0x21c]|=16
  assert address in self.regs,hex(address)
  return self.regs[address]
 def read(self,cpu,access,address,size,value,cookie):
  if R<=address<R+0x400:assert not self.regs[D+0x31c]&16,'RTC read inside active IRQ'
  self.reads.append((self.el,address));self.put(address,self.value(address),size)
 def write(self,cpu,access,address,size,value,cookie):
  if R<=address<R+0x400:assert not self.regs[D+0x31c]&16,'RTC write before port EOI'
  self.writes.append((self.el,address,value))
  if address==D+0x19c:self.regs[D+0x11c]&=~value;self.events.append('mask228')
  elif address==D+0x11c:self.regs[address]|=value
  elif address in (D+0x9c,D+0xd1c,D+0xc38,D+0x4e4,D+0x6720):
   if self.variant!='route-partial' or address!=D+0x9c:self.regs[address]=value
  elif address==R:
   assert a.operation=='source' and value>>16==0x16aa
   self.regs[R]=value&65535
   if self.variant=='source-partial':self.regs[R]^=16
   if value&1 and self.variant!='source-no-lock':self.regs[R+4]=1
   elif self.variant=='source-no-lock':self.regs[R]&=~1;self.regs[R+4]=0
  elif address==R+4:assert value==2;self.regs[address]&=~2
  elif address in (R+0x20,R+0x24,R+0x28,R+0x2c):
   assert a.operation=='alarm';self.regs[address]=value
   if self.variant=='alarm-partial' and address==R+0x20:self.regs[address]=value+1
  elif address==R+0x30:
   assert a.operation=='alarm' and value==1;self.events.append('RTC_W1C');self.regs[address]&=~1;self.regs[D+0x21c]&=~16
  else:raise AssertionError(('unowned write',hex(address),hex(value)))
 def code(self,cpu,address,size,cookie):
  u=self.u;word=self.get(address,4);kind=word&0xffffffe0;reg=word&31
  if address==IRQ_RETURN:
   self.events.append('EOI228');self.regs[D+0x31c]&=~16;u.context_restore(self.irq_context);self.irq_context=None;self.ret();return
  if kind==0xd5384600:u.reg_write(UC_ARM64_REG_X0+reg,self.pmr);u.reg_write(UC_ARM64_REG_PC,address+4);return
  if kind==0xd5184600:self.pmr=u.reg_read(UC_ARM64_REG_X0+reg);u.reg_write(UC_ARM64_REG_PC,address+4);return
  if kind in (0xd5384240,0xd53e1000,0xd53e1100,0xd53e5200,0xd53be000,0xd53be020):
   if kind in (0xd53e1000,0xd53e1100,0xd53e5200):assert self.el==12,'EL3 register read before CurrentEL guard';self.privileged.append(kind)
   values={0xd5384240:self.el,0xd53e1000:self.sctlr,0xd53e1100:self.scr,0xd53e5200:0x5e000000,0xd53be000:24000000,0xd53be020:self.time*24}
   if kind==0xd53be020:self.time+=50
   u.reg_write(UC_ARM64_REG_X0+reg,values[kind]);u.reg_write(UC_ARM64_REG_PC,address+4);return
  name=self.hooks.get(address)
  if not name:return
  args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(7)]
  if name=='monitor_init':
   self.legacy+=1;self.regs[D+0x11c]=0;self.scr=0x505
   self.put(0x42000000,0x534b4552);self.put(0x42000018,0);self.ret()
  elif name=='platform_init':self.ret()
  elif name=='xTaskCreateStatic':
   assert args[0]==image.address('probe',unit=f'/apps/rtc-{a.operation}-returning/main.c') and args[2]==2048 and args[3]==0
   self.entry=args[0];self.ret(0x43000000)
  elif name=='vTaskStartScheduler':u.reg_write(UC_ARM64_REG_PC,self.entry);u.reg_write(UC_ARM64_REG_X0,0)
  elif name=='vTaskDelay':
   assert args[0]==1;self.delays+=1;self.time+=1000
   self.value(R+0x30)
   if a.operation=='alarm' and self.regs[R+0x30]&self.regs[R+0x2c]&1 and self.regs[D+0x11c]&16 and self.variant!='no-notify':
    self.events.append('IAR228');self.regs[D+0x31c]|=16;self.irq_context=u.context_save()
    u.reg_write(UC_ARM64_REG_PC,image.address('vApplicationIRQHandler'));u.reg_write(UC_ARM64_REG_X0,228);u.reg_write(UC_ARM64_REG_LR,IRQ_RETURN)
   else:self.ret()
  elif name=='platform_exit':self.exit=args[0];u.emu_stop()
  elif name=='monitor_exception':self.forward=args[:3];u.emu_stop()
  else:raise AssertionError('Unexpected panic')
 def call(self,name,args=(),limit=12000000):
  for i,value in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,value)
  self.u.reg_write(UC_ARM64_REG_SP,0x43ffe000);self.u.reg_write(UC_ARM64_REG_LR,END)
  try:self.u.emu_start(image.address(name),END,count=limit)
  except Exception as exc:raise RuntimeError((self.variant,name,hex(self.u.reg_read(UC_ARM64_REG_PC)),hex(self.get(self.u.reg_read(UC_ARM64_REG_PC),4)),self.events)) from exc
 def route(self,name):
  symbol='rtc_source_admission' if a.operation=='source' else 'rtc_alarm_route';typ=symbol
  return self.get(image.address(symbol)+image.layouts[typ][name],4)
 def log(self):return bytes(self.u.mem_read(0x42000100,self.get(0x42000018))).decode()
 def begin(self):
  self.call('__wrap_monitor_init');assert self.legacy==1
  symbol='rtc_source_admission' if a.operation=='source' else 'rtc_alarm_route';size=64 if a.operation=='source' else 112
  address=image.address(symbol);before=bytes(self.u.mem_read(address,size))
  bs=image.address('__bss_start');be=image.address('__bss_end');self.u.mem_write(bs,bytes(be-bs))
  assert bytes(self.u.mem_read(address,size))==before,'EL1 BSS clearing destroyed retained admission'
  assert address+size<=bs
 def task(self):self.el=4;self.u.reg_write(UC_ARM64_REG_PSTATE,5);self.call('kernel_main');assert self.exit is not None
 def finish(self,status=None,kind=1):
  self.el=12;self.u.reg_write(UC_ARM64_REG_PSTATE,0xd);self.call('__wrap_monitor_exception',[0x733,self.exit if status is None else status,kind]);assert self.forward is not None
cases=[]
admission_cases=('wrong-el','mmu-on','original-enabled','original-pending','original-active','ds-set')
for variant in admission_cases:
 m=Machine(variant);m.begin();assert not m.route('ready')
 if variant=='wrong-el':assert not m.privileged
 if a.operation=='source':
  assert not m.writes
  if variant in ('original-enabled','original-pending','original-active'):
   m.task();assert m.exit==0 and not m.writes and not any(R<=addr<R+0x400 for el,addr in m.reads),m.log()
 else:assert not m.route('mutated') and not m.writes
 cases.append({'name':variant,'passed':True,'scope':'new EL3 admission only; unchanged monitor body modeled','own_mmio_writes':m.writes})
variants=('source-success','bus-off','invalid-calendar','inherited-rtc-event','source-partial','source-no-lock') if a.operation=='source' else ('alarm-fired','internal-refusal','clean-cancel','no-notify','alarm-partial','route-partial')
for variant in variants:
 m=Machine(variant);m.begin();m.task();text=m.log()
 if a.operation=='source':
  success=variant=='source-success';uncertain=variant in ('source-partial','source-no-lock')
  assert m.exit==(1 if uncertain else 0),(variant,text)
  assert ('RTC_SOURCE_OUTCOME=0x0000000000000000' in text)==success,(variant,text)
  assert ('COMPLETE native RTC source result' in text)==(not uncertain),(variant,text)
  rtc_writes=[(addr,value) for el,addr,value in m.writes if R<=addr<R+0x400]
  assert all(addr in (R,R+4) for addr,value in rtc_writes)
  assert (bool(rtc_writes))==(success or uncertain),(variant,text)
  if success:assert m.regs[R]&0xc01f==0x401d and m.regs[R+4]==1 and 'RTC_SOURCE_STAGE=0x0000000000000006' in text
  if not uncertain:assert m.regs[R+0x20]==m.original[R+0x20] and m.regs[R+0x24]==m.original[R+0x24]
 else:
  success=variant=='alarm-fired';uncertain=variant in ('alarm-partial','route-partial')
  assert m.exit==(1 if uncertain else 0),(variant,text)
  assert ('RTC_ALARM_OUTCOME=0x0000000000000000' in text)==success,(variant,text)
  if success:assert m.events.count('IAR228')==1 and m.events.count('EOI228')==1 and m.events.index('EOI228')<m.events.index('RTC_W1C')
  writes_before=list(m.writes);m.finish();text=m.log()
  if uncertain:assert m.forward[1]==1 and m.route('retained')==1 and not m.route('restored')
  else:
   assert m.forward[1]==0 and m.route('restored')==1 and m.route('retained')==0,(variant,text)
   for addr in (D+0x9c,D+0xd1c,D+0xc38,D+0x4e4,D+0x6720):assert m.regs[addr]==m.original[addr],(variant,hex(addr))
   assert not (m.regs[D+0x11c]|m.regs[D+0x21c]|m.regs[D+0x31c])&16
   assert not (m.regs[R+0x28]|m.regs[R+0x2c]|m.regs[R+0x30]|m.regs[R+0x50])&1
  assert all(addr not in (R,R+4,R+0x10,R+0x14,R+0x50) for el,addr,value in m.writes)
 (e/(variant+'.log')).write_text(text);cases.append({'name':variant,'passed':True,'exit':m.exit,'events':m.events,'mmio_writes':m.writes,'modeled_elapsed_us':m.time-1000000,'forwarded':m.forward})
if a.operation=='alarm':
 for variant in ('monitor-timeout','nonclean-normal','nonclean-active','target-mismatch','post-complete-mismatch'):
  m=Machine('alarm-fired' if variant=='post-complete-mismatch' else variant);m.begin();assert m.route('ready')
  if variant=='post-complete-mismatch':m.task();assert m.exit==0 and 'COMPLETE native RTC alarm result' in m.log();m.regs[R+0x20]+=1
  m.regs[D+0x11c]|=16
  if variant in ('monitor-timeout','nonclean-normal'):m.regs[R+0x28]=1;m.regs[R+0x2c]=1;m.regs[R+0x30]=1;m.regs[D+0x21c]=16
  elif variant=='nonclean-active':m.regs[D+0x31c]=16
  elif variant=='target-mismatch':m.regs[R+0x20]+=1
  before=len(m.writes);read_before=len(m.reads);m.finish(status=0,kind=2 if variant=='monitor-timeout' else 1)
  assert m.forward[1]==1 and m.route('retained')==1 and not m.route('restored')
  assert not m.regs[D+0x11c]&16
  assert [(addr,value) for el,addr,value in m.writes[before:]]==[(D+0x19c,16)]
  if variant=='monitor-timeout':assert not any(R<=addr<R+0x400 for el,addr in m.reads[read_before:])
  cases.append({'name':variant,'passed':True,'forwarded':m.forward,'failure_fence_writes':m.writes[before:]})
result={'passed':True,'physical_access':False,'operation':a.operation,'elf_sha256':hashlib.sha256((e/'build/probe.elf').read_bytes()).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'scope':'Actual linked new device EL3 hooks, retained admission across real BSS extent clearing, task construction/client, production RTC/counter/GIC instructions and default FP-safe dispatcher bridge. Original monitor body/return, scheduler, architectural observations, IAR/EOI completion and MMIO effects modeled. Does not establish physical privilege, timing, oscillator accuracy, interrupt delivery or recovery. Unchanged monitor/readonly/calendar arithmetic evidence reused.','cases':cases}
(e/'check-result.json').write_text(json.dumps(result,indent=2)+'\n');print(f'{len(cases)} focused linked {a.operation} cases passed')
