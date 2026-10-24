#!/usr/bin/env python3
"""Linked NPU thermal driver, native MMIO, policy, clock registry and admission.

CCU/THS/SID/GIC registers, task scheduling, address translation and the shared
register mutex are modeled. Counter construction/queries, clock leases, thermal
calibration/start/read/stop, policy and client gate execute from the native ELF.
The NPU runtime and tensor windows are not exercised by this check.
"""
import argparse,hashlib,json,struct,sys
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'checks'))
import harness
LAYOUTS=('npu_board_application','npu_board_client','npu_board_thermal','board_application','board_provider',
         'a733_native_usb','a733_counter_clock','a733_clock_leases','a733_thermal','a733_thermal_policy')
class Machine:
 def __init__(self,image,session):
  self.e=image;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  self.u.mem_map(0x02000000,0x07000000);self.u.mem_map(0x40000000,0x04000000)
  self.u.reg_write(UC_ARM64_REG_PSTATE,0x3c5)
  for at,data in image.parts:
   if data and at<0x44000000:self.u.mem_write(at,data)
  self.time=1000000;self.done=False;self.held=False;self.retained=False;self.live=True;self.failed=False
  self.trace=[];self.notifications=0;self.now_calls=0;self.jump_on_now=None
  self.bootstrap_calls=0;self.waits=0
  self.app=image.address('npu_board');self.thermal=image.address('thermal',unit='apps/npu-board/thermal.c')
  self.tl=image.layouts['npu_board_thermal'];self.cl=image.layouts['npu_board_client']
  self.client=self.app+image.layouts['npu_board_application']['client']
  self.shared=image.address('providers')+image.layouts['board_provider']['shared']
  self.counter=self.shared+image.layouts['a733_native_usb']['counter']
  self.registry=self.shared+image.layouts['a733_native_usb']['clocks']
  raw=bytearray(session);struct.pack_into('<Q',raw,160,20000000)
  self.u.mem_write(self.app,bytes(raw));self.u.mem_write(image.address('board'),bytes(raw[:592]))
  self.put(self.client+self.cl['now'],image.address('a733_counter_clock_now'),8)
  self.put(self.client+self.cl['clock'],self.counter,8);self.put(self.client+self.cl['session_end'],19990000,8)
  self.put(self.client+self.cl['task'],0x43001000,8);self.put(self.client+self.cl['bootstrap'],0x43002000,8)
  self.put(self.client+self.cl['admission'],(self.time+1000000)<<3,8)
  self.regs={0x07090160:0x10000002,0x02002000:0xf8000000|(99<<8)|(99<<16),
             0x02002518:1,0x02002fc0:0x40,0x02002fe4:0x20}
  bits=250
  for i,code in enumerate((2420,2400,2500,1769,1770)):bits|=code<<(12+12*i)
  for i in range(3):self.regs[0x03006244+4*i]=(bits>>(32*i))&0xffffffff
  names=('xTaskGetCurrentTaskHandle','xTaskCreateStaticAffinitySet','aarch64_device_translate',
         'a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','board_live','board_fail',
         'vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR',
         'xTaskGenericNotify','vTaskGenericNotifyGiveFromISR','a733_counter_clock_now',
         'npu_runtime_startup_poll','npu_service_activate','ulTaskGenericNotifyTake','vTaskSuspend','vTaskDelay')
  self.hooks={image.address(n):n for n in names if image.symbols.get(n)}
  self.special={}
  for at,data in image.parts:
   if at>=0x44000000:continue
   for off in range(0,len(data)-3,4):
    word=int.from_bytes(data[off:off+4],'little')
    if word&0xffffffe0 in (0xd53be020,0xd53be000):self.special[at+off]=word
  for at in set(self.hooks)|set(self.special)|{0x43fff000}:
   self.u.hook_add(UC_HOOK_CODE,self.code,begin=at,end=at)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read,begin=0x02000000,end=0x08ffffff)
  self.u.hook_add(UC_HOOK_MEM_WRITE,self.write,begin=0x02000000,end=0x08ffffff)
  assert self.call('a733_counter_clock_init',self.counter,24000000)==1
  assert self.call('a733_clock_leases_init',self.registry,image.address('a733_counter_clock_now'),self.counter)==1
  assert self.call('npu_board_thermal_start')==1
 def put(self,at,value,n=4):self.u.mem_write(at,int(value).to_bytes(n,'little'))
 def get(self,at,n=4):return int.from_bytes(self.u.mem_read(at,n),'little')
 def gate(self):return self.get(self.client+self.cl['admission'],8)
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def code(self,u,at,size,unused):
  if at==0x43fff000:self.done=True;u.emu_stop();return
  if at in self.special:
   word=self.special[at];u.reg_write(UC_ARM64_REG_X0+(word&31),self.time*24 if word&0xffffffe0==0xd53be020 else 24000000)
   u.reg_write(UC_ARM64_REG_PC,at+4);return
  name=self.hooks[at];r=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='a733_counter_clock_now':
   self.now_calls+=1
   if self.jump_on_now and self.now_calls==self.jump_on_now[0]:self.time=self.jump_on_now[1]
   return
  if name=='npu_runtime_startup_poll':self.bootstrap_calls+=1;self.ret(0);return
  if name=='npu_service_activate':self.ret(0);return
  if name=='ulTaskGenericNotifyTake':
   assert r[2]==25,r;self.waits+=1;self.time+=25000;self.ret(0);return
  if name=='vTaskDelay':self.time+=r[0]*1000;self.ret();return
  if name=='vTaskSuspend':self.done=True;u.emu_stop();return
  if name=='xTaskGetCurrentTaskHandle':self.ret(self.get(self.thermal+self.tl['handle'],8))
  elif name=='xTaskCreateStaticAffinitySet':assert r[7]==1;self.ret(r[6])
  elif name=='aarch64_device_translate':u.mem_write(r[2],struct.pack('<QII',r[0],0,2));self.ret(1)
  elif name=='a733_platform_lock_enter':assert not self.held;self.held=True;self.ret(1)
  elif name=='a733_platform_lock_leave':assert self.held;self.held=False;self.ret()
  elif name=='a733_platform_lock_retain':self.retained|=self.held;self.held=False;self.ret(1)
  elif name=='board_live':self.ret(self.live)
  elif name=='board_fail':self.failed=True;self.ret()
  elif 'Notify' in name:self.notifications+=1;self.ret(1)
  else:self.ret()
 def read(self,u,access,at,size,value,unused):
  assert size==4 and (self.held or 0x03400000<=at<0x03500000),(hex(at),size,self.held)
  self.put(at,self.regs.get(at,0));self.trace.append(('read',at))
 def write(self,u,access,at,size,value,unused):
  assert size==4 and (self.held or 0x03400000<=at<0x03500000),(hex(at),size,self.held)
  self.trace.append(('write',at,value))
  if 0x02522020<=at<=0x0252202c:self.regs[at]=self.regs.get(at,0)&~value
  elif at==0x03400188:self.regs[0x03400108]=self.regs.get(0x03400108,0)&~value
  elif at==0x03400108:self.regs[at]=self.regs.get(at,0)|value
  else:self.regs[at]=value
 def call(self,name,*args,unit=None):
  self.done=False;self.u.reg_write(UC_ARM64_REG_SP,0x43ffeff0);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  self.u.emu_start(self.e.address(name,unit=unit),0,count=2000000);assert self.done,name
  return self.u.reg_read(UC_ARM64_REG_X0)&0xffffffffffffffff
 def step(self):return self.call('npu_board_thermal_step')&0xffffffff
 def sample(self,codes=None,mask=31):
  self.time+=25000;self.regs[0x02522020]=mask
  for i,code in enumerate(codes or [2400]*5):self.regs[0x025220c0+4*i]=code
  return self.step()
 def ready(self):
  assert self.step()==1 and self.step()==1 and not self.gate()&1
  assert self.sample()==1 and self.gate()&1
  assert self.call('npu_board_client_claim',self.client)==1
  self.call('npu_board_client_complete',self.client)

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','output'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--group',choices=('sensor','bootstrap','irq'),action='append',help='Run only selected affected groups')
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
 e=harness.Image(a.image,layout_names=LAYOUTS);session=a.session.read_bytes();results={}
 def save(name,value):
  results[name]=value;(a.output/'result.json').write_text(json.dumps(dict(cases=results,physical_access=False,
    image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),scope=__doc__),indent=2)+'\n')
 if not a.group or 'sensor' in a.group:
  m=Machine(e,session);assert m.call('npu_board_client_claim',m.client)==0;m.ready()
  save('calibrated-startup-all-five-fresh',dict(passed=True,register_accesses=len(m.trace)))
  # CPU channel code1100 is above90C but below110C, exercising actual conversion.
  assert m.sample([1100,2400,2400,2400,2400])==1 and not m.gate()&1
  assert m.call('npu_board_client_claim',m.client)==0
  assert m.sample()==1 and m.gate()&1
  save('hot-refusal-and-hysteresis-recovery',dict(passed=True))
  until=m.gate()>>3;m.time=until
  assert m.call('npu_board_client_claim',m.client)==0
  m.time+=1;m.step();assert not m.gate()&1
  save('stale-readings-refuse',dict(passed=True))
  m=Machine(e,session);m.ready();until=m.gate()>>3
  m.jump_on_now=(m.now_calls+2,until)
  assert m.call('npu_board_client_claim',m.client)==0 and not m.gate()&2
  save('preempted-claim-rechecks-freshness',dict(passed=True))
  m=Machine(e,session);m.ready();assert m.call('npu_board_client_claim',m.client)==1
  assert m.sample([2400,2400,970,2400,2400])==1 and m.gate()&4 and m.gate()&2
  deadline=m.gate()>>3;m.call('npu_board_client_complete',m.client)
  assert m.gate()&4 and not m.gate()&2 and m.gate()>>3==deadline
  assert m.step()==1 and m.regs[0x02522004]&31==0 and m.regs[0x03400108]&128==0
  assert not m.failed
  save('critical-keeps-accepted-job-stop-latched',dict(passed=True,drain_deadline=deadline,converter_disabled=True))
  m=Machine(e,session);m.ready();until=m.gate()>>3;m.time=until+4000000
  word=m.call('npu_board_client_watch',m.client,m.time)
  assert word&4 and word>>3==m.time+5000000
  m.time+=100000;m.call('npu_board_client_thermal',m.client,1,0,m.time+1000000,m.time)
  assert m.gate()==word
  save('frozen-monitor-independent-stop-deadline',dict(passed=True,lost_at=until+4000000,deadline=word>>3))
  m=Machine(e,session);m.ready();m.regs[0x02002fc0]=0
  assert m.sample()==0xffffffff and m.gate()&4 and not m.failed
  save('sensor-clock-fault-closes-admission-before-board-fail',dict(passed=True))
 if not a.group or 'bootstrap' in a.group:
  m=Machine(e,session);m.ready();m.sample([2400,2400,970,2400,2400])
  m.call('bootstrap',0,unit='apps/npu-board/main.c')
  assert not m.bootstrap_calls and not m.failed and m.get(m.app+e.layouts['npu_board_application']['stopped'])==1
  save('critical-before-bootstrap-performs-no-npu-startup',dict(passed=True))
  m=Machine(e,session);m.ready();until=m.gate()>>3
  m.call('bootstrap',0,unit='apps/npu-board/main.c')
  assert m.bootstrap_calls==1 and m.failed and m.gate()&4
  assert until+9000000<=m.time<=until+9025000,(until,m.time)
  save('bootstrap-enforces-frozen-monitor-and-drain-deadline',dict(passed=True,waits=m.waits,failed_at=m.time))
 if not a.group or 'irq' in a.group:
  m=Machine(e,session);m.ready();m.regs[0x02522020]=31
  m.call('npu_board_thermal_irq')
  assert not m.regs[0x03400108]&128 and m.get(m.thermal+m.tl['pending'])==1
  assert m.step()==1 and m.regs[0x03400108]&128 and not m.get(m.thermal+m.tl['pending'])
  save('irq-masks-notifies-reads-and-rearms',dict(passed=True))
 print('PASS linked NPU thermal:',len(results),'cases')
if __name__=='__main__':main()
