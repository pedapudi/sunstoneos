#!/usr/bin/env python3
"""Execute linked UART/DMA leaves with modeled MMIO, RTOS and shared providers.

No CPU startup, routing, scheduling, cache visibility or physical wire is tested.
The constructor, native UART MMIO policy, UART DMA state, copied DMA service,
controller, descriptors and result producer execute their actual instructions.
"""
import argparse,hashlib,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
class Image:
 def __init__(self,path):
  self.layouts={};self.sizes={};self.functions={};self.symbols={}
  with path.open('rb') as f:
   e=ELFFile(f);self.parts=[(int(p['p_vaddr']),p.data()) for p in e.iter_segments() if p['p_type']=='PT_LOAD']
   for s in e.get_section_by_name('.symtab').iter_symbols():
    if s['st_value']:self.symbols.setdefault(s.name,set()).add(int(s['st_value']))
   for cu in e.get_dwarf_info().iter_CUs():
    unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
    for d in cu.iter_DIEs():
     if 'DW_AT_name' not in d.attributes:continue
     n=d.attributes['DW_AT_name'].value.decode()
     if d.tag=='DW_TAG_subprogram' and 'DW_AT_low_pc' in d.attributes:self.functions[unit,n]=int(d.attributes['DW_AT_low_pc'].value)
     if d.tag=='DW_TAG_structure_type' and 'DW_AT_byte_size' in d.attributes:
      fields={x.attributes['DW_AT_name'].value.decode():int(x.attributes['DW_AT_data_member_location'].value) for x in d.iter_children() if x.tag=='DW_TAG_member' and 'DW_AT_name' in x.attributes and 'DW_AT_data_member_location' in x.attributes}
      if fields:self.layouts[n]=fields;self.sizes[n]=int(d.attributes['DW_AT_byte_size'].value)
 def at(self,n,unit=None):
  if unit:
   m=[v for (u,k),v in self.functions.items() if u.endswith(unit) and k==n];assert len(m)==1,(n,m);return m[0]
  s=self.symbols[n];assert len(s)==1,(n,s);return next(iter(s))
