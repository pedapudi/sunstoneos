#!/usr/bin/env python3
"""Execute the final ISP0 application with explicit modeled hardware effects.

Actual allocation, upload, media/clock/rail ledgers, ISP0/DMA/IOMMU, copy and
serializer instructions run. PMIC replies, lock scheduling, device mappings,
register effects, USB calls and ISP-produced pixel bytes are modeled. Pixel
bytes exercise ownership/copy only; they are not an ISP numerical oracle.
"""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X29,UC_ARM64_REG_X30,UC_ARM64_REG_SP_EL0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1];STOP=0x50fff000;STACK=0x50ffeff0;SCRATCH=0x50e00000
spec=importlib.util.spec_from_file_location('isp0_elf_helpers',ROOT/'checks/jpeg-encode-native.py');helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
class Machine:
 def __init__(self,image,session,contract,raw):
  self.im=image;self.session=session;self.contract=contract;self.raw=raw;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  for a,n in [(0x41000000,0x3000000),(0x50000000,0x1000000),(0x02002000,0x2000),(0x07010000,0x1000),(0x07060000,0xa000),(0x07090000,0x1000),(0x03900000,0x20000),(0x05800000,0x10000),(0x05830000,0x1000),(0x05900000,0x20000)]:self.u.mem_map(a,n)
  self.u.reg_write(UC_ARM64_REG_SP_EL0,STACK);self.u.reg_write(UC_ARM64_REG_PSTATE,4)
  for a,data in image.parts:self.u.mem_write(a,data)
  self.base=image.address('isp0');self.f=image.layouts['isp0_application'];self.cf=image.layouts['a733_smp_contract'];self.bf=image.layouts['a733_board_session'];self.now=struct.unpack_from('<Q',contract,self.cf['armed_at'])[0]//24+100;self.done=False;self.failed=False;self.held=False;self.regs={};self.events=[];self.started=0;self.variant='nominal';self.callbacks={};self.incoming=bytearray();self.tx=[];self.notification=0;self.injections={}
  self.u.mem_write(self.base,session);self.u.mem_write(image.address('board'),session[:592]);self.u.mem_write(0x41f00000,session);self.u.mem_write(SCRATCH,contract)
  bf=image.layouts['board_application'];board=image.address('board');self.put(board+bf['session']+self.bf['deadline_us'],struct.unpack_from('<Q',contract,self.cf['diagnostic_deadline'])[0]//24,8)
  self.put(self.base+self.f['handle'],1,8)
  self.shared=image.address('providers')+image.layouts['board_provider']['shared'];nf=image.layouts['a733_native_usb'];self.clock=self.shared+nf['clocks'];self.gpu=self.shared+nf['gpu'];self.rails=self.shared+nf['rails'];self.nf=nf
  names=['a733_cpu_coherence_publish','aarch64_device_cache_stride','board_live','board_fail','a733_smp_counter','a733_counter_clock_now','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState','aarch64_device_translate','a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','a733_pmic_submit','a733_pmic_result','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','tud_speed_get']
  self.hooks={image.address(n):n for n in names};self.u.hook_add(UC_HOOK_CODE,self.code)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read_mmio,begin=0x02000000,end=0x070fffff);self.u.hook_add(UC_HOOK_MEM_WRITE,self.write_mmio,begin=0x02000000,end=0x070fffff)
 def get(self,a,n=8):return int.from_bytes(self.u.mem_read(a,n),'little')
 def put(self,a,v,n=4):self.u.mem_write(a,int(v).to_bytes(n,'little'))
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def args(self):return [self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
 def memory(self,index):return self.get(self.base+self.f['memory']+index*8)
 def code(self,u,pc,size,unused):
  if pc==STOP:self.done=True;u.emu_stop();return
  if pc in self.callbacks:
   kind=self.callbacks[pc];x=self.args()
   if kind=='read':self.put(x[2],self.regs.get(x[1],0));self.ret(1)
   elif kind=='update':
    old=self.regs.get(x[1],0);assert old&x[2]==x[2];self.regs[x[1]]=old|x[3];self.ret(1)
   else:raise AssertionError('Unexpected rail hardware mutation')
   return
  name=self.hooks.get(pc)
  if not name:return
  x=self.args()
  if name=='vTaskDelay':self.done=True;u.emu_stop()
  elif name=='board_live':self.ret(not self.failed)
  elif name=='a733_cpu_coherence_publish':self.ret(1)
  elif name=='aarch64_device_cache_stride':self.ret(64)
  elif name=='board_fail':self.failed=True;self.ret()
  elif name=='a733_smp_counter':self.now+=1;self.ret(self.now*24)
  elif name=='a733_counter_clock_now':self.now+=1;self.put(x[1],self.now,8);self.ret(1)
  elif name=='xTaskGetCurrentTaskHandle':self.ret(1)
  elif name=='xTaskGetSchedulerState':self.ret(2)
  elif name=='aarch64_device_translate':
   attr,share=(0x44,3) if 0x41c00000<=x[0]<0x41e00000 else (0xff,3) if self.im.address('__device_pool_start')<=x[0]<self.im.address('__device_pool_end') else (0,2)
   self.u.mem_write(x[2],struct.pack('<Q2I',x[0],attr,share));self.ret(1)
  elif name=='a733_platform_lock_enter':assert not self.held;self.held=True;self.ret(1)
  elif name=='a733_platform_lock_leave':assert self.held;self.held=False;self.ret()
  elif name=='a733_platform_lock_retain':self.held=True;self.ret(1)
  elif name=='a733_pmic_submit':
   f=self.im.layouts['a733_pmic_request'];self.put(x[5]+f['id'],1,8);self.put(x[5]+f['owner'],x[1],8);self.ret(0)
  elif name=='a733_pmic_result':
   f=self.im.layouts['a733_pmic_snapshot'];values={'rail':1,'page_before':0,'chip':3,'revision':0,'enable':2,'selector':30,'page_after':0,'enabled':1,'decoded':1,'programmed_uv':800000}
   for k,v in values.items():self.put(x[2]+f[k],v,1 if k in ['page_before','chip','revision','enable','selector','page_after'] else 4)
   self.ret(2)
  elif name=='tud_vendor_n_available':self.ret(len(self.incoming))
  elif name=='tud_vendor_n_read':n=min(x[2],len(self.incoming));self.u.mem_write(x[1],bytes(self.incoming[:n]));del self.incoming[:n];self.ret(n)
  elif name=='tud_vendor_n_write':self.tx.append(bytes(self.u.mem_read(x[1],x[2])));self.ret(x[2])
  elif name=='tud_vendor_n_write_flush':self.ret(0)
  elif name=='tud_speed_get':self.ret(1) # FS; transport effects remain modeled.
  else:self.ret(1)
 def read_mmio(self,u,access,a,size,value,unused):
  if self.started and self.variant=='micro-fault' and a==0x03900838:self.inject('micro');self.regs[a]=1
  if self.started and self.variant=='global-fault' and a==0x03900088:self.inject('global');self.regs[a]=1
  if self.variant=='late-fault' and self.get(self.base+self.f['copied']) and a==0x03900838:self.inject('late');self.regs[a]=1
  if 0x03900000<=a<0x03920000:self.events.append(['iommu-read',a-0x03900000])
  self.put(a,self.regs.get(a,0),size)
 def write_mmio(self,u,access,a,size,value,unused):
  if self.started and a not in range(0x03900000,0x03920000):self.events.append(['post-submit-write',a,value])
  if a in (0x0590002c,0x05910030):
   if self.variant!='stale-status':self.regs[a]=self.regs.get(a,0)&~value
   elif a==0x0590002c:self.inject('stale-clear-refusal')
  elif a==0x05830254:self.regs[a]=self.regs.get(a,0)&~value
  else:self.regs[a]=value
  if a==0x05800804 and value&0x10:self.effect()
 def mapped(self,iova,n,permission):
  tables=self.base+self.f['tables'];tf=self.im.layouts['a733_iommu_tables'];sf=self.im.layouts['a733_iommu_span'];ss=self.im.sizes['a733_iommu_span']
  for i in range(self.get(tables+tf['count'],4)):
   at=tables+tf['spans']+i*ss;va=self.get(at+sf['iova']);size=self.get(at+sf['bytes']);access=self.get(at+sf['access'],4)
   if va<=iova and iova-va<=size and n<=size-(iova-va) and access&permission==permission:return self.get(at+sf['physical'])+iova-va
  raise AssertionError(('Unmapped modeled DMA',hex(iova),n,permission))
 def inject(self,name):
  if name not in self.injections:self.injections[name]=1;self.events.append(['injection',name])
 def effect(self):
  assert not self.started;self.started=1;self.events.append(['modeled-ISP-pixel-writes'])
  raw=bytes(self.u.mem_read(self.mapped(0x100000,614464,1),614464));assert raw[:614400]==self.raw and raw[614400:]==bytes(64)
  assert self.regs[0x05800820]==614400 and self.regs[0x05800824]==0x100000>>2
  assert self.regs[0x05800808]&0x073f00ff==0x062c0008
  assert self.regs[0x05900024]==0x200000>>2 and self.regs[0x0590003c]==0x210000>>2 and self.regs[0x05900040]==0x220000>>2
  assert self.regs[0x05830220]==0x300000>>2 and self.regs[0x05830228]==0x380000>>2
  assert self.regs[0x05910050]==0x230000>>2
  load=bytes(self.u.mem_read(self.mapped(0x200000,0x7700,1),0x7700));assert load[0x130:0x138]==bytes(8)
  assert struct.unpack_from('<4I',load,0x160)==(0x1e0c1e1e,0x00240130,0x241e0c1e,0x000c0130)
  # Explicitly modeled pixel writes; no demosaic/colour algorithm is claimed.
  self.expected=bytes((i*7+23)&255 for i in range(307200))+bytes([0x40,0xc0])*153600
  self.u.mem_write(self.mapped(0x300000,307200,2),self.expected[:307200]);self.u.mem_write(self.mapped(0x380000,307200,2),self.expected[307200:])
  if self.variant in ('missing-save','dma-error'):self.inject(self.variant)
  self.regs[0x05800804]=1;self.regs[0x0590002c]=0xb if self.variant=='missing-save' else 0xf;self.regs[0x05910030]=0x100;self.regs[0x05830254]=0x101 if self.variant=='dma-error' else 1
  if self.variant=='guard':self.inject('guard');self.put(self.memory(2)-1,0,1)
 def call(self,name,*args,local=None,count=100000000):
  self.done=False;self.u.reg_write(UC_ARM64_REG_SP_EL0,STACK);self.u.reg_write(UC_ARM64_REG_PSTATE,4);self.u.reg_write(UC_ARM64_REG_SP,STACK);self.u.reg_write(UC_ARM64_REG_LR,STOP)
  for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  address=self.im.local(name,local) if local else self.im.address(name)
  try:self.u.emu_start(address,0,count=count)
  except Exception as e:
   self.checkpoint(self.output/'failure.pkl.gz');raise RuntimeError((name,hex(self.u.reg_read(UC_ARM64_REG_PC)),self.get(self.base+self.f['stage'],4),self.events[-8:])) from e
  if not self.done:self.checkpoint(self.output/'budget-stop.pkl.gz')
  assert self.done,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)));return self.u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 def prepare(self):
  # Consume root's one post-launch artifact, retaining original producer bytes.
  self.u.mem_write(SCRATCH,self.contract);assert self.call('a733_smp_contract_valid',SCRATCH,self.now*24)==1
  assert self.call('isp0_session_prepare',SCRATCH)==1
  assert self.call('isp0_upload_allocate')==1
  supplied=self.base+self.f['supplied'];packet=struct.pack('<4I2Q',0x55495053,1,614400,0,*struct.unpack_from('<2Q',self.session,24))+self.raw;at=0
  while at<len(packet):
   n=min(4096,self.call('isp0_input_remaining',supplied),len(packet)-at);assert n
   self.u.mem_write(SCRATCH,packet[at:at+n]);r=self.call('isp0_input_feed',supplied,self.base,self.im.address('a733_counter_clock_now'),0,self.deadline(),SCRATCH,n);assert r in (0,1);at+=n
  assert r==1
  clock=self.im.address('a733_counter_clock_now');assert self.call('a733_clock_leases_init',self.clock,clock,0)==1
  pointers=[clock]
  for i,kind in enumerate(('read','update','rail')):
   a=0x50f00000+4*i;self.callbacks[a]=kind;self.u.mem_write(a,bytes.fromhex('c0035fd6'));pointers.append(a)
  self.u.mem_write(SCRATCH,struct.pack('<4Q',*pointers));assert self.call('a733_gpu_resources_init',self.gpu,SCRATCH,0)==1
  self.u.mem_write(SCRATCH,struct.pack('<5Q',self.clock,self.gpu,999,clock,0));assert self.call('a733_rail_leases_init',self.rails,SCRATCH)==1
  self.put(self.shared+self.nf['pmic_ready'],1)
  self.regs.update({0x07090160:0x10000002,0x070101ac:1,0x02002000:0xf8636300,0x02002120:0xf8303500,0x02003910:0x00770077,0x02003990:0x00770000,0x020025c0:0x110103ff,0x020025e0:0xc1055803,0x020025e4:0x40302,0x0200258c:0x10007,0x020025b4:0x10007,0x02002500:0,0x02002580:0x80000000,0x02002588:0x80000000})
  for i in (0,1,2,3,8):self.regs[0x07060000+i*0x1000]=self.regs[0x07060008+i*0x1000]=8
 def deadline(self):return self.get(self.im.address('board')+self.bf['deadline_us'])-10000
 def rows(self):
  pf=self.im.layouts['device_memory_pool'];af=self.im.layouts['device_memory_allocation'];asz=self.im.sizes['device_memory_allocation'];rows={}
  for name,count in [('boot_pool',2),('uncached_pool',8)]:
   pool=self.im.address(name);back=pool+pf['backing'];out=[]
   for i in range(count):
    a=pool+pf['allocations']+i*asz;first=self.get(a+af['first'],4);pages=self.get(a+af['pages'],4)
    if pages:out.append(dict(slot=i,cpu=self.get(back)+4096*first,physical=self.get(back+8)+4096*first,bytes=4096*pages,state=self.get(a+af['state'],4)))
   rows[name]=out
  return rows
 def checkpoint(self,path):
  state={k:getattr(self,k) for k in ('now','failed','held','regs','events','started','variant','callbacks','incoming','tx','notification','injections')}
  state['cpu']={r:self.u.reg_read(r) for r in [*[UC_ARM64_REG_X0+i for i in range(29)],UC_ARM64_REG_X29,UC_ARM64_REG_X30,UC_ARM64_REG_SP_EL0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_SP_EL0,UC_ARM64_REG_PSTATE]}
  state.update(memory=[(a,bytes(self.u.mem_read(a,n))) for a,n in [(0x41000000,0x3000000),(0x50000000,0x1000000)]],image=self.image_hash(),session=hashlib.sha256(self.session).hexdigest())
  if hasattr(self,'expected'):state['expected']=self.expected
  path.write_bytes(gzip.compress(pickle.dumps(state),compresslevel=1));return state
 def restore(self,state):
  state=pickle.loads(pickle.dumps(state))
  assert state['image']==self.image_hash() and state['session']==hashlib.sha256(self.session).hexdigest()
  for a,data in state['memory']:self.u.mem_write(a,data)
  for r,v in state['cpu'].items():self.u.reg_write(r,v)
  for k,v in state.items():
   if k not in ('memory','image','session','cpu'):setattr(self,k,v)
 def image_hash(self):return hashlib.sha256(self.image_path.read_bytes()).hexdigest()
 def tick(self):self.call('isp0_task',0)
 def run(self):
  for i in range(700):
   self.tick()
   if self.variant=='missing-save' and self.started and i>200:self.now=self.deadline()+1
   if self.get(self.base+self.f['result'],4):return
  raise AssertionError(('Unbounded model steps',self.variant,self.get(self.base+self.f['stage'],4)))

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for k in ('image','session','contract','input','output'):p.add_argument('--'+k,type=Path,required=True)
 p.add_argument('--resume',type=Path);p.add_argument('--cases',default='nominal,missing-save,dma-error,micro-fault,global-fault,late-fault,guard,stale-status')
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);im=helper.Image(a.image);m=Machine(im,a.session.read_bytes(),a.contract.read_bytes(),a.input.read_bytes());m.image_path=a.image;m.output=a.output
 # Resolve native boundaries and ordinary pool geometry before executing cases.
 for n in ['isp0_upload_allocate','isp0_allocate','isp0_task','a733_isp0_poll','a733_camera_dma_poll','a733_iommu_poll']:im.address(n)
 assert im.address('__device_pool_end')-im.address('__device_pool_start')==2097152
 assert im.address('__uncached_pool_end')-im.address('__uncached_pool_start')==2097152
 results={}
 if a.resume:
  state=pickle.loads(gzip.decompress(a.resume.read_bytes()));m.restore(state)
 else:
  m.prepare()
  for i in range(150):
   m.tick()
   if m.get(m.base+m.f['stage'],4)==1:break
   assert not m.get(m.base+m.f['result'],4),(m.get(m.base+m.f['result'],4),m.events[-12:])
  else:raise AssertionError('Provider setup did not complete')
  assert m.call('a733_iommu_validate',m.base+m.f['tables'])==1 and m.call('isp0_guards')==1
  state=m.checkpoint(a.output/'prepared.pkl.gz');table=m.memory(0);data=bytes(m.u.mem_read(table,32768));(a.output/'tables.bin').write_bytes(data)
  tf=im.layouts['a733_iommu_tables'];sf=im.layouts['a733_iommu_span'];ss=im.sizes['a733_iommu_span'];tables=m.base+m.f['tables'];spans=[]
  for i in range(m.get(tables+tf['count'],4)):
   at=tables+tf['spans']+i*ss;spans.append({k:m.get(at+sf[k],4 if k=='access' else 8) for k in ('iova','physical','bytes','access')})
  report=dict(schema='a733-isp0-native-allocation-model-v1',modeled=True,physical_authority=False,image=m.image_hash(),session=hashlib.sha256(m.session).hexdigest(),allocations=m.rows(),spans=spans,table_physical=table,table_bytes=32768,table_sha256=hashlib.sha256(data).hexdigest())
  (a.output/'allocation.json').write_text(json.dumps(report,indent=2)+'\n')
 for variant in a.cases.split(','):
  m.restore(state)
  m.variant=variant
  assert m.started==0 and not m.failed and m.get(m.base+m.f['stage'],4)==1 and m.get(m.base+m.f['result'],4)==0 and m.get(m.base+m.f['copied'])==0
  m.injections={}
  if variant=='stale-status':m.regs[0x0590002c]=0xf
  m.run();result=m.get(m.base+m.f['result'],4)
  assert result==(1 if variant=='nominal' else 2),(variant,result)
  assert m.started==(0 if variant=='stale-status' else 1),(variant,m.started,m.events[-8:])
  assert all(row['state']==2 for row in m.rows()['uncached_pool'])
  if variant=='nominal':
   assert m.started==1 and m.get(m.base+m.f['copied'])==614400 and bytes(m.u.mem_read(m.get(m.base+m.f['payload']),614400))==m.expected
   m.checkpoint(a.output/'copied.pkl.gz');(a.output/'modeled-pixels.nv16').write_bytes(m.expected)
  else:
   assert not m.get(m.base+m.f['complete'],4) and m.failed
   assert len(m.injections)==1 and sum(m.injections.values())==1,(variant,m.injections)
   stage=m.get(m.base+m.f['stage'],4);dma=m.base+m.f['dma'];df=im.layouts['a733_camera_dma'];io=m.base+m.f['iommu'];iff=im.layouts['a733_iommu'];fault=io+iff['fault'];ff=im.layouts['a733_iommu_fault']
   if variant=='missing-save':assert stage==4 and m.now>=m.deadline() and m.get(dma+df['state'],4)==7
   elif variant=='dma-error':assert m.get(dma+df['state'],4)==15 and m.get(dma+df['events'],4)&0x100
   elif variant in ('micro-fault','late-fault'):assert m.get(io+iff['state'],4)==7 and m.get(fault+ff['micro_status']+8,4)==1
   elif variant=='global-fault':assert m.get(io+iff['state'],4)==7 and m.get(fault+ff['bank_status'],4)==1
   elif variant=='guard':assert stage==4 and m.get(dma+df['state'],4)==15 and m.get(m.memory(2)-1,1)==0
   elif variant=='stale-status':assert m.get(m.base+m.f['engine']+im.layouts['a733_isp0']['fault'],4)==1
   if variant=='late-fault':assert m.get(m.base+m.f['copied'])==614400
   else:assert m.get(m.base+m.f['copied'])==0
   m.checkpoint(a.output/(variant+'.pkl.gz'))
   if variant=='guard':assert m.call('isp0_guards')==0
  results[variant]=dict(passed=True,result=result,triggers=m.started,bytes_copied=m.get(m.base+m.f['copied']),device_extents_retained=True,injections=m.injections,native_stage=m.get(m.base+m.f['stage'],4))
  (a.output/'progress.json').write_text(json.dumps(results,indent=2)+'\n')
 (a.output/'result.json').write_text(json.dumps(dict(status='pass',cases=results,image_sha256=m.image_hash(),physical_access=False,numerical_qualified=False,scope=__doc__),indent=2)+'\n')
if __name__=='__main__':main()
