#!/usr/bin/env python3
"""One finite native HDMI audio run, using explicit modeled hardware and RTOS effects.

The selected ELF executes its constructors, PMIC/output/DMA task bodies, native
providers, sampler, FIFO job and retained publication. Architectural services,
queues, serialized scheduling, electrical state and device transfers are modeled.
This check never accesses a board and cannot establish transmitted or rendered sound.
"""
import argparse,copy,hashlib,importlib.util,io,json,pickle,struct,sys
import elftools,unicorn
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn import arm64_const as arm
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class Image:
 def __init__(self,path):
  self.path=path;self.symbols={};self.layouts={};self.sizes={};self.parts=[];self.text=[]
  self.raw=path.read_bytes()
  with io.BytesIO(self.raw) as f:
   e=ELFFile(f)
   for p in e.iter_segments():
    if p['p_type']=='PT_LOAD':self.parts.append((int(p['p_vaddr']),int(p['p_memsz']),p.data(),int(p['p_flags'])))
   for s in e.get_section_by_name('.symtab').iter_symbols():
    if s.name and s['st_value']:self.symbols.setdefault(s.name,set()).add(int(s['st_value']))
   text=e.get_section_by_name('.text');self.text=[int(text['sh_addr']),text.data()]
   for cu in e.get_dwarf_info().iter_CUs():
    for d in cu.iter_DIEs():
     if d.tag!='DW_TAG_structure_type' or 'DW_AT_name' not in d.attributes or 'DW_AT_byte_size' not in d.attributes:continue
     n=d.attributes['DW_AT_name'].value.decode();fields={}
     for m in d.iter_children():
      if m.tag=='DW_TAG_member' and 'DW_AT_name' in m.attributes and 'DW_AT_data_member_location' in m.attributes:
       fields[m.attributes['DW_AT_name'].value.decode()]=int(m.attributes['DW_AT_data_member_location'].value)
     if n in self.layouts:assert self.layouts[n]==fields,('ambiguous layout',n)
     self.layouts[n]=fields;self.sizes[n]=int(d.attributes['DW_AT_byte_size'].value)
 def address(self,n):
  assert len(self.symbols[n])==1,('ambiguous symbol',n);return next(iter(self.symbols[n]))
 def field(self,n,k):return self.layouts[n][k]
 def bytes(self,a,n):
  for b,_,d,_ in self.parts:
   if b<=a and a+n<=b+len(d):return d[a-b:a-b+n]
  raise AssertionError(('not initialized',hex(a),n))

def seed(image,out):
 out.mkdir(parents=True,exist_ok=False);i=Image(image)
 assert i.sizes['hdmi_session']==656 and i.field('hdmi_session','audio')==440
 raw=bytearray(656);struct.pack_into('<6Q',raw,0,0x4153373348444131,1,656,100,101,102)
 raw[48:80]=i.bytes(i.address('a733_smp_build_id'),32)
 for p in (80,112,144,176,440,472,504):raw[p:p+32]=bytes([p%251+1])*32
 struct.pack_into('<2Q2I',raw,208,103,104,0,0)
 request=i.field('hdmi_session','request');r=i.layouts['a733_hdmi_request'];initial=request+r['initial'];il=i.layouts['a733_hdmi_initial']
 struct.pack_into('<2Q',raw,initial,105,101)
 struct.pack_into('<8I',raw,initial+il['words'],8,0xa0,0x00110001,0x10000000,0x76543210,0,0,0x001f0040)
 struct.pack_into('<2I',raw,initial+il['resistor_control'],0xa5a50000,0xabcdef12)
 struct.pack_into('<4I',raw,request+r['mode'],4,0,0,26000000)
 p=i.field('hdmi_session','pmic');pl=i.layouts['a733_pmic_config'];struct.pack_into('<3Q',raw,p,106,101,0)
 raw[p+pl['pins']:p+pl['pins']+6]=bytes((2,1,1,2,1,1))
 p=i.field('hdmi_session','pins');pin=i.layouts['a733_pin_handoff'];struct.pack_into('<I',raw,p+11*4,3)
 # PL0/1 stay in inherited PMIC function2; no DATA writer is granted.
 pcm=b''.join(struct.pack('<hh',(n%101)-50,50-(n%101)) for n in range(2049))
 raw[440:472]=hashlib.sha256(pcm).digest();struct.pack_into('<2Q',raw,536,107,2049)
 d=552;cfg=i.layouts['a733_dma_platform_config'];struct.pack_into('<3I',raw,d,0,0x80000000,0x80000000)
 struct.pack_into('<3I',raw,d+12,24000000,24000000,24000000)
 struct.pack_into('<5I',raw,d+24,0xff,0xff00,0xff,0xff00,0xff)
 struct.pack_into('<3Q',raw,d+cfg['owner'],108,109,0);struct.pack_into('<I',raw,d+cfg['cold'],0)
 struct.pack_into('<2QI2H',raw,632,0x04601000,109,0x5f,0xffff,1)
 edid=bytearray(256);edid[:8]=bytes((0,255,255,255,255,255,255,0));edid[18:21]=bytes((1,4,128));edid[126]=1
 edid[128:132]=bytes((2,3,16,0x40));edid[132:144]=bytes((0x41,4,0x65,3,12,0,0x10,0,0x23,9,4,1))
 for b in (0,128):edid[b+127]=(-sum(edid[b:b+127]))&255
 (out/'baseline-session.bin').write_bytes(raw);(out/'input.pcm').write_bytes(pcm);(out/'sink.edid').write_bytes(edid)
 save(out/'seed.json',dict(physical=False,image_sha256=sha(image),session_sha256=sha(out/'baseline-session.bin'),pcm_sha256=sha(out/'input.pcm'),edid_sha256=sha(out/'sink.edid')))

