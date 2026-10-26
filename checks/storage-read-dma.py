#!/usr/bin/env python3
"""Actual supervised SD service with the existing card/IDMAC ownership model.

Only the cache invalidate hook publishes device bytes into CPU memory. The
scheduler, translations, shared providers, card and USB FIFO are modeled.
"""
import argparse,copy,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
modes=module('read_dma_modes',ROOT/'checks/storage-read-sd-modes.py')
idmac=module('read_dma_idmac',ROOT/'checks/storage-read-idmac.py')
observe=module('read_dma_observe',ROOT/'apps/storage-read/observe.py')
idmac.AREA=0x41c00000
PAYLOAD=32768
class Machine(idmac.Idmac,modes.Machine):
 def __init__(self,image,session):
  super().__init__(image,session);self.dma_init();self.final=False;self.lost=False
  self.bad_page=None;self.translations=[];self.max_tx=2048;self.worker_ticks=0
  self.hooks[image.address('aarch64_device_cache')]='aarch64_device_cache'
 def code(self,cpu,a,size,unused):
  name=self.hooks.get(a);args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(4)]
  if name=='aarch64_device_translate':
   self.translations.append(args[0])
   if args[0]==self.bad_page:self.ret(0);return
   attr=0x44 if 0x41c00000<=args[0]<0x41e00000 else 0xff if args[0]>=0x40000000 else 0
   self.cpu.mem_write(args[2],struct.pack('<Q2I',args[0],attr,3 if args[0]>=0x40000000 else 2));self.ret(1)
  elif name=='aarch64_device_cache':self.ret(self.dma_cache(*args[:3]))
  elif name=='board_live' and self.lost:self.ret(0)
  else:super().code(cpu,a,size,unused)
 def read_mmio(self,cpu,access,a,size,value,unused):
  if self.dma:assert a!=0x04020200,'DMA payload reached CPU FIFO'
  self.dma_read_progress(a);super().read_mmio(cpu,access,a,size,value,unused)
 def write_mmio(self,cpu,access,a,size,value,unused):
  assert self.held and size==4
  if not self.dma_write_mmio(a,value):super().write_mmio(cpu,access,a,size,value,unused)
  if self.variant=='loss-active' and self.dma:self.lost=True
  if self.variant=='deadline-active' and self.dma:
   self.time=self.read(self.slot(0)+self.image.layouts['storage_read_slot']['deadline'],8)
 def owner(self):self.worker_ticks+=1;super().owner()
 def frame(self,client=2,capacity=288+PAYLOAD):
  assert self.invoke(client,'storage_read_copy',0x50010000,capacity,0x50020000)==1
  return bytes(self.cpu.mem_read(0x50010000,self.read(0x50020000)))
 def clone(self,pages):
  m=Machine(self.image,self.session)
  for a,data in pages.items():m.cpu.mem_write(a,data)
  for k,v in self.__dict__.items():
   if k not in ('cpu','image','session','hooks'):setattr(m,k,copy.deepcopy(v))
  m.trace=[];m.commands=[];m.tx=[];m.translations=[];m.cache_events=[];m.dma_reads=[];m.worker_ticks=0
  return m
 def finish(self,slot):
  for _ in range(400):
   self.owner()
   if self.state_of(slot)==1:return
   if self.field('result')==2:return
  raise AssertionError('Read remained pending')

def image(path):
 e=modes.service.base.harness.Image(path,modes.service.base.SD0_LAYOUTS+('a733_mmc_platform_config',));e.sizes={};e.symbol_sizes={}
 with path.open('rb') as f:
  elf=ELFFile(f)
  for s in elf.get_section_by_name('.symtab').iter_symbols():e.symbol_sizes[s.name]=int(s['st_size'])
  for cu in elf.get_dwarf_info().iter_CUs():
   for d in cu.iter_DIEs():
    if d.tag=='DW_TAG_structure_type' and 'DW_AT_name' in d.attributes and 'DW_AT_byte_size' in d.attributes:
     e.sizes[d.attributes['DW_AT_name'].value.decode()]=int(d.attributes['DW_AT_byte_size'].value)
 return e

