#!/usr/bin/env python3
"""Execute linked CLOSE, local-stop coordination, finish gate and worker park.

RTOS queue replies, the counter provider and platform-stop completion are
modeled effects. The linked OS wrappers, bootstrap, immutable publication,
finish/terminal readiness checks and service parked loop execute from the ELF.
The companion terminal check executes all eight cores' real cleanup assembly.
"""
import argparse
import importlib.util
from pathlib import Path
import struct
import hashlib
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_LR,UC_ARM64_REG_SP
from harness import Image,save
CHECK_INPUTS = ['apps/cpu-npu-concurrency/main.c', 'apps/cpu-npu-concurrency/gemm.h', 'apps/cpu-npu-concurrency/receipt.h', 'apps/cpu-npu-concurrency/host.py', 'apps/cpu-npu-concurrency/vectors.py', 'apps/os/npu-observe.c', 'apps/os/application.c', 'apps/os/cpu-return.c', 'apps/os/npu.c', 'apps/os/npu-declaration.c', 'apps/os/npu-irq.c', 'apps/os/return-terminal.patch', 'apps/os/thermal.c', 'apps/os/cpu-return-host.py', 'checks/os-cpu-return.py', 'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_runtime.c', 'platform/a733/npu_irq.c', 'include/sunstone_app.h', 'include/npu_service.h', 'include/a733_npu_irq.h', 'cmake/sunstone-os-core.cmake', 'checks/cpu-npu-concurrency/producer.c', 'checks/cpu-npu-concurrency/FreeRTOS.h', 'checks/cpu-npu-concurrency/task.h', 'checks/cpu-npu-concurrency/npu.h', 'checks/cpu-npu-concurrency/a733_npu_irq.h']
ROOT=Path(__file__).resolve().parents[1]
GATE_RUNS = [['{elf:ret-npu-load}', '--out', '{output}']]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
owner=module('cpu_npu_return_owners',ROOT/'checks/os-cpu-return-services.py')
prior=owner.prior
terminal=module('cpu_npu_return_terminal',ROOT/'checks/os-cpu-return.py')