class Machine:
 def __init__(self,image,session,contract,pcm,edid):
  self.i=image;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.time=100;self.scheduler=1;self.task=0;self.tasks=[];self.holders={};self.queues={};self.notifications={};self.events=[];self.trace=[];self.registers={};self.pending={};self.internal={};self.publishes=[];self.boundary=None;self.stopped=False;self.edid=edid;self.completion_reported=False;self.dma_started=0;self.dma_done=0;self.fifo_empty_at=0;self.dma_payload=b'';self.callsites=[];self.phase="constructor";self.active_handle=None;self.sequence=0
  self.regions=[(0x41000000,0x03000000),(0x02000000,0x4000),(0x02535000,4096),(0x03000000,4096),(0x03400000,4096),(0x04601000,8192),(0x05510000,0x20000),(0x05600000,4096),(0x05730000,4096),(0x07010000,4096),(0x07025000,4096),(0x0706a000,4096),(0x07083000,4096),(0x07090000,4096)]
  for a,n in self.regions:self.u.mem_map(a,n)
  for a,_,d,_ in image.parts:
   if d:self.u.mem_write(a,d)
  self.u.mem_write(0x41f00000,session);self.u.mem_write(0x41f10000,pcm);self.u.mem_write(0x42004000,contract)
  self.u.reg_write(UC_ARM64_REG_PSTATE,0x345)
  runtime=image.address('a733_cached_runtime');rl=image.layouts['a733_cached_runtime'];cl=image.layouts['a733_smp_contract']
  for key in ('epoch','startup_deadline','diagnostic_deadline'):
   self.put(runtime+rl[key],struct.unpack_from('<Q',contract,cl[key])[0])
  names=('a733_smp_counter','a733_smp_device_preparation_admitted','a733_cpu_coherence_publish','vTaskStartScheduler','aarch64_device_translate','aarch64_device_cache_stride','aarch64_device_cache','a733_counter_clock_init','a733_counter_clock_now','xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xTaskCreateStatic','xTaskCreateStaticAffinitySet','xQueueGenericCreateStatic','xQueueCreateMutexStatic','vTaskEnterCritical','vTaskExitCritical','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','xQueueReceive','vTaskDelay','vPortYield','vTaskSuspend','ulTaskGenericNotifyTake','xTaskGenericNotify','platform_panic','vAssertCalled')
  self.hooks={image.address(n):n for n in names if n in image.symbols};sites=set(self.hooks)|{0x43fff000}
  a,data=image.text
  for off in range(0,len(data)-3,4):
   word=int.from_bytes(data[off:off+4],'little')
   if word&0xffffffe0 in (0xd5384240,0xd5384200,0xd53be000,0xd53be020) or word in (0xd5034fdf,0xd4000d21):sites.add(a+off)
  for a in sites:self.u.hook_add(UC_HOOK_CODE,self.code,begin=a,end=a)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read,begin=0x02000000,end=0x07ffffff)
  self.u.hook_add(UC_HOOK_MEM_WRITE,self.write,begin=0x02000000,end=0x07ffffff)
  hw=module('audio_pmic_lower',ROOT/'checks/displayport-typec-hardware.py');self.power=hw.TypeCHardware(lambda:self.time)
  self.power.pmic[0x21]|=16;self.power.pmic[0x30]=13
  initial={0x07090160:0x10008002,0x02002000:0xf8675f00,0x070101ac:0x40,0x0701019c:0x10001,
   0x07025000:0x22,0x07025014:0x11,0x07025024:5,0x0200368c:0x70000,0x03000160:0xa5a50000,0x03000164:0xabcdef12,
   0x02002500:0,0x02002580:0x80000000,0x02002588:0x80000000,0x02002704:0x10001,0x020025e4:0x41303,
   0x04601028:4,0x04601034:0xabcd00ff,0x04601038:0x9876ff00,
   0x02002280:0xf8047f00,0x0200391c:0x60006,0x0200399c:0x60000}
  for off in (0x16ec,0x16cc,0x1604):initial[0x02002000+off]=0x10000
  for off,value in zip((0,4,0x20,0x24,0x28,0x2c,0x30,0x34),(8,0xa0,0x00110001,0x10000000,0x76543210,0,0,0x001f0040)):initial[0x05600000+off]=value
  for off,value in ((0x0c,0x40),(0x10,0xf8),(0x14,0x29),(0x20,0x30),(0x200,0xf81000),(0x210,0x80c0),(0x218,0x100010)):initial[0x07083000+off]=value
  self.registers.update(initial)
  for a,v in initial.items():self.put(a,v,4)
 def put(self,a,v,n=8):self.u.mem_write(a,int(v).to_bytes(n,'little'))
 def get(self,a,n=8):return int.from_bytes(self.u.mem_read(a,n),'little')
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def stop(self,kind,ticks=0):self.boundary=(kind,ticks);self.stopped=True;self.u.emu_stop()
 def code(self,u,at,size,unused):
  for a,v in self.pending.items():self.registers[a]=v[0];self.put(a,v[0],v[1])
  self.pending.clear()
  if at==0x43fff000:self.stop('return');return
  name=self.hooks.get(at)
  if name is None:
   word=self.get(at,4);mask=word&0xffffffe0
   if word==0xd4000d21:self.u.reg_write(UC_ARM64_REG_PC,at+4);self.stop('yield');return
   if word==0xd5034fdf:self.u.reg_write(UC_ARM64_REG_PC,at+4);return
   values={0xd5384240:4,0xd5384200:0,0xd53be000:24000000,0xd53be020:self.time*24}
   assert mask in values,hex(at)
   rt=word&31
   if rt!=31:self.u.reg_write(getattr(arm,'UC_ARM64_REG_X'+str(rt)),values[mask])
   self.u.reg_write(UC_ARM64_REG_PC,at+4);return
  a=[self.u.reg_read(UC_ARM64_REG_X0+j) for j in range(8)];self.trace.append([name,self.task,self.time])
  if name in ('platform_panic','vAssertCalled'):raise AssertionError((name,a[:2],self.trace[-15:]))
  if name=='a733_smp_counter':self.ret(self.time*24)
  elif name=='a733_smp_device_preparation_admitted':self.ret(1)
  elif name=='a733_cpu_coherence_publish':self.publishes.append([a[0],a[1],self.time]);self.ret(1)
  elif name=='aarch64_device_translate':
   attr=0x44 if 0x41c00000<=a[0]<0x41e00000 else 0xff if a[0]>=0x40000000 else 0
   self.u.mem_write(a[2],struct.pack('<QII',a[0],attr,3 if attr else 2));self.ret(1)
  elif name=='aarch64_device_cache_stride':self.ret(64)
  elif name=='aarch64_device_cache':self.ret(1)
  elif name=='a733_counter_clock_init':
   l=self.i.layouts['a733_counter_clock'];self.put(a[0]+l['frequency'],24000000,4);self.put(a[0]+l['initialized'],1,4);self.ret(1)
  elif name=='a733_counter_clock_now':self.time+=1;self.put(a[1],self.time);self.ret(1)
  elif name=='xTaskGetSchedulerState':self.ret(self.scheduler)
  elif name=='xTaskGetCurrentTaskHandle':self.ret(self.task)
  elif name in ('xTaskCreateStatic','xTaskCreateStaticAffinitySet'):self.tasks.append(a[:7]);self.ret(a[6])
  elif name=='xQueueGenericCreateStatic':self.queues[a[3]]=dict(size=a[1],capacity=a[0],items=[]);self.ret(a[3])
  elif name=='xQueueCreateMutexStatic':self.ret(a[1])
  elif name in ('vTaskEnterCritical','vTaskExitCritical'):self.ret()
  elif name=='xQueueGetMutexHolder':self.ret(self.holders.get(a[0],0))
  elif name=='xQueueSemaphoreTake':
   if self.holders.get(a[0],0):self.ret(0)
   else:self.holders[a[0]]=self.task;self.ret(1)
  elif name=='xQueueGenericSend':
   if a[0] in self.queues:
    q=self.queues[a[0]];assert len(q['items'])<q['capacity'];q['items'].append(bytes(self.u.mem_read(a[1],q['size'])));self.ret(1)
   else:assert self.holders.get(a[0])==self.task;self.holders[a[0]]=0;self.ret(1)
  elif name=='xQueueReceive':
   q=self.queues[a[0]]
   if q['items']:self.u.mem_write(a[1],q['items'].pop(0));self.ret(1)
   elif not a[2]:self.ret(0)
   else:self.ret(0);self.stop('queue',a[2])
  elif name=='ulTaskGenericNotifyTake':
   if self.notifications.get(self.task,0):self.notifications[self.task]=0;self.ret(1)
   else:self.ret(0);self.stop('notify',a[2])
  elif name=='xTaskGenericNotify':self.notifications[a[0]]=1;self.ret(1)
  elif name in ('vTaskDelay','vPortYield','vTaskSuspend'):
   if name=='vTaskSuspend':assert a[0] in (0,self.task)
   self.ret();self.stop('delay',a[0]) if name=='vTaskDelay' else self.stop('yield' if name=='vPortYield' else 'suspend')
  elif name=='vTaskStartScheduler':self.stop('scheduler')
  else:raise AssertionError(name)
 def read(self,u,access,a,n,value,unused):
  value=self.power.read(a)
  if value is None:value=self.registers.get(a,0)
  if a==0x0706a008:value=self.registers.get(0x0706a000,0)&15
  elif a==0x05600040:value=int(self.registers.get(0x05600020,0)&0xe0000000==0xe0000000)
  elif a==0x05523004:value=2|int(bool(self.registers.get(0x05523000,0)&8))
  elif a==0x02535018:value=128<<16 if not self.dma_started or self.time>=self.fifo_empty_at else 64<<16
  elif a==0x04601100 and self.dma_started and self.time>=self.dma_done:value=0;self.registers[a]=0
  elif a==0x04601030:value=1 if self.dma_started and self.time<self.dma_done else 0
  elif a==0x04601138 and self.dma_started and self.time>=self.dma_done and not self.completion_reported:value=4
  self.put(a,value,n)
 def write(self,u,access,a,n,value,unused):
  self.events.append([self.time,self.task,a,n,value]);self.registers[a]=value
  if self.power.write(a,value):return
  if a==0x04601138 and value&4:self.completion_reported=True
  if a==0x04601138:self.pending[a]=(0,4)
  if a in (0x05520105,0x05520108,0x05520103,0x0253500c,0x04601010,0x04601014):self.pending[a]=(0,n)
  elif a==0x05527e04:
   index=self.get(0x05527e0a,1)*256+self.get(0x05527e01,1);assert value in (1,2) and index<len(self.edid)
   self.pending[0x05527e03]=(self.edid[index],1);self.pending[0x05520105]=(2,1)
  elif a==0x05523026:
   index=self.get(0x05523021,1)
   if value==0x10:self.internal[index]=self.get(0x05523022,1)*256+self.get(0x05523023,1)
   else:
    assert value==1;v=self.internal[index];self.pending[0x05523024]=(v>>8,1);self.pending[0x05523025]=(v&255,1)
   self.pending[0x05520108]=(2,1)
  elif a==0x05523100:self.pending[a]=(value&~0x80,1)
  elif a==0x02535014:self.pending[a]=(value&~(1<<25),4)
  elif a==0x0253501c and value&0x80 and not self.dma_started:
   # Decode the actual native descriptor only after the actual client enables
   # FIFO requests. Device consumption and completion below are modeled.
   desc=self.registers.get(0x04601108,0);assert 0x41c00000<=desc<0x41e00000
   words=struct.unpack('<6I',bytes(self.u.mem_read(desc,24)));source=words[1];destination=words[2];count=words[3]
   audio=self.i.address('audio');nf=self.i.layouts['hdmi_audio_native'];extent=audio+nf['extent'];ef=self.i.layouts['device_memory_extent']
   expected_desc=self.get(extent+ef['physical']);expected_source=expected_desc+4096
   assert self.get(extent+ef['cpu'])==expected_desc==desc and source==expected_source
   assert count==self.get(self.i.address('app')+self.i.field('hdmi_application','session')+536+8)*4
   assert words==(0x03860280,expected_source,0x02535020,count,8,0xfffff800),words
   for at,expected in ((0x04601100,1),(0x04601104,0),(0x04601128,0),(0x04601134,12),(0x0460113c,0)):
    assert self.registers.get(at,0)==expected,(hex(at),self.registers.get(at),expected)
   assert self.registers[0x04601034]&1 and not self.registers[0x04601038]&1
   assert self.registers[0x02002704]&0x10001==0x10001 and self.registers[0x020025e4]&1
   for at,mask,expected in ((0x02003230,0x8700001f,0x82000018),(0x0200323c,0x10001,0x10001),
       (0x02535000,0x70137,0x60115),(0x02535004,0xffff,0x1f37),(0x02535024,0x1ff,0x50),
       (0x02535030,15,1),(0x02535034,0xff000f,0x110003),(0x02535044,0xffffffff,0),
       (0x02535048,0xffffffff,0x10),(0x02535014,0x800ff07f,0x80040075),
       (0x05523200,255,0),(0x05523201,255,0x18),(0x05523202,15,0),
       (0x05523205,255,0),(0x05523206,7,4),(0x05523100,0x2f,0x21),
       (0x05523101,255,16),(0x05523103,7,4),(0x05521063,0xf1,0xf0),(0x05524001,8,0)):
    assert self.registers.get(at,0)&mask==expected,(hex(at),hex(self.registers.get(at,0)),hex(expected))
   cts=[e for e in self.events if e[2]==0x05523205];n1=[e for e in self.events if e[2]==0x05523200]
   assert len(cts)==2 and n1[-1][0]<=cts[-1][0] and self.time>=cts[-1][0]+5000
   self.dma_payload=bytes(self.u.mem_read(source,count));self.dma_started=self.time
   self.dma_done=self.time+count*1000000//(48000*4);self.fifo_empty_at=self.dma_done+2000
 def call(self,name,*args,budget=30000000):
  self.boundary=None;self.stopped=False;self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for j,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+j,v)
  self.u.emu_start(self.i.address(name),0,count=budget)
  assert self.stopped,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  return self.u.reg_read(UC_ARM64_REG_X0)
 def checkpoint(self,out,label,tasks):
  memory=[]
  for j,(a,n) in enumerate(self.regions):
   p=out/(label+f'-ram-{j}.bin');p.write_bytes(bytes(self.u.mem_read(a,n)));memory.append([a,n,p.name])
  if self.phase=='task-slice':
   active=[t for t in tasks if t['handle']==self.active_handle];assert len(active)==1
   active[0]['context']=self.u.context_save()
  state={k:copy.deepcopy(v) for k,v in vars(self).items() if k not in ('i','u','power')}
  state['power']=self.power.export();state['cpu']=self.u.context_save();state['memory']=memory;state['task_contexts']=copy.deepcopy(tasks)
  statepath=out/(label+'.pickle');statepath.write_bytes(pickle.dumps(state))
  save(out/(label+'-identity.json'),dict(identity=self.identity,source_sha256=self.fixture_source_sha,hardware_sha256=self.hardware_source_sha,files={name:dict(bytes=count,sha256=sha(out/name)) for _,count,name in memory},checkpoint_sha256=sha(statepath)))
  return state

