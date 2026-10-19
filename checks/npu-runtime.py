#!/usr/bin/env python3
"""Actual linked runtime USB/mailbox/client; service and hardware replies modeled."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
base=module('npu_runtime_layout',ROOT/'checks/storage-read-native.py');host=module('npu_runtime_host',ROOT/'apps/npu-runtime/host.py')
class Machine:
 def __init__(self,image):
  self.image=image;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(0x40000000,0x04000000)
  for at,data in image.parts:
   if data:self.u.mem_write(at,data)
  self.ticks=24000000;self.client_context=None;self.client_started=False;self.closed=False;self.stop_ready=True
  self.client_suspended=False;self.current='call';self.incoming=bytearray();self.tx=[];self.max_tx=128;self.operations=[];self.inputs=[];self.job=0;self.ticket=0;self.operation=0;self.completions=1;self.timeout=False
  self.c=0x43000000;self.slot=image.address('npu_runtime');self.layout=image.layouts['npu_runtime_slot'];self.reply=image.layouts['npu_service_reply']
  names=['a733_npu_application_ready','a733_npu_application_client','npu_convolution_send','npu_convolution_collect','npu_convolution_output','a733_npu_application_stopped','npu_service_irq_completions','npu_service_irq_stopped','vTaskSuspend','vTaskDelay','npu_single_core_usb_poll','a733_npu_application_observer_admit','a733_observe_deadline','a733_usb_poll','tusb_rhport_init','tud_task_ext','tud_mounted','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush']
  self.hooks={image.address(n):n for n in names};self.u.hook_add(UC_HOOK_CODE,self.code)
  self.prepared=dict(epoch=5,generation=7,lease=9,build_id=self.data(image.address('a733_smp_build_id'),32).hex())
  at=image.address('a733_observe_identity');ll=image.layouts['a733_observe_identity']
  for k,v in {**{k:self.prepared[k] for k in ('epoch','generation','lease')},'armed_at':self.ticks-24,'deadline':self.ticks+480000000}.items():self.put(at+ll[k],v)
 def put(self,a,v,n=8):self.u.mem_write(a,int(v).to_bytes(n,'little'))
 def data(self,a,n):return bytes(self.u.mem_read(a,n))
 def get(self,a,n=8):return int.from_bytes(self.data(a,n),'little')
 def state(self):return self.get(self.slot+self.layout['state'],4)
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def code(self,cpu,at,size,unused):
  if at==0x43fff000:self.u.emu_stop();return
  word=self.get(at,4)
  if word&0xffffffe0==0xd53be020:
   self.u.reg_write(UC_ARM64_REG_X0+(word&31),self.ticks);self.u.reg_write(UC_ARM64_REG_PC,at+4);self.ticks+=24;return
  name=self.hooks.get(at)
  if name is None:return
  a=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(7)]
  if name=='a733_npu_application_client':self.ret(self.c)
  elif name in ('a733_npu_application_ready','npu_single_core_usb_poll','a733_npu_application_observer_admit','tusb_rhport_init','tud_mounted'):self.ret(1)
  elif name=='npu_service_irq_completions':self.ret(self.completions)
  elif name=='npu_service_irq_stopped':self.ret(int(self.closed))
  elif name=='a733_npu_application_stopped':self.ret(int(self.closed and self.stop_ready))
  elif name=='npu_convolution_send':
   assert a[0]==self.c;self.operation=a[1];self.operations.append(a[1]);self.ticket+=1;self.put(a[6],self.ticket)
   if a[1]==2:
    assert not self.closed;self.job=self.ticket;self.inputs.append(self.data(a[2],a[3]))
   if a[1]==5:self.closed=True
   self.ret(1)
  elif name=='npu_convolution_collect':
   assert a[1]==self.ticket
   if self.timeout:self.ticks+=24000000;self.ret(0);return
   self.u.mem_write(a[2],bytes(40));op=self.operation
   st={1:2,2:3,0:6,4:2,5:9}[op]
   if op==0:self.completions=len(self.inputs)+1
   for k,v in [('ticket',self.ticket),('job_ticket',0 if op in (1,5) else self.job),('state',st),('owns_session',int(op!=5))]:self.put(a[2]+self.reply[k],v,8 if k in ('ticket','job_ticket') else 4)
   self.ret(1)
  elif name=='npu_convolution_output':
   raw=self.inputs[-1];self.u.mem_write(a[1],bytes((raw[i%len(raw)]+i*3)%256 for i in range(a[2])));self.ret(1)
  elif name=='vTaskDelay':self.ret();self.ticks+=24000;self.u.emu_stop()
  elif name=='vTaskSuspend':self.client_suspended=True;self.ret();self.u.emu_stop()
  elif name=='tud_vendor_n_available':self.ret(len(self.incoming))
  elif name=='tud_vendor_n_read':
   n=min(a[2],len(self.incoming),17);self.u.mem_write(a[1],bytes(self.incoming[:n]));del self.incoming[:n];self.ret(n)
  elif name=='tud_vendor_n_write':
   n=min(a[2],self.max_tx);self.tx.append(self.data(a[1],n));self.ret(n)
  else:self.ret()
 def call(self,name,*args):
  self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  self.u.emu_start(self.image.address(name),0,count=3000000)
  return self.u.reg_read(UC_ARM64_REG_X0)
 def client(self):
  if self.client_suspended:return
  if self.client_context:self.u.context_restore(self.client_context)
  else:
   self.u.reg_write(UC_ARM64_REG_SP,0x43ff0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000);self.u.reg_write(UC_ARM64_REG_PC,self.image.address('client',unit='/apps/npu-runtime/main.c'))
  self.u.emu_start(self.u.reg_read(UC_ARM64_REG_PC),0,count=3000000);self.client_context=self.u.context_save()
 def usb(self,ack=True):
  before=len(self.tx);self.call('a733_observe_privileged')
  if ack:
   for raw in self.tx[before:]:self.call('tud_vendor_tx_cb',0,len(raw))
 def exchange(self,packet,clients=True):
  self.incoming.extend(packet);start=len(self.tx)
  for _ in range(500):
   self.usb()
   if clients:self.client()
   raw=b''.join(self.tx[start:])
   if len(raw)==1040:return raw
  raise AssertionError('Runtime exchange incomplete')
def negatives(image_path,out):
 out.mkdir(parents=True,exist_ok=True);image=base.Image(image_path);results={}
 def header(m,packet):
  m.u.mem_write(0x43100000,bytes(packet[:64]));return m.call('npu_runtime_header',0x43100000)
 def done():
  m=Machine(image);ll=m.layout;count=m.get(image.address('a733_npu_application_reference_bytes'))
  header(m,host.packet(1,host.SUBMIT,m.prepared,1,5000000,bytes(m.get(image.address('a733_npu_application_input_bytes')))))
  for k,v in [('state',host.DONE),('start',m.ticks),('end',m.ticks+24),('job',2),('jobs',1),('irq_count',2)]:m.put(m.slot+ll[k],v,4 if k in ('state','jobs','irq_count') else 8)
  return m
 m=Machine(image);packet=bytearray(host.packet(1,host.SUBMIT,m.prepared,1,5000000,bytes(432)));packet[16]^=1;header(m,packet);assert m.state()==host.FAILED;results['stale-epoch']=True
 m=Machine(image);header(m,host.packet(1,host.SUBMIT,m.prepared,1,5000000,bytes(431)));assert m.state()==host.FAILED;results['wrong-shape']=True
 m=Machine(image);header(m,host.packet(1,host.SUBMIT,m.prepared,1,5000000,bytes(432)));m.ticks=m.get(m.slot+m.layout['deadline']);m.u.mem_write(0x43100000,b'x');m.call('npu_runtime_payload',0x43100000,1);assert m.state()==host.FAILED and not m.operations;results['late-incomplete-upload']=True
 m=Machine(image);header(m,host.packet(1,host.SUBMIT,m.prepared,2,5000000,bytes(432)));m.put(m.slot+m.layout['state'],host.IDLE,4);header(m,host.packet(2,host.SUBMIT,m.prepared,2,5000000,bytes(432)));assert m.state()==host.FAILED;results['reused-request-id']=True
 m=Machine(image);frame=m.exchange(host.packet(1,host.SUBMIT,m.prepared,1,5000000,bytes(432)),False);frame=m.exchange(host.packet(2,host.READ,m.prepared,1),False);assert host.decode(frame,2,m.prepared,1)['state']==host.QUEUED and m.state()==host.QUEUED;results['pending-read-callback-keeps-request']=True
 for partial in (False,True):
  m=done();m.incoming.extend(host.packet(1,host.READ,m.prepared,1))
  for _ in range(4):m.usb(False)
  assert not m.tx
  if partial:m.usb(False);assert sum(map(len,m.tx))==128
  m.ticks=m.get(m.slot+m.layout['deadline'])
  for _ in range(10):m.usb(False)
  raw=b''.join(m.tx)
  if partial:
   assert len(raw)==128
   try:host.decode(raw,1,m.prepared,1)
   except ValueError:pass
   else:raise AssertionError('Truncated success accepted')
  else:assert host.decode(raw,1,m.prepared,1)['state']==host.FAILED
  assert m.state()==host.FAILED;results['partial-success-expiry' if partial else 'unsent-success-expiry']=True
 m=Machine(image);m.timeout=True;m.client();assert m.operations==[1] and m.state()==host.FAILED;results['collect-timeout-retains-exact-ticket']=True
 m=Machine(image);m.stop_ready=False;header(m,host.packet(1,host.CLOSE,m.prepared,1,5000000))
 for _ in range(8):
  m.client();m.ticks+=24000000
  if m.state()==host.FAILED:break
 assert m.closed and m.state()==host.FAILED;results['close-without-local-stop-not-published']=True
 (out/'result.json').write_text(json.dumps(dict(image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),cases=results,scope='Only malformed/stale/expiry/response-lifetime and pending service-ticket branches; modeled provider/service/scheduler effects. No nominal sequence rerun or physical qualification.'),indent=2)+'\n');print('Focused NPU runtime lifetime cases passed')
def run(image_path,out):
 out.mkdir(parents=True,exist_ok=False);image=base.Image(image_path);m=Machine(image);results={};frames=[];sequence=0
 n=m.get(image.address('a733_npu_application_input_bytes'));count=m.get(image.address('a733_npu_application_reference_bytes'))
 for rid,data in enumerate((bytes(i%256 for i in range(n)),bytes((i*7+31)%256 for i in range(n))),1):
  sequence+=1;m.exchange(host.packet(sequence,host.SUBMIT,m.prepared,rid,5000000,data))
  for _ in range(100):
   sequence+=1;frame=m.exchange(host.packet(sequence,host.READ,m.prepared,rid));value=host.decode(frame,sequence,m.prepared,rid)
   if value['state']==host.DONE:break
  assert value['output']==bytes((data[i%n]+i*3)%256 for i in range(count)) and value['input_sha256']==hashlib.sha256(data).hexdigest()
  (out/f'output-{rid}.bin').write_bytes(value['output']);(out/f'frame-{rid}.bin').write_bytes(frame);frames.append(value)
 sequence+=1;frame=m.exchange(host.packet(sequence,host.CLOSE,m.prepared,3,5000000));value=host.decode(frame,sequence,m.prepared,3)
 if value['state']!=host.CLOSED:
  sequence+=1;frame=m.exchange(host.packet(sequence,host.READ,m.prepared,3));value=host.decode(frame,sequence,m.prepared,3)
 assert value['state']==host.CLOSED and value['jobs']==2 and value['irq_completions']==3 and m.closed
 assert m.operations==[1,2,0,4,2,0,4,5] and m.inputs[0]!=m.inputs[1]
 (out/'closed-frame.bin').write_bytes(frame);results['runtime-sequence']={'passed':True,'operations':m.operations,'distinct_inputs':2,'copied_outputs':2,'closed_after_stop':True}
 (out/'result.json').write_text(json.dumps(dict(image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),cases=results,scope='Actual linked retained USB fragment/response/callback, mailbox and NPU client instructions. Convolution service/API replies, generated output, startup health, scheduler, transport and stop modeled; unchanged wrapper/controller evidence reused. Injected input-derived bytes test copying, not inference or physical IRQ.'),indent=2)+'\n');print('Actual linked runtime input/repeated output/healthy CLOSE passed')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--negative-only',action='store_true');a=p.parse_args();(negatives if a.negative_only else run)(a.image,a.output)