def run(path,session,out,cases):
 out.mkdir(parents=True,exist_ok=False);raw=session.read_bytes();profile=struct.unpack_from('<I',raw,860)[0]
 prep=module('read_dma_prepare',ROOT/'apps/storage-read/prepare.py');prep.embedded_reference(path.read_bytes(),raw)
 e=image(path);initial=Machine(e,raw);initial.establish();pages=initial.checkpoint()
 assert initial.read(initial.core+initial.c['borrowed'],8)==0
 buffer=initial.base+initial.layout['borrowed']
 if profile in (2,3):
  assert not buffer&63 and not (buffer+PAYLOAD)&63
  expected=set(range(buffer&~4095,(buffer+PAYLOAD+4095)&~4095,4096))|{0x41c00000}
  assert expected<={a&~4095 for a in initial.translations}
 results={}
 for name in cases:
  m=initial.clone(pages);m.variant=name
  if name=='two-clients':
   assert profile==2
   ok,t=m.submit(2,19,64,1);assert ok==1
   ok,other=m.submit(3,99,64,1);assert ok==1 and other>t
   assert m.invoke(2,'storage_read_ack',t)==0
   m.finish(0);assert m.state_of(0)==1 and len(m.dma_reads)==1
   first=m.frame();assert first[288:]==modes.service.payload(19,64)
   saved_slot=bytes(m.cpu.mem_read(m.slot(0),e.sizes['storage_read_slot']))
   assert m.invoke(3,'storage_read_ack',t)==0 and m.invoke(2,'storage_read_ack',t+1)==0
   m.finish(1);assert m.state_of(1)==1 and len(m.dma_reads)==2
   assert bytes(m.cpu.mem_read(m.slot(0),len(saved_slot)))==saved_slot
   again=m.frame();assert again[288:]==first[288:] and again[32:64]==first[32:64] and again[240:272]==first[240:272]
   second=m.frame(3);assert second[288:]==modes.service.payload(99,64)
   assert m.invoke(2,'storage_read_copy',0x50010000,len(first)-1,0x50020000)==0
   assert m.submit(2,20,1,2)[0]==0
   assert m.invoke(2,'storage_read_ack',t)==1 and m.invoke(3,'storage_read_ack',other)==1
   assert m.submit(2,20,64,2)[0]==1
   (out/'first-frame.bin').write_bytes(first);(out/'second-frame.bin').write_bytes(second)
   assert not m.translations,'Per-read mapping checks were added'
  elif name=='usb-final-in':
   assert m.submit(2,19,64,1)[0]==1;m.finish(0);m.usb_step(b'\0')
   while sum(map(len,m.tx))<33056:
    assert m.state_of(0)==1
    assert m.queued and m.queued<=2048
    m.invoke(2,'tud_vendor_tx_cb',0,m.queued);m.usb_step()
   assert sum(map(len,m.tx))==33056 and m.queued
   n=m.queued;m.invoke(2,'tud_vendor_tx_cb',0,n-1);m.usb_step();assert m.state_of(0)==1
   m.invoke(2,'tud_vendor_tx_cb',0,1);assert m.state_of(0)==1
   m.usb_step();assert m.state_of(0)==3
   frame=b''.join(m.tx);assert frame[288:]==modes.service.payload(19,64)
   observe.decode(frame,raw);(out/'usb-frame.bin').write_bytes(frame)
  elif name=='native-client':
   for _ in range(100):
    m.native_client();m.owner()
    if m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['result']):break
   assert m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['result'])==1
   assert m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['completed'])==2
   assert [x['blocks'] for x in m.dma_reads]==[64,64] and m.state_of(1)==3
   (out/'host-idle-frame.bin').write_bytes(m.frame())
   assert m.submit(2,19,64,1)[0]==1;m.finish(0)
   frame=m.frame();assert observe.decode(frame,raw)['application_reads']==2
   (out/'host-frame.bin').write_bytes(frame)
  elif name in ('mapping-descriptor','mapping-buffer'):
   m=Machine(e,raw);m.prepare_only()
   m.bad_page=0x41c00000 if name=='mapping-descriptor' else ((m.base+m.layout['borrowed'])&~4095)+4096
   m.owner();assert m.field('result')==2 and m.field('reason')==2
   assert not m.commands and not m.cache_events and m.bad_page in m.translations
  elif name=='cache-contract':
   buffer=m.base+m.layout['borrowed']
   # Call the actual private callback with malformed requests. No cache or
   # device access is admissible for any of these arguments.
   for cookie,address,count,phase in ((m.native+8,buffer,512,1),(m.native,buffer+64,512,1),
       (m.native,buffer,513,1),(m.native,buffer,65536,1),(m.native,buffer,512,99)):
    assert m.call('dma_sync',cookie,address,count,phase)==0
   assert not m.cache_events and not m.commands
   off=e.layouts['a733_mmc_platform_config']['dma']
   m.write(m.base+m.layout['config']+off,0x41c00040,8)
   assert m.submit(2,19,64,1)[0]==1;m.finish(0)
   assert m.field('result')==2 and not m.dma_reads and not m.cache_events
  elif name=='bounds-alias':
   assert m.submit(2,0,65,1)[0]==-1 and m.submit(2,0xffffffff,2,1)[0]==-1
   assert not m.commands and not m.cache_events
   for address in (0x41c00000,0x41c02000,0x41dfffff):
    assert m.invoke(2,'storage_read_copy',address,33056,0x50020000)==0
    assert m.invoke(2,'storage_read_submit',address,0x50040100)==0xffffffff
    assert m.invoke(2,'storage_read_submit',0x50040000,address)==0xffffffff
   assert not m.commands and not m.cache_events
  elif name in ('cache-before-fail','cache-after-fail','loss-active','deadline-active'):
   assert m.submit(2,19,64,1)[0]==1
   m.finish(0);assert m.field('result')==2 and m.read(m.platform+m.p['fault'])==1
   assert observe.decode(m.frame(),raw)['status']==2
   assert m.submit(2,20,1,2)[0]==-2 and not m.dma_reads
  elif name=='single-read':
   blocks=64 if profile in (2,3) else 8
   assert m.submit(2,19,blocks,1)[0]==1;m.finish(0);assert m.state_of(0)==1
   frame=m.frame();value=observe.decode(frame,raw);assert frame[288:]==modes.service.payload(19,blocks)
   if profile in (2,3):assert len(m.dma_reads)==1 and len(m.cache_events)==2
   else:assert not m.dma_reads and m.submit(3,20,9,1)[0]==-1
   if profile==3:assert value['timing']==1 and not value['signal_1v8']
   (out/'read-frame.bin').write_bytes(frame)
  else:raise ValueError(name)
  results[name]=dict(passed=True,worker_ticks=m.worker_ticks,commands=m.commands,
    cache_events=m.cache_events,dma_reads=[{k:v for k,v in x.items() if k!='data'} for x in m.dma_reads],
    cpu_fifo_words=sum(x[:2]==['read',0x04020200] for x in m.trace))
  (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,elf=hashlib.sha256(path.read_bytes()).hexdigest(),profile=profile,cases=results),indent=2)+'\n')
  print(name,results[name]['worker_ticks'],flush=True)
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session','output'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--case',action='append',required=True);a=p.parse_args();run(a.image,a.session,a.output,a.case)