def preflight(m,raw):
 # Bounded read-only audit of the initial lower model. Masks match the native
 # consumers; this establishes fixture compatibility, never physical facts.
 r=m.registers;checked=[]
 def check(a,mask,want):
  got=r.get(a,0);assert got&mask==want,(hex(a),hex(got),hex(want));checked.append([a,mask,want])
 pins=raw[328:334];assert pins==bytes((2,1,1,2,1,1))
 check(0x07025000,255,pins[0]|pins[3]<<4)
 check(0x07025014,255,pins[2]|pins[5]<<4)
 check(0x07025024,15,pins[1]|pins[4]<<2)
 check(0x07090160,0x1000c002,0x10008002);check(0x02002000,0xffffffff,0xf8675f00)
 for a in (0x0701000c,0x07010010):check(a,0x0700001f,0)
 check(0x0701019c,0x10001,0x10001)
 for a,mask,want in ((0x0708300c,255,0x40),(0x07083010,255,0xf8),(0x07083014,0x1ff,0x29),
     (0x07083020,0x35,0x30),(0x07083018,1,0),(0x0708301c,0xffffffff,0),
     (0x07083200,0x8f000003,0),(0x07083210,0xc3,0xc0),(0x07083214,0xf0003,0),
     (0x07083218,0x01000100,0),(0x0708321c,0x007f007f,0),
     (0x07083314,0xffff,0),(0x07083318,0xffffff,0),(0x0708331c,0xffffff,0)):
  check(a,mask,want)
 for a in (0x03400000,0x0340011c,0x0340021c,0x0340031c):check(a,0x80000800,0)
 p=m.power.pmic
 assert [p.get(a,0) for a in (0xff,0x0e,0x0f,0x13,0x30)]==[0,3,0,30,13]
 assert p[0x10]&2 and p[0x21]&16
 for j,off in enumerate((0x500,0x580,0x588)):
  check(0x02002000+off,(0x0300001f,0x8700001f,0x8700001f)[j],struct.unpack_from('<I',raw,552+4*j)[0])
 check(0x02002704,0x10001,0x10001);check(0x020025e4,0x40303,0x40303)
 check(0x04601028,0xffffffff,4);check(0x04601030,0xffffffff,0)
 check(0x04601034,0xffff,0xff);check(0x04601038,0xffff,0xff00)
 for ch in range(16):
  for off in (0,4,0x28,0x34,0x38,0x3c):check(0x04601100+ch*64+off,0xffffffff,0)
 for a,mask,want in ((0x02002280,0xf907ff00,0xf8047f00),(0x02002288,0x80000000,0),
     (0x0200228c,0x08000000,0),(0x0200391c,0x60006,0x60006),(0x0200399c,0x60000,0x60000),
     (0x02003230,0x80000000,0),(0x0200323c,0x10001,0),(0x0706a000,15,0)):
  check(a,mask,want)
 for off in (0x1640,0x1680,0x1684,0x1690,0x1694):check(0x02002000+off,0x80000000,0)
 for off in (0x16cc,0x1604,0x160c,0x164c,0x168c):check(0x02002000+off,1,0)
 for j,off in enumerate((0,4,0x20,0x24,0x28,0x2c,0x30,0x34)):
  check(0x05600000+off,0xffffffff,struct.unpack_from('<I',raw,248+4*j)[0])
 check(0x03000160,0xffffffff,struct.unpack_from('<I',raw,280)[0]);check(0x03000164,0xffffffff,struct.unpack_from('<I',raw,284)[0])
 return dict(passed=True,physical=False,readbacks=checked,correction='PL drive nibble spacing: original5 corrected11; mux22 and pull5 unchanged',sources=['checks/a733-hdmi-native.c:setup','drivers/pinctrl/a733_pin_tables.c:a733_pin_fields_banked','drivers/power/a733_pmic.c:baseline','drivers/dma/a733_dma_platform.c:baseline/controller','drivers/audio/a733_i2s_platform.c:baseline','platform/a733/hdmi_native.c:baseline'])

