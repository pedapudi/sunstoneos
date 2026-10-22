#!/usr/bin/env python3
"""Actual DisplayPort image constructors with modeled architectural services.

Scheduling, translation attributes, publication and physical admission are modeled.
The actual allocation, immutable pattern, IOMMU table constructor and native
provider constructors execute from the linked image. No hardware access occurs.
"""
import argparse,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('dp_image',ROOT/'checks/npu-large-native.py')
class Machine:
 def __init__(self,image,session,contract):
  self.image=image;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  self.regions=[(0x41000000,0x03000000),(0x78000000,0x01000000)]
  for a,n in self.regions:self.u.mem_map(a,n)
  self.u.reg_write(UC_ARM64_REG_PSTATE,13)
  for a,data in image.parts:
   if data:self.u.mem_write(a,data)
  self.time=100;self.scheduler=1;self.task=0;self.tasks=[];self.trace=[];self.translations=[]
  self.stopped=False;self.holders={};self.callname='';self.registers={};self.mmio_read=None;self.mmio_write=None
  self.mapped_pages=set();self.clocks=[]
  self.u.mem_write(0x41f00000,session);self.u.mem_write(0x42004000,contract)
  self.record=image.address('displayport_admission');self.u.mem_write(self.record,contract)
  self.app=image.address('dp_app');self.af=image.layouts['dp_application']
  self.board=image.address('board');self.bf=image.layouts['board_application']
  self.providers=image.address('providers');self.pf=image.layouts['board_provider'];self.shared=self.providers+self.pf['shared']
  runtime=image.address('a733_cached_runtime');rl=image.layouts['a733_cached_runtime'];cl=image.layouts['a733_smp_contract']
  for field in ('epoch','startup_deadline','diagnostic_deadline'):
   self.put(runtime+rl[field],struct.unpack_from('<Q',contract,cl[field])[0])
  names=['a733_smp_counter','a733_smp_device_preparation_admitted','a733_device_memory_ready','a733_cpu_coherence_publish',
   'vTaskStartScheduler','aarch64_device_translate','a733_counter_clock_init','a733_counter_clock_now','xTaskGetSchedulerState','xTaskGetCurrentTaskHandle',
   'xTaskCreateStatic','xTaskCreateStaticAffinitySet','xQueueGenericCreateStatic','xQueueCreateMutexStatic','vTaskEnterCritical','vTaskExitCritical',
   'xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay','vPortYield','platform_panic']
  self.hooks={image.address(k):k for k in names if image.symbols.get(k)}
  sites=set(self.hooks)|{0x43fff000}
  for a,data in image.parts:
   for off in range(0,len(data)-3,4):
    word=int.from_bytes(data[off:off+4],'little')
    if word&0xffffffe0 in (0xd5384240,0xd53be000,0xd53be020) or word==0xd5034fdf:sites.add(a+off)
  for a in sites:self.u.hook_add(UC_HOOK_CODE,self.code,begin=a,end=a)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read_mmio,begin=0x02000000,end=0x07ffffff)
  self.u.hook_add(UC_HOOK_MEM_WRITE,self.write_mmio,begin=0x02000000,end=0x07ffffff)
 def map_mmio(self,a,n):
  for p in range(a&~4095,(a+n+4095)&~4095,4096):
   if p not in self.mapped_pages:self.u.mem_map(p,4096);self.mapped_pages.add(p)
 def put(self,a,v,n=8):self.u.mem_write(a,int(v).to_bytes(n,'little'))
 def get(self,a,n=8):return int.from_bytes(self.u.mem_read(a,n),'little')
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def code(self,cpu,at,size,unused):
  if at==0x43fff000:self.u.emu_stop();return
  name=self.hooks.get(at)
  if name is None:
   word=self.get(at,4)
   if word==0xd5034fdf:self.u.reg_write(UC_ARM64_REG_PC,at+4);return
   if word&0xffffffe0 in (0xd5384240,0xd53be000,0xd53be020):
    value=4 if word&0xffffffe0==0xd5384240 else 24000000 if word&0xffffffe0==0xd53be000 else self.time*24
    self.u.reg_write(UC_ARM64_REG_X0+(word&31),value);self.u.reg_write(UC_ARM64_REG_PC,at+4);return
   raise AssertionError(('Unknown instruction hook',hex(at)))
  a=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)];self.trace.append(name)
  if name=='platform_panic':raise AssertionError(('panic',bytes(self.u.mem_read(a[0],80)).split(b'\0')[0],a[1],self.callname,self.trace[-20:]))
  if name=='a733_smp_counter':self.ret(self.time*24)
  elif name in ('a733_smp_device_preparation_admitted','a733_device_memory_ready','a733_cpu_coherence_publish'):self.ret(1)
  elif name=='aarch64_device_translate':
   attr=0x44 if 0x78000000<=a[0]<0x79000000 else 0xff if a[0]>=0x40000000 else 0
   self.u.mem_write(a[2],struct.pack('<QII',a[0],attr,3 if attr else 2));self.translations.append(a[:2]);self.ret(1)
  elif name=='a733_counter_clock_init':
   l=self.image.layouts['a733_counter_clock'];self.put(a[0]+l['frequency'],24000000,4);self.put(a[0]+l['initialized'],1,4);self.clocks.append(a[0]);self.ret(1)
  elif name=='a733_counter_clock_now':self.put(a[1],self.time);self.ret(1)
  elif name=='xTaskGetSchedulerState':self.ret(self.scheduler)
  elif name=='xTaskGetCurrentTaskHandle':self.ret(self.task)
  elif name in ('xTaskCreateStatic','xTaskCreateStaticAffinitySet'):self.tasks.append(a[:7]);self.ret(a[6])
  elif name=='xQueueGenericCreateStatic':self.ret(a[3])
  elif name=='xQueueCreateMutexStatic':self.ret(a[1])
  elif name in ('vTaskEnterCritical','vTaskExitCritical'):self.ret()
  elif name=='xQueueGetMutexHolder':self.ret(self.holders.get(a[0],0))
  elif name=='xQueueSemaphoreTake':
   if self.holders.get(a[0],0):self.ret(0)
   else:self.holders[a[0]]=self.task;self.ret(1)
  elif name=='xQueueGenericSend':
   assert self.holders.get(a[0])==self.task;self.holders[a[0]]=0;self.ret(1)
  elif name in ('vTaskDelay','vPortYield'):self.time+=a[0]*1000 if name=='vTaskDelay' else 1;self.ret();self.stopped=True;self.u.emu_stop()
  elif name=='vTaskStartScheduler':self.stopped=True;self.u.emu_stop()
  else:raise AssertionError(('Unknown boundary',name,a))
 def read_mmio(self,u,access,a,n,value,unused):
  if self.mmio_read is not None:value=self.mmio_read(a,n)
  else:value=self.registers.get(a,0)
  if value is not None:self.put(a,value,n)
 def write_mmio(self,u,access,a,n,value,unused):
  self.registers[a]=value
  if self.mmio_write is not None:self.mmio_write(a,n,value)
 def call(self,name,*args,unit=None,budget=10000000):
  self.callname=name;self.stopped=False;self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  self.u.emu_start(self.image.address(name,unit),0,count=budget)
  assert self.stopped or self.u.reg_read(UC_ARM64_REG_PC)==0x43fff000,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)),self.trace[-12:])
  return self.u.reg_read(UC_ARM64_REG_X0)
 def checkpoint(self,out,label):
  out=Path(out);out.mkdir(parents=True,exist_ok=True)
  files=[]
  for i,(a,n) in enumerate(self.regions):
   p=out/(label+f'-ram-{i}.bin');p.write_bytes(bytes(self.u.mem_read(a,n)));files.append([a,n,str(p)])
  for p in sorted(self.mapped_pages):files.append([p,4096,bytes(self.u.mem_read(p,4096))])
  state={k:getattr(self,k) for k in ('time','scheduler','task','tasks','holders','registers','clocks','mapped_pages')}
  state['ram']=files;state['cpu']=self.u.context_save();(out/(label+'.pickle')).write_bytes(pickle.dumps(state));return state

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for k in ('image','session','contract','output'):p.add_argument('--'+k,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);image=base.Image(a.image)
 raw=a.session.read_bytes();contract=a.contract.read_bytes();m=Machine(image,raw,contract)
 assert m.call('a733_smp_contract_valid',m.record,200)==1
 assert m.call('displayport_board_prepare',m.record)==1
 assert m.call('displayport_session_prepare',m.record)==1
 assert m.call('a733_memory_uncached_prepare')==1
 # Hashing the complete immutable frame executes the linked SHA256 and
 # byte-copy routines; keep a checkpoint if its instruction bound expires.
 try:m.call('a733_smp_diagnostic_start',budget=1000000000)
 except BaseException:
  m.checkpoint(a.output,'stopped')
  (a.output/'failure.json').write_text(json.dumps(dict(pc=m.u.reg_read(UC_ARM64_REG_PC),trace=m.trace[-40:],physical=False),indent=2)+'\n')
  raise
 assert m.stopped and len(m.tasks)==6,(m.tasks,m.trace[-20:])
 f=m.app+m.af['memory'];fl=image.layouts['a733_display_framebuffer'];el=image.layouts['device_memory_extent']
 cpu=m.get(f+fl['extent']+el['cpu']);physical=m.get(f+fl['extent']+el['physical']);size=m.get(f+fl['extent']+el['bytes'])
 assert size==2097152 and cpu==physical and cpu%16384==0 and 0x78000000<=cpu<cpu+size<=0x79000000
 data=bytes(m.u.mem_read(cpu,size));(a.output/'allocation.bin').write_bytes(data);(a.output/'page-table.bin').write_bytes(data[:32768])
 digest=lambda b:hashlib.sha256(b).hexdigest()
 proof=dict(schema='a733-displayport-allocation-v1',model=True,physical_authority=False,image_sha256=digest(a.image.read_bytes()),session_sha256=digest(raw),
  allocation=dict(cpu=cpu,physical=physical,bytes=size,state=2),spans=[dict(iova=0x10000000,physical=physical+32768,bytes=size-32768,access=1)],
  table_physical=physical,table_bytes=32768,table_sha256=digest(data[:32768]),pixel_physical=physical+36864,pixel_bytes=1843200,pixel_sha256=digest(data[36864:36864+1843200]),allocation_sha256=digest(data),scanned_masters=0)
 (a.output/'allocation.json').write_text(json.dumps(proof,indent=2)+'\n');m.checkpoint(a.output,'constructor')
 (a.output/'summary.json').write_text(json.dumps(dict(passed=True,physical=False,scope=__doc__,tasks=m.tasks,allocation=proof),indent=2)+'\n')
 files={str(f.relative_to(a.output)):{'sha256':digest(f.read_bytes()),'bytes':f.stat().st_size} for f in a.output.iterdir() if f.is_file()}
 manifest=dict(schema='a733-displayport-constructor-checkpoint-v1',image_sha256=digest(a.image.read_bytes()),session_sha256=digest(raw),contract_sha256=digest(contract),machine_sha256=digest(Path(__file__).read_bytes()),files=files,physical=False)
 (a.output/'constructor-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 print('PASS actual constructors and retained allocation',hex(cpu),size)
if __name__=='__main__':main()