class Machine:
 def __init__(self,image,session,case):
  self.i=image;self.case=case;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.reg_write(UC_ARM64_REG_PSTATE,4)
  for a,n in ((0x41000000,0x1200000),(0x43000000,0x1000000),(0x02000000,0x100000),(0x02500000,0x10000),(0x03400000,0x10000),(0x04600000,0x4000)):self.u.mem_map(a,n)
  for a,b in image.parts:
   if b:self.u.mem_write(a,b)
  self.now=1;self.task=1;self.scheduler=1;self.queues={};self.worker_ctx=None;self.worker_entry=None;self.in_worker=False;self.cache=[];self.writes=[];self.done=False;self.retains=0;self.owner_steps=0;self.channel_stops=[];self.late=0;self.bad=False;self.shifter_waits=0;self.usb_input=bytearray();self.usb_output=[];self.usb_mode=False
  self.app=image.at('uart');self.dma=image.at('uart_dma');self.providers=image.at('providers');self.board=image.at('board')
  self.put(self.app+image.layouts['uart_application']['session'],session)
  self.put(self.app+image.layouts['uart_application']['tx'],session[800:800+struct.unpack_from('<I',session,792)[0]])
  self.field(self.app,'uart_application','handle',1,8);self.field(self.app,'uart_application','deadline',5000000,8)
  bs=self.board+image.layouts['board_application']['session'];self.field(bs,'a733_board_session','deadline_us',60000000,8);self.val(bs+168,8,0)
  shared=self.providers+image.layouts['board_provider']['shared'];self.shared=shared
  counter=shared+image.layouts['a733_native_usb']['counter'];registry=shared+image.layouts['a733_native_usb']['clocks'];locks=shared+image.layouts['a733_native_usb']['locks'];self.locks=locks
  self.field(counter,'a733_counter_clock','initialized',1)
  for a,t in ((registry,'a733_clock_leases'),(locks,'a733_platform_locks')):
   self.field(a,t,'initialized',1);self.field(a,t,'now',image.at('a733_counter_clock_now'),8);self.field(a,t,'context',counter,8)
  self.hooks={}
  for n in ('xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','aarch64_device_translate','aarch64_device_cache_stride','aarch64_device_cache','a733_counter_clock_now','xQueueGenericCreateStatic','xTaskCreateStaticAffinitySet','xQueueGenericSend','xQueueReceive','board_live','a733_dma_startup_poll','a733_dma_startup_ready','a733_dma_startup_prepared','a733_uart_platform_poll','a733_uart_platform_close','a733_uart_platform_retain','board_fail','platform_panic','vTaskGenericNotifyGiveFromISR','xTaskGenericNotify','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','vTaskDelay'):
   if n in image.symbols:self.hooks[image.at(n)]=n
  self.u.hook_add(UC_HOOK_CODE,self.code);self.u.hook_add(UC_HOOK_MEM_WRITE,self.write);self.u.hook_add(UC_HOOK_MEM_READ,self.read)
  self.sysregs={}
  for a,b in image.parts:
   for off in range(0,len(b)-3,4):
    w=int.from_bytes(b[off:off+4],'little')
    if w&0xffffffe0 in (0xd53be000,0xd53be020):self.sysregs[a+off]=w&31
 def put(self,a,b):self.u.mem_write(a,bytes(b))
 def get(self,a,n=8):return int.from_bytes(self.u.mem_read(a,n),'little')
 def val(self,a,n,v):self.put(a,int(v).to_bytes(n,'little'))
 def field(self,a,t,n,v,size=4):self.val(a+self.i.layouts[t][n],size,v)
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def call(self,n,*args,unit=None):
  self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for k,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+k,v)
  self.u.emu_start(self.i.at(n,unit),0,count=3000000)
  assert self.u.reg_read(UC_ARM64_REG_PC)==0x43fff000,(n,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  return self.u.reg_read(UC_ARM64_REG_X0)
 def code(self,u,at,size,v):
  if at==0x43fff000:u.emu_stop();return
  if at in self.sysregs:u.reg_write(UC_ARM64_REG_X0+self.sysregs[at],self.now*24);u.reg_write(UC_ARM64_REG_PC,at+4);return
  n=self.hooks.get(at)
  if not n:return
  a=[u.reg_read(UC_ARM64_REG_X0+k) for k in range(6)]
  if n=='vTaskDelay':self.ret();u.emu_stop()
  elif n in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init'):self.ret(1)
  elif n in ('tud_task_ext','tud_vendor_n_write_flush'):self.ret(0)
  elif n=='tud_vendor_n_available':self.ret(len(self.usb_input))
  elif n=='tud_vendor_n_read':
   size=min(a[2],len(self.usb_input));self.put(a[1],self.usb_input[:size]);del self.usb_input[:size];self.ret(size)
  elif n=='tud_vendor_n_write':
   size=min(a[2],173)
   if size:self.usb_output.append(bytes(u.mem_read(a[1],size)))
   self.ret(size)
  elif n=='xTaskGetSchedulerState':self.ret(self.scheduler)
  elif n=='xTaskGetCurrentTaskHandle':self.ret(self.task)
  elif n=='board_live':self.ret(1)
  elif n=='aarch64_device_translate':
   ram=0x41000000<=a[0]<0x42200000;self.put(a[2],struct.pack('<QII',a[0],255 if ram else 0,3 if ram else 2));self.ret(1)
  elif n=='aarch64_device_cache_stride':self.ret(64)
  elif n=='aarch64_device_cache':self.cache.append(a[:3]);self.ret(1)
  elif n=='a733_counter_clock_now':self.val(a[1],8,self.now);self.ret(1)
  elif n=='xQueueGenericCreateStatic':self.queues[a[3]]={'size':a[1],'data':[]};self.ret(a[3])
  elif n=='xTaskCreateStaticAffinitySet':
   assert a[0]==self.i.at('worker','/drivers/dma/dma_service.c')
   self.worker_entry=a[0];self.worker_arg=a[3];self.worker_handle=a[6] if len(a)>6 else u.reg_read(UC_ARM64_REG_X6)
   assert self.call_privilege(a[0],a[3]);self.ret(self.worker_handle)
  elif n=='xQueueGenericSend':
   q=self.queues[a[0]];q['data'].append(bytes(u.mem_read(a[1],q['size'])));self.ret(1)
  elif n=='xQueueReceive':
   q=self.queues[a[0]]
   if q['data']:self.put(a[1],q['data'].pop(0));self.ret(1)
   else:
    self.ret(0)
    if self.in_worker:u.emu_stop()
  elif n in ('a733_dma_startup_ready','a733_dma_startup_prepared','xTaskGenericNotify'):self.ret(1)
  elif n=='a733_dma_startup_poll':self.ret(0)
  elif n=='a733_uart_platform_poll':
   self.ret(3 if self.get(self.app+self.i.layouts['uart_application']['stage'],4)==3 else 2)
  elif n=='a733_uart_platform_close':self.ret(0)
  elif n=='a733_uart_platform_retain':
   self.retains+=1
   # Shared retention would poison worker health; require completed stop first.
   assert not self.get(0x04601030,4),('retained active DMA',self.case)
   self.ret(4)
  else:raise AssertionError(('unexpected fatal',n,self.case))
 def call_privilege(self,entry,arg):
  # The actual startup hook is checked separately by root; verify exact worker
  # creation metadata here without nested emulation.
  return entry==self.i.at('worker','/drivers/dma/dma_service.c') and arg==self.dma+self.i.layouts['uart_dma_application']['service']
 def write(self,u,access,a,n,v,_):
  if not (0x02502000<=a<=0x025020a4 or 0x04601000<=a<0x04603000):return
  self.writes.append((a,v))
  if a==0x0250208c and v==3:
   assert self.get(0x04601100,4)==1 and self.get(0x04601140,4)==1
   self.transfer=True
  if a in (0x04601100,0x04601140):
   ch=(a-0x04601100)//64
   if v:self.val(0x04601030,4,self.get(0x04601030,4)|(1<<ch))
   elif self.get(0x04601030,4)&(1<<ch):self.channel_stops.append(ch);self.val(0x04601030,4,self.get(0x04601030,4)&~(1<<ch))
  if a in (0x04601138,0x04601178):
   # W1C: preserve hardware pending state after the modeled write completes.
   self.pending_w1c=(a,self.get(a,4)&~v)
 def read(self,u,access,a,n,v,_):
  if getattr(self,'pending_w1c',None):
   p,x=self.pending_w1c;self.val(p,4,x);self.pending_w1c=None
  if a==0x02502014:
   lsr=0x60|(2 if self.case=='line-error' and getattr(self,'transfer',False) else 0)
   if self.case=='shifter-pending' and self.done and self.get(0x0250208c,4)==0 and self.shifter_waits<2:lsr=0x20;self.shifter_waits+=1
   self.val(a,4,lsr)
  if a==0x0250208c and self.case=='disable-mismatch' and self.done and self.get(a,4)==0:self.val(a,4,3)
  if a==0x0250207c:self.val(a,4,6)
  if getattr(self,'transfer',False) and not self.done and self.case not in ('timeout','line-error'):
   self.done=True
   e=self.dma+self.i.layouts['uart_dma_application']['extent'];base=self.get(e+self.i.layouts['device_memory_extent']['cpu']);length=self.get(self.app+792,4)
   self.put(base+8192,bytes(self.u.mem_read(base+12288,length)))
   if self.case=='bad-echo':self.put(base+8192,b'\xff')
   for ch in (0,1):self.val(0x04601138+64*ch,4,4)
 def work(self):
  self.in_worker=True;self.task=self.worker_handle
  if self.worker_ctx:self.u.context_restore(self.worker_ctx)
  else:
   self.u.reg_write(UC_ARM64_REG_SP,0x43fa0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000);self.u.reg_write(UC_ARM64_REG_X0,self.worker_arg);self.u.reg_write(UC_ARM64_REG_PC,self.worker_entry)
  
  try:self.u.emu_start(self.u.reg_read(UC_ARM64_REG_PC),0,count=1000000)
  except Exception as ex:raise AssertionError(('worker setup',hex(self.u.reg_read(UC_ARM64_REG_PC)),hex(self.u.reg_read(UC_ARM64_REG_PSTATE)),self.get(self.app+self.i.layouts['uart_application']['stage'],4))) from ex
  self.worker_ctx=self.u.context_save();self.in_worker=False;self.task=1
 def setup(self):
  assert self.call('uart_dma_construct')==1
  self.scheduler=2;self.val(0x04601034,4,3)
  p=self.app+self.i.layouts['uart_application']['platform'];core=p+self.i.layouts['a733_uart_platform']['uart'];native=self.app+self.i.layouts['uart_application']['native']
  self.field(native,'a733_uart_platform_native','initialized',1);self.field(native,'a733_uart_platform_native','owner_task',1,8);self.field(native,'a733_uart_platform_native','port',2);self.field(native,'a733_uart_platform_native','locks',self.locks,8)
  self.field(p,'a733_uart_platform','initialized',1)
  plan=core+self.i.layouts['a733_uart']['plan'];self.field(plan,'a733_uart_plan','base',0x02502000,8);self.field(plan,'a733_uart_plan','actual_baud',115384)
  self.field(core,'a733_uart','state',2);self.field(core,'a733_uart','context',native,8)
  ops=core+self.i.layouts['a733_uart']['ops']
  for off,n in enumerate(('read32','write32','now')):self.val(ops+8*off,8,self.i.at(n,'/drivers/uart/a733_uart_platform_native.c'))
 def run(self):
  self.setup()
  for step in range(500):
   self.call('uart_dma_step')
   if self.get(self.dma+self.i.layouts['uart_dma_application']['activated'],4):self.work()
   self.now+=1000
   if self.case=='timeout' and getattr(self,'transfer',False) and self.now<5000000:self.now=5000000
   result=self.get(self.app+self.i.layouts['uart_application']['result'],4)
   if result:break
  assert result==(1 if self.case in ('success','shifter-pending') else 2),(self.case,result,self.get(self.app+self.i.layouts['uart_application']['stage'],4),self.writes[-8:])
  assert (self.get(0x0250208c,4)==0 or self.case=='disable-mismatch') and self.get(0x04601030,4)==0
  if result==1:
   if self.case=='shifter-pending':assert self.shifter_waits==2
   before=self.get(self.app+self.i.layouts['uart_application']['payload_bytes'],4);self.now=6000000;assert self.call('uart_dma_step')==1
   payload=bytes(self.u.mem_read(self.app+self.i.layouts['uart_application']['payload'],before));return payload
  assert self.retains;before=self.retains;self.call('uart_dma_step');assert self.retains>before
  return None
 def usb(self,payload,session):
  self.usb_mode=True;self.scheduler=2
  self.field(self.app,'uart_application','result',1);self.field(self.app,'uart_application','payload_bytes',len(payload))
  self.put(self.app+self.i.layouts['uart_application']['payload'],payload)
  self.put(self.app+self.i.layouts['uart_application']['digest'],hashlib.sha256(payload).digest())
  self.put(self.app+self.i.layouts['uart_application']['session_digest'],hashlib.sha256(session).digest())
  self.put(self.board+self.i.layouts['board_application']['session'],session[:592])
  self.field(self.board+self.i.layouts['board_application']['session'],'a733_board_session','deadline_us',60000000,8)
  self.usb_input.extend(b'\0');context=None;ack=0
  def batch():
   nonlocal context
   if context:self.u.context_restore(context)
   else:
    self.u.reg_write(UC_ARM64_REG_SP,0x43f60000);self.u.reg_write(UC_ARM64_REG_X0,0);self.u.reg_write(UC_ARM64_REG_PC,self.i.at('board_usb0_task'))
   self.u.emu_start(self.u.reg_read(UC_ARM64_REG_PC),0,count=1000000);context=self.u.context_save()
  for _ in range(32):
   batch()
   if sum(map(len,self.usb_output))==128+len(payload):break
  frame=b''.join(self.usb_output);assert len(frame)==128+len(payload)
  self.usb_input.extend(b'\0')
  for _ in range(3):batch()
  assert b''.join(self.usb_output)==frame and self.usb_input==b'\0'
  # Each completion is matched against bytes accepted, including the final
  # short fragment. A second request cannot replace the private response first.
  for part in self.usb_output:self.call('tud_vendor_tx_cb',0,len(part))
  batch();assert not self.usb_input
  return frame

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--session',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--usb-payload',type=Path);p.add_argument('--case',choices=('success','line-error','timeout','bad-echo','shifter-pending','disable-mismatch'),default='success');a=p.parse_args()
 i=Image(a.image);m=Machine(i,a.session.read_bytes(),a.case);payload=None if a.usb_payload else m.run();a.output.mkdir(parents=True,exist_ok=False)
 if payload:(a.output/'payload.bin').write_bytes(payload)
 if a.usb_payload:(a.output/'frame.bin').write_bytes(m.usb(a.usb_payload.read_bytes(),a.session.read_bytes()))
 (a.output/'result.json').write_text(json.dumps(dict(result='pass',physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),session_sha256=hashlib.sha256(a.session.read_bytes()).hexdigest(),case=a.case,actual_linked_constructor_service_controller_uart=not bool(a.usb_payload),actual_linked_usb_copy_and_completion=bool(a.usb_payload),cache_operations=m.cache,channel_stops=m.channel_stops,mmio_writes=m.writes),indent=2)+'\n')
if __name__=='__main__':main()
