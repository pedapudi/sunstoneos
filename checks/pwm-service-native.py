#!/usr/bin/env python3
"""Run the actual PWM image task, native aperture, clock registry, ISR and USB copy."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
Base=module('pwm_existing_image_reader',ROOT/'checks/storage-read-native.py')
class Image(Base.Image):
 def __init__(self,path):
  super().__init__(path)
  with path.open('rb') as f:
   e=ELFFile(f)
   for cu in e.get_dwarf_info().iter_CUs():
    unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
    for d in cu.iter_DIEs():
     if d.tag!='DW_TAG_subprogram' or 'DW_AT_low_pc' not in d.attributes:continue
     origin=d
     while 'DW_AT_name' not in origin.attributes and 'DW_AT_abstract_origin' in origin.attributes:origin=origin.get_DIE_from_attribute('DW_AT_abstract_origin')
     if 'DW_AT_name' in origin.attributes:self.functions.setdefault((unit,origin.attributes['DW_AT_name'].value.decode()),set()).add(int(d.attributes['DW_AT_low_pc'].value))
HOOKS=['board_live','a733_smp_counter','a733_counter_clock_now','aarch64_device_translate','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay','vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR','a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','a733_rail_acquire','a733_rail_poll','a733_rail_status','a733_rail_retain','a733_pmic_identity','a733_pmic_live','a733_pmic_submit','a733_pmic_result','a733_pmic_reap','a733_pins_acquire_banked','a733_pins_pwm0_fan_low','a733_pins_pwm0_fan_resume','a733_pins_write_outputs','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','a733_cpu_coherence_publish']
class Machine:
 def __init__(self,e,session,variant):
  self.e=e;self.variant=variant;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.reg_write(UC_ARM64_REG_PSTATE,5)
  for a,n in [(0x02000000,0x06000000),(0x40000000,0x04000000),(0x50000000,0x01000000)]:self.cpu.mem_map(a,n)
  for a,data in e.parts:self.cpu.mem_write(a,data)
  self.base=e.address('pwm');self.l=e.layouts['pwm_application'];self.driver=self.base+self.l['driver'];self.p=e.layouts['a733_pwm'];self.report=self.driver+self.p['report'];self.r=e.layouts['a733_pwm_report'];self.native=self.base+self.l['native']
  self.shared=e.address('providers')+e.layouts['board_provider']['shared'];u=e.layouts['a733_native_usb'];self.pmic=self.shared+u['pmic'];self.locks=self.shared+u['locks'];self.clock=self.shared+u['clocks'];pins=self.shared+u['pins'];rails=self.shared+u['rails'];gpu=self.shared+u['gpu']
  self.time=1000000;self.finished=False;self.held=False;self.appheld=False;self.retained=False;self.trace=[];self.pinlow=0;self.tx=[];self.request=None;self.max_tx=512;self.max_rx=64;self.task_identity=1;self.pin_resume=0;self.led=0;self.contention=False
  self.cpu.mem_write(self.base,session);self.cpu.mem_write(e.address('board'),session[:592]);self.write(e.address('board')+160,61000000,8);self.write(self.base+self.l['handle'],1,8);self.write(self.base+self.l['mutex'],1,8)
  ll=e.layouts['a733_platform_locks'];self.write(self.locks+ll['initialized'],1);self.write(self.locks+ll['mutex'],1,8);self.write(self.locks+ll['now'],e.address('a733_counter_clock_now'),8)
  cl=e.layouts['a733_clock_leases'];self.write(self.clock+cl['initialized'],1);self.write(self.clock+cl['next_id'],1,8);self.write(self.clock+cl['now'],e.address('a733_counter_clock_now'),8)
  self.write(pins+e.layouts['a733_pins']['initialized'],1)
  rl=e.layouts['a733_rail_leases'];rb=e.layouts['a733_rail_binding'];self.write(rails+rl['self'],rails,8);self.write(rails+rl['binding']+rb['clocks'],self.clock,8);self.write(rails+rl['binding']+rb['gpu'],gpu,8)
  pl=e.layouts['a733_pmic'];po=e.layouts['a733_pmic_ops']
  for field,value in [('clocks',self.clock),('pins',pins),('rails',rails),('cookie',self.native)]:self.write(self.pmic+pl[field],value,8)
  self.write(self.pmic+pl['ops']+po['outside'],e.address('outside','/drivers/pwm/a733_pwm_native.c'),8)
  self.regs={0x07090160:0x10000002,0x02002000:0xf8000000|(99<<16)|(99<<8),0x03400088:8,0x03400440:0xd8000000}
  self.hooks={e.address(n):n for n in HOOKS};self.cpu.hook_add(UC_HOOK_CODE,self.code);self.cpu.hook_add(UC_HOOK_MEM_READ,self.mmread,begin=0x02000000,end=0x07ffffff);self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.mmwrite,begin=0x02000000,end=0x07ffffff)
 def write(self,a,v,n=4):self.cpu.mem_write(a,int(v).to_bytes(n,'little'))
 def read(self,a,n=4):return int.from_bytes(self.cpu.mem_read(a,n),'little')
 def field(self,n):return self.read(self.base+self.l[n])
 def rf(self,n):return self.read(self.report+self.r[n])
 def ret(self,v=0):self.cpu.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 def code(self,cpu,a,size,unused):
  if a==0x50fff000:self.finished=True;cpu.emu_stop();return
  name=self.hooks.get(a)
  if not name:return
  q=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='board_live':self.ret(1)
  elif name=='a733_smp_counter':self.ret(self.time*24)
  elif name=='a733_counter_clock_now':self.write(q[1],self.time,8);self.ret(1)
  elif name=='aarch64_device_translate':self.cpu.mem_write(q[2],struct.pack('<Q2I',q[0],0,2));self.ret(1)
  elif name=='xTaskGetCurrentTaskHandle':self.ret(self.task_identity)
  elif name=='xTaskGetSchedulerState':self.ret(2)
  elif name=='xQueueGetMutexHolder':self.ret(1 if self.held else 0)
  elif name=='xQueueSemaphoreTake':assert not self.appheld;self.appheld=True;self.ret(1)
  elif name=='xQueueGenericSend':assert self.appheld;self.appheld=False;self.ret(1)
  elif name=='vTaskDelay':assert not self.appheld;self.finished=True;cpu.emu_stop()
  elif name.startswith('vTask') and 'Critical' in name:self.ret()
  elif name=='a733_platform_lock_enter':
   assert not self.held and not self.retained
   if self.contention:self.contention=False;self.ret(0)
   else:self.held=True;self.ret(1)
  elif name=='a733_platform_lock_leave':assert self.held;self.held=False;self.ret()
  elif name=='a733_platform_lock_retain':assert self.held;self.held=False;self.retained=True;self.ret(1)
  elif name=='a733_rail_acquire':self.cpu.mem_write(q[5],struct.pack('<4Q',1,q[1],q[2],q[3]));self.ret(1)
  elif name in ('a733_rail_poll','a733_rail_status'):self.ret(0)
  elif name=='a733_rail_retain':self.ret(5)
  elif name=='a733_pmic_identity':self.cpu.mem_write(q[1],struct.pack('<3Q',self.pmic,10,1));self.ret(1)
  elif name=='a733_pmic_live':self.ret(1)
  elif name=='a733_pmic_submit':self.pending_rail=q[3];assert self.pending_rail in (0,15);self.write(q[5]+24,1,8);self.ret(0)
  elif name=='a733_pmic_result':
   p=q[2];self.cpu.mem_write(p,bytes(24));pl=self.e.layouts['a733_pmic_snapshot']
   for field,value in [('rail',self.pending_rail),('chip',3),('enable',64),('selector',13),('enabled',1),('decoded',1),('programmed_uv',1800000 if self.pending_rail==15 else 3300000)]:self.write(p+pl[field],value,1 if field in ('chip','enable','selector') else 4)
   self.ret(2)
  elif name=='a733_pmic_reap':self.ret(2)
  elif name=='a733_pins_acquire_banked':self.cpu.mem_write(q[4],struct.pack('<2Q',1,q[1])+bytes(52));self.ret(0)
  elif name=='a733_pins_pwm0_fan_low':
   assert not self.regs.get(0x02527080,0) and not self.regs.get(0x02527040,0);self.pinlow+=1;self.ret(4 if self.variant=='pin-low-fault' else 0)
  elif name=='a733_pins_pwm0_fan_resume':
   assert not self.regs.get(0x02527080,0) and self.regs.get(0x02527040,0)==32;self.pin_resume+=1;self.ret(0)
  elif name=='a733_pins_write_outputs':
   assert q[2]==11 and q[3]==128 and q[4] in (0,128);self.led=q[4]>>7;self.ret(4 if self.variant=='led-fault' else 0)
  elif name in ('a733_native_usb_step' ,'a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_cpu_coherence_publish'):self.ret(1)
  elif name=='tud_vendor_n_available':self.ret(min(len(self.request),self.max_rx) if self.request else 0)
  elif name=='tud_vendor_n_read':n=min(len(self.request),q[2],self.max_rx);self.cpu.mem_write(q[1],self.request[:n]);self.request=self.request[n:];self.ret(n)
  elif name=='tud_vendor_n_write':n=min(q[2],self.max_tx);self.tx.append(bytes(cpu.mem_read(q[1],n)));self.ret(n)
  elif name=='tud_vendor_n_write_flush':self.ret(0)
  else:raise AssertionError(name)
 def mmread(self,cpu,access,a,size,value,unused):
  assert size==4 and (self.held or 0x03400000<=a<0x03410000),(hex(a),self.held)
  v=self.regs.get(a,0);self.trace.append(['read',a,v]);self.write(a,v)
 def mmwrite(self,cpu,access,a,size,v,unused):
  assert size==4 and (self.held or a in (0x03400108,0x03400188)),(hex(a),self.held)
  self.trace.append(['write',a,v])
  if a==0x03400188:assert v==8;self.regs[0x03400108]=self.regs.get(0x03400108,0)&~v
  elif a==0x03400288:assert v==8;self.regs[0x03400208]=self.regs.get(0x03400208,0)&~v
  elif a==0x02527004:assert v==32;self.regs[a]=self.regs.get(a,0)&~v
  else:self.regs[a]=v
 def call(self,name,*args):
  self.finished=False;self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.emu_start(self.e.address(name),0,count=3000000)
  assert self.finished,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)),self.rf('phase'),self.rf('error'))
  return self.cpu.reg_read(UC_ARM64_REG_X0)
 def frame(self):assert self.call('pwm_copy',0x50010000)==1;return bytes(self.cpu.mem_read(0x50010000,512))
def run(image,session,out):
 out.mkdir(parents=True,exist_ok=False);raw=session.read_bytes();prep=module('pwm_service_preparer',ROOT/'apps/pwm/prepare.py');prep.embedded_reference(image.read_bytes(),raw)
 e=Image(image);names=HOOKS+['pwm_task','pwm_copy','pwm_session_valid','pwm_application_irq','pwm_cancel','pwm_submit','a733_pwm_control','board_usb0_task','tud_vendor_tx_cb']
 mapping={n:e.address(n) for n in names};mapping['native.outside']=e.address('outside','/drivers/pwm/a733_pwm_native.c')
 (out/'preflight.json').write_text(json.dumps(dict(image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),session_sha256=hashlib.sha256(raw).hexdigest(),hooks=mapping,layouts=e.layouts,physical_access=False),indent=2)+'\n')
 decoder=module('pwm_service_decoder',ROOT/'apps/pwm/observe.py');results={}
 def ready(m):
  assert m.call('pwm_session_valid',m.base)==1
  for i in range(120):
   m.call('pwm_task',0);m.time+=1000
   if m.field('ready'):break
  assert m.field('ready') and m.rf('phase')==16 and m.pinlow==1
 def submit(m,seq,kind,arg=0,budget=100000):
  packet=decoder.command(raw,seq,kind,arg,budget);m.cpu.mem_write(0x50020000,packet[1:]);return m.call('pwm_submit',0x50020000)
 def finish(m):
  first=True
  for i in range(20):
   m.call('pwm_task',0);m.time+=1000
   if m.read(m.driver+m.p['service_step'])==6:
    m.regs[0x02527004]=32;m.call('pwm_application_irq')
    if first:
     m.contention=True;m.call('pwm_task',0);assert m.field('pending')==1 and not m.rf('irq_seen');first=False
   if not m.field('command_pending'):break
  assert m.field('ack_state')==2 and not m.field('command_pending') and m.field('result')==1
  return decoder.decode(m.frame(),raw)
 for variant in ('reusable','command-deadline','idle-expiry','led-fault'):
  m=Machine(e,raw,variant);ready(m)
  if variant=='reusable':
   m.task_identity=2;n=len(m.trace);assert m.call('a733_pwm_control',m.driver,300,1,m.time+50000)==6 and len(m.trace)==n;m.task_identity=1
   commands=[(1,300),(2,599),(4,1),(4,0),(3,0),(1,1),(3,0)]
   for seq,(kind,arg) in enumerate(commands,1):
    assert submit(m,seq,kind,arg)==1;deadline=m.read(m.base+m.l['command_deadline'],8);n=len(m.trace)
    assert submit(m,seq,kind,arg)==1 and m.read(m.base+m.l['command_deadline'],8)==deadline and len(m.trace)==n
    if kind==1:
     assert submit(m,seq,kind,301 if arg!=301 else 302)==2 and m.field('rejection')==3
     assert submit(m,seq+1,3)==2 and m.field('rejection')==4
     assert submit(m,seq,kind,arg)==1
    decoded=finish(m);assert decoded['command']['sequence']==seq
   assert m.pin_resume==3 and m.pinlow==6 and m.led==0
   assert submit(m,1,1,300)==2 and m.field('rejection')==5
   frame=m.frame();(out/'native-frame.bin').write_bytes(frame)
   # Real USB fragmented81B command delivery plus copied512B staging/cancel.
   m.request=decoder.command(raw,8,1,300)
   for i in range(4):m.call('board_usb0_task',0)
   assert m.field('command_pending')==1 and m.read(m.base+m.l['accepted']+24,8)==8
   finish(m)
   m.max_tx=64;m.request=b'\0';m.call('board_usb0_task',0);assert len(m.tx)==1 and len(m.tx[0])==64
   m.request=b'\1';m.call('board_usb0_task',0);assert len(m.tx)==1
   m.call('tud_vendor_tx_cb',0,64);m.request=b'\0';m.max_tx=512;m.call('board_usb0_task',0);assert len(m.tx)==2 and m.tx[-1][8]==2
   m.call('pwm_task',0);assert m.read(m.driver+m.p['fault'])==1
  elif variant=='idle-expiry':
   m.time=m.read(m.base+m.l['deadline'],8);n=len(m.trace);m.call('pwm_task',0)
   assert m.field('result')==2 and m.read(m.driver+m.p['fault'])==1 and not any(t[0]=='write' and 0x02527000<=t[1]<0x02528000 for t in m.trace[n:])
   decoded=decoder.decode(m.frame(),raw)
  else:
   assert submit(m,1,1,300)==1
   if variant=='command-deadline':
    for i in range(20):
     m.call('pwm_task',0)
     if m.read(m.driver+m.p['service_step'])==6:break
    assert m.read(m.driver+m.p['service_step'])==6
    m.time=m.read(m.base+m.l['command_deadline'],8);n=len(m.trace);m.call('pwm_task',0);m.call('pwm_task',0)
    assert m.field('result')==2 and not m.rf('gpio_low') and not any(t[0]=='write' and 0x02527000<=t[1]<0x02528000 for t in m.trace[n:])
   else:
    finish(m);assert submit(m,2,4,1)==1;m.call('pwm_task',0);m.call('pwm_task',0)
    assert m.field('result')==2 and m.read(m.driver+m.p['fault'])==1 and not m.rf('gpio_low') and m.regs[0x02527080]==32
   decoded=decoder.decode(m.frame(),raw)
   (out/(variant+'-frame.bin')).write_bytes(m.frame())
  results[variant]=dict(passed=True,result=decoded,pin_low_calls=m.pinlow,pin_resume_calls=m.pin_resume,mmio_accesses=len(m.trace))
  (out/(variant+'-trace.json')).write_text(json.dumps(m.trace,indent=2)+'\n')
  (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results,scope='Actual linked reusable task/native/core/clock/GIC/ISR/USB. Shared pin/rail/PMIC replies, MMIO/time/scheduling modeled; actual provider composition separately checked. No physical waveform/optical qualification.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.image,a.session,a.output)
