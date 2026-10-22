#!/usr/bin/env python3
"""Execute linked DI301 allocation/task code; model hardware and external services explicitly."""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_LR,UC_ARM64_REG_SP,UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1];STOP=0x90fff000;STACK=0x90ffeff0;SCRATCH=0x90e00000
s=importlib.util.spec_from_file_location('deinterlace_image',ROOT/'checks/jpeg-encode-native.py');helpers=importlib.util.module_from_spec(s);s.loader.exec_module(helpers)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
class Machine:
 def __init__(self,im,session):
  self.im=im;self.session=session;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  self.ram=[(0x41000000,0x3000000),(0x48000000,0x1000000),(0x78000000,0x1000000),(0x90000000,0x1000000)]
  for at,n in self.ram:self.u.mem_map(at,n)
  for at,data in im.parts:
   if data:self.u.mem_write(at,data)
  self.base=im.address('deinterlace');self.fields=im.layouts['deinterlace_application'];self.now=1000;self.done=False;self.held=False;self.started=0;self.status=1;self.events=[];self.regs={};self.variant='nominal';self.callbacks={};self.hooks={};self.installed=set()
  self.u.mem_write(self.base,session);self.u.mem_write(im.address('board'),session[:592]);self.put(im.address('board')+160,1000000,8)
  for name in ['a733_device_memory_ready','board_live','a733_counter_clock_now','vTaskDelay','aarch64_device_translate','a733_platform_lock_enter','a733_platform_lock_leave','a733_pmic_submit','a733_pmic_result','vTaskEnterCritical','vTaskExitCritical']:
   address=im.address(name);self.hooks[address]=name;self.install(address)
  self.install(STOP)
  self.mmio=[(0x02002000,0x2000),(0x07010000,0x1000),(0x07060000,0x2000),(0x07090000,0x1000),(0x03900000,0x20000),(0x05400000,0x40000)]
  for at,n in self.mmio:
   self.u.mem_map(at,n);self.u.hook_add(UC_HOOK_MEM_READ,self.read_mmio,begin=at,end=at+n-1);self.u.hook_add(UC_HOOK_MEM_WRITE,self.write_mmio,begin=at,end=at+n-1)
 def install(self,address):
  if address not in self.installed:self.u.hook_add(UC_HOOK_CODE,self.hook,begin=address,end=address);self.installed.add(address)
 def get(self,at,n=8):return int.from_bytes(self.u.mem_read(at,n),'little')
 def put(self,at,value,n=4):self.u.mem_write(at,int(value).to_bytes(n,'little'))
 def args(self):return [self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
 def ret(self,value=0):self.u.reg_write(UC_ARM64_REG_X0,value&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def hook(self,u,pc,size,unused):
  if pc==STOP:self.done=True;u.emu_stop();return
  a=self.args()
  if pc in self.callbacks:
   if self.callbacks[pc]=='read':self.put(a[2],self.regs.get(a[1],0));self.ret(1)
   elif self.callbacks[pc]=='update':
    value=self.regs.get(a[1],0);assert (value&a[2])==a[2];self.regs[a[1]]=value|a[3];self.ret(1)
   else:raise AssertionError('Unexpected power write callback')
   return
  name=self.hooks.get(pc)
  if name=='vTaskDelay':self.done=True;u.emu_stop();return
  if name in ['a733_device_memory_ready','board_live']:self.ret(1);return
  if name=='a733_counter_clock_now':self.now+=1;self.put(a[1],self.now,8);self.ret(1);return
  if name=='aarch64_device_translate':
   # The extended pools carry their MAIR attribute and inner shareability;
   # other addresses keep the earlier attribute-free model.
   attributes,shareability=(0x44,3) if 0x78000000<=a[0]<0x79000000 else (0xff,3) if 0x48000000<=a[0]<0x78000000 else (0,2)
   t=self.im.layouts['aarch64_translation'];self.put(a[2]+t['physical'],a[0],8);self.put(a[2]+t['attributes'],attributes,4);self.put(a[2]+t['shareability'],shareability,4);self.ret(1);return
  if name=='a733_platform_lock_enter':assert not self.held;self.held=True;self.ret(1);return
  if name=='a733_platform_lock_leave':assert self.held;self.held=False;self.ret();return
  if name in ['vTaskEnterCritical','vTaskExitCritical']:self.ret();return # One emulated core: the ledger's critical section has no effect.
  if name=='a733_pmic_submit':
   f=self.im.layouts['a733_pmic_request'];self.put(a[5]+f['id'],1,8);self.put(a[5]+f['owner'],a[1],8);self.ret(0);return
  if name=='a733_pmic_result':
   f=self.im.layouts['a733_pmic_snapshot'];values={'rail':1,'page_before':0,'chip':3,'revision':0,'enable':2,'selector':30,'page_after':0,'enabled':1,'decoded':1,'programmed_uv':800000}
   for key,value in values.items():self.put(a[2]+f[key],value,1 if key in ['page_before','chip','revision','enable','selector','page_after'] else 4)
   self.ret(2);return
 def call(self,name,*args):
  self.done=False;self.u.reg_write(UC_ARM64_REG_SP,STACK);self.u.reg_write(UC_ARM64_REG_LR,STOP)
  for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  self.u.emu_start(self.im.address(name),0,count=200000000);assert self.done,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  return self.u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 def pools(self):
  # The actual extended preparations check each 2 MiB block's translation and
  # type the pools: dma32 write-back and Normal non-cacheable.
  assert self.call('a733_memory_reservations_prepare')==1 and self.call('a733_memory_uncached_prepare')==1
  assert self.call('deinterlace_upload_allocate')==1
 def upload(self,raw,kind='nominal'):
  supplied=self.base+self.fields['supplied'];epoch,generation=struct.unpack_from('<2Q',self.session,24)
  packet=struct.pack('<4I2Q',0x55494453,1,len(raw),0,epoch,generation+(kind=='generation'))+raw
  if kind=='hash':packet=packet[:-1]+bytes([packet[-1]^1])
  if kind=='truncated':packet=packet[:-1]
  at=0;r=0
  while at<len(packet):
   n=min(197,self.call('deinterlace_input_remaining',supplied),len(packet)-at);assert n
   self.u.mem_write(SCRATCH,packet[at:at+n]);r=self.call('deinterlace_input_feed',supplied,self.base,self.im.address('a733_counter_clock_now'),0,990000,SCRATCH,n);at+=n
   if r==0xffffffff:return -1
  return r
 def rows(self):
  result={};pf=self.im.layouts['device_memory_pool'];af=self.im.layouts['device_memory_allocation'];size=self.im.sizes['device_memory_allocation']
  for name,count in [('boot_pool',2),('uncached_pool',3)]:
   base=self.im.address(name);back=base+pf['backing'];rows=[]
   for i in range(count):
    p=base+pf['allocations']+i*size;first=self.get(p+af['first'],4);pages=self.get(p+af['pages'],4)
    if pages:rows.append(dict(slot=i,cpu=self.get(back)+first*4096,physical=self.get(back+8)+first*4096,bytes=pages*4096,state=self.get(p+af['state'],4)))
   result[name]=rows
  return result
 def configure(self):
  im=self.im;clk=im.address('a733_counter_clock_now');native=im.address('providers')+im.layouts['board_provider']['shared'];nf=im.layouts['a733_native_usb'];clock=native+nf['clocks'];gpu=native+nf['gpu'];rails=native+nf['rails']
  assert self.call('a733_clock_leases_init',clock,clk,0)==1;pointers=[clk]
  for i,kind in enumerate(['read','update','rail']):
   at=0x90f00000+4*i;self.callbacks[at]=kind;self.install(at);self.u.mem_write(at,bytes.fromhex('c0035fd6'));pointers.append(at)
  self.u.mem_write(SCRATCH,struct.pack('<4Q',*pointers));assert self.call('a733_gpu_resources_init',gpu,SCRATCH,0)==1
  self.u.mem_write(SCRATCH,struct.pack('<5Q',clock,gpu,999,clk,0));assert self.call('a733_rail_leases_init',rails,SCRATCH)==1
  self.put(native+nf['pmic_ready'],1)
  self.regs.update({0x07090160:0x10008002,0x02002000:0xf8675f00,0x020020a0:0xfe126310,0x02003908:0x0a000a00,0x02003988:0x0a000000,0x070101ac:1,0x07061000:8,0x07061008:8,0x020025c0:1<<5,0x020025e0:1<<11,0x0200258c:0x10007,0x020025b4:0x10007})
  for i,off in enumerate([0x500,0x580,0x588]):self.regs[0x02002000+off]=struct.unpack_from('<I',self.session,488+4*i)[0]
 def read_mmio(self,u,access,at,size,value,unused):
  if 0x03900000<=at<0x03920000:
   off=at-0x03900000
   if self.started and self.variant=='global' and off==0x10088:self.regs[at]=1
   if self.started and self.variant=='micro' and off==0x10a38:self.regs[at]=1
   if self.variant=='late-fault' and self.get(self.base+self.fields['copied']) and off==0x10a38:self.regs[at]=1
  if at==0x05400018:
   value=self.status
   if self.variant=='unstable' and self.get(self.base+self.fields['copied']):value|=0x100
   self.put(at,value);return
  self.put(at,self.regs.get(at,0))
 def mapped(self,iova,n,permission):
  t=self.base+self.fields['tables'];tf=self.im.layouts['a733_iommu_tables'];sf=self.im.layouts['a733_iommu_span'];size=self.im.sizes['a733_iommu_span']
  for i in range(self.get(t+tf['count'],4)):
   at=t+tf['spans']+i*size;va=self.get(at+sf['iova']);length=self.get(at+sf['bytes']);access=self.get(at+sf['access'],4)
   if va<=iova and n<=length and iova-va<=length-n and access&permission==permission:return self.get(at+sf['physical'])+iova-va
  raise AssertionError(('Device translation refused',hex(iova),n,permission))
 def weave_effect(self):
  # Device writes are modeled from the actual submitted address/pitch/format
  # words, never from a precomputed replacement payload or copied-output oracle.
  r=lambda off:self.regs.get(0x05400000+off,0)
  assert r(0x20)&15==1 and r(0x24)&0x91111111==0x80100010
  assert r(0x30)&0x07ff07ff==(479<<16)|639 and r(0x34)&0x10033333==0x00010010
  assert r(0x2000)&0x00110011==0 and r(0x2000)&0x01000000
  for plane,rows,addr_index,pitch_index,out_index in [(0,480,0,0,0),(1,240,4,4,4)]:
   top=r(0xb0+addr_index);bottom=r(0xc0+addr_index);pitch=r(0x40+pitch_index)>>16;dest=r(0x110+out_index);outpitch=r(0x70+pitch_index)&65535
   assert pitch==1280 and outpitch==640 and bottom==top+640
   for y in range(rows):
    source=(top if not y&1 else bottom)+(y//2)*pitch
    self.u.mem_write(self.mapped(dest+y*outpitch,640,2),bytes(self.u.mem_read(self.mapped(source,640,1),640)))
  self.events.append(('modeled-weave',640,480))
 def write_mmio(self,u,access,at,size,value,unused):
  assert size==4,('Unexpected MMIO width',hex(at),size)
  self.events.append(('write',at,value))
  if at==0x05400018:
   assert value==1
   if self.variant!='stale':self.status&=~1
  elif at==0x05400010:
   assert value==1 and not self.status&0x101 and not self.regs.get(0x05400014,0)&1
   self.started+=1;self.weave_effect();self.status=0 if self.variant=='nonfinish' else 0x101 if self.variant=='busy-finish' else 1
  else:self.regs[at]=value
 def tick(self):self.call('deinterlace_task',0)
 def result(self):return self.get(self.base+self.fields['result'],4)
 def checkpoint(self,path):
  rows=[]
  for i,(at,n) in enumerate(self.ram):
   p=path.parent/(path.stem+'-'+str(i)+'.bin.gz');raw=bytes(self.u.mem_read(at,n));p.write_bytes(gzip.compress(raw,compresslevel=1,mtime=0));rows.append(dict(address=at,file=p.name,sha256=digest(p),encoding='gzip'))
  value=dict(image_sha256=self.image_hash,session_sha256=hashlib.sha256(self.session).hexdigest(),memory=rows,now=self.now,regs=self.regs,status=self.status,started=self.started,events=self.events,held=self.held,callbacks=self.callbacks)
  path.write_text(json.dumps(value,indent=2)+'\n');return value
 def restore(self,path):
  v=json.loads(path.read_text());assert v['image_sha256']==self.image_hash and v['session_sha256']==hashlib.sha256(self.session).hexdigest()
  for row in v['memory']:
   p=path.parent/row['file'];assert digest(p)==row['sha256'];self.u.mem_write(row['address'],gzip.decompress(p.read_bytes()))
  for key in ['now','status','started','events','held']:setattr(self,key,v[key])
  self.regs={int(k):v for k,v in v['regs'].items()};self.callbacks={int(k):v for k,v in v['callbacks'].items()}
  for at in self.callbacks:self.install(at)
 def run(self):
  for _ in range(2000):
   self.tick()
   if self.variant=='nonfinish' and self.started:self.now+=10000
   if self.result():return
  raise AssertionError('Native bounded task made no terminal progress')
def allocation_report(m,session,image):
 f=m.fields;b=m.base;table=m.get(b+f['table']);t=b+f['tables'];tf=m.im.layouts['a733_iommu_tables'];sf=m.im.layouts['a733_iommu_span'];size=m.im.sizes['a733_iommu_span'];spans=[]
 assert m.get(t+tf['count'],4)==2 and m.call('a733_iommu_validate',t)==1
 for i in range(2):spans.append({key:m.get(t+tf['spans']+i*size+sf[key],4 if key=='access' else 8) for key in ['iova','physical','bytes','access']})
 return dict(physical_authority=False,modeled=True,image=digest(image),session=hashlib.sha256(session).hexdigest(),rounded=m.get(b+f['rounded']),table_bytes=32768,allocations=m.rows(),spans=spans,table_physical=table,source_physical=m.get(b+f['input']),destination_physical=m.get(b+f['output']),table_sha256=hashlib.sha256(bytes(m.u.mem_read(table,32768))).hexdigest(),scope='Actual native input authentication, guarded allocation, pinned DI301 register generation and IOMMU table construction. Prepared memory/board/time authority modeled, no device access.')
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for key in ['image','session','input','output']:p.add_argument('--'+key,type=Path,required=True)
 p.add_argument('--allocation-only',action='store_true');p.add_argument('--resume',action='store_true');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=a.resume);im=helpers.Image(a.image);session=a.session.read_bytes();raw=a.input.read_bytes();assert len(session)==888 and len(raw)==460800 and hashlib.sha256(raw).digest()==session[768:800]
 image_hash=digest(a.image);session_hash=hashlib.sha256(session).hexdigest();checkpoint=a.output/'prepared.json';result_path=a.output/'result.json'
 report=json.loads(result_path.read_text()) if result_path.exists() else dict(image_sha256=image_hash,session_sha256=session_hash,cases={},physical_access=False,models=['Prepared-pool/board liveness and monotonic counter','Translation metadata, scheduler/register mutex','External PMIC read-service response','GPU-resource MMIO callbacks','DI/IOMMU status effects; actual submitted weave words interpreted, never precomputed output replacement'],scope=__doc__)
 assert report['image_sha256']==image_hash and report['session_sha256']==session_hash,'Refuse stale image/session evidence'
 def passed(name,**value):
  report['cases'][name]=dict(passed=True,check_source_sha256=digest(Path(__file__)),**value);result_path.write_text(json.dumps(report,indent=2)+'\n');print('PASS',name,flush=True)
 m=Machine(im,session);m.image_hash=image_hash
 if a.allocation_only or 'nominal' not in report['cases']:
  m.u.mem_write(SCRATCH,session);assert m.call('a733_deinterlace_session_valid',SCRATCH)==1
  m.pools();assert m.upload(raw)==1
  if a.allocation_only:
   assert m.call('deinterlace_allocate')==1;allocation=allocation_report(m,session,a.image);table=m.get(m.base+m.fields['table']);(a.output/'page-table.bin').write_bytes(bytes(m.u.mem_read(table,32768)));(a.output/'allocation.json').write_text(json.dumps(allocation,indent=2)+'\n');print('PASS actual allocation and master9 table proof');return
  m.configure()
  for _ in range(100):
   m.tick()
   if m.get(m.base+m.fields['stage'],4)==1:break
   assert m.result()==0,('Native setup refused',m.events[-12:])
  else:raise AssertionError('Setup bound')
  m.checkpoint(checkpoint)
  m.call('a733_deinterlace_bitfields',SCRATCH);assert struct.unpack('<6I',m.u.mem_read(SCRATCH,24))==(0x101,0x80100010,1280<<16,0x00020020,0x178,0x24)
  m.run();assert m.result()==1 and m.started==1 and m.get(m.base+m.fields['copied'])==len(raw)
  payload=bytes(m.u.mem_read(m.get(m.base+m.fields['payload']),len(raw)));assert payload==raw and m.get(m.base+m.fields['guards_ok'],4)==1
  m.checkpoint(a.output/'copied.json');(a.output/'output.nv12').write_bytes(payload)
  passed('nominal',submissions=1,copied=len(raw),native_bitfields=True)
 else:m.restore(a.output/'copied.json')
 if 'terminal' not in report['cases']:
  before=len(m.events);m.tick();assert len(m.events)==before and m.result()==1;passed('terminal',no_more_device_writes=True,retained=True)
 m.u=None
 for variant in ['stale','nonfinish','busy-finish','global','micro','late-fault','unstable','guard']:
  if variant in report['cases']:continue
  q=Machine(im,session);q.image_hash=image_hash;q.restore(checkpoint);q.variant=variant
  if variant=='guard':q.put(q.get(q.base+q.fields['output'])-1,0,1)
  q.run();assert q.result()==2,(variant,q.result());assert all(row['state']==2 for row in q.rows()['uncached_pool'])
  assert q.started==(0 if variant in ['stale','guard'] else 1)
  passed(variant,submissions=q.started,copied=q.get(q.base+q.fields['copied']),retained=True);q.u=None
 for variant in ['generation','hash','truncated']:
  if variant in report['cases']:continue
  q=Machine(im,session);q.pools();r=q.upload(raw,variant);assert r==(-1 if variant!='truncated' else 0);assert not q.events and q.call('deinterlace_input_ready',q.base+q.fields['supplied'])==0
  passed(variant,no_device_effects=True);q.u=None
 report['passed']=True;result_path.write_text(json.dumps(report,indent=2)+'\n');print('PASS selected final-image DI301 native cases')
if __name__=='__main__':main()