class Model(terminal.KernelPages,owner.Model):
 def __init__(self,image,layout):
  super().__init__(image,layout)
  self.n=layout.address('npu','apps/os/npu.c');self.nf=layout.layouts['os_npu_application']
  self.device=terminal.npu_device(self,self.s,layout,self.n);self.df=layout.layouts['os_npu_device']
  # Kernel-only on the board (checks/os-cpu-return.py KernelPages): the device state and the runtime's slot table, which is EL1
  # metadata there and is at 0x41f01000 here.
  self.kernel_only(self.device,self.device+layout.layouts['os_npu_device']['stacks']+2*4096*8);self.kernel_only(0x41f01000,0x41f02000)
  self.rt=self.n+self.nf['runtime'];self.service=self.rt+layout.layouts['npu_runtime']['service']
  self.sf=layout.layouts['npu_service'];self.report=layout.address('os_npu_published_report')
  self.closed_operation=None;self.stop_calls=0;self.stop_pending=0;self.publish_stop=False
  self.failures=[];self.callback_after_park=0;self.wakes=0;self.waits=[];self.delays=0
  self.hooks[layout.address('os_application_wake_main')]=self.wake_main
  self.hooks[layout.address('os_granted')]=lambda:int(self.uc.reg_read(owner.prior.fault.REGS[0])==1) # the modeled session grants the NPU (bit 0) alone; admission is not under test
  self.deadline_wait=layout.address('sunstone_deadline_wait_until')
  self.hooks[layout.address('board_service_blocked_until')]=lambda:1 # the service's health window is the board's, not under test
  self.put32(self.n+self.nf['started'],1);self.put32(self.n+self.nf['active'],1)
  self.put32(self.n+self.nf['opened'],1);self.put64(self.n+self.nf['end'],55000000)
  self.put64(self.n+self.nf['initial_deadline'],55000000)
  self.put64(self.n+self.nf['gate'],(55000000<<3)|1)
  self.put64(self.n+self.nf['client'],0x3000);self.put64(self.n+self.nf['bootstrap'],0x4000)
  # The application's task has its own handle, which xTaskGetCurrentTaskHandle returns while its SDK entries run (call).
  self.put64(layout.address('main_task','apps/os/application.c'),0x5000)
  self.hooks[layout.address('xTaskGetCurrentTaskHandle')]=lambda:0x5000 if self.el0 else 0x3000
  self.el1_hook(layout)
  grant=layout.address('grant','apps/os/application.c');self.put64(grant+layout.layouts['ss_grant']['finish_by_us'],55000000)
  # The slot table is carved from the runtime's reservation and the runtime points to it (fail_at reads slot 0).
  self.put64(self.rt+layout.layouts['npu_runtime']['slot'],0x41f01000)
  for k in ('initialized','irq_bound','irq_stopped'):self.put32(self.service+self.sf[k],1)
  self.put64(layout.address('instance','drivers/npu/npu_service.c'),self.service)
  self.put64(self.service+self.sf['runtime'],self.rt) # nonnull authority; parked loop uses no hooks
  self.put32(self.service+self.sf['activation_state'],2)
  self.put32(self.service+self.sf['controller']+layout.layouts['a733_npu_controller']['state'],9)
  self.put64(self.report,0x000000015255504e);self.put32(self.report+8,104)
  self.put64(self.report+16,4660);self.put64(self.report+24,9)
  self.uc.mem_write(0x41f00000,b'producer-receipt')
  for name,fn in [('a733_counter_clock_now',self.clock_now),('npu_runtime_send',self.send),
   ('npu_runtime_collect',self.collect),('npu_runtime_stop_poll',self.local_stop),
   ('npu_runtime_startup_poll',lambda:0),('npu_service_activate',lambda:0)]:self.hooks[layout.address(name)]=fn
  self.hooks[layout.address('xTaskGetSchedulerState')]=lambda:2
  self.notify_wait=layout.address('ulTaskGenericNotifyTake') if 'ulTaskGenericNotifyTake' in layout.symbols else layout.address('ulTaskNotifyTake')
  self.s['host_bootstrap']=layout.address('bootstrap','apps/os/npu.c')
  self.s['host_runtime_worker']=layout.address('runtime_worker','drivers/npu/npu_service.c')
 def call(self,c,symbol,args=(),budget=200000):
  # The SDK entries (ss_*) on core 6 are the application's task, at EL0 on the board.
  self.el0=c==6 and symbol.startswith('ss_')
  try:r=super().call(c,symbol,args,budget)
  finally:self.el0=False
  self.el0_touched(symbol);return r
 def wake_main(self):
  self.wakes+=1;return 0
 def delay(self):
  self.delays+=1;return super().delay()
 def clock_now(self):
  self.put64(self.uc.reg_read(prior.fault.REGS[1]),self.time//24);return 1
 def send(self):
  self.closed_operation=self.uc.reg_read(prior.fault.REGS[2]);assert self.closed_operation==5
  self.put64(self.uc.reg_read(prior.fault.REGS[6]),42);return 1
 def collect(self):
  at=self.uc.reg_read(prior.fault.REGS[3]);self.uc.mem_write(at,bytes(40))
  self.put64(at,42);self.put32(at+self.layout.layouts['npu_service_reply']['state'],9);return 1
 def local_stop(self):
  self.stop_calls+=1
  return 2 if self.stop_calls<=self.stop_pending else 0 # platform PENDING=2 READY=0
 def code(self,uc,address,size,unused):
  if address==self.layout.address('board_fail_at'):
   self.failures.append((uc.reg_read(prior.fault.REGS[0]),uc.reg_read(prior.fault.REGS[1])))
   self.stop='failure';uc.emu_stop();return
  if address==self.deadline_wait:
   self.waits.append(uc.reg_read(prior.fault.REGS[0]));self.stop='waiting';uc.emu_stop();return
  if address==self.notify_wait:
   self.stop='blocked';uc.emu_stop();return
  super().code(uc,address,size,unused)
 def stopped(self):
  self.put32(self.n+self.nf['stopped'],1);self.put32(self.service+self.sf['runtime_parked'],1)
  self.put32(self.report+12,1);self.put32(self.report+56,1)

p=argparse.ArgumentParser(description=__doc__);p.add_argument('elf',type=Path);p.add_argument('--out',type=Path,required=True);p.add_argument('--payload',type=Path);p.add_argument('--late-isr-only',action='store_true');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
names={'board_application','a733_board_session','board_provider','a733_native_usb','a733_pmic','a733_pmic_slot',
'a733_platform_locks','os_thermal_service','os_npu_application','os_npu_device','npu_runtime','npu_runtime_slot','npu_service','a733_npu_controller','a733_npu_startup','ss_grant','npu_service_reply'}
layout=Image(a.elf,names);image=prior.fault.Image(a.elf);results=[]
if not a.late_isr_only:
 m=Model(image,layout)
 assert m.call(1,'os_npu_close')==1 and m.closed_operation==5
 assert m.u32(m.n+m.nf['closed'])==1 and m.u32(m.n+m.nf['opened'])==0
 assert m.call(0,'os_npu_complete')==0
 results.append(dict(case='actual-close-does-not-authorize-local-stop',passed=True))
 # Execute the actual final runtime loop: it must publish parked, then block
 # before any backend/runtime callback can execute, including on another wake.
 m.call(1,'host_runtime_worker',(m.service,));assert m.stop=='blocked'
 assert m.u32(m.service+m.sf['runtime_parked'])==1
 results.append(dict(case='final-close-worker-parks-before-backend',passed=True))
 # The real generic stop entry refuses provider access until worker park and
 # retains a late runtime fault before testing that acknowledgment.
 for parked,failed,pending,expected,calls in [(0,0,0,2,0),(1,0,0,0,1),(0,1,0,3,0),(1,0,3,3,0)]:
  m=Model(image,layout);m.hooks.pop(layout.address('npu_runtime_stop_poll'))
  startup=m.device+m.df['startup'];rf=layout.layouts['npu_runtime']
  m.put64(m.rt+rf['startup'],startup);m.put32(m.rt+rf['bound'],1);m.put32(m.rt+rf['prepared'],1)
  slots=0x41f01000 # the slot table is carved from the reservation: the runtime points to it
  m.put64(m.rt+rf['slot'],slots);m.put32(m.rt+rf['slots'],1);m.put32(slots+layout.layouts['npu_runtime_slot']['state'],8)
  m.put64(startup+layout.layouts['a733_npu_startup']['task'],0x3000)
  m.put32(m.service+m.sf['runtime_parked'],parked);m.put32(m.rt+rf['failed'],failed)
  if pending:
   m.put32(m.service+m.sf['irq_pending'],pending);m.put32(m.service+m.sf['irq_stopped'],0)
  stop_calls=[];m.hooks[layout.address('a733_npu_startup_stop')]=lambda:stop_calls.append(1) or 0
  assert m.call(1,'npu_runtime_stop_poll',(m.rt,))==expected
  assert len(stop_calls)==calls
  results.append(dict(case=f'actual-stop-entry-park{parked}-fault{failed}-pending{pending}',passed=True,startup_stop_calls=len(stop_calls)))
 # Preserve/resume the real bootstrap frame across a separate actual CLOSE.
 # Its notification wait is an RTOS effect; the local C loop and all stop
 # ordering execute without replacing the bootstrap function.
 m=Model(image,layout);m.call(1,'host_bootstrap');assert m.stop=='blocked'
 context=m.uc.context_save();sp=m.uc.reg_read(UC_ARM64_REG_SP)
 stack=bytes(m.uc.mem_read(sp,m.s['__a733_smp_boot_stack_top']-sp))
 assert m.call(1,'os_npu_close')==1
 m.uc.mem_write(sp,stack);m.uc.context_restore(context)
 m.uc.reg_write(prior.fault.REGS[0],1);m.uc.reg_write(UC_ARM64_REG_PC,m.uc.reg_read(UC_ARM64_REG_LR));m.stop=None
 m.uc.emu_start(m.uc.reg_read(UC_ARM64_REG_PC),0,count=50000)
 assert m.stop=='blocked' and not m.stop_calls and not m.u32(m.n+m.nf['stopped'])
 # Worker park notification then allows local stop to remain pending twice and
 # publish its completion only after the third platform reply.
 context=m.uc.context_save();sp=m.uc.reg_read(UC_ARM64_REG_SP);stack=bytes(m.uc.mem_read(sp,m.s['__a733_smp_boot_stack_top']-sp))
 m.call(1,'host_runtime_worker',(m.service,));assert m.stop=='blocked'
 m.uc.mem_write(sp,stack);m.uc.context_restore(context);m.stop_pending=2
 m.uc.reg_write(prior.fault.REGS[0],1);m.uc.reg_write(UC_ARM64_REG_PC,m.uc.reg_read(UC_ARM64_REG_LR));m.stop=None
 m.uc.emu_start(m.uc.reg_read(UC_ARM64_REG_PC),0,count=100000)
 assert m.stop=='suspended' and m.stop_calls==3 and m.u32(m.n+m.nf['stopped'])==1
 assert m.wakes>=1 # the local stop wakes the application's end wait
 results.append(dict(case='actual-bootstrap-close-park-delayed-local-stop',passed=True,platform_stop_calls=m.stop_calls))

 # Bootstrap begins with accepted startup false; force its already-started
 # active loop by resuming the real closed-local-stop interval through a hook.
 # Its local-stop function executes separately below via the finished gate.
# A real late ISR revokes the stopped proof while the parked worker performs
# no device callback. The task-side retirement check retains original flags
# as the first NPU operand before generic completion failure.
m=Model(image,layout);m.stopped()
notify=next(name for name in ('vTaskGenericNotifyGiveFromISR','vTaskNotifyGiveFromISR') if name in layout.symbols)
m.hooks[layout.address(notify)]=lambda:0
m.call(1,'npu_service_irq_notify_from_isr',(m.service,1,0x41f00000))
assert m.u32(m.service+m.sf['irq_pending'])==3 and not m.u32(m.service+m.sf['irq_stopped'])
assert m.call(6,'os_npu_complete')==0
m.call(6,'ss_finish',(0x41f00000,16))
assert m.stop=='failure' and m.failures and m.failures[0][0]&0xf000==0xa000
assert m.failures[0][1]&3==3
results.append(dict(case='actual-parked-late-isr-retains-original-flags',passed=True,failures=m.failures))
if not a.late_isr_only:
 for failure in [None,'before-thermal','before-pmic','before-ready']:
  m=Model(image,layout);m.stopped();assert m.call(0,'os_cpu_return_start')==1
  m.call(6,'ss_finish',(0x41f00000,16));assert m.stop=='suspended'
  def inject():
   m.put32(m.service+m.sf['irq_pending'],3);m.put32(m.service+m.sf['irq_stopped'],0)
  if failure=='before-thermal':inject()
  m.call(0,'os_thermal_task')
  if failure=='before-thermal':
   assert m.stop=='failure' and not m.u32(m.flags['thermal_done'])
  else:
   assert m.stop=='suspended' and m.u32(m.flags['thermal_done'])==1
   m.call(0,'os_cpu_return_supervise');assert not m.u32(m.flags['ready'])
   if failure=='before-pmic':inject()
   m.call(0,'os_cpu_return_pmic')
   if failure=='before-pmic':assert m.stop=='failure' and not m.u32(m.flags['pmic_done'])
   else:
    assert m.stop=='suspended' and m.u32(m.flags['pmic_done'])==1
    if failure=='before-ready':inject()
    m.call(0,'os_cpu_return_supervise')
    if failure=='before-ready':assert m.stop=='failure' and not m.u32(m.flags['ready'])
    else:assert m.stop=='suspended' and m.u32(m.flags['ready'])==1
  if failure:assert m.failures[0][0]&0xf000==0xa000 and m.failures[0][1]&3==3
  results.append(dict(case='actual-retirement-owners-'+str(failure),passed=True,failures=m.failures))

if not a.late_isr_only:
 for variant in ['complete','unpublished' ,'pending-local-stop','worker-not-parked','late-irq','late-runtime-fault','reserve-exhausted']:
  m=Model(image,layout);m.stopped()
  if variant=='unpublished':m.put32(m.report+12,0)
  if variant=='pending-local-stop':m.put32(m.n+m.nf['stopped'],0)
  if variant=='worker-not-parked':m.put32(m.service+m.sf['runtime_parked'],0)
  if variant=='late-irq':m.put32(m.service+m.sf['irq_pending'],1)
  if variant=='late-runtime-fault':m.put32(m.rt+layout.layouts['npu_runtime']['failed'],1)
  if variant=='reserve-exhausted':m.time=55000000*24
  elif variant!='complete':m.time=(55000000-50)*24
  payload=a.payload.read_bytes() if a.payload else b'producer-receipt'
  m.uc.mem_write(0x41f00000,payload)
  m.call(6,'ss_finish',(0x41f00000,len(payload)))
  record=layout.address('ss_result_record');finished=layout.address('finished','apps/os/application.c')
  if variant=='complete':
   assert m.stop=='suspended' and m.u32(finished)==1 and m.u64(record)==0x31544c5553455241,(m.stop,m.u32(finished),hex(m.u64(record)),m.failures)
   assert m.u32(m.flags['closing'])==1 and not m.failures
   (a.out/'native-result.bin').write_bytes(bytes(m.uc.mem_read(record,56+len(payload))))
  else:
   assert m.stop=='failure' and m.u32(finished)==0 and not m.u32(m.flags['closing']),(variant,m.stop,m.failures)
  results.append(dict(case='finish-'+variant,passed=True,failures=m.failures))
# A session whose NPU retirement never completes must end with the named failure of ss_finish (site group 4, detail
# 0x75f, value the unmet NPU conditions), recorded before the supervisor's cutoff test, and the supervisor's own cutoff
# failure must carry the application's latest progress site and the unmet conditions instead of an empty record.
if not a.late_isr_only:
 SECOND=24000000
 for variant in ['unpublished','not-stopped']:
  m=Model(image,layout);m.stopped()
  if variant=='unpublished':m.put32(m.report+12,0)
  else:m.put32(m.n+m.nf['stopped'],0)
  m.time=(55000000-1000000+50)*24 # inside the last second before the cutoff of the supervisor
  m.uc.mem_write(0x41f00000,b'producer-receipt')
  m.call(6,'ss_finish',(0x41f00000,16))
  assert m.stop=='failure' and m.failures,(variant,m.stop)
  site,value=m.failures[0]
  assert site&0x80000fff==0x8000075f and (site>>12)&15==4,(variant,hex(site))
  assert value&0x4000 and value&{'unpublished':1,'not-stopped':8}[variant],(variant,hex(value))
  results.append(dict(case='finish-names-the-unmet-npu-conditions-'+variant,passed=True,failures=m.failures))
 m=Model(image,layout);m.stopped();m.put32(m.report+12,0)
 m.put32(layout.address('progress_site','apps/os/application.c'),0x41234568)
 m.time=55000000*24
 m.call(0,'os_cpu_return_supervise')
 assert m.stop=='failure' and m.failures
 site,value=m.failures[0]
 assert site&0x80000fff==0x8000073c and (site>>12)&15==3,hex(site)
 assert (site>>16)&0x4001==0x4001 and value==0x41234568,(hex(site),hex(value))
 results.append(dict(case='supervisor-cutoff-names-progress-site-and-npu-state',passed=True,failures=m.failures))
# The retirement wait of ss_finish is an event wait, not a poll: with the retirement incomplete and time to spare, the main
# task sleeps once in the deadline wait, which ends one second before the cutoff, and calls vTaskDelay never.
if not a.late_isr_only:
 m=Model(image,layout);m.stopped();m.put32(m.report+12,0)
 m.time=(55000000-5000000)*24
 m.uc.mem_write(0x41f00000,b'producer-receipt')
 m.call(6,'ss_finish',(0x41f00000,16))
 assert m.stop=='waiting' and m.waits==[54000000] and m.delays==0,(m.stop,m.waits,m.delays,[(hex(a),hex(b)) for a,b in m.failures])
 results.append(dict(case='finish-waits-for-the-retirement-event',passed=True,wait_until_us=m.waits))
save(a.out/'results.json',dict(physical_access=False,model_boundary=__doc__,cases=results,
 image_sha256=hashlib.sha256(a.elf.read_bytes()).hexdigest(),
 sources={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in CHECK_INPUTS},
 command=['checks/cpu-npu-integration.py',str(a.elf),'--out',str(a.out)]))
print('PASS',len(results),'linked CPU/NPU close/finish/park cases')
