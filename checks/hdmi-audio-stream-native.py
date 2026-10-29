#!/usr/bin/env python3
"""Execute HDMI streaming constructors and output/DMA tasks with modeled hardware.

The producer uses native upload APIs; USB transport is checked separately.
Architectural services, serialized scheduling, DMA/FIFO and IRQ delivery are
modeled. This establishes no physical timing, visibility or sink operation.
"""
import argparse,copy,hashlib,importlib.util,json,pickle,struct,traceback
from pathlib import Path
from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
n=module('finite_hdmi',ROOT/'checks/hdmi-audio-native.py')
class Image(n.Image):
 def __init__(self,path):
  self.path=path;self.symbols={};self.layouts={};self.sizes={};self.parts=[];self.raw=path.read_bytes()
  with path.open('rb') as f:
   e=n.ELFFile(f)
   for p in e.iter_segments():
    if p['p_type']=='PT_LOAD':self.parts.append((int(p['p_vaddr']),int(p['p_memsz']),p.data(),int(p['p_flags'])))
   for s in e.get_section_by_name('.symtab').iter_symbols():
    if s.name and s['st_value']:self.symbols.setdefault(s.name,set()).add(int(s['st_value']))
   text=e.get_section_by_name('.text');self.text=[int(text['sh_addr']),text.data()]
   for cu in e.get_dwarf_info().iter_CUs():
    for d in cu.iter_DIEs():
     if d.tag!='DW_TAG_structure_type' or 'DW_AT_name' not in d.attributes or 'DW_AT_byte_size' not in d.attributes:continue
     name=d.attributes['DW_AT_name'].value.decode()
     if not name.startswith(('hdmi_','a733_','dma_','device_')):continue
     fields={m.attributes['DW_AT_name'].value.decode():int(m.attributes['DW_AT_data_member_location'].value) for m in d.iter_children() if m.tag=='DW_TAG_member' and 'DW_AT_name' in m.attributes and 'DW_AT_data_member_location' in m.attributes}
     if name in self.layouts:assert self.layouts[name]==fields,('ambiguous layout',name)
     self.layouts[name]=fields;self.sizes[name]=int(d.attributes['DW_AT_byte_size'].value)

