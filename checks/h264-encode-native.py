#!/usr/bin/env python3
"""Execute linked encoder allocation and retained native path with explicit MMIO effects."""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1];STOP=0x7afff000;STACK=0x7affeff0;SCRATCH=0x7ae00000

def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

class Machine:
 def __init__(self,image,session):
  self.image=image;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(0x40000000,0x4000000);self.u.mem_map(0x7a000000,0x1000000)
  for at,n in [(0x01c10000,4096),(0x02002000,8192),(0x07063000,4096),(0x07010000,4096),(0x07090000,4096),(0x03400000,8192),(0x03900000,0x20000)]:self.u.mem_map(at,n)
  self.u.mem_map(0x48000000,0x800000);self.u.mem_map(0x78000000,0x1000000)
  for at,data in image.parts:self.u.mem_write(at,data)
  self.base=image.address('h264_encode');self.a=image.layouts['h264_encode_application'];self.s=self.base+self.a['supplied'];self.il=image.layouts['h264_encode_input'];self.done=False;self.now=1000;self.calls=[];self.tx=bytearray();self.incoming=bytearray();self.triggers=0;self.codec_writes=[];self.io=[];self.quant=[];self.status=0;self.count=0;self.variant='nominal';self.finish_reads=0;self.native_poll_result=None
  self.u.mem_write(self.base,session);self.u.mem_write(image.address('board'),session[:592]);self.put(image.address('board')+160,1000000,8)
  self.hooks={image.address(n):n for n in ('a733_memory_reservations_init','a733_memory_uncached_init','board_live','a733_counter_clock_now','aarch64_device_translate','a733_clock_token_status','a733_gpu_resources_status','a733_platform_lock_enter','a733_platform_lock_leave','a733_native_usb_step','a733_native_usb_health','vTaskDelay','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush')}
  self.u.hook_add(UC_HOOK_CODE,self.hook);self.u.hook_add(UC_HOOK_MEM_READ,self.mmread);self.u.hook_add(UC_HOOK_MEM_WRITE,self.mmwrite)
 def put(self,at,v,n=4):self.u.mem_write(at,int(v).to_bytes(n,'little'))
 def get(self,at,n=8):return int.from_bytes(self.u.mem_read(at,n),'little')
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def hook(self,u,at,size,unused):
  if at==STOP:self.done=True;u.emu_stop();return
  name=self.hooks.get(at)
  if name is None:return
  x=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)];self.calls.append(name)
  if name=='vTaskDelay':self.done=True;u.emu_stop()
  elif name=='a733_memory_reservations_init':self.ret(self.image.address('boot_pool'))
  elif name=='a733_memory_uncached_init':self.ret(self.image.address('uncached_pool'))
  elif name=='a733_counter_clock_now':self.now+=1;self.put(x[1],self.now,8);self.ret(1)
  elif name=='aarch64_device_translate':u.mem_write(x[2],struct.pack('<QII',x[0],0,2));self.ret(1)
  elif name in ('a733_clock_token_status','a733_gpu_resources_status'):self.ret(0)
  elif name=='a733_platform_lock_leave':self.ret()
  elif name=='tud_vendor_n_available':self.ret(len(self.incoming))
  elif name=='tud_vendor_n_read':
   n=min(x[2],len(self.incoming));u.mem_write(x[1],bytes(self.incoming[:n]));del self.incoming[:n];self.ret(n)
  elif name=='tud_vendor_n_write':
   n=min(x[2],31);self.tx.extend(u.mem_read(x[1],n));self.ret(n)
  elif name=='tud_vendor_n_write_flush':self.ret(1)
  else:self.ret(1)
 def mmread(self,u,access,at,size,value,unused):
  if 0x03900000<=at<0x03920000:self.io.append(('r',at-0x03900000))
  if at==0x01c10b1c:
   value=self.status
   if self.triggers and self.status&15:
    value=0 if self.variant=='nonfinish' else 2 if self.variant=='stall' else 8 if self.variant=='error' else self.status
   if self.variant=='p-error' and self.triggers==2:value=8
   if self.variant!='header-timeout':value|=0x200
   self.put(at,value)
  if at==0x01c10b90:self.put(at,self.count)
  if at==0x03900088 and self.variant=='later-fault' and self.get(self.base+self.a['copied'])>=32768:self.put(at,1)
 def mmwrite(self,u,access,at,size,value,unused):
  if 0x03900000<=at<0x03920000:self.io.append(('w',at-0x03900000))
  if not 0x01c10000<=at<0x01c11000:return
  off=at-0x01c10000;self.codec_writes.append((off,value))
  if off==0xb1c:self.status&=~value
  if off==0xb18 and value==8:
   self.triggers+=1;cursor=self.get(self.base+self.a['run']+self.image.layouts['a733_h264_encode_run']['cursor']);first=self.get(0x01c10b88,4)//8
   n=65568 if self.variant=='later-fault' else 512 if self.triggers==1 else 384
   self.count=(first+n)*8
   if self.variant=='empty':self.count=cursor*8
   if self.variant=='overflow':self.count=(cursor+1048576)*8
   if self.variant=='unaligned':self.count+=1
   st=self.base+self.a['storage'];ring=self.get(st+self.image.layouts['h264_encode_storage']['buffers']+2*8)
   self.u.mem_write(ring+first,bytes([0x3c if self.triggers==2 else 0x55])*n);self.status=1
 def call(self,name,*values,limit=200000000):
  self.done=False;self.u.reg_write(UC_ARM64_REG_SP,STACK);self.u.reg_write(UC_ARM64_REG_LR,STOP)
  for i,v in enumerate(values):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  self.u.emu_start(self.image.address(name),0,count=limit);assert self.done,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  v=self.u.reg_read(UC_ARM64_REG_X0)&0xffffffff;return v if v<1<<31 else v-(1<<32)
 def pools(self):
  for pool,lo,hi in [('boot_pool','__device_pool_start','__device_pool_end'),('uncached_pool','__uncached_pool_start','__uncached_pool_end')]:
   start=self.image.address(lo);end=self.image.address(hi);assert end<=0x7a000000 or start>=0x7b000000,(pool,hex(start),hex(end))
   assert self.call('device_memory_pool_init',self.image.address(pool),start,start,end-start)==1
  assert self.call('h264_encode_storage_allocate')==1
 def upload(self,raw,variant='nominal'):
  session=bytes(self.u.mem_read(self.base,856));packet=struct.pack('<4I2Q',0x55454853,1,len(raw),2,*struct.unpack_from('<2Q',session,24))+raw
  if variant=='generation':packet=bytearray(packet);packet[24]^=1
  if variant=='hash':packet=bytearray(packet);packet[-1]^=1
  if variant=='truncated':packet=packet[:-1]
  at=0;result=0
  while at<len(packet):
   left=self.call('h264_encode_input_remaining',self.s);n=min(left,197,len(packet)-at);assert n
   self.u.mem_write(SCRATCH,bytes(packet[at:at+n]));result=self.call('h264_encode_input_feed',self.s,self.base,self.image.local('h264_encode_now','apps/h264-encode/native.c'),self.base,990000,SCRATCH,n);at+=n
   if result<0:break
  return result
 def configuration(self):
  # Reviewed inherited 600MHz VE0 and 24MHz fabrics; shared lease observations modeled.
  t=self.image.layouts['a733_encode_platform_config'];c=self.base+self.a['config'];token=self.image.layouts['a733_clock_token'];lease=self.image.layouts['a733_gpu_resource_lease']
  def field(n,v,sz=8):self.put(c+t[n],v,sz)
  field('clocks',SCRATCH+0x10000);field('power',SCRATCH+0x20000);field('power_client',3,4);field('owner',self.get(self.base+616));field('handoff',self.get(self.base+32));field('module',0x80000001,4);field('module_hz',600000000,4)
  for i in range(3):self.put(c+t['fabric']+4*i,0 if i==0 else 0x80000000);self.put(c+t['fabric_hz']+4*i,24000000)
  scopes=SCRATCH+0x500;assert self.call('a733_encode_platform_scopes',c,scopes)==1
  cl=c+t['clock'];self.put(cl+token['id'],5,8);self.put(cl+token['owner'],self.get(self.base+616),8);self.u.mem_write(cl+token['shared'],bytes(self.u.mem_read(scopes,16)));self.put(cl+token['exclusive'],1<<71,16)
  pc=c+t['pck'];self.put(pc+lease['token'],7,8);self.put(pc+lease['mask'],9);self.put(pc+lease['mode'],1)
  for at,v in {0x07090160:0x10000002,0x02002000:0xf8636300,0x02002220:0xf8006302,0x02003918:0x10001,0x02003998:0x10000,0x070101ac:1,0x07063000:8,0x07063008:8,0x02002a80:0x80000001,0x02002a8c:0x10001,0x020025c0:2,0x020025e0:1<<12,0x020025e4:2,0x02002580:0x80000000,0x02002588:0x80000000,0x0200258c:0x10007,0x020025b4:0x10007,0x01c108e0:0x33310,0x01c108e4:0x21320}.items():self.put(at,v)
  # Resolve static callbacks by defining compilation unit; never last-symbol-wins.
  names=['h264_encode_now','authority','memory','prime','copied','h264_encode_enter','h264_encode_leave','a733_encode_native_read','a733_encode_h264_native_write']
  pointers=[self.image.address(n) if n not in ('authority','memory','prime','copied') else self.image.local(n,'apps/h264-encode/native.c') for n in names]
  self.u.mem_write(SCRATCH,struct.pack('<9Q',*pointers));assert self.call('a733_h264_encode_platform_init',self.base+self.a['platform'],c,SCRATCH,self.base)==1
  ops=self.call('a733_encode_platform_h264_ops');job=self.base+self.a['storage']+self.image.layouts['h264_encode_storage']['job']
  assert self.call('a733_h264_encode_begin',self.base+self.a['run'],ops,self.base+self.a['platform'],job,990000)==1
