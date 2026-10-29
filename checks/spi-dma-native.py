#!/usr/bin/env python3
"""Linked SPI/DMA application, controller, startup, allocation and native adapters.

Register effects, cache primitive results, translations, scheduler, PMIC/rail
replies and USB transport are modeled. No physical qualification is claimed.
"""
import argparse, hashlib, importlib.util, json, struct
from pathlib import Path
from unicorn import UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('spi_irq_model',ROOT/'checks/spi-interrupt-native.py')
irq=importlib.util.module_from_spec(spec);spec.loader.exec_module(irq)
base=irq.base

class Machine(irq.Machine):
 def __init__(self,image,session,variant,nominal):
  super().__init__(image,session,variant,nominal)
  self.cpu.mem_map(0x04601000,0x2000)
  self.cpu.hook_add(UC_HOOK_MEM_READ,self.memory,begin=0x04601000,end=0x04602fff)
  self.dma=image.address('spi_dma');self.dlayout=image.layouts['spi_dma_application']
  self.cache_ops=[];self.pending=[0,0];self.injected=0;self.wakes=0;self.stops=[];self.cpu_fifo=0
  self.dma_reads=0;self.dma_writes=0;self.fatal=False;self.copied=False
  for name in ('aarch64_device_cache_stride','aarch64_device_cache'):
   self.hooks[image.address(name)]=name
  self.hooks[image.address('native_ticks','/platform/a733/dma_native.c')]='dma_ticks'
  self.fatal_address=image.address('fatal','/apps/spi/dma.c')
  self.copy_address=image.address('spi_dma_copy')
  self.put(self.objects['counter']+image.layouts['a733_counter_clock']['initialized'],1)
  self.put(0x02543088,0xe5) # NDMA reset threshold from the SPI core contract.
  for a,v in ((0x02002500,0),(0x02002580,0x83000002),(0x02002588,0x84000001),
              (0x020020a0,0xfe126310),(0x02003908,0x0a060a06),(0x02003988,0x0a060000),
              (0x020025e4,0x40302)):
   self.put(a,v)
  assert self.call('a733_memory_reservations_prepare')==1
 def dfield(self,name):return self.get(self.dma+self.dlayout[name])
 def channel(self,j):
  return self.dma+self.dlayout['controller']+self.image.layouts['a733_dma_controller']['channels']+j*self.image.sizes['a733_dma_channel']
 def complete(self,j):
  self.pending[j]=4;self.put(0x04601100+j*64,0)
 def memory(self,cpu,access,a,size,value,unused):
  if 0x04601000<=a<0x04603000:
   self.dma_reads+=1
   if self.variant=='mmio-authority-loss' and self.started and not self.injected:
    self.put(0x03400110,1<<15);self.injected+=1
   if a in (0x04601138,0x04601178):self.put(a,self.pending[(a-0x04601138)//64])
   if a==0x04601030 and self.variant=='failed-drain' and self.stops:self.put(a,1)
   return
  if a==0x02543300:self.cpu_fifo+=1;raise AssertionError('CPU RX FIFO access in DMA mode')
  if a==0x0254301c:self.put(a,0);return
  super().memory(cpu,access,a,size,value,unused)
 def write(self,cpu,access,a,size,value,unused):
  if 0x04601000<=a<0x04603000:
   self.dma_writes+=1
   if a in (0x04601138,0x04601178):self.pending[(a-0x04601138)//64]&=~value
   if a in (0x04601100,0x04601140) and not value and self.started:
    assert not self.get(0x02543018)&0x01000100
    self.stops.append((a-0x04601100)//64);self.events.append('dma-stop')
   return
  if a==0x02543200:self.cpu_fifo+=1;raise AssertionError('CPU TX FIFO access in DMA mode')
  if a==0x02543018 and value&0x01000100:
   assert self.dfield('submitted')==3
   for j in range(2):assert self.get(self.channel(j)+self.image.layouts['a733_dma_channel']['status'])==1
  if a==0x02543008 and value&0x80000000:
   assert self.dfield('submitted')==3 and self.held
   self.started=True;self.status=0x1000;self.events.append('exchange')
   extent=int.from_bytes(cpu.mem_read(self.dma+self.dlayout['extent'],8),'little')
   assert bytes(cpu.mem_read(extent+12288,2))==self.session[800:802]
   cpu.mem_write(extent+8192,bytes(range(256))*16)
   self.complete(1)
   if self.variant!='delayed-rx':self.complete(0)
   if self.variant=='dma-error':self.pending[0]=8;self.injected+=1
   return
  super().write(cpu,access,a,size,value,unused)
 def code(self,cpu,a,size,unused):
  name=self.hooks.get(a);args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(4)]
  if a==getattr(self,'fatal_address',None):self.fatal=True
  if a==getattr(self,'copy_address',None):
   assert self.dfield('drained')==3 and self.dfield('completed')==3 and self.stops==[0,1]
   self.copied=True
  if name=='aarch64_device_translate':
   cpu.mem_write(args[2],struct.pack('<Q2I',args[0],255 if 0x41000000<=args[0]<0x41f00000 else 0,3 if 0x41000000<=args[0]<0x41f00000 else 2));self.returned(1);return
  if name=='aarch64_device_cache_stride':self.returned(64);return
  if name=='aarch64_device_cache':
   self.cache_ops.append(args[:3]);assert args[2]==4096
   if args[0]==2:
    assert self.stops==[0] and not self.get(0x02543018)&0x01000100
    if self.variant=='cache-failure':self.injected+=1;self.returned(0);return
    if self.variant=='shared-lock-loss':
     self.put(self.objects['locks']+self.image.layouts['a733_platform_locks']['retained'],1);self.injected+=1
   self.returned(1);return
  if name=='dma_ticks':self.returned(self.tick*24);return
  if name=='a733_cpu_coherence_publish':
   assert args[1]==len(self.session)+8 and bytes(cpu.mem_read(args[0],len(self.session)))==self.session
   self.returned(1);return
  super().code(cpu,a,size,unused)
 def step(self):self.wakes+=1;self.tick+=1000;return self.call('spi_step')

def run(image,session,out,nominal,cases):
 out.mkdir(parents=True,exist_ok=False);elf=base.Image(image);raw=session.read_bytes();nom=json.loads(nominal.read_text());results={}
 assert len(raw)==960 and struct.unpack_from('<Q',raw,600)[0]==3
 for variant in cases:
  m=Machine(elf,raw,variant,nom)
  if variant=='admission':
   assert m.call('spi_session_valid',0x41f00000)==1
   mutations={'profile':(600,'Q',2),'session-size':(608,'Q',832),'command-count':(792,'I',17),
    'response-count':(796,'I',4095),'fabric':(832,'I',1),'routing':(864,'I',1),
    'controller':(896,'Q',0x04024000),'channels':(918,'H',1),'extent-authority':(912,'I',0x1f)}
   for label,(offset,fmt,value) in mutations.items():
    bad=bytearray(raw);struct.pack_into('<'+fmt,bad,offset,value);m.cpu.mem_write(0x41f00000,bytes(bad))
    assert m.call('spi_session_valid',0x41f00000)==0,label
   m.cpu.mem_write(0x41f00000,raw)
   prep=base.module('dma_native_prepare',ROOT/'apps/spi/prepare.py');_,entry,identity=prep.base.image_parts(image)
   _,contract=prep.request_bytes(entry,identity,raw,*struct.unpack_from('<3Q',raw,24));contract=bytearray(contract[:224])
   struct.pack_into('<Q',contract,0,prep.CONTRACT_MAGIC);struct.pack_into('<4Q',contract,80,100,24000100,1440000100,1440000100)
   m.cpu.mem_write(0x50001000,bytes(contract));assert m.call('spi_session_prepare',0x50001000)==1
   assert m.call('spi_session_prepare',0x50001000)==0
   results[variant]=dict(passed=True,refused=list(mutations),full_session_sealed=True)
   (out/'progress.json').write_text(json.dumps(results,indent=2)+'\n');continue
  if variant=='mapping-admission':
   assert m.call('spi_construct')==1
   address=m.dma+m.dlayout['mappings'];n=elf.sizes['a733_dma_mapping'];mapping=bytes(m.cpu.mem_read(address,n));scratch=0x50004000
   m.cpu.mem_write(scratch,mapping)
   assert m.call('mapping',m.dma,scratch,unit='/apps/spi/dma.c')==1
   for field,value in [('bytes',4095),('bus',0x43000000),('cpu',0x43000000),('access',0)]:
    bad=bytearray(mapping);offset=elf.layouts['a733_dma_mapping'][field]
    struct.pack_into('<I' if field=='access' else '<Q',bad,offset,value);m.cpu.mem_write(scratch,bytes(bad))
    assert m.call('mapping',m.dma,scratch,unit='/apps/spi/dma.c')==0,field
   results[variant]=dict(passed=True,refused=['extent','physical-address','CPU-address','role'])
   (out/'progress.json').write_text(json.dumps(results,indent=2)+'\n');continue
  m.construct()
  for _ in range(100):
   r=m.step()
   if m.field('stage')==11 or m.field('result')==2:break
   assert r==0,(variant,r,m.field('stage'),m.dfield('stage'))
  else:raise AssertionError('Unbounded startup')
  if variant not in ('dma-error','mmio-authority-loss'):
   assert m.field('stage')==11 and m.started,(variant,m.field('stage'),m.dfield('stage'))
   assert m.step()==0 and not m.notification and not m.copied
   if variant=='missing-irq':m.tick=6000000;m.step()
   else:
    m.put(0x03400304,1<<27);m.call('spi_interrupt');assert m.notification==1
    assert m.step()==0 and not m.stops
    m.put(0x03400304,0)
    for _ in range(150):
     r=m.step()
     if variant=='delayed-rx' and m.field('stage')==8 and not m.injected:
      assert not m.stops and not m.copied and not m.get(0x02543018)&0x01000100
      assert m.step()==0 and not m.stops;m.complete(0);m.injected+=1
     if r in (1,0xffffffff) or m.fatal:break
    else:raise AssertionError(('Unbounded completion',variant,m.field('stage')))
  success=variant in ('success','delayed-rx','output-revocation')
  if success:
   assert m.field('result')==1 and m.copied and m.dfield('drained')==3
   assert [op[0] for op in m.cache_ops]==[1,0,0,0,2]
   assert m.call('spi_output_ready')==1
   if variant=='output-revocation':m.field('failed',1);m.injected+=1;assert m.call('spi_output_ready')==0
   m.call('board_usb0_task');n=128 if variant=='output-revocation' else 4320
   frame=bytes(m.cpu.mem_read(elf.address('spi_response'),n));decoder=base.module('dma_decoder',ROOT/'apps/spi/observe.py');info=decoder.decode(frame,raw)
   assert info['status']==(2 if variant=='output-revocation' else 1)
   if variant=='success':(out/'native-frame.bin').write_bytes(frame)
  else:
   assert m.field('result')==2 and not m.copied and m.call('spi_output_ready')==0
   if variant in ('cache-failure','shared-lock-loss','mmio-authority-loss'):
    assert m.fatal and m.injected==1 and m.dfield('drained')==0
    assert m.get(m.channel(0)+elf.layouts['a733_dma_channel']['status'])!=3
  assert m.cpu_fifo==0
  results[variant]=dict(passed=True,notifications=m.notification,cache_operations=m.cache_ops,cache_bytes=sum(op[2] for op in m.cache_ops),owner_wakes=m.wakes,dma_reads=m.dma_reads,dma_writes=m.dma_writes,stops=m.stops,injections=m.injected,nonreturning_failure=m.fatal,copied=m.copied)
  (out/'progress.json').write_text(json.dumps(results,indent=2)+'\n')
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=results,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),physical_access=False,scope=__doc__),indent=2)+'\n')

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','output','nominal'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--case',action='append',choices=('admission','mapping-admission','success','delayed-rx','missing-irq','dma-error','failed-drain','cache-failure','shared-lock-loss','mmio-authority-loss','output-revocation'))
 a=p.parse_args();run(a.image,a.session,a.output,a.nominal,a.case or ['admission','mapping-admission','success','delayed-rx','missing-irq','dma-error','failed-drain','cache-failure','shared-lock-loss','mmio-authority-loss','output-revocation'])
