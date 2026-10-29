#!/usr/bin/env python3
"""Actual linked SPI app, IRQ, core, platform and lease/pin manager instructions.

SPI/GIC register effects, pin callbacks, PMIC/rail replies, RTOS and USB transport
are modeled. This is not physical electrical or interrupt-delivery evidence.
"""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('spi_base',ROOT/'checks/spi-application-native.py');base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
class Machine(base.Machine):
 def __init__(self,image,session,variant,nominal):
  super().__init__(image,session,variant,nominal)
  for a,n in [(0x02000000,0x10000),(0x03400000,0x10000),(0x07090000,0x1000)]:self.cpu.mem_map(a,n)
  self.scheduler=1;self.tick=1000000;self.status=0;self.rxleft=0;self.txbytes=[];self.started=False;self.events=[];self.notification=0;self.owner=1
  for name in ('a733_spi_platform_poll','a733_spi_platform_submit','a733_spi_platform_reap','a733_clock_retain'):self.hooks.pop(image.address(name))
  for name in ('vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR','vTaskGenericNotifyGiveFromISR','vTaskYieldWithinAPI'):
   self.hooks[image.address(name)]=name
  for i,name in enumerate(('pin_enter','pin_leave','pin_now','pin_read','pin_write')):self.hooks[0x50010000+16*i]=name
  self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.write)
  self.put(0x07090160,0x10008002);self.put(0x02002000,0xf8675f00)
  self.put(0x02002f20,0x07000302);self.put(0x02002f24,0x20000000)
  self.put(0x02000280,0xffff);self.put(0x03400438,0xb0000000)
  # Real constructors install modeled shared pin callbacks and clock callback.
  self.cpu.mem_write(self.objects['clocks'],bytes(image.sizes['a733_clock_leases']))
  assert self.call('a733_clock_leases_init',self.objects['clocks'],image.address('a733_counter_clock_now'),self.objects['counter'])==1
  self.cpu.mem_write(self.objects['pins'],bytes(image.sizes['a733_pins']))
  self.cpu.mem_write(0x50002000,struct.pack('<5Q',*(0x50010000+16*i for i in range(5))))
  self.cpu.mem_write(0x50003000,bytes(1024));self.put(0x50003000+4*4,15)
  assert self.call('a733_pins_init_banks',self.objects['pins'],0x50002000,0,0x50003000)==1
 def put(self,a,v):self.cpu.mem_write(a,struct.pack('<I',v))
 def get(self,a):return int.from_bytes(self.cpu.mem_read(a,4),'little')
 def memory(self,cpu,access,a,size,value,unused):
  super().memory(cpu,access,a,size,value,unused)
  if a==0x02543004 and self.get(a)&0x80000000:self.put(a,0x80)
  if a==0x02543018:self.put(a,self.get(a)&~0x80008000)
  if a==0x02543008 and self.started:self.put(a,self.get(a)&~0x80000000)
  if a==0x02543014:self.put(a,self.status)
  if a==0x0254301c:self.put(a,self.rxleft if self.started else len(self.txbytes)<<16)
  if a==0x02543300:
   assert self.held and self.rxleft
   received=struct.unpack_from('<I',self.session,796)[0]-self.rxleft+1;cpu.mem_write(a,bytes([received]));self.rxleft-=1
 def write(self,cpu,access,a,size,value,unused):
  if a==0x03400184:self.put(0x03400104,self.get(0x03400104)&~value);self.events.append('gic-mask')
  if a==0x03400104:self.events.append('gic-arm')
  if a==0x02543200:assert self.held;self.txbytes.append(value)
  if a==0x02543014:self.status&=~value;self.events.append('status-clear')
  if a==0x02543010:self.events.append('ier-'+hex(value))
  if a==0x02543008 and value&0x80000000:
   assert self.held and bytes(self.txbytes)==self.session[800:800+struct.unpack_from('<I',self.session,792)[0]]
   self.started=True;self.rxleft=struct.unpack_from('<I',self.session,796)[0];self.status=0x100 if self.variant=='controller-error' else 0x1000;self.events.append('exchange')
 def code(self,cpu,a,size,unused):
  name=self.hooks.get(a);args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(4)]
  if name=='a733_smp_counter':self.tick+=1;self.time=self.tick;self.returned(self.tick*24);return
  if name=='a733_counter_clock_now':self.tick+=1;self.time=self.tick;cpu.mem_write(args[1],struct.pack('<Q',self.tick));self.returned(1);return
  if name=='xTaskGetSchedulerState':self.returned(self.scheduler);return
  if name=='xTaskGetCurrentTaskHandle':self.returned(self.owner);return
  if name in ('vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR','vTaskYieldWithinAPI'):self.returned(0);return
  if name=='vTaskGenericNotifyGiveFromISR':assert args[0]==1;self.notification+=1;self.returned(0);return
  if name=='pin_enter':assert not self.held;self.held=True;self.returned(1);return
  if name=='pin_leave':assert self.held;self.held=False;self.returned(0);return
  if name=='pin_now':self.tick+=1;cpu.mem_write(args[1],struct.pack('<Q',self.tick));self.returned(1);return
  if name=='pin_read':assert self.held;cpu.mem_write(args[2],bytes(cpu.mem_read(args[1],4)));self.returned(1);return
  if name=='pin_write':assert self.held;self.put(args[1],args[2]);self.returned(1);return
  super().code(cpu,a,size,unused)
 def construct(self):
  assert self.call('spi_construct')==1 and self.call('spi_irq_bind')==1;self.scheduler=2
 def until(self,stage):
  for _ in range(80):
   result=self.call('spi_step')
   if self.field('stage')==stage:return
   assert result==0,(self.variant,result,self.field('stage'))
  raise AssertionError('Unbounded setup')