class Machine(n.Machine):
 def __init__(self,i,raw,contract,manifest,edid):
  super().__init__(i,raw,contract,manifest,edid)
  self.u.mem_map(0x04100000,0x10000);self.regions.append((0x04100000,0x10000))
  for address,value in {0x070101ac:self.registers.get(0x070101ac,0)|1,0x020025a4:0x30001,0x020025c0:1<<9,0x020025e0:1<<31}.items():
   self.registers[address]=value;self.put(address,value,4)
  self.power.pmic.update({0x11:self.power.pmic[0x11]|1,0x1a:70,0x23:8,0x3f:16})
  at=i.address('tusb_rhport_init');self.hooks[at]='usb-prepared';self.u.hook_add(UC_HOOK_CODE,self.code,begin=at,end=at)
  self.usb_prepared=False;self.saved_preopen=False;self.saved_playing=False;self.saved_tail=False
  self.parts=[];self.irq_count=0;self.active=False;self.reads=0;self.hash_bytes=0;self.copy_bytes=0;self.counters={};self.irq_sent=False;self.payload_hash_bytes=0;self.stack_min={}
  for name in ('ulPortSetInterruptMask','vPortClearInterruptMask','xTaskGenericNotifyFromISR','vTaskCoreAffinitySet','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR','vTaskGenericNotifyGiveFromISR'):
   if name in i.symbols:
    at=i.address(name);self.hooks[at]=name;self.u.hook_add(UC_HOOK_CODE,self.code,begin=at,end=at)
  for name in ('storage_digest_bytes','memcpy','a733_dma_poll','a733_i2s_stream_poll'):
   at=i.address(name);self.u.hook_add(UC_HOOK_CODE,self.count,begin=at,end=at);self.counters[at]=name
 def count(self,u,at,size,unused):
  name=self.counters[at];self.counters[name]=self.counters.get(name,0)+1
  a=[u.reg_read(UC_ARM64_REG_X0+j) for j in range(3)]
  if name=='storage_digest_bytes':
   self.hash_bytes+=a[1]
   if self.i.address('stream')<=a[0]<self.i.address('stream')+self.i.sizes['hdmi_stream']:self.payload_hash_bytes+=a[1]
  if name=='memcpy' and 0x41c01000<=a[0]<0x41e00000:self.copy_bytes+=a[2]
 def code(self,u,at,size,unused):
  name=self.hooks.get(at)
  if name is None and self.get(at,4)&0xffffffe0==0xd53be020:self.time+=1
  if name=='aarch64_device_translate' and getattr(self,'inject_deadline',0) and 0x02535000<=u.reg_read(UC_ARM64_REG_X0)<0x02535080:
   self.time=self.inject_deadline;self.inject_deadline=0;self.deadline_injected=True
  if name=='xQueueGenericSend':
   q=self.queues.get(u.reg_read(UC_ARM64_REG_X0))
   if q and len(q['items'])==q['capacity']:self.ret(0);return
  if name=='usb-prepared':self.usb_prepared=True;self.stop('usb-prepared');return
  if name in ('vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR'):self.ret(0);return
  if name=='vTaskCoreAffinitySet':self.ret();return
  if name=='ulPortSetInterruptMask':self.ret(0);return
  if name=='vPortClearInterruptMask':self.ret();return
  if name in ('xTaskGenericNotifyFromISR','vTaskGenericNotifyGiveFromISR'):self.notifications[u.reg_read(UC_ARM64_REG_X0)]=1;self.ret(1);return
  return super().code(u,at,size,unused)
 def read(self,u,access,a,nbytes,value,unused):
  if 0x02535000<=a<0x02535080:self.reads+=1
  if a==0x02535018:
   value=(128 if not self.parts or self.time>=self.fifo_empty_at else 32)<<16;self.put(a,value,nbytes);return
  if a==0x04601030 and getattr(self,'drain_unknown',False):self.put(a,1,nbytes);return
  if a==0x02535028:self.put(a,sum(len(p) for p in self.parts)//2,nbytes);return
  return super().read(u,access,a,nbytes,value,unused)
 def write(self,u,access,a,nbytes,value,unused):
  if a==0x04601100 and value==0 and self.parts and self.time<self.dma_done:
   # A nominal channel disable drains its outstanding modeled bus writes.
   # Unknown-drain injection keeps STATUS busy independently below.
   self.dma_done=self.time+2;self.completion_reported=True
  if a==0x03400190:
   self.registers[0x03400110]=0;self.put(0x03400110,0,4)
  if a==0x0253501c:
   self.events.append([self.time,self.task,a,nbytes,value]);self.registers[a]=value
   if value&128:
    desc=self.registers[0x04601108];words=struct.unpack('<6I',bytes(u.mem_read(desc,24)));source,count=words[1],words[3]
    assert words[0]==0x03860280 and words[2]==0x02535020 and words[4:]==(8,0xfffff800),words
    assert 0x41c00000<=desc<0x41e00000 and desc%64==0 and 0<count<=4096
    self.parts.append(bytes(u.mem_read(source,count)));self.dma_started=self.time;self.dma_done=self.time+(count*1000000+191999)//192000
    self.fifo_empty_at=self.dma_done+500;self.completion_reported=False;self.irq_sent=False
   return
  if a==0x02535000:
   self.events.append([self.time,self.task,a,nbytes,value]);self.registers[a]=value;return
  return super().write(u,access,a,nbytes,value,unused)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--resume',type=Path);p.add_argument('--inputs-dir',type=Path,required=True);p.add_argument('--constructor-only',action='store_true');p.add_argument('--case',choices=('normal','corrupt','cancel','underrun','drain','tail-ready','deadline','busy','busy-reply'),default='normal');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 (a.output/'fixture-source.py').write_bytes(Path(__file__).read_bytes())
 i=Image(a.image)
 raw,contract,pcm,manifest,edid=[(a.inputs_dir/name).read_bytes() for name in ('session.bin','contract.bin','input.pcm','manifest.bin','sink.edid')]
 for name,data in zip(('session.bin','contract.bin','input.pcm','manifest.bin','sink.edid'),(raw,contract,pcm,manifest,edid)):(a.output/name).write_bytes(data)
 m=Machine(i,raw,contract,manifest,edid);tasks=[]
 m.identity=dict(image=n.sha(a.image),session=hashlib.sha256(raw).hexdigest(),contract=hashlib.sha256(contract).hexdigest(),pcm=hashlib.sha256(pcm).hexdigest(),manifest=hashlib.sha256(manifest).hexdigest())
 m.fixture_source_sha=n.sha(__file__);m.hardware_source_sha=n.sha(ROOT/'checks/displayport-typec-hardware.py')
 try:
  cl=i.layouts['a733_smp_contract'];m.time=struct.unpack_from('<Q',contract,cl['armed_at'])[0]//24+100
  if a.resume:
   provenance=json.loads(a.resume.with_name(a.resume.stem+'-identity.json').read_text());assert provenance['identity']==m.identity
   saved=pickle.loads(a.resume.read_bytes());assert n.sha(a.resume)==provenance['checkpoint_sha256']
   for address,count,name in saved['memory']:
    data=(a.resume.parent/name).read_bytes();assert len(data)==count and n.sha(a.resume.parent/name)==provenance['files'][name]['sha256'];m.u.mem_write(address,data)
   for key,value in saved.items():
    if key not in ('cpu','memory','task_contexts','power','fixture_source_sha','hardware_source_sha','hooks'):setattr(m,key,copy.deepcopy(value))
   m.power.restore(saved['power']);m.u.context_restore(saved['cpu']);tasks=copy.deepcopy(saved['task_contexts'])
  else:
   assert m.call('a733_smp_contract_valid',0x42004000,m.time*24)==1
   assert m.call('hdmi_session_prepare',0x42004000)==1
   assert m.call('a733_memory_reservations_prepare')==1
   assert m.call('a733_memory_uncached_prepare')==1
   m.call('a733_smp_diagnostic_start',budget=100000000);assert m.boundary==('scheduler',0) and len(m.tasks)==4
  app=i.address('app');af=i.layouts['hdmi_application'];audio=i.address('audio');nf=i.layouts['hdmi_audio_native'];stream=i.address('stream');sf=i.layouts['hdmi_stream']
  handles=[m.get(app+af['handles']+j*8) for j in range(3)];worker=m.get(audio+nf['service']+i.field('dma_service','worker'))
  for entry,name,depth,argument,priority,stack,handle in ([] if tasks else m.tasks):
   tasks.append(dict(entry=entry,handle=handle,stack=stack,bytes=depth*8,argument=argument,priority=priority,context=None,wake=m.time,last=0,runs=0))
  if not a.resume:m.checkpoint(a.output,'constructor',tasks)
  if a.constructor_only:return
  if a.case in ('deadline','busy','busy-reply'):
   m.scheduler=2
   if a.case=='deadline':
    m.task=worker;until=m.time+1000
    assert m.call('stream_health',audio,until)==0
    prior=m.reads;m.inject_deadline=until;m.deadline_injected=False
    assert m.call('stream_read',audio,0x02535018,0x43fe0000)==0
    assert m.deadline_injected and m.reads==prior and m.get(audio+nf['held'],4)==1
    assert m.get(audio+nf['failed'],4)==1 and worker in m.holders.values()
   else:
    m.task=handles[1];service=audio+nf['service'];queue=m.get(service+i.field('dma_service','requests'));q=m.queues[queue]
    if a.case=='busy':q['items']=[bytes(q['size']) for _ in range(q['capacity'])]
    else:
     client=service+i.field('dma_service','clients');replies=m.queues[m.get(client+i.field('dma_service_client','replies'))]
     def busy_reply():
      assert len(q['items'])==1;q['items'].pop()
      raw_reply=bytearray(i.sizes['dma_service_reply']);struct.pack_into('<Q',raw_reply,i.field('dma_service_reply','ticket'),m.get(stream+sf['request_ticket']))
      struct.pack_into('<I',raw_reply,i.field('dma_service_reply','result'),3);replies['items'].append(bytes(raw_reply))

    assert m.call('hdmi_audio_stream_request',1,100000,0,0x43fe0000)==0
    until=m.get(stream+sf['request_until'])
    if a.case=='busy-reply':busy_reply()
    for _ in range(3):
     m.time+=10;assert m.call('hdmi_audio_stream_request',1,100000,0,0x43fe0000)==0
     assert m.get(stream+sf['request_until'])==until
     if a.case=='busy-reply' and m.get(stream+sf['request_ticket']):busy_reply()
    m.time=until;assert m.call('hdmi_audio_stream_request',1,100000,0,0x43fe0000)==(1<<32)-1
   m.checkpoint(a.output,'terminal',tasks);n.save(a.output/'result.json',dict(passed=True,case=a.case,identity=m.identity,deadline=until));return

  for task in tasks:
   def stack_write(u,access,address,size,value,unused,handle=task['handle']):m.stack_min[handle]=min(m.stack_min.get(handle,address),address)
   m.u.hook_add(UC_HOOK_MEM_WRITE,stack_write,begin=task['stack'],end=task['stack']+task['bytes']-1)
  interrupted=m.active_handle if m.phase=='task-slice' else None
  m.scheduler=2;sequence=m.sequence
  if not interrupted:m.phase='scheduling'
  injected=False
  if a.case=='underrun':m.underrun=True;injected=True
  if a.case=='drain':m.drain_unknown=True;injected=True
  if a.case=='cancel':
   m.task=handles[2];assert m.call('hdmi_stream_cancel')==1;injected=True


  while m.time<15000000 and m.get(app+af['state'],4) not in (2,3):
   # The caller fills native WB slots only while producer credits exist.
   m.task=handles[2]
   head=m.get(stream+sf['head']);total=(len(pcm)+4095)//4096
   if m.usb_prepared and head==total-1 and not m.saved_tail:
    m.saved_tail=True;m.checkpoint(a.output,'before-tail',tasks)
   if a.case=='tail-ready' and head==total-1 and m.call('hdmi_stream_credits'):
    m.checkpoint(a.output,'tail-ready',tasks)
    n.save(a.output/'tail-ready.json',dict(image=n.sha(a.image),session_sha256=hashlib.sha256(raw).hexdigest(),ram_sha256=n.sha(a.output/'tail-ready-ram-0.bin'),time=m.time,handles=handles,ram='tail-ready-ram-0.bin'))
    return
   if m.usb_prepared and head<total and m.call('hdmi_stream_credits'):
    part=pcm[head*4096:(head+1)*4096];ptr=m.call('hdmi_stream_upload_buffer',head,0,len(part));assert ptr
    if a.case=='corrupt' and not injected:part=bytes([part[0]^1])+part[1:];injected=True
    m.u.mem_write(ptr,part);assert m.call('hdmi_stream_uploaded',head,len(part))==1
   elif m.usb_prepared and head==total and not m.get(stream+sf['eof'],4):assert m.call('hdmi_stream_end',head)==1
   if m.dma_started and not m.irq_sent and m.time>=m.dma_done and m.registers.get(0x03400110,0)&0x8000:
    m.call('dma_service_i2s_irq_handler',audio+nf['irq']);m.irq_count+=1;m.irq_sent=True
    next(t for t in tasks if t['handle']==worker)['wake']=m.time
   ready=[t for t in tasks if t['wake']<=m.time]
   if not ready:
    wake=min(t['wake'] for t in tasks)
    if m.dma_started and not m.irq_sent and m.time<m.dma_done:wake=min(wake,m.dma_done)
    m.time=wake;continue
   task=next(t for t in tasks if t['handle']==interrupted) if interrupted else min(ready,key=lambda t:t['last']);interrupted=None;m.task=task['handle'];m.boundary=None;m.stopped=False
   if task['context'] is None:
    m.u.reg_write(UC_ARM64_REG_SP,(task['stack']+task['bytes'])&~15);m.u.reg_write(UC_ARM64_REG_LR,0x43fff000);m.u.reg_write(UC_ARM64_REG_X0,task['argument']);at=task['entry']
   else:m.u.context_restore(task['context']);at=m.u.reg_read(UC_ARM64_REG_PC)
   m.phase='task-slice';m.active_handle=task['handle'];m.sequence=sequence
   m.u.emu_start(at,0,count=1000000);assert m.stopped,('task budget',hex(m.u.reg_read(UC_ARM64_REG_PC)))
   task['context']=m.u.context_save();task['runs']+=1;sequence+=1;task['last']=sequence;m.sequence=sequence;m.phase='scheduling';m.active_handle=None
   if m.get(audio+nf['stage'],4)==4 and m.get(stream+sf['stage'],4)==0 and not m.saved_preopen:
    m.saved_preopen=True;m.checkpoint(a.output,'pre-open',tasks)
   if len(m.parts)>=5 and not m.saved_playing:
    m.saved_playing=True;m.checkpoint(a.output,'playing',tasks)
   kind,ticks=m.boundary
   if kind=='usb-prepared':
    task['wake']=1<<60
    m.checkpoint(a.output,'usb-prepared',tasks)
    n.save(a.output/'usb-prepared.json',dict(image=n.sha(a.image),session_sha256=hashlib.sha256(raw).hexdigest(),ram_sha256=n.sha(a.output/'usb-prepared-ram-0.bin'),time=m.time,handles=handles,ram='usb-prepared-ram-0.bin'))
    continue
   assert kind not in ('return','scheduler','suspend'),kind
   task['wake']=(m.time//1000+1)*1000 if kind in ('delay','queue','notify') else m.time
  m.checkpoint(a.output,'terminal',tasks)
  n.save(a.output/'terminal.json',dict(image=n.sha(a.image),session_sha256=hashlib.sha256(raw).hexdigest(),ram_sha256=n.sha(a.output/'terminal-ram-0.bin'),time=m.time,handles=handles,ram='terminal-ram-0.bin'))
  result=bytes(m.u.mem_read(i.address('hdmi_audio_result'),160));(a.output/'result.bin').write_bytes(result);words=struct.unpack('<12Q',result[:96]);state=m.get(app+af['state'],4)
  if a.case!='normal':
   assert m.get(audio+nf['record']+i.field('device_memory_allocation','state'),4)==2
   channel=audio+nf['service']+i.field('dma_service','channels')
   assert m.get(channel+i.field('dma_service_channel','holds_buffers'),4)==1
   if a.case=='corrupt':assert m.get(stream+sf['held'])!=0
   assert injected
   assert state==(2 if a.case=='cancel' else 3) and words[3]==(4 if a.case=='cancel' else 3),(a.case,state,words)
   assert words[11]==0
   n.save(a.output/'result.json',dict(passed=True,case=a.case,identity=m.identity,source_consumed_bytes=words[6],periods=len(m.parts),retained=True,wire_drained=False))
   return
  assert state==2 and words[1]==2 and words[3]==2 and words[6]==len(pcm),(state,words,m.time,len(m.parts),m.trace[-12:])
  assert b''.join(m.parts)==pcm and words[11]==0
  assert m.copy_bytes==m.payload_hash_bytes==len(pcm)
  n.save(a.output/'result.json',dict(passed=True,scope=__doc__,identity=m.identity,service_messages=m.get(audio+nf['service']+i.field('dma_service','clients')+i.field('dma_service_client','next_ticket')),busy_replies=None,periods=len(m.parts),irq_count=m.irq_count,time_us=m.time,copy_bytes=m.copy_bytes,hash_bytes=m.hash_bytes,payload_hash_bytes=m.payload_hash_bytes,stack_bytes={str(t['handle']):t['stack']+t['bytes']-m.stack_min.get(t['handle'],t['stack']+t['bytes']) for t in tasks},i2s_reads=m.reads,counters={str(k):v for k,v in m.counters.items()},tasks=[{k:v for k,v in t.items() if k!='context'} for t in tasks]))
 except BaseException:
  m.checkpoint(a.output,'interrupted',tasks);(a.output/'failure.txt').write_text(traceback.format_exc());raise
if __name__=='__main__':main()
