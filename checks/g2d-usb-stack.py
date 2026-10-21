#!/usr/bin/env python3
"""Execute the final G2D USB task and linked TinyUSB FIFO/endpoint stack.

DCD physical transfers, native provider readiness and scheduling are modeled.
Completed G2D output is restored from its owner's hashed native checkpoint;
this check neither executes G2D nor establishes physical USB operation.
"""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct
from collections import Counter
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE,UC_ARM64_REG_Q0,UC_ARM64_REG_FPCR,UC_ARM64_REG_FPSR,UC_ARM64_REG_SP_EL0
ROOT=Path(__file__).resolve().parents[1]
EXPECTED='4e043dad7e691ad50c4164dd9085ae118a0e0911d7fd8aea93e16610e7966328'
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
base=module('g2d_usb_image',ROOT/'checks/storage-read-native.py')
def metadata(path):
 types={};sizes={};enums={}
 def resolve(d):
  while d.tag in ('DW_TAG_typedef','DW_TAG_const_type','DW_TAG_volatile_type','DW_TAG_atomic_type'):d=d.get_DIE_from_attribute('DW_AT_type')
  return d
 def members(d,offset=0):
  result={}
  for c in resolve(d).iter_children():
   if c.tag!='DW_TAG_member':continue
   at=offset+int(c.attributes.get('DW_AT_data_member_location',type('V',(),{'value':0})()).value);name=c.attributes.get('DW_AT_name');t=resolve(c.get_DIE_from_attribute('DW_AT_type'))
   prefix=name.value.decode() if name else ''
   if prefix:result[prefix]=at
   if t.tag in ('DW_TAG_structure_type','DW_TAG_union_type'):
    result.update({(prefix+'.' if prefix else '')+k:v for k,v in members(t,at).items()})
  return result
 with path.open('rb') as f:
  for cu in ELFFile(f).get_dwarf_info().iter_CUs():
   for d in cu.iter_DIEs():
    if d.tag=='DW_TAG_enumeration_type':
     enums.update({c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_const_value'].value) for c in d.iter_children() if c.tag=='DW_TAG_enumerator'})
    name=d.attributes.get('DW_AT_name')
    if name and name.value.decode() in ('dcd_event_t','g2d_application','board_application') and d.tag in ('DW_TAG_typedef','DW_TAG_structure_type'):
     t=resolve(d)
     if 'DW_AT_byte_size' in t.attributes:types[name.value.decode()]=members(t);sizes[name.value.decode()]=int(t.attributes['DW_AT_byte_size'].value)
 return types,sizes,enums
def usb_variables(path):
 result={}
 with path.open('rb') as f:
  dwarf=ELFFile(f).get_dwarf_info()
  for cu in dwarf.iter_CUs():
   if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/g2d/usb.c'):continue
   for d in cu.iter_DIEs():
    if d.tag!='DW_TAG_variable' or 'DW_AT_name' not in d.attributes or 'DW_AT_location' not in d.attributes:continue
    loc=d.attributes['DW_AT_location']
    if loc.form!='DW_FORM_exprloc':continue
    ops=DWARFExprParser(dwarf.structs).parse_expr(loc.value)
    if len(ops)==1 and ops[0].op_name in ('DW_OP_addr','DW_OP_addrx'):result[d.attributes['DW_AT_name'].value.decode()]=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dwarf.get_addr(cu,ops[0].args[0])
 return result
class Machine:
 def __init__(self,image,types,sizes,enums,speed):
  self.image=image;self.t=types;self.sizes=sizes;self.enums=enums;self.speed=speed;self.mps=512 if speed==2 else 64
  self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.reg_write(UC_ARM64_REG_PSTATE,4) # Linked privileged task entry is EL1t, using SP_EL0.
  for at,size in [(0x41000000,0x3000000),(0x48000000,0x1000000),(0x78000000,0x1000000),(0x90000000,0x1000000)]:self.cpu.mem_map(at,size)
  for at,data in image.parts:
   if data:self.cpu.mem_write(at,data)
  self.names={};self.seen=Counter();self.opened={};self.pending={};self.events=[];self.packets=[];self.transfers=[];self.started=False;self.now=1000000;self.stop=None;self.failure=False
  modeled=['board_live','board_fail','a733_native_usb_step','a733_native_usb_health','a733_smp_counter','vTaskDelay','dcd_init','dcd_int_enable','dcd_int_disable','dcd_edpt_open','dcd_edpt_xfer','dcd_edpt_close_all','dcd_edpt0_setup_begin','dcd_edpt_stall','dcd_sof_enable']
  observed=['tusb_rhport_init','dcd_event_handler','tud_task_ext','vendord_open','vendord_xfer_cb','tud_vendor_n_write','tud_vendor_n_read','tud_vendor_tx_cb','usbd_edpt_claim','usbd_edpt_release','usbd_edpt_xfer','tu_fifo_write_n_access_mode','tu_fifo_read_n_access_mode']
  self.modeled=set(modeled)
  for name in modeled+observed:
   if image.symbols.get(name):self.names[image.address(name)]=name
  for at in set(self.names)|{0x90fff000}:self.cpu.hook_add(UC_HOOK_CODE,self.hook,begin=at,end=at)
 def write(self,at,value,n=4):self.cpu.mem_write(at,int(value).to_bytes(n,'little'))
 def word(self,at,n=4):return int.from_bytes(self.cpu.mem_read(at,n),'little')
 def ret(self,value=0):self.cpu.reg_write(UC_ARM64_REG_X0,value);self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 def hook(self,cpu,at,size,unused):
  if at==0x90fff000:self.stop='returned';cpu.emu_stop();return
  name=self.names[at];self.seen[name]+=1
  if name not in self.modeled:return
  a=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(5)]
  if name=='board_live':self.ret(not self.failure);return
  if name=='board_fail':self.failure=True;self.ret();return
  if name in ('a733_native_usb_step','a733_native_usb_health'):self.ret(1);return
  if name=='a733_smp_counter':self.ret(self.now*24);return
  if name=='vTaskDelay':self.now+=1000;self.ret();self.stop='yield';cpu.emu_stop();return
  assert a[0]&255==0,(name,a)
  if name=='dcd_edpt_open':
   desc=bytes(cpu.mem_read(a[1],7));assert desc[:2]==b'\x07\x05' and desc[3]==2
   ep=desc[2];assert ep in (2,0x81);assert int.from_bytes(desc[4:6],'little')==self.mps;self.opened[ep]=self.mps
  elif name=='dcd_edpt_xfer':
   ep,ptr,count=a[1]&255,a[2],a[3]&65535;assert ep not in self.pending,(ep,count,self.pending.keys());assert ep in (0,0x80,2,0x81)
   if ep in (2,0x81):assert ep in self.opened and count<=512
   self.pending[ep]=dict(pointer=ptr,count=count,data=bytes(cpu.mem_read(ptr,count)) if count and ep&128 else b'')
  elif name=='dcd_edpt_stall':raise AssertionError('Unexpected endpoint stall')
  elif name=='dcd_edpt_close_all':self.pending={k:v for k,v in self.pending.items() if k in (0,0x80)};self.opened={}
  self.ret(1)
 def call(self,name,*args):
  saved=self.cpu.context_save();self.stop=None
  self.cpu.reg_write(UC_ARM64_REG_PSTATE,4);self.cpu.reg_write(UC_ARM64_REG_SP_EL0,0x90efd000);self.cpu.reg_write(UC_ARM64_REG_SP,0x90efd000);self.cpu.reg_write(UC_ARM64_REG_LR,0x90fff000)
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.emu_start(self.image.address(name),0,count=4000000);assert self.stop=='returned',(name,self.stop,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
  value=self.cpu.reg_read(UC_ARM64_REG_X0);self.cpu.context_restore(saved);return value
 def tick(self):
  for ep,p in self.pending.items():
   if ep&128:assert bytes(self.cpu.mem_read(p['pointer'],p['count']))==p['data'],'Endpoint bytes changed before completion'
  if not self.started:
   self.started=True;self.cpu.reg_write(UC_ARM64_REG_PSTATE,4);self.cpu.reg_write(UC_ARM64_REG_SP_EL0,0x90ffd000);self.cpu.reg_write(UC_ARM64_REG_SP,0x90ffd000);self.cpu.reg_write(UC_ARM64_REG_X0,0);at=self.image.address('board_usb0_task')
  else:self.cpu.context_restore(self.context);at=self.cpu.reg_read(UC_ARM64_REG_PC)
  self.stop=None;self.cpu.emu_start(at,0,count=8000000);assert self.stop=='yield',hex(self.cpu.reg_read(UC_ARM64_REG_PC));self.context=self.cpu.context_save()
 def event(self,kind,**fields):
  t=self.t['dcd_event_t'];raw=bytearray(self.sizes['dcd_event_t']);raw[t['event_id']]=self.enums['DCD_EVENT_'+kind]
  for name,value in fields.items():
   if name=='setup_received':raw[t[name]:t[name]+8]=value
   else:raw[t[name]:t[name]+(1 if name in ('xfer_complete.ep_addr','xfer_complete.result') else 4)]=value.to_bytes(1 if name in ('xfer_complete.ep_addr','xfer_complete.result') else 4,'little')
  self.cpu.mem_write(0x90e00000,bytes(raw));self.call('dcd_event_handler',0x90e00000,0)
 def complete(self,ep,data=None):
  p=self.pending.pop(ep);count=p['count'] if data is None else len(data);assert count<=p['count']
  if data is not None:self.cpu.mem_write(p['pointer'],data)
  if ep==0x81:
   assert bytes(self.cpu.mem_read(p['pointer'],count))==p['data']
   self.transfers.append(p['data']);self.packets.extend([self.mps]*(count//self.mps)+([count%self.mps] if count%self.mps else []) if count else [0])
  self.event('XFER_COMPLETE',**{'xfer_complete.ep_addr':ep,'xfer_complete.result':0,'xfer_complete.len':count})
 def enumerate(self):
  self.tick();assert not self.failure
  self.event('BUS_RESET_END',**{'bus_reset.speed':self.speed});self.tick()
  self.event('SETUP_RECEIVED',setup_received=struct.pack('<BBHHH',0,9,1,0,0));self.tick()
  assert self.opened=={2:self.mps,0x81:self.mps};assert self.pending[0x80]['count']==0 and 2 in self.pending
  self.complete(0x80);self.tick();assert not self.failure
 def request(self,data):self.complete(2,data);self.tick()

def main():
 global EXPECTED,ACTIVE_MODEL,ACTIVE_OUTPUT
 p=argparse.ArgumentParser(description=__doc__)
 for k in ('image','checkpoint','output'):p.add_argument('--'+k,type=Path,required=True)
 p.add_argument('--expected-image',default=EXPECTED,help='Pinned final ELF SHA256; must match native checkpoint')
 p.add_argument('--speed',choices=['full','high'],required=True)
 p.add_argument('--variant',choices=['retained','exact-total'],default='retained')
 a=p.parse_args();assert len(a.expected_image)==64 and all(x in '0123456789abcdef' for x in a.expected_image);EXPECTED=a.expected_image;assert hashlib.sha256(a.image.read_bytes()).hexdigest()==EXPECTED
 # Checkpoint schema supplied by the native G2D owner; mapped ranges are hashed.
 checkpoint=json.loads(a.checkpoint.read_text());e=base.Image(a.image,layout_names=());t,sizes,enums=metadata(a.image);m=Machine(e,t,sizes,enums,2 if a.speed=='high' else 1)
 assert checkpoint['image_sha256']==EXPECTED
 ACTIVE_MODEL=m;ACTIVE_OUTPUT=a.output
 frozen=[Path(__file__),ROOT/'checks/storage-read-native.py',ROOT/'apps/g2d/usb.c',a.image,a.checkpoint]
 for row in checkpoint['memory']:
  q=Path(row['file']);frozen.append(q if q.is_absolute() else a.checkpoint.parent/q)
 identities={str(q.resolve()):hashlib.sha256(q.read_bytes()).hexdigest() for q in frozen}
 for row in checkpoint['memory']:
  path=Path(row['file']);path=path if path.is_absolute() else a.checkpoint.parent/path;data=path.read_bytes();assert hashlib.sha256(data).hexdigest()==row['sha256'];m.cpu.mem_write(row['address'],data)
 a.output.mkdir(parents=True,exist_ok=True)
 (a.output/'inputs.json').write_text(json.dumps(identities,indent=2)+'\n')
 g=e.address('g2d');gt=t['g2d_application'];board=e.address('board');bt=t['board_application']
 def field(name,n=4):return m.word(g+gt[name],n)
 uv=usb_variables(a.image)
 for name in ['response','response_bytes','sent','delivered','acked','acking','terminal_zlp']:
  if name not in uv:uv[name]=e.address(name)
 def private(name,n=8):return m.word(uv[name],n)
 if 'session_sha256' in checkpoint:assert hashlib.sha256(bytes(m.cpu.mem_read(g+gt['session'],904))).hexdigest()==checkpoint['session_sha256']
 assert field('result')==1 and field('copied',8)==field('bytes',8)==16384
 payload_at=field('payload',8);payload=bytes(m.cpu.mem_read(payload_at,field('bytes',8)))
 assert hashlib.sha256(payload).digest()==bytes(m.cpu.mem_read(g+gt['digest'],32))
 if a.variant=='exact-total':
  # A separate transport-only terminal fixture exercises an exact HS multiple.
  # It reuses copied bytes but does not claim a second executed/admitted G2D job.
  payload=payload[:16192];m.write(g+gt['bytes'],len(payload),8)
  m.cpu.mem_write(g+gt['digest'],hashlib.sha256(payload).digest())
  for name,value in dict(width=1,height=506,stride=32,dx=0,dy=0,w=1,h=506,sx=0,sy=0).items():m.write(g+gt['session.'+name],value)
 setup=a.output/'configured.pkl.gz'
 regs=[*[UC_ARM64_REG_X0+i for i in range(29)],UC_ARM64_REG_SP,UC_ARM64_REG_SP_EL0,UC_ARM64_REG_LR,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE,UC_ARM64_REG_FPCR,UC_ARM64_REG_FPSR,*[UC_ARM64_REG_Q0+i for i in range(32)]]
 # FP/X29 has a noncontiguous Unicorn register ID.
 from unicorn.arm64_const import UC_ARM64_REG_FP
 regs.append(UC_ARM64_REG_FP)
 if setup.exists():
  saved=pickle.loads(gzip.decompress(setup.read_bytes()));assert saved['image']==EXPECTED and saved['speed']==a.speed and saved['variant']==a.variant and saved['checkpoint_sha256']==hashlib.sha256(a.checkpoint.read_bytes()).hexdigest()
  for at,data in saved['memory']:m.cpu.mem_write(at,data)
  for reg,value in saved['registers'].items():m.cpu.reg_write(reg,value)
  for key,value in saved['attributes'].items():setattr(m,key,value)
  m.context=m.cpu.context_save()
 else:
  m.enumerate()
  saved=dict(image=EXPECTED,speed=a.speed,variant=a.variant,checkpoint_sha256=hashlib.sha256(a.checkpoint.read_bytes()).hexdigest(),memory=[(at,bytes(m.cpu.mem_read(at,count))) for at,count in [(0x41000000,0x3000000),(0x48000000,0x1000000),(0x78000000,0x1000000),(0x90000000,0x1000000)]],registers={reg:m.cpu.reg_read(reg) for reg in regs},attributes={key:getattr(m,key) for key in ['seen','opened','pending','events','packets','transfers','started','now','stop','failure']})
  setup.write_bytes(gzip.compress(pickle.dumps(saved),mtime=0))
 proof={name:m.seen[name] for name in ['tusb_rhport_init','dcd_event_handler','tud_task_ext','vendord_open','usbd_edpt_claim','usbd_edpt_xfer']}
 assert all(proof.values()),proof
 print('PASS actual TinyUSB configured stack for '+a.speed+'/'+a.variant,flush=True)
 m.request(b'\0');m.tick();assert 0x81 in m.pending and private('response_bytes')==192+len(payload)
 header=bytes(m.cpu.mem_read(uv['response'],192))
 assert struct.unpack_from('<4I',header)==(0x44324753,1,1,len(payload))
 assert header[96:128]==hashlib.sha256(payload).digest()
 assert private('delivered',1)==private('acked',1)==0
 expected=header+payload;early_ack=False;pending_final=None;first=len(m.transfers);samples=[]
 for iteration in range(10000):
  if 0x81 not in m.pending:m.tick();continue
  pending=m.pending[0x81];done=sum(len(x) for x in m.transfers[first:]);remaining=len(expected)-done
  assert pending['count']<=remaining or pending['count']==0
  assert pending['data']==expected[done:done+pending['count']]
  # No early short/ZLP may split the logical header+payload response.
  if pending['count']<remaining:assert pending['count'] and pending['count']%m.mps==0
  # Hold an early ACK in the real RX FIFO while the final data/ZLP is pending.
  if remaining==pending['count'] and not early_ack:
   generation=m.word(board+bt['session.generation'],8)
   ack=b'\1'+struct.pack('<Q',generation)+hashlib.sha256(payload).digest()
   m.request(ack);assert not m.failure and private('acked',1)==private('acking',1)==private('delivered',1)==0
   early_ack=True
  if not pending['count']:
   assert remaining==0 and len(expected)%m.mps==0
   assert private('terminal_zlp',1)==1 and private('delivered',1)==0 and private('acked',1)==0
  before=private('sent');m.complete(0x81);m.tick();after=private('sent')
  if after>before:samples.append(dict(completed=done+pending['count'],sent_before=before,sent_after=after))
  if private('response_bytes')==0:break
 else:raise AssertionError('Bounded response transfer made no terminal progress')
 wire=b''.join(m.transfers[first:]);assert wire==expected and not m.failure
 transfer_lengths=[len(x) for x in m.transfers[first:]]
 assert transfer_lengths[-1]==(0 if len(expected)%m.mps==0 else len(expected)%512)
 assert samples and max(x['sent_after'] for x in samples)>4096
 assert private('delivered',1)==1 and 0x81 not in m.pending
 # ACK may be processed only now; its first byte was not consumed before final
 # physical completion. The remaining 40 bytes bind generation and output SHA.
 for _ in range(4):m.tick()
 assert private('acked',1)==1 and not m.failure
 m.request(b'\0');m.tick();start_ack=len(m.transfers)
 for _ in range(8):
  if 0x81 in m.pending:m.complete(0x81)
  m.tick()
  if private('response_bytes')==0:break
 acknowledged=b''.join(m.transfers[start_ack:]);assert len(acknowledged)==192
 assert struct.unpack_from('<2I',acknowledged,8)==(3,0) and acknowledged[16:]==header[16:]
 assert m.transfers[start_ack:][-1]==(b'' if m.speed==1 else acknowledged)
 assert all(m.seen[name] for name in ['tu_fifo_write_n_access_mode','tu_fifo_read_n_access_mode','vendord_xfer_cb','tud_vendor_tx_cb','usbd_edpt_claim','usbd_edpt_xfer'])
 (a.output/'response.bin').write_bytes(wire);(a.output/'acknowledged.bin').write_bytes(acknowledged)
 result=dict(passed=True,image_sha256=EXPECTED,checkpoint_sha256=hashlib.sha256(a.checkpoint.read_bytes()).hexdigest(),speed=a.speed,variant=a.variant,total_bytes=len(wire),payload_sha256=hashlib.sha256(payload).hexdigest(),dcd_transfer_lengths=transfer_lengths,modeled_dcd_packet_lengths=m.packets,fifo_refill_progress=samples,executed=dict(m.seen),early_ack_accepted_only_after_final_completion=True,modeled=['DCD physical transfer/completion effects','Native board/provider readiness','Scheduler/counter','Previously modeled accelerator result in retained checkpoint']+(['Synthetic terminal geometry/length only; no second G2D job/admission'] if a.variant=='exact-total' else []),scope=__doc__,physical_access=False)
 for path,sha in identities.items():assert hashlib.sha256(Path(path).read_bytes()).hexdigest()==sha
 (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
 print('PASS FIFO refills, header bridge, terminal completion and ACK ownership',flush=True)
if __name__=='__main__':
 try:main()
 except BaseException as error:
  if 'ACTIVE_MODEL' in globals():
   m=ACTIVE_MODEL;ACTIVE_OUTPUT.mkdir(parents=True,exist_ok=True)
   snapshot=dict(error=repr(error),image=EXPECTED,cpu=m.cpu.context_save(),task_context=getattr(m,'context',None),registers={reg:m.cpu.reg_read(reg) for reg in [*[UC_ARM64_REG_X0+i for i in range(29)],UC_ARM64_REG_SP,UC_ARM64_REG_SP_EL0,UC_ARM64_REG_PC,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE]},memory=[(at,bytes(m.cpu.mem_read(at,n))) for at,n in [(0x41000000,0x3000000),(0x48000000,0x1000000),(0x78000000,0x1000000),(0x90000000,0x1000000)]],attributes={k:getattr(m,k) for k in ['seen','opened','pending','events','packets','transfers','started','now','stop','failure']})
   (ACTIVE_OUTPUT/'interrupted.pkl.gz').write_bytes(gzip.compress(pickle.dumps(snapshot),compresslevel=1))
   (ACTIVE_OUTPUT/'failure.json').write_text(json.dumps(dict(error=repr(error)),indent=2)+'\n')
  raise