def run(image,session,out,nominal,cases=None):
 out.mkdir(parents=True,exist_ok=False);elf=base.Image(image);raw=session.read_bytes();nom=json.loads(nominal.read_text());results={}
 assert struct.unpack_from('<Q',raw,600)[0]==2
 for variant in cases or ('success','no-interrupt-deadline','active-deferral','controller-error','unexpected-interrupt'):
  m=Machine(elf,raw,variant,nom);m.construct()
  if variant=='unexpected-interrupt':
   m.call('spi_interrupt');assert m.field('failed')==1 and m.field('result')==2 and not m.txbytes
  else:
   m.until(11);assert m.get(0x03400104)==1<<27 and m.get(0x02543010)==0x1700 and m.started
   # The latched TC must not be consumed by task polling without an ISR.
   before=m.rxleft;assert m.call('spi_step')==0 and m.rxleft==before and not m.notification
   if variant=='no-interrupt-deadline':
    m.tick=6000000;assert m.call('spi_step')==0xffffffff and m.field('result')==2 and m.rxleft==before
   else:
    m.put(0x03400304,1<<27);m.call('spi_interrupt');assert m.notification==1 and not m.get(0x03400104)
    assert m.call('spi_step')==0 and m.rxleft==before # active until modeled EOI
    if variant!='active-deferral':m.put(0x03400304,0)
    for _ in range(40):
     result=m.call('spi_step')
     if result in (1,0xffffffff):break
    else:raise AssertionError('Unbounded completion')
    if variant=='success':
     assert result==1 and m.field('result')==1 and m.call('spi_irq_notifications')==1
     assert m.events.index('exchange')<m.events.index('ier-0x1700')<m.events.index('gic-arm')
     assert m.events[-2:]==['ier-0x0','status-clear'],m.events
     # Actual immutable copied frame; USB transport effects remain modeled.
     m.call('board_usb0_task');frame=bytes(m.cpu.mem_read(elf.address('spi_response'),224));decoder=base.module('irq_decoder',ROOT/'apps/spi/observe.py');info=decoder.decode(frame,raw)
     assert info['interrupt_notifications']==1 and info['response_hex']=='0102030405060708'
     (out/'native-frame.bin').write_bytes(frame)
    else:assert result==0xffffffff and m.field('result')==2
  assert not m.get(0x03400104)
  results[variant]=dict(passed=True,events=m.events,notifications=m.notification,remaining_fifo=m.rxleft)
  (out/'progress.json').write_text(json.dumps(results,indent=2)+'\n')
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=results,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),physical_access=False,scope=__doc__),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','output','nominal'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--case',action='append',choices=('success','no-interrupt-deadline','active-deferral','controller-error','unexpected-interrupt'))
 a=p.parse_args();run(a.image,a.session,a.output,a.nominal,a.case)