def load_checkpoint(path,identity,expected_source,expected_hardware):
 provenance=json.loads(path.with_name(path.stem+'-identity.json').read_text())
 assert provenance['identity']==identity and provenance['checkpoint_sha256']==sha(path)
 assert provenance['source_sha256']==expected_source
 source_snapshot=path.parent/'inputs/5-hdmi-audio-native.py'
 if source_snapshot.is_file():assert sha(source_snapshot)==expected_source
 # The first constructor predates the explicit hardware key. Its already
 # frozen inputs and copied model source still supply the exact binding.
 if 'hardware_sha256' in provenance:assert provenance['hardware_sha256']==expected_hardware
 else:
  old_inputs=json.loads((path.parent/'inputs.json').read_text());assert old_inputs[str((ROOT/'checks/displayport-typec-hardware.py').resolve())]==expected_hardware
  assert sha(path.parent/'inputs/6-displayport-typec-hardware.py')==expected_hardware
 saved=pickle.loads(path.read_bytes())
 expected_names=[path.stem+f'-ram-{j}.bin' for j in range(len(saved['memory']))]
 assert list(provenance['files'])==expected_names
 assert [name for _,_,name in saved['memory']]==expected_names
 for (_,count,name),expected in zip(saved['memory'],expected_names):
  p=path.parent/name;assert p.resolve().parent==path.parent.resolve() and p.name==expected
  record=provenance['files'][name]
  assert record['bytes']==count==p.stat().st_size and sha(p)==record['sha256']
 return saved

