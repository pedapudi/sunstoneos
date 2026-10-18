#!/usr/bin/env python3
"""New two-client SD0 composition cases against one actual native image."""
import argparse,copy,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
base=module('storage_read_service_model',ROOT/'checks/storage-read-native.py')
class Machine(base.Machine):
 def __init__(self,image,session,byte_addressed=False):
  self.current=1;self.present=True;self.incoming=bytearray();self.dirty=set();self.created=[];self.byte_addressed=byte_addressed
  super().__init__(image,session,'service');self.max_tx=4384;self.first_sector=0;self.api_busy=False
  if byte_addressed:
   csd=(9<<80)|(5<<84)|(4095<<62)|(7<<47);self.csd=[(csd>>(96-32*i))&0xffffffff for i in range(4)]
  for name in ('xTaskCreateStatic','xQueueCreateMutexStatic'):self.hooks[image.address(name)]=name
  self.cpu.hook_add(UC_HOOK_MEM_WRITE,lambda cpu,access,a,n,v,unused:self.dirty.update(range(a&~4095,(a+n+4095)&~4095,4096)))
 def code(self,cpu,a,size,unused):
  name=self.hooks.get(a);args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='xTaskGetCurrentTaskHandle':self.ret(self.current)
  elif name=='xQueueSemaphoreTake' and self.api_busy:self.ret(0)
  elif name=='a733_pins_read_inputs_banked':self.write(args[4],0 if self.present else 1<<6);self.ret(0)
  elif name=='xQueueCreateMutexStatic':self.ret(1)
  elif name=='xTaskCreateStatic':
   entries={self.image.address('storage_read_task'):1,self.image.address('storage_read_client_task'):3}
   assert args[0] in entries and args[2]==2048 and args[3]==0 and args[4]==4
   self.created.append(entries[args[0]]);self.ret(entries[args[0]])
  elif name=='tud_vendor_n_available':self.ret(len(self.incoming))
  elif name=='tud_vendor_n_read':
   n=min(args[2],len(self.incoming));self.cpu.mem_write(args[1],bytes(self.incoming[:n]));del self.incoming[:n];self.ret(n)
  else:super().code(cpu,a,size,unused)
 def read_mmio(self,cpu,access,a,size,v,unused):
  if a==0x04020200:
   assert self.held and size==4 and self.fifo<self.total
   value=sum(((((self.first_sector+(self.fifo+i)//512)*13+(self.fifo+i)%512)*37+11)%256)<<(8*i) for i in range(4))
   self.fifo+=4;self.trace.append(['read',a,value]);self.write(a,value);return
  if a==0x04020038 and self.data_command==18 and self.fifo==self.total:self.regs[a]=self.regs.get(a,0)|(1<<14)
  super().read_mmio(cpu,access,a,size,v,unused)
 def write_mmio(self,cpu,access,a,size,v,unused):
  assert self.held and size==4 and a!=0x04020200
  self.trace.append(['write',a,v]);self.regs[a]=v
  if a==0x04020038:self.regs[a]=self.read(a)&~v
  if a==0x04020000:self.regs[a]=v&~7
  if a==0x03400198:self.regs[0x03400118]=self.regs.get(0x03400118,0)&~v
  if a!=0x04020018:return
  assert not v&(1<<10),'Media-write direction reached actual MMIO'
  self.regs[a]=v&~(1<<31)
  if v&(1<<21):self.regs[a]=0;return
  n=v&63;arg=self.regs.get(0x0402001c,0);self.commands.append([n,arg]);assert n in (0,8,55,41,2,3,9,7,13,16,17,18)
  self.regs[0x04020038]=4;self.regs[0x04020020]=(self.state<<9)|0x100;self.data_command=self.total=self.fifo=0
  if n==0:self.state=0
  elif n==8:assert arg==0x1aa;self.regs[0x04020020]=0x1aa
  elif n==55:self.regs[0x04020020]|=1<<5
  elif n==41:
   self.ocr+=1;self.regs[0x04020020]=(0x00300000 if self.byte_addressed else 0x40300000)|(0x80000000 if self.ocr>=2 else 0)
   if self.ocr>=2:self.state=1
  elif n in (2,9):
   for i,word in enumerate(self.cid if n==2 else self.csd):self.regs[0x0402002c-4*i]=word
   if n==2:self.state=2
  elif n==3:self.state=3;self.regs[0x04020020]=0x12340700
  elif n==7:self.state=4
  elif n in (13,16):self.regs[0x04020020]=0x900
  elif n in (17,18):
   total=self.regs[0x04020014];assert total==512 if n==17 else total%512==0 and 1024<=total<=4096
   if self.byte_addressed:assert arg%512==0
   self.first_sector=arg//512 if self.byte_addressed else arg
   self.total=total;self.data_command=n;self.regs[0x04020020]=0x900;self.regs[0x04020024]=0x900
 def invoke(self,client,name,*args):
  self.current=client;return self.call(name,*args)&0xffffffff
 def owner(self):self.invoke(1,'storage_read_task',0);self.time+=1000
 def native_client(self):self.invoke(3,'storage_read_client_task',0)
 def slot(self,i):return self.base+self.layout['slots']+i*self.image.sizes['storage_read_slot']
 def state_of(self,i):return self.read(self.slot(i)+self.image.layouts['storage_read_slot']['state'])
 def submit(self,client,first,blocks,request_id):
  self.cpu.mem_write(0x50040000,struct.pack('<2Q2I',first,request_id,blocks,0));self.write(0x50040100,0,8)
  r=self.invoke(client,'storage_read_submit',0x50040000,0x50040100)
  return (r if r<2**31 else r-2**32),self.read(0x50040100,8)
 def frame(self,client=2):
  assert self.invoke(client,'storage_read_copy',0x50010000,4384,0x50012000)==1
  return bytes(self.cpu.mem_read(0x50010000,self.read(0x50012000)))
 def usb_step(self,packet=b''):
  self.incoming.extend(packet);self.invoke(2,'board_usb0_task',0)
 def establish(self):
  raw=self.session;self.cpu.mem_write(self.board+self.image.layouts['board_application']['admitted_session'],raw[:592])
  self.write(self.board+self.image.layouts['board_application']['handles']+8,2,8)
  values=[0x4153373352444331,2,224,*struct.unpack_from('<2Q',raw,24),struct.unpack_from('<Q',raw,32)[0]^((1<<64)-1)]
  armed=23999900;end=armed+1440000000
  contract=struct.pack('<6Q',*values)+raw[48:80]+struct.pack('<14Q',armed,armed+24000000,end,end,struct.unpack_from('<Q',raw,40)[0],0,0,0,0,0,0,0,0x41f00000,880)+hashlib.sha256(raw).digest()
  self.cpu.mem_write(0x50020000,contract)
  assert self.call('a733_smp_contract_valid',0x50020000,24000000)==1
  assert self.call('storage_read_session_prepare',0x50020000)==1
  assert self.call('board_application_start')==1 and self.created==[3,1]
  for _ in range(1200):
   self.owner()
   if self.field('card_valid'):break
  assert self.field('card_valid')==1 and self.field('result')==3,(self.field('stage'),self.field('reason'))
  assert all(self.state_of(i)==3 for i in range(2))
  assert self.call('a733_mmc_platform_scopes',self.base+self.layout['config'],0x50031000)==1
 def checkpoint(self):
  pages=set(self.dirty)
  for name in ('storage_read','storage_read_consumer','board','providers'):
   a=self.image.address(name);n=self.image.symbol_sizes[name];pages.update(range(a&~4095,(a+n+4095)&~4095,4096))
  return {a:bytes(self.cpu.mem_read(a,4096)) for a in pages}
 def clone(self,pages):
  m=Machine(self.image,self.session,self.byte_addressed)
  for a,data in pages.items():m.cpu.mem_write(a,data)
  for k,v in self.__dict__.items():
   if k not in ('cpu','image','session','hooks'):setattr(m,k,copy.deepcopy(v))
  m.trace=[];m.commands=[];m.tx=[];m.incoming=bytearray();return m
 def complete(self,i,clients=False):
  for _ in range(400):
   if clients:self.native_client()
   self.owner()
   if self.state_of(i)==1:return
   assert self.field('result')!=2,(self.field('stage'),self.field('reason'))
  raise AssertionError('Read did not complete')
def payload(first,blocks):return bytes(((((first+i//512)*13+i%512)*37+11)%256) for i in range(blocks*512))
def run(image,session,out,cases=None):
 selected=set(cases) if cases else None
 if selected:assert selected<=set(['two-clients-repeat', 'ranges-identity', 'max-ticket-exhaustion', 'queued-cancel-retains-active', 'removal-permanent', 'usb-partial-cancel-terminal', 'usb-retention-overrides-rejection', 'usb-fragmentation-ack-reuse', 'usb-disconnect-retains', 'unread-deadline', 'byte-addressed-read'])
 out.mkdir(parents=True,exist_ok=True);raw=session.read_bytes();prep=module('storage_read_service_prepare',ROOT/'apps/storage-read/prepare.py');prep.embedded_reference(image.read_bytes(),raw)
 e=base.Image(image);e.sizes={};e.symbol_sizes={}
 with image.open('rb') as f:
  elf=ELFFile(f)
  for s in elf.get_section_by_name('.symtab').iter_symbols():
   if s.name in ('storage_read','storage_read_consumer','board','providers'):e.symbol_sizes[s.name]=int(s['st_size'])
  for cu in elf.get_dwarf_info().iter_CUs():
   for die in cu.iter_DIEs():
    if die.tag=='DW_TAG_structure_type' and 'DW_AT_name' in die.attributes and 'DW_AT_byte_size' in die.attributes:e.sizes[die.attributes['DW_AT_name'].value.decode()]=int(die.attributes['DW_AT_byte_size'].value)
 required=['storage_read_task','storage_read_client_task','storage_read_submit','storage_read_copy','storage_read_ack','storage_read_cancel','board_application_start','board_usb0_task']
 for name in required:e.address(name)
 initial=Machine(e,raw)
 checkpoint=out/'ready-checkpoint.json'
 if checkpoint.exists():
  saved=json.loads(checkpoint.read_text());assert saved['image_sha256']==hashlib.sha256(image.read_bytes()).hexdigest() and saved['session_sha256']==hashlib.sha256(raw).hexdigest()
  pages={int(a):bytes.fromhex(body) for a,body in saved['pages'].items()}
  for a,body in pages.items():initial.cpu.mem_write(a,body)
  for key,value in saved['state'].items():setattr(initial,key,value)
  initial.regs={int(a):v for a,v in initial.regs.items()}
 else:
  initial.establish();pages=initial.checkpoint()
  state={k:v for k,v in initial.__dict__.items() if k not in ('cpu','image','session','hooks','dirty','incoming')}
  checkpoint.write_text(json.dumps(dict(image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),session_sha256=hashlib.sha256(raw).hexdigest(),pages={str(a):body.hex() for a,body in pages.items()},state=state))+'\n')
 assert 'apbs0' in e.layouts['a733_pmic']
 (out/'preflight.json').write_text(json.dumps(dict(image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),symbols={n:e.address(n) for n in required},layouts=e.layouts,sizes=e.sizes,checkpoint_pages=len(pages),initial_commands=initial.commands,physical_access=False),indent=2)+'\n')
 decoder=module('storage_read_service_decoder',ROOT/'apps/storage-read/observe.py');results=json.loads((out/'result.json').read_text())['cases'] if (out/'result.json').exists() else {}
 (out/'idle-frame.bin').write_bytes(initial.frame())
 def record(name,m,extra=None):
  results[name]=dict(passed=True,commands=m.commands,mmio_accesses=len(m.trace),**(extra or {}))
  (out/(name+'-trace.json')).write_text(json.dumps(m.trace)+'\n');(out/'result.json').write_text(json.dumps(dict(passed=True,cases=results,physical_access=False,scope='Actual linked two-client application, service, MMC/native/core and copied USB paths; shared providers, scheduling/time, card/MMIO and transport completion modeled. Initial native setup checkpoint reused.'),indent=2)+'\n')
 if 'two-clients-repeat' not in results and (selected is None or 'two-clients-repeat' in selected):
  # Actual native consumer overlaps USB. One unread result stays immutable while
  # the other client performs its second acknowledged read through public API.
  m=initial.clone(pages);r,ticket=m.submit(2,19,2,1);assert r==1
  (out/'pending-frame.bin').write_bytes(m.frame())
  m.native_client();m.complete(0,True);frame=m.frame();value=decoder.decode(frame,raw)
  assert frame[288:]==payload(19,2) and value['ticket']==ticket and value['client']==0
  for _ in range(500):
   m.native_client();m.owner()
   if m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['result'])==1:break
  assert m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['completed'])==2
  assert m.frame()[288:]==frame[288:] and m.read(m.core+m.c['borrowed'],8)==0
  assert m.invoke(3,'storage_read_ack',ticket)==0 and m.invoke(2,'storage_read_ack',ticket+1)==0
  assert m.invoke(2,'storage_read_ack',ticket)==1
  assert m.submit(2,33554432-8,8,2)[0]==1
  (out/'pending-last-frame.bin').write_bytes(m.frame())
  m.complete(0);last=m.frame();(out/'last-frame.bin').write_bytes(last)
  assert last[288:]==payload(33554432-8,8);assert m.invoke(2,'storage_read_ack',decoder.decode(last,raw)['ticket'])==1
  (out/'native-frame.bin').write_bytes(frame);record('two-clients-repeat',m,dict(native_completed_reads=2))
 if 'ranges-identity' not in results and (selected is None or 'ranges-identity' in selected):
  # Refusals and copied input identity cause no controller access.
  m=initial.clone(pages);before=len(m.trace)
  for first,blocks,rid in [(0,0,1),(0,9,1),(33554432,1,1),(33554431,2,1),((1<<64)-1,8,1),(0,1,0)]:assert m.submit(2,first,blocks,rid)[0]==-1
  assert m.submit(9,0,1,1)[0]==-1 and len(m.trace)==before
  assert m.invoke(2,'storage_read_submit',0x50040000,m.base+m.layout['next_ticket'])==0xffffffff
  assert m.invoke(2,'storage_read_copy',0x50010000,4384,0x50010004)==0
  r,t=m.submit(2,23,1,1);assert r==1
  m.cpu.mem_write(0x50040000,bytes(24));assert m.submit(2,24,1,2)[0]==0
  m.complete(0);v=decoder.decode(m.frame(),raw);assert v['first_sector']==23 and v['ticket']==t
  assert m.invoke(2,'storage_read_ack',t)==1 and m.submit(2,25,1,1)[0]==-1
  r,new=m.submit(2,25,1,2);assert r==1 and new>t
  assert m.invoke(2,'storage_read_cancel',t)==0xffffffff and not m.field('cancel')
  record('ranges-identity',m)
 if 'max-ticket-exhaustion' not in results and (selected is None or 'max-ticket-exhaustion' in selected):
  m=initial.clone(pages);m.write(m.base+m.layout['next_ticket'],2**64-2,8)
  r,t=m.submit(2,1,1,1);assert r==1 and t==2**64-1;m.complete(0)
  assert decoder.decode(m.frame(),raw)['ticket']==t and m.invoke(2,'storage_read_ack',t)==1
  before=len(m.trace);assert m.submit(2,1,1,2)[0]==-1 and len(m.trace)==before
  record('max-ticket-exhaustion',m)
 if 'queued-cancel-retains-active' not in results and (selected is None or 'queued-cancel-retains-active' in selected):
  # Queued cancellation revokes both and never returns another active borrower.
  m=initial.clone(pages);_,active=m.submit(2,2,8,1);_,queued=m.submit(3,14,1,1)
  for _ in range(40):
   m.owner()
   if m.read(m.core+m.c['borrowed'],8):break
  assert m.read(m.core+m.c['borrowed'],8)==m.base+m.layout['borrowed']
  assert m.invoke(3,'storage_read_cancel',queued)==1;m.owner();before=len(m.trace)
  assert m.submit(2,0,1,2)[0]==-2 and m.read(m.core+m.c['borrowed'],8)==m.base+m.layout['borrowed']
  m.present=False;m.owner();assert len(m.trace)==before
  record('queued-cancel-retains-active',m)
 if 'removal-permanent' not in results and (selected is None or 'removal-permanent' in selected):
  # Observed idle removal is permanent, including after the detect pin returns.
  m=initial.clone(pages);m.present=False;m.owner();assert m.field('result')==2
  before=len(m.trace);m.present=True;assert m.submit(2,0,1,1)[0]==-2;m.owner();assert len(m.trace)==before
  record('removal-permanent',m)
 if 'usb-partial-cancel-terminal' not in results and (selected is None or 'usb-partial-cancel-terminal' in selected):
  # Partial USB transfer owns its copied response until matching callbacks.
  m=initial.clone(pages);_,t=m.submit(2,4,2,1);m.complete(0);m.max_tx=64;m.usb_step(b'\0')
  assert len(m.tx)==1 and len(m.tx[0])==64 and m.submit(2,5,1,2)[0]==0
  m.usb_step(b'\1');m.invoke(2,'tud_vendor_tx_cb',0,64);m.owner();before=len(m.tx)
  m.usb_step();assert len(m.tx)==before and m.field('cancel')==1
  # Terminal submit must remain RETAINED rather than become an invalid request.
  packet=decoder.read_request(5,1,2)
  m.max_tx=4384
  for part in (packet[:1],packet[1:7],packet[7:]):m.usb_step(part)
  assert decoder.decode(m.tx[-1],raw)['status']==2
  record('usb-partial-cancel-terminal',m)
 if 'usb-retention-overrides-rejection' not in results and (selected is None or 'usb-retention-overrides-rejection' in selected):
  # Submission can be refused before a concurrent revocation. RETAINED still
  # wins when the later serialized copy observes the terminal lifetime.
  m=initial.clone(pages);m.api_busy=True;packet=decoder.read_request(5,1,1)
  m.usb_step(packet[:1]);m.usb_step(packet[1:]);assert not m.tx
  m.api_busy=False;m.invoke(2,'storage_read_revoke',1);m.usb_step()
  assert decoder.decode(m.tx[-1],raw)['status']==2
  record('usb-retention-overrides-rejection',m)
 if 'usb-fragmentation-ack-reuse' not in results and (selected is None or 'usb-fragmentation-ack-reuse' in selected):
  # Full USB result consumes only its own exact completed ticket after callback.
  m=initial.clone(pages);packet=decoder.read_request(31,1,1)
  for part in (packet[:1],packet[1:6],packet[6:]):m.usb_step(part)
  assert m.state_of(0)==0
  # Consume the initial pending header before querying the completed record.
  assert len(m.tx)==1;m.invoke(2,'tud_vendor_tx_cb',0,len(m.tx[0]));m.complete(0);m.usb_step(b'\0')
  body=m.tx[-1];v=decoder.decode(body,raw);assert body[288:]==payload(31,1) and m.state_of(0)==1
  m.invoke(2,'tud_vendor_tx_cb',0,len(body));assert m.state_of(0)==1;m.usb_step();assert m.state_of(0)==3
  m.usb_step(decoder.read_request(32,1,2));m.usb_step();assert m.state_of(0)==0
  record('usb-fragmentation-ack-reuse',m)
 if 'usb-disconnect-retains' not in results and (selected is None or 'usb-disconnect-retains' in selected):
  # Disconnect cannot turn an unresolved transfer into a new session's storage.
  m=initial.clone(pages);m.submit(2,9,1,1);m.complete(0);m.max_tx=64;m.usb_step(b'\0');m.invoke(2,'tud_umount_cb');m.owner()
  before=len(m.tx);m.invoke(2,'tud_mount_cb');m.usb_step();assert len(m.tx)==before and m.submit(2,0,1,2)[0]==-2
  record('usb-disconnect-retains',m)
 if 'unread-deadline' not in results and (selected is None or 'unread-deadline' in selected):
  # Deadline cuts off unread success and any queued/active other client.
  m=initial.clone(pages);_,t=m.submit(2,8,1,1);m.complete(0);sl=e.layouts['storage_read_slot'];m.time=m.read(m.slot(0)+sl['deadline'],8)
  assert m.invoke(2,'storage_read_output_ready')==0;m.owner();assert decoder.decode(m.frame(),raw)['status']==2
  record('unread-deadline',m)
 if 'byte-addressed-read' not in results and (selected is None or 'byte-addressed-read' in selected):
  # Exercise actual SDSC identification and byte arguments, not a fabricated
  # later READY core. Its one extra CMD16 is part of this new addressing case.
  m=Machine(e,raw,True);m.establish();assert m.submit(2,5,2,1)[0]==1;m.complete(0)
  body=m.frame();assert body[288:]==payload(5,2) and decoder.decode(body,raw)['sector_addressed']==0
  assert [18,5*512] in m.commands;record('byte-addressed-read',m)
 return results
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session','output'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--case',action='append',choices=['two-clients-repeat', 'ranges-identity', 'max-ticket-exhaustion', 'queued-cancel-retains-active', 'removal-permanent', 'usb-partial-cancel-terminal', 'usb-retention-overrides-rejection', 'usb-fragmentation-ack-reuse', 'usb-disconnect-retains', 'unread-deadline', 'byte-addressed-read'])
 a=p.parse_args();run(a.image,a.session,a.output,a.case)