class Image:
 def __init__(self,path):
  old=module('jpeg_native_image',ROOT/'checks/jpeg-runtime-native.py').Image(path);self.parts=old.parts;self.symbols=old.symbols;self.layouts=old.layouts;self.address=old.address;self.locals={};self.sizes={}
  with path.open('rb') as f:
   e=ELFFile(f);d=e.get_dwarf_info()
   for cu in d.iter_CUs():
    top=cu.get_top_DIE();unit=top.attributes['DW_AT_name'].value.decode()
    for die in cu.iter_DIEs():
     attrs=die.attributes;n=attrs.get('DW_AT_name')
     if n and die.tag=='DW_TAG_structure_type' and 'DW_AT_byte_size' in attrs:self.sizes[n.value.decode()]=attrs['DW_AT_byte_size'].value
     if n and die.tag=='DW_TAG_subprogram' and 'DW_AT_low_pc' in attrs:self.locals.setdefault((n.value.decode(),unit),set()).add(attrs['DW_AT_low_pc'].value)
 def local(self,name,unit):
  values={v for (n,p),vs in self.locals.items() if n==name and p.endswith(unit) for v in vs};assert len(values)==1,(name,unit,values);return next(iter(values))

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','input','session','output'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--resume',type=Path);p.add_argument('--case',required=True,choices=['allocation','nominal','generation','hash','truncated','nonfinish','stall','error','empty','overflow','unaligned','later-fault','header-timeout','p-failure','duplicate-upload'])
 a=p.parse_args();a.output.mkdir(exist_ok=False);im=Image(a.image);m=Machine(im,a.session.read_bytes());raw=a.input.read_bytes();assert len(raw)==921600
 # Freeze this fixture before any case, including failures.
 (a.output/'fixture.py').write_bytes(Path(__file__).read_bytes())
 def checkpoint(name):
  state=dict(image=hashlib.sha256(a.image.read_bytes()).hexdigest(),session=hashlib.sha256(a.session.read_bytes()).hexdigest(),now=m.now,memory=[(at,bytes(m.u.mem_read(at,n))) for at,n in [(0x41000000,0x1000000),(0x48000000,0x800000),(0x78000000,0x1000000)]])
  (a.output/name).write_bytes(gzip.compress(pickle.dumps(state)))
 if a.resume:
  state=pickle.loads(gzip.decompress(a.resume.read_bytes()));assert state['image']==hashlib.sha256(a.image.read_bytes()).hexdigest() and state['session']==hashlib.sha256(a.session.read_bytes()).hexdigest()
  for at,data in state['memory']:m.u.mem_write(at,data)
  m.now=state['now'];r=1
 else:m.pools();r=m.upload(raw,a.case)
 if a.case=='duplicate-upload':
  assert a.resume and m.call('h264_encode_input_ready',m.s)==1
  before=bytes(m.u.mem_read(m.get(m.s+im.layouts['h264_encode_input']['bytes']),921600));m.u.mem_write(SCRATCH,b'X')
  assert m.call('h264_encode_input_feed',m.s,m.base,im.address('h264_encode_now'),m.base,990000,SCRATCH,1)==-1
  assert bytes(m.u.mem_read(m.get(m.s+im.layouts['h264_encode_input']['bytes']),921600))==before and not m.codec_writes
  report=dict(duplicate_refused=True,immutable_input_preserved=True,device_writes=0)
 elif a.case in ('generation','hash','truncated'):
  assert r==(-1 if a.case!='truncated' else 0) and m.call('h264_encode_input_ready',m.s)==0 and not m.codec_writes
  if a.case=='truncated':
   m.now=990000;m.u.mem_write(SCRATCH,b'X');assert m.call('h264_encode_input_feed',m.s,m.base,im.address('h264_encode_now'),m.base,990000,SCRATCH,1)==-1
  report=dict(upload_result=r,device_writes=0)
 else:
  assert r==1;storage=m.base+m.a['storage']
  if not a.resume:assert m.call('h264_encode_storage_publish',storage,m.base+768)==1
  assert m.call('a733_iommu_validate',storage+im.layouts['h264_encode_storage']['tables'])==1
  pt=im.layouts['device_memory_pool'];alloc=im.layouts['device_memory_allocation'];rows={}
  for name,count in [('boot_pool',3),('uncached_pool',9)]:
   base=im.address(name);items=[]
   for i in range(count):
    at=base+pt['allocations']+i*im.sizes['device_memory_allocation'];items.append(dict(first=m.get(at+alloc['first'],4),pages=m.get(at+alloc['pages'],4),state=m.get(at+alloc['state'],4)))
   rows[name]=items
  wb=max(x['first']+x['pages'] for x in rows['boot_pool'])*4096;nc=max(x['first']+x['pages'] for x in rows['uncached_pool'])*4096
  assert wb==3022848 and nc==6254592 and im.sizes['h264_encode_application']<=65536
  report=dict(nc_bytes=nc,wb_bytes=wb,cpu_metadata_bytes=im.sizes['h264_encode_application'],allocations=rows,device_writes=0)
  if not a.resume:checkpoint('allocated.pkl.gz')
  if a.case!='allocation':
   m.configuration();m.variant=a.case if a.case not in ('p-failure','header-timeout') else 'nominal'
   result=0;outputs=[]
   for ordinal in range(2):
    if ordinal:
     # Caller ACK ownership is modeled here; actual USB consumer has separate checks.
     m.put(m.base+m.a['acked'],1);m.put(m.base+m.a['result'],1)
     assert m.call('a733_h264_encode_next',m.base+m.a['run'])==1
     if a.case=='p-failure':m.variant='p-error'
     if a.case=='header-timeout':m.variant='header-timeout'
    for i in range(5000):
     result=m.call('a733_h264_encode_poll',m.base+m.a['run'])
     if result:break
    if a.case=='nonfinish' or (ordinal and a.case=='header-timeout'):
     assert result==0;m.now=990000;result=m.call('a733_h264_encode_poll',m.base+m.a['run'])
    expected=1 if a.case=='nominal' or (ordinal==0 and a.case in ('p-failure','header-timeout')) else -1
    assert result==expected,(a.case,ordinal,result,i)
    if result<0:break
    n=m.get(m.base+m.a['output_bytes']+ordinal*8);out=m.get(storage+im.layouts['h264_encode_storage']['copies']+ordinal*8);bits=bytes(m.u.mem_read(out,n));outputs.append(bits)
    assert n==(544 if ordinal==0 else 393),(ordinal,n)
    (a.output/('picture%d.h264'%ordinal)).write_bytes(bits)
    checkpoint('completed%d.pkl.gz'%ordinal)
    if ordinal==0:first=bits
   if a.case in ('p-failure','header-timeout'):
    out=m.get(storage+im.layouts['h264_encode_storage']['copies']);assert bytes(m.u.mem_read(out,len(first)))==first
   for _,off in m.io:
    if off%0x10000>=0x400:assert 0xa00<=off<0xc00,hex(off)
   report.update(result=result,triggers=m.triggers,programmed=m.codec_writes,io_accesses=m.io,device_writes=len(m.codec_writes),copied_sizes=[len(x) for x in outputs])
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,case=a.case,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),input_sha256=hashlib.sha256(raw).hexdigest(),session_sha256=hashlib.sha256(a.session.read_bytes()).hexdigest(),physical_access=False,scope='Actual linked allocation/upload/controller/typed encoder provider/lowest native MMIO policy/IOMMU/copy. Memory readiness, translation attributes, lease status, locks, clocks, MMIO completion and placeholder entropy modeled. Caller ACK modeled here. No CPU startup, numerical encoding, physical visibility or drain qualification.',**report),indent=2)+'\n');print('PASS',a.case)
if __name__=='__main__':main()
