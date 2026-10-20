#!/usr/bin/env python3
"""Continue actual DisplayPort task contexts to one immutable terminal reply.

The input is the native Type-C negotiation checkpoint, including its pending
PMIC request, independent bus owners, real task stacks and negotiated token.
Only architectural scheduling, elapsed time, MMIO, sink replies and frame-end
events are modeled. Scheduling is serialized, with modeled one-millisecond tick
preemption at eligible counter returns. SMP concurrency and priority inheritance
are not exercised. Injected frame-end
events establish application handling, not scanout. No physical link, pixel
output or DMA drain is qualified.
"""
import argparse,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_TPIDR_EL1
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):Path(path).write_text(json.dumps(value,indent=2)+'\n')

class DisplayHardware:
 def __init__(self,m,controller):
  self.m=m
  # Reuse the already reviewed lower AUX/PHY/sink model, without executing its
  # isolated controller or copying its manufactured config/provider storage.
  self.sink=controller.Model(m.image)
  self.sink.registers=m.registers
  self.frame_due=None;self.frame_ends=[];self.clears=[];self.events=[]
 def read(self,a,w):
  if 0x05740000<=a<0x05770000 or 0x06c01000<=a<0x06cb0000:
   self.sink.time=self.m.time;v=self.sink.read(a,w);self.m.time=self.sink.time;return v
  if a==0x05008144 and self.frame_due is not None and self.m.time>=self.frame_due:
   self.m.registers[a]=self.m.registers.get(a,0)|1
   self.frame_ends.append(self.m.time);self.frame_due=self.m.time+16667
  return None
 def write(self,a,w,v):
  if 0x05740000<=a<0x05770000 or 0x06c01000<=a<0x06cb0000:
   self.sink.time=self.m.time;self.sink.write(a,w,v);self.m.time=self.sink.time
   if a==0x05740800 and v&1:
    assert self.m.registers.get(0x05008140,0)&1
    self.frame_due=self.m.time+16667
   elif a==0x05740800 and not(v&1):self.frame_due=None
   return
  if a==0x05008144:
   # Only the published W1C bits clear; ordinary control bits retain the
   # supplied image. This prevents a stale frame-end from satisfying capture.
   old=self.m.get(a,4)
   self.m.registers[a]=(v&~13)|(old&13&~v)
   if v&1:self.clears.append(self.m.time)
  if a in (0x07061000,0x0706a000):
   self.m.registers[a+8]=(self.m.registers.get(a+8,0)&~15)|(v&15)
  self.events.append([self.m.time,a,w,v])
 def export(self):
  sink={k:getattr(self.sink,k) for k in ('time','events','transactions','mode','pending','counter','fifo','code_reply','pattern','cr','eq','edid_offset')}
  return dict(sink=sink,frame_due=self.frame_due,frame_ends=self.frame_ends,clears=self.clears,events=self.events)

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','contract','checkpoint','checkpoint-manifest','hardware-source','output'):
  p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--checkpoint-manifest-sha256',required=True)
 p.add_argument('--resume',type=Path)
 p.add_argument('--observe-first-fault',action='store_true')
 p.add_argument('--observe-pc',type=lambda value:int(value,0))
 a=p.parse_args();assert not a.output.exists();a.output.mkdir(parents=True)
 base=module('dp_composed_base',ROOT/'checks/displayport-native.py')
 controller=module('dp_composed_controller',ROOT/'checks/displayport-controller.py')
 lower=module('dp_composed_typec',a.hardware_source)
 manifest=json.loads(a.checkpoint_manifest.read_text())
 assert sha(a.checkpoint_manifest)==a.checkpoint_manifest_sha256
 image_hash=sha(a.image);raw=a.session.read_bytes();contract=a.contract.read_bytes()
 assert manifest['image_sha256']==image_hash
 assert manifest['session_sha256']==sha(a.session) and manifest['contract_sha256']==sha(a.contract)
 directory=a.checkpoint.resolve()
 for name,row in manifest['files'].items():
  file=(directory/name).resolve();assert directory in file.parents
  assert file.stat().st_size==row['bytes'] and sha(file)==row['sha256'],name
 assert manifest['sources'][str(a.hardware_source.resolve())]==sha(a.hardware_source)
 pd_inputs=json.loads((directory/'inputs.json').read_text())
 assert pd_inputs[str((ROOT/'checks/displayport-native.py').resolve())]==sha(ROOT/'checks/displayport-native.py')
 inputs={str(x.resolve()):sha(x) for x in (a.image,a.session,a.contract,a.checkpoint_manifest,a.hardware_source,
  Path(__file__),ROOT/'checks/displayport-native.py',ROOT/'checks/displayport-controller.py')}
 save(a.output/'inputs.json',inputs)
 class Machine(base.Machine):
  def eligible(self):
   core=self.u.reg_read(UC_ARM64_REG_TPIDR_EL1)
   if self.u.reg_read(UC_ARM64_REG_PSTATE)&0x80 or core>=8:return False
   return all(self.get(self.image.address(symbol)+core*8)==0 for symbol in ('ullCriticalNestings','ullPortInterruptNestings'))
  def code(self,u,at,n,unused):
   if at==a.observe_pc:
    self.fault_observation=dict(time_us=self.time,pc=at,task=self.task,return_pc=self.u.reg_read(UC_ARM64_REG_LR),
     registers={f'x{i}':self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(29)})
    self.boundary=('fault-observation',0);self.stopped=True;self.u.emu_stop();return
   name=self.hooks.get(at)
   assert name not in ('vTaskEnterCritical','vTaskExitCritical'), 'unexpected unmodeled critical-section API'
   if name=='a733_dp_fault':
    if self.observe_fault:
     self.fault_observation=dict(time_us=self.time,object=self.u.reg_read(UC_ARM64_REG_X0),
      error=self.u.reg_read(UC_ARM64_REG_X0+1),return_pc=self.u.reg_read(UC_ARM64_REG_LR),task=self.task)
     self.boundary=('fault-observation',0);self.stopped=True;self.u.emu_stop()
    return
   queue=self.u.reg_read(UC_ARM64_REG_X0)
   if name=='xQueueSemaphoreTake':
    wait=self.u.reg_read(UC_ARM64_REG_X0+1);held=self.holders.get(queue,0)
    pending=self.waiters.get(self.task)
    if pending:
     assert pending['queue']==queue
     if not held:del self.waiters[self.task]
     elif self.time>=pending['until']:
      del self.waiters[self.task];self.wait_events.append(['timeout',self.time,self.task,queue]);self.ret(0);return
    if held and wait:
     assert held!=self.task and self.eligible()
     if not pending:
      self.wait_sequence+=1
      pending=dict(queue=queue,until=(self.time//1000+wait)*1000,ticket=self.wait_sequence,blocked=True)
      self.waiters[self.task]=pending
     elif not pending['blocked']:
      # A woken call may find that another task took the mutex first. Its
      # timeout stays fixed, but rejoining the event list gets a new ticket.
      self.wait_sequence+=1;pending['ticket']=self.wait_sequence;pending['blocked']=True
     self.wait_events.append(['block',self.time,self.task,queue,held,pending['until']])
     # Keep PC at the take entry. On release or timeout, this same API call
     # completes before its native caller proceeds.
     self.boundary=('mutex',0);self.stopped=True;self.u.emu_stop();return
   inline=at in getattr(self,'yield_sites',set())
   if inline or name in ('vTaskDelay','vPortYield','vTaskSuspend'):
    ticks=self.u.reg_read(UC_ARM64_REG_X0) if name=='vTaskDelay' else 0
    kind='delay' if name=='vTaskDelay' else 'suspend' if name=='vTaskSuspend' else 'yield'
    self.boundary=(kind,ticks);self.trace.append('inline-SVC105' if inline else name)
    if inline:self.u.reg_write(UC_ARM64_REG_PC,at+4)
    else:self.ret()
    self.stopped=True;self.u.emu_stop();return
   if name=='a733_counter_clock_now':self.time+=1;self.counter_calls+=1
   super().code(u,at,n,unused)
   if name=='xQueueGenericSend':
    pending=[(handle,slot) for handle,slot in self.waiters.items()
     if slot['queue']==queue and slot['blocked'] and self.time<slot['until']]
    if pending:
     assert not self.holders.get(queue,0)
     handle,slot=min(pending,key=lambda row:(-self.task_priorities[row[0]],row[1]['ticket']))
     slot['blocked']=False
     target=next(task for task in self.runnable_tasks if task['handle']==handle);target['wake']=self.time
     self.wait_events.append(['wake',self.time,handle,queue,self.task])
     if self.task_priorities[handle]>self.task_priorities[self.task] and self.eligible():
      self.boundary=('unblock',0);self.stopped=True;self.u.emu_stop();return
   if name=='a733_counter_clock_now':
    # Complete the modeled counter function before considering an interrupt:
    # its out parameter, return value and caller PC are already coherent.
    if self.time>=self.next_tick:self.tick_pending=True
    if self.tick_pending and self.eligible():
     self.next_tick=(self.time//1000+1)*1000;self.tick_pending=False
     self.tick_events.append([self.time,self.task]);self.boundary=('tick',0)
     self.stopped=True;self.u.emu_stop()
 image=base.base.Image(a.image);m=Machine(image,raw,contract)
 m.observe_fault=a.observe_first_fault;m.fault_observation=None
 fault=image.address('a733_dp_fault');m.hooks[fault]='a733_dp_fault';m.u.hook_add(UC_HOOK_CODE,m.code,begin=fault,end=fault)
 if a.observe_pc is not None:m.u.hook_add(UC_HOOK_CODE,m.code,begin=a.observe_pc,end=a.observe_pc)
 m.yield_sites=set()
 for start,data in image.parts:
  for offset in range(0,len(data)-3,4):
   if int.from_bytes(data[offset:offset+4],'little')==0xd4000d21:
    site=start+offset;m.yield_sites.add(site);m.u.hook_add(UC_HOOK_CODE,m.code,begin=site,end=site)
 if image.symbols.get('vTaskSuspend'):
  at=image.address('vTaskSuspend');m.hooks[at]='vTaskSuspend';m.u.hook_add(UC_HOOK_CODE,m.code,begin=at,end=at)
 source=a.resume.resolve() if a.resume else directory
 if a.resume:
  previous=json.loads((source/'manifest.json').read_text())
  assert previous['image_sha256']==image_hash and previous['session_sha256']==sha(a.session) and previous['contract_sha256']==sha(a.contract)
  for name,row in previous['files'].items():
   file=(source/name).resolve();assert source in file.parents
   assert file.stat().st_size==row['bytes'] and sha(file)==row['sha256']
  assert json.loads((source/'failure.json').read_text())['resumable']
 state=pickle.loads((source/('stopped.pickle' if a.resume else 'negotiated.pickle')).read_bytes())
 for at,count,data in state['ram']:
  if at<0x41000000:m.map_mmio(at,count)
  if isinstance(data,bytes):payload=data
  else:
   file=Path(data).resolve();records=previous['files'] if a.resume else manifest['files']
   assert file.parent==source and file.name in records
   assert file.stat().st_size==count==records[file.name]['bytes'] and sha(file)==records[file.name]['sha256']
   payload=file.read_bytes()
  assert len(payload)==count
  m.u.mem_write(at,payload)
 m.u.context_restore(state['cpu'])
 for key in ('time','scheduler','task','tasks','holders','registers','clocks','mapped_pages'):setattr(m,key,state[key])
 with a.image.open('rb') as stream:
  for segment in ELFFile(stream).iter_segments():
   if segment['p_type']=='PT_LOAD' and not(int(segment['p_flags'])&2):
    assert bytes(m.u.mem_read(int(segment['p_vaddr']),len(segment.data())))==segment.data()
 assert bytes(m.u.mem_read(0x41f00000,len(raw)))==raw
 assert bytes(m.u.mem_read(m.record,len(contract)))==contract
 assert bytes(m.u.mem_read(0x42004000,len(contract)))==contract
 scheduler=pickle.loads((source/('stopped-scheduler.pickle' if a.resume else 'scheduler.pickle')).read_bytes())
 m.next_tick=scheduler.get('next_tick',(m.time//1000+1)*1000)
 m.tick_pending=scheduler.get('tick_pending',False);m.tick_events=scheduler.get('tick_events',[])
 m.counter_calls=0;slice_trace=[];wire_trace=[]
 tasks=scheduler['tasks'];last=scheduler['last'];sequence=scheduler['sequence']
 m.runnable_tasks=tasks;m.task_priorities={task['handle']:task['priority'] for task in tasks}
 m.waiters=scheduler.get('waiters',{});m.wait_sequence=scheduler.get('wait_sequence',0);m.wait_events=scheduler.get('wait_events',[])
 assert scheduler['phase'] in ('detached-validation-complete','scheduler-boundary','scheduler-task')
 interrupted=scheduler.get('active_handle') if scheduler['phase']=='scheduler-task' else None
 if scheduler['phase']=='scheduler-task':assert interrupted in [t['handle'] for t in tasks]
 # Bind terminal output to values obtained from actual restored constructors
 # and negotiated provider state, before any continuation executes.
 sf=image.layouts['a733_displayport_session'];bf=image.layouts['a733_board_session']
 session=m.app+m.af['session'];mf=image.layouts['dp_memory'];memory_at=m.app+m.af['memory'];ef=image.layouts['device_memory_extent']
 nf=image.layouts['dp_native'];native_at=m.app+m.af['native'];tf=image.layouts['a733_typec_dp_token'];token_at=native_at+nf['token']
 expected=dict(epoch=m.get(session+sf['board']+bf['epoch']),generation=m.get(session+sf['board']+bf['generation']),lease=m.get(session+sf['board']+bf['lease']))
 for name in ('cpu','physical','bytes'):expected['allocation_'+name]=m.get(memory_at+mf['extent']+ef[name])
 expected['pixel_digest']=bytes(m.u.mem_read(memory_at+mf['pixel_digest'],32))
 expected['build_id']=bytes(m.u.mem_read(image.address('a733_smp_build_id'),32))
 for name in ('generation','hpd_irq','polarity','pin_assignment','hpd'):expected['token_'+name]=m.get(token_at+tf[name],8 if name in ('generation','hpd_irq') else 4)
 baseline={key:value.hex() if isinstance(value,bytes) else value for key,value in expected.items()}
 if (directory/'expected.json').is_file():assert baseline==json.loads((directory/'expected.json').read_text())
 if a.resume:
  # An interrupted continuation cannot promote corrupted restored fields into
  # new expected values. Keep the original negotiated baseline unchanged.
  assert baseline==json.loads((source/'expected.json').read_text())
 save(a.output/'expected.json',baseline)
 hw=lower.TypeCHardware(lambda:m.time,polarity=1)
 hw.restore(pickle.loads((source/('stopped-hardware.pickle' if a.resume else 'hardware.pickle')).read_bytes()))
 display=DisplayHardware(m,controller)
 if a.resume:
  saved=pickle.loads((source/'stopped-display.pickle').read_bytes())
  for key,value in saved['sink'].items():setattr(display.sink,key,value)
  for key in ('frame_due','frame_ends','clears','events'):setattr(display,key,saved[key])
 else:
  # Explicit modeled inherited register images. The native implementation
  # checks/adopts these facts; this does not measure or qualify real clocks.
  sf=image.layouts['a733_displayport_session'];session=m.app+m.af['session'];ccu=0x02002000
  seed={ccu:m.get(session+sf['reference'],4),ccu+0x140:m.get(session+sf['video1_pll'],4),
   ccu+0x13c0:m.get(session+sf['serdes_cfg'],4),0x0709016c:m.get(session+sf['rtc_serdes0'],4),
   ccu+0x2e0:0xf4036300,ccu+0x1928:0x20002,ccu+0x19a8:0x20000,
   ccu+0x5c0:m.registers.get(ccu+0x5c0,0)|(1<<5),ccu+0x5e0:m.registers.get(ccu+0x5e0,0)|(1<<11),
   ccu+0x5e4:0x40302,0x070101ac:1}
  for i,off in enumerate((0x500,0x580,0x588)):seed[ccu+off]=m.get(session+sf['board']+image.layouts['a733_board_session']['fabric']+4*i,4)
  for i,off in enumerate((0x58c,0x5b4)):seed[ccu+off]=m.get(session+sf['iommu_gates']+4*i,4)
  m.registers.update(seed)
 for at,count in ((0x02002000,0x2000),(0x03900000,0x20000),(0x05000000,0x400000),(0x05510000,0x1000),(0x05730000,0x40000),(0x06c01000,0xa0000),(0x07060000,0xb000)):
  m.map_mmio(at,count)
 for at,value in m.registers.items():m.put(at,value,4)
 def read(at,width):
  v=hw.read(at)
  if v is None:v=display.read(at,width)
  v=m.registers.get(at,0) if v is None else v
  return v
 def write(at,width,value):
  m.mmio_in_progress=True
  handled=hw.write(at,value)
  if handled and at in (hw.TWI0+0xc,hw.TWI1+0xc):
   bus=hw.bus[at&~4095];wire_trace.append([m.time,at,value,bus['due'],bus['wire'],bus['stop']])
  if not handled:display.write(at,width,value)
  m.mmio_in_progress=False
 original_read=read
 def checked_read(at,width):
  m.mmio_in_progress=True;value=original_read(at,width);m.mmio_in_progress=False;return value
 m.mmio_read=checked_read;m.mmio_write=write;m.mmio_in_progress=False
 terminal=m.app+m.af['terminal'];active=None;phase='scheduler-boundary'
 try:
  while m.time<5000000 and sequence<1500000:
   ready=[t for t in tasks if t['wake']<=m.time]
   if not ready:m.time=min(t['wake'] for t in tasks);continue
   if interrupted is not None:
    active=next(t for t in tasks if t['handle']==interrupted);interrupted=None
   else:active=min(ready,key=lambda t:(-t['priority'],last[t['handle']]))
   if active['context'] is None:
    m.u.reg_write(UC_ARM64_REG_SP,(active['stack']+active['stack_bytes'])&~15)
    m.u.reg_write(UC_ARM64_REG_LR,0x43fff000);m.u.reg_write(UC_ARM64_REG_X0,0);pc=active['entry']
   else:m.u.context_restore(active['context']);pc=m.u.reg_read(UC_ARM64_REG_PC)
   m.task=active['handle'];m.boundary=None;m.stopped=False;m.callname=active['role'];phase='scheduler-task'
   started_us=m.time;started_calls=m.counter_calls
   m.u.emu_start(pc,0,count=1000000000)
   assert m.stopped and m.boundary,(active['role'],hex(m.u.reg_read(UC_ARM64_REG_PC)),m.trace[-25:])
   active['context']=m.u.context_save();phase='scheduler-boundary';active['runs']+=1
   kind,ticks=m.boundary
   if kind=='fault-observation':phase='scheduler-task';raise RuntimeError('paused before first native DisplayPort fault')
   if kind=='yield':active['yields']+=1;active['wake']=m.time;m.time+=1
   elif kind in ('tick','unblock'):active['wake']=m.time
   elif kind=='mutex':active['wake']=m.waiters[active['handle']]['until']
   elif kind=='suspend':active['wake']=1<<60
   else:assert ticks==1;active['delays']+=1;active['wake']=(m.time//1000+ticks)*1000
   last[active['handle']]=sequence;sequence+=1
   provider=m.app+m.af['typec']+image.layouts['a733_typec_native']['provider'];ccf=image.layouts['a733_typec']
   slice_trace.append([started_us,m.time,active['role'],kind,ticks,active['wake'],m.counter_calls-started_calls,
    m.get(provider+ccf['state'],4),m.get(provider+ccf['step'],4),m.get(provider+ccf['pd_turn'],4),
    m.get(provider+ccf['published']+image.layouts['a733_typec_token']['observed_us'])])
   if (active['role']=='display' and m.get(native_at+nf['stage'],4)==6 and
       not m.get(native_at+nf['controller']+image.layouts['a733_dp']['self']) and
       not (a.output/'pre-controller').exists()):
    # Keep the last coherent application boundary before the first controller
    # operation, so later lower-model fixes need not repeat PD or supply work.
    point=a.output/'pre-controller';point.mkdir();m.checkpoint(point,'negotiated')
    (point/'scheduler.pickle').write_bytes(pickle.dumps(dict(sequence=sequence,last=last,phase='scheduler-boundary',tasks=tasks,next_tick=m.next_tick,tick_pending=m.tick_pending,tick_events=m.tick_events,waiters=m.waiters,wait_sequence=m.wait_sequence,wait_events=m.wait_events)))
    (point/'hardware.pickle').write_bytes(pickle.dumps(hw.export()))
    save(point/'inputs.json',inputs);save(point/'expected.json',baseline)
    files={f.name:dict(bytes=f.stat().st_size,sha256=sha(f)) for f in point.iterdir() if f.is_file()}
    save(point/'negotiated-manifest.json',dict(schema='a733-displayport-pre-controller-checkpoint-v1',image_sha256=image_hash,session_sha256=sha(a.session),contract_sha256=sha(a.contract),sources={str(a.hardware_source.resolve()):sha(a.hardware_source)},files=files,physical=False))
   if m.get(terminal,4):break
  else:raise AssertionError(('bounded completion not reached',m.time,sequence))
  reply=bytes(m.u.mem_read(m.app+m.af['reply'],1024));(a.output/'reply.bin').write_bytes(reply)
  rf=image.layouts['a733_displayport_reply'];word=lambda name:struct.unpack_from('<I',reply,rf[name])[0]
  qword=lambda name:struct.unpack_from('<Q',reply,rf[name])[0]
  for name in ('epoch','generation','lease','allocation_cpu','allocation_physical','allocation_bytes'):assert qword(name)==expected[name],name
  assert reply[rf['session_digest']:rf['session_digest']+32]==hashlib.sha256(raw).digest()
  for name in ('build_id','pixel_digest'):assert reply[rf[name]:rf[name]+32]==expected[name],name
  assert qword('pd_generation')==expected['token_generation']==expected['generation']
  assert qword('hpd_irq')==expected['token_hpd_irq']
  for name in ('polarity','pin_assignment','hpd'):assert word(name)==expected['token_'+name],name
  assert word('polarity') in (1,2) and word('pin_assignment') in (4,16) and word('hpd')==1
  assert tuple(word(name) for name in ('pixel_iova','pitch','width','height'))==(0x10001000,2560,1280,720)
  assert word('status')==1 and word('error')==0 and word('retained')==1
  assert word('stage')==6 and word('de_state')==3 and word('iommu_state')==5
  assert word('scanned_masters')==0x100 and word('guards')==1 and word('padding_valid')==1
  assert word('core_bytes')==352 and struct.unpack_from('<4I',reply,rf['core'])==(4,0,6,1)
  assert reply[rf['core']:rf['core']+352]==bytes(m.u.mem_read(native_at+nf['controller']+image.layouts['a733_dp']['result'],352))
  assert not any(reply[rf['reserved']:rf['reserved']+12]) and not any(reply[rf['tail']:])
  pixels=bytes(m.u.mem_read(expected['allocation_cpu']+36864,1843200))
  assert hashlib.sha256(pixels).digest()==expected['pixel_digest']
  assert display.clears and display.frame_ends and min(display.frame_ends)>max(display.clears)
  assert m.get(m.app+m.af['typec_stop'],4)==2 and not(hw.gpio&256)
  assert not m.registers[0x05740800] and not(m.registers[0x05731090]&(1<<31)) and not(m.registers[0x05008140]&1)
  m.checkpoint(a.output,'terminal')
  memory=[]
  for i,(at,count) in enumerate(m.regions):
   file=a.output/('terminal-ram-'+str(i)+'.bin');memory.append(dict(address=at,bytes=count,file=str(file),sha256=sha(file)))
  save(a.output/'terminal-memory.json',dict(image_sha256=image_hash,memory=memory,reply_sha256=sha(a.output/'reply.bin'),physical=False,scope=__doc__))
  save(a.output/'summary.json',dict(passed=True,physical=False,image_sha256=image_hash,session_sha256=sha(a.session),contract_sha256=sha(a.contract),
   time_us=m.time,sequence=sequence,frame_ends=display.frame_ends,clears=display.clears,tick_events=m.tick_events,mutex_events=m.wait_events,scope=__doc__))
  print('PASS actual composed native retained-stop reply')
 except BaseException as error:
  if active is not None and phase=='scheduler-task':active['context']=m.u.context_save()
  m.checkpoint(a.output,'stopped')
  (a.output/'stopped-scheduler.pickle').write_bytes(pickle.dumps(dict(sequence=sequence,last=last,phase=phase,tasks=tasks,active_handle=active['handle'] if active is not None and phase=='scheduler-task' else None,next_tick=m.next_tick,tick_pending=m.tick_pending,tick_events=m.tick_events,waiters=m.waiters,wait_sequence=m.wait_sequence,wait_events=m.wait_events)))
  (a.output/'stopped-hardware.pickle').write_bytes(pickle.dumps(hw.export()))
  (a.output/'stopped-display.pickle').write_bytes(pickle.dumps(display.export()))
  save(a.output/'failure.json',dict(error=repr(error),time_us=m.time,sequence=sequence,phase=phase,task=active['role'] if active else None,trace=m.trace[-40:],physical=False,resumable=not m.mmio_in_progress,first_fault=m.fault_observation))
  raise
 finally:
  save(a.output/'slice-trace.json',dict(columns=['start_us','end_us','task','boundary','ticks','wake_us','counter_calls','cc_state','cc_step','pd_turn','published_us'],rows=slice_trace))
  save(a.output/'wire-trace.json',dict(columns=['time_us','address','control','due_us','wire_state','stopping'],rows=wire_trace))
  for path,digest in inputs.items():assert sha(path)==digest,('input changed',path)
  files={str(p.relative_to(a.output)):dict(bytes=p.stat().st_size,sha256=sha(p)) for p in a.output.rglob('*') if p.is_file()}
  save(a.output/'manifest.json',dict(image_sha256=image_hash,session_sha256=sha(a.session),contract_sha256=sha(a.contract),files=files,physical=False))
if __name__=='__main__':main()