def restore(m,path,identity,expected_source,expected_hardware):
 saved=load_checkpoint(path,identity,expected_source,expected_hardware)
 assert [(a,n) for a,n,_ in saved['memory']]==m.regions
 for a,n,name in saved['memory']:
  data=(path.parent/name).read_bytes();assert len(data)==n;m.u.mem_write(a,data)
 for key,value in saved.items():
  if key not in ('cpu','memory','task_contexts','power','fixture_source_sha','hardware_source_sha'):setattr(m,key,copy.deepcopy(value))
 m.power.restore(saved['power']);m.u.context_restore(saved['cpu'])
 for a,_,data,flags in m.i.parts:
  if not flags&2:assert bytes(m.u.mem_read(a,len(data)))==data
 return saved

def run(args):
 out=args.output;out.mkdir(parents=True,exist_ok=False)
 paths=[args.image,args.session,args.contract,args.pcm,args.edid,Path(__file__),ROOT/'checks/displayport-typec-hardware.py',Path(sys.executable).resolve()]
 for package in (unicorn,elftools):
  paths += sorted(p for p in Path(package.__file__).parent.rglob('*') if p.is_file() and (p.suffix=='.py' or '.so' in p.name))
 venv=Path(sys.prefix)/'pyvenv.cfg'
 if venv.is_file():paths.append(venv)
 loaded={str(p.resolve()):p.read_bytes() for p in paths};inputs={p:hashlib.sha256(b).hexdigest() for p,b in loaded.items()}
 snapshots=out/'inputs';snapshots.mkdir()
 for j,(p,data) in enumerate(loaded.items()):(snapshots/(str(j)+'-'+Path(p).name)).write_bytes(data)
 save(out/'inputs.json',inputs)
 i=Image(args.image);assert hashlib.sha256(i.raw).hexdigest()==inputs[str(args.image.resolve())]
 raw,contract,pcm,edid=[loaded[str(p.resolve())] for p in (args.session,args.contract,args.pcm,args.edid)]
 assert len(raw)==656 and len(contract)==224 and hashlib.sha256(pcm).digest()==raw[440:472]
 m=Machine(i,raw,contract,pcm,edid);tasks=[];m.identity={k:inputs[str(v.resolve())] for k,v in dict(image=args.image,session=args.session,contract=args.contract,pcm=args.pcm,edid=args.edid).items()};m.fixture_source_sha=inputs[str(Path(__file__).resolve())];m.hardware_source_sha=inputs[str((ROOT/'checks/displayport-typec-hardware.py').resolve())];active_machine=m;active_tasks=tasks
 for path,digest in inputs.items():assert sha(path)==digest
 try:
  if args.checkpoint:
   saved=restore(m,args.checkpoint,m.identity,args.checkpoint_source_sha256,args.checkpoint_hardware_sha256);tasks=copy.deepcopy(saved['task_contexts']);active_tasks=tasks
  else:
   assert m.call('a733_smp_contract_valid',0x42004000,200)==1
   assert m.call('hdmi_session_prepare',0x42004000)==1
   m.call('a733_smp_diagnostic_start',budget=100000000)
   assert m.boundary==('scheduler',0) and len(m.tasks)==3
  app=i.address('app');af=i.layouts['hdmi_application'];audio=i.address('audio');nf=i.layouts['hdmi_audio_native']
  assert m.get(audio+nf['initialized'],4)==1 and m.get(audio+nf['failed'],4)==0
  if args.correct_initial_pmic_drive:
   assert args.checkpoint and m.phase=='constructor' and not m.events
   assert m.get(app+af['pmic']+i.field('a733_pmic','stage'),4)==0
   assert m.get(app+af['hdmi']+i.field('a733_hdmi_native','stage'),4)==0
   assert m.get(audio+nf['stage'],4)==0 and m.registers[0x07025014]==5
   m.registers[0x07025014]=0x11;m.put(0x07025014,0x11,4)
  if m.phase=='constructor':save(out/'initial-provider-preflight.json',preflight(m,raw))

  for entry,name,depth,argument,priority,stack,handle in ([] if tasks else m.tasks):
   assert priority==4 and stack>=0x41000000 and stack+depth*8<=0x41c00000
   assert not (stack<0x43fd0000<stack+depth*8)
   tasks.append(dict(entry=entry,handle=handle,stack=stack,bytes=depth*8,argument=argument,priority=priority,context=None,wake=m.time,last=0,runs=0))
  assert len(set(t['handle'] for t in tasks))==3
  expected_handles=[m.get(app+af['handles']+j*8) for j in range(2)]+[m.get(audio+nf['service']+i.field('dma_service','worker'))]
  assert set(expected_handles)=={t['handle'] for t in tasks}
  for t in tasks:
   assert t['entry'] and t['handle'] and t['bytes']>=16384
   for q in tasks:
    if t is not q:assert t['stack']+t['bytes']<=q['stack'] or q['stack']+q['bytes']<=t['stack']
  forks={}
  if args.checkpoint:
   for stage,label in ((4,'before-sampler'),(7,'submitted-requests-disabled')):
    path=args.checkpoint.parent/(label+'.pickle')
    if path.is_file():forks[stage]=(load_checkpoint(path,m.identity,args.checkpoint_source_sha256,args.checkpoint_hardware_sha256),path.parent,args.checkpoint_source_sha256,args.checkpoint_hardware_sha256)
  else:
   m.checkpoint(out,'constructor',tasks)
  if args.constructor_only:
   assert not args.checkpoint
   for path,digest in inputs.items():assert sha(path)==digest,('input changed during constructor',path)
   save(out/'constructor-result.json',dict(passed=True,physical=False,identity=m.identity));return
  m.scheduler=2;sequence=m.sequence
  interrupted=m.active_handle if m.phase=='task-slice' else None
  if m.phase!='task-slice':m.phase='scheduling'
  while m.time<15000000 and m.get(app+af['state'],4)!=2:
   ready=[t for t in tasks if t['wake']<=m.time]
   if not ready:m.time=min(t['wake'] for t in tasks);continue
   task=next(t for t in tasks if t['handle']==interrupted) if interrupted else min(ready,key=lambda t:t['last']);interrupted=None;m.task=task['handle'];m.boundary=None;m.stopped=False
   if task['context'] is None:
    m.u.reg_write(UC_ARM64_REG_SP,(task['stack']+task['bytes'])&~15);m.u.reg_write(UC_ARM64_REG_LR,0x43fff000);m.u.reg_write(UC_ARM64_REG_X0,task['argument']);at=task['entry']
   else:m.u.context_restore(task['context']);at=m.u.reg_read(UC_ARM64_REG_PC)
   m.phase='task-slice';m.active_handle=task['handle'];m.sequence=sequence
   m.u.emu_start(at,0,count=1000000);assert m.stopped,('task instruction bound',hex(m.u.reg_read(UC_ARM64_REG_PC)))
   task['context']=m.u.context_save();task['runs']+=1;sequence+=1;task['last']=sequence;m.sequence=sequence;m.phase='scheduling';m.active_handle=None
   kind,ticks=m.boundary
   assert kind not in ('return','scheduler','suspend'),(kind,task)
   task['wake']=(m.time//1000+1)*1000 if kind in ('delay','queue','notify') else m.time
   # Serialized cooperative model wakes blocked queue/notify tasks each tick;
   # it preserves actual call contexts and does not claim SMP/preemption.
   state=m.get(app+af['state'],4)
   stage=m.get(audio+nf['stage'],4)
   if stage in (4,7) and stage not in forks:
    label='before-sampler' if stage==4 else 'submitted-requests-disabled'
    forks[stage]=(m.checkpoint(out,label,tasks),out,m.fixture_source_sha,m.hardware_source_sha)
   if state in (2,3):break
  state=m.get(app+af['state'],4)
  m.checkpoint(out,'terminal',tasks)
  result=bytes(m.u.mem_read(i.address('hdmi_audio_result'),160));(out/'result.bin').write_bytes(result)
  ready=m.get(i.address('hdmi_audio_result_ready'),4);(out/'ready.bin').write_bytes(struct.pack('<I',ready))
  words=struct.unpack('<12Q',result[:96]);assert ready==1 and words[3]==2,(words,state,m.time)
  assert state==2 and words[6]==len(pcm) and words[7]==1 and words[8]==1 and words[9]==73937500 and words[11]==0
  assert result[96:128]==hashlib.sha256(pcm).digest() and result[128:160]==hashlib.sha256(raw).digest()
  assert m.dma_payload==pcm and m.dma_started and m.time>=m.fifo_empty_at
  assert m.get(audio+nf['local']) and m.get(audio+nf['startup']+i.field('a733_dma_startup','token'))
  for path,digest in inputs.items():assert sha(path)==digest,('input changed during execution',path)
  save(out/'events.json',m.events);save(out/'publishes.json',m.publishes)
  assert set(forks)=={4,7}
  fault_results=[]
  for stage,label in ((4,'sink-without-lpcm-retained'),(7,'interrupted-submitted-dma-retained')):
   saved,producer,producer_source,producer_hardware=forks[stage];f=Machine(i,raw,contract,pcm,edid);f.identity=m.identity;f.fixture_source_sha=m.fixture_source_sha;f.hardware_source_sha=m.hardware_source_sha
   label_source='before-sampler' if stage==4 else 'submitted-requests-disabled'
   saved=restore(f,producer/(label_source+'.pickle'),m.identity,producer_source,producer_hardware);fork_tasks=copy.deepcopy(saved['task_contexts']);active_machine=f;active_tasks=fork_tasks
   assert f.get(audio+nf['stage'],4)==stage
   for at,_,data,flags in i.parts:
    if not flags&2:assert bytes(f.u.mem_read(at,len(data)))==data
   output=[t for t in fork_tasks if t['handle']==expected_handles[1]];assert len(output)==1;output=output[0]
   assert output['context'] is not None
   f.time=max(f.time,output['wake']);f.task=output['handle'];f.u.context_restore(output['context']);f.phase='task-slice';f.active_handle=f.task
   if stage==4:
    h=app+af['hdmi']+i.field('a733_hdmi_native','controller');where=h+i.field('a733_hdmi','edid')
    changed=bytearray(edid);changed[131]&=~0x40;changed[142]=0;changed[255]=0;changed[255]=(-sum(changed[128:255]))&255
    f.u.mem_write(where,bytes(changed))
   else:f.put(app+af['cancel'],1,4)
   previous=len(f.events);f.boundary=None;f.stopped=False
   f.u.emu_start(f.u.reg_read(UC_ARM64_REG_PC),0,count=1000000)
   assert f.stopped and f.boundary==('delay',1)
   output['context']=f.u.context_save();f.phase='scheduling';f.active_handle=None
   fault=bytes(f.u.mem_read(i.address('hdmi_audio_result'),160));fw=struct.unpack('<12Q',fault[:96])
   assert f.get(app+af['state'],4)==3 and fw[3]==3 and fw[6]==0 and fw[7]==0 and fw[11]==0
   assert f.get(i.address('hdmi_audio_result_ready'),4)==1 and len(f.events)==previous
   assert f.get(audio+nf['local']) and f.get(audio+nf['record']+i.field('device_memory_allocation','state'),4)==2
   if stage==7:
    channel=audio+nf['service']+i.field('dma_service','channels')
    assert f.get(channel+i.field('dma_service_channel','holds_buffers'),4)==1
    assert f.registers[0x04601100]==1 and f.dma_started==0
   f.checkpoint(out,label,fork_tasks);(out/(label+'-result.bin')).write_bytes(fault)
   fault_results.append(dict(case=label,passed=True,no_later_device_writes=True,retained_memory=True,source_consumed=0,wire_drained=False))
  save(out/'faults.json',fault_results)
  for path,digest in inputs.items():assert sha(path)==digest,('input changed during continuations',path)
  active_machine=m;active_tasks=tasks
  save(out/'summary.json',dict(passed=True,physical=False,image_sha256=inputs[str(args.image.resolve())],session_sha256=inputs[str(args.session.resolve())],contract_sha256=inputs[str(args.contract.resolve())],pcm_sha256=inputs[str(args.pcm.resolve())],result_sha256=sha(out/'result.bin'),ready=ready,stage=state,time_us=m.time,source_consumed=len(pcm),fifo_empty=True,wire_drained=False,tasks=[{k:v for k,v in t.items() if k!='context'} for t in tasks],scope=__doc__))
 except BaseException as e:
  active_machine.checkpoint(out,'stopped',active_tasks);save(out/'failure.json',dict(error=repr(e),pc=hex(active_machine.u.reg_read(UC_ARM64_REG_PC)),time_us=active_machine.time,trace=active_machine.trace[-30:]));raise
 files={str(p.relative_to(out)):dict(sha256=sha(p),bytes=p.stat().st_size) for p in out.rglob('*') if p.is_file()}
 save(out/'manifest.json',dict(schema='a733-hdmi-audio-model-v1',physical=False,inputs=inputs,files=files))
 print('PASS actual finite native HDMI audio; source consumed, FIFO empty; wire/sound unobserved')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--seed',action='store_true');p.add_argument('--checkpoint',type=Path);p.add_argument('--constructor-only',action='store_true');p.add_argument('--checkpoint-source-sha256');p.add_argument('--checkpoint-hardware-sha256');p.add_argument('--correct-initial-pmic-drive',action='store_true')
 for n in ('session','contract','pcm','edid'):p.add_argument('--'+n,type=Path)
 a=p.parse_args()
 if a.checkpoint:assert a.checkpoint_source_sha256 and a.checkpoint_hardware_sha256
 if a.seed:seed(a.image,a.output)
 else:
  assert all(getattr(a,n) is not None for n in ('session','contract','pcm','edid'));run(a)
if __name__=='__main__':main()
