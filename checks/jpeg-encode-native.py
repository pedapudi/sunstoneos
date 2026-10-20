#!/usr/bin/env python3
"""Execute linked encoder allocation and retained native path with explicit MMIO effects."""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1];STOP=0x50fff000;STACK=0x50ffeff0;SCRATCH=0x50e00000

def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

class Machine:
 def __init__(self,image,session):
  self.image=image;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(0x40000000,0x4000000);self.u.mem_map(0x50000000,0x1000000)
  for at,n in [(0x01c10000,4096),(0x02002000,8192),(0x07063000,4096),(0x07010000,4096),(0x07090000,4096),(0x03400000,8192),(0x03900000,0x20000)]:self.u.mem_map(at,n)
  for at,data in image.parts:self.u.mem_write(at,data)
  self.base=image.address('jpeg_encode');self.a=image.layouts['jpeg_encode_application'];self.s=self.base+self.a['supplied'];self.il=image.layouts['jpeg_encode_input'];self.done=False;self.now=1000;self.calls=[];self.tx=bytearray();self.incoming=bytearray();self.triggers=0;self.codec_writes=[];self.io=[];self.quant=[];self.status=0;self.count=0;self.variant='nominal';self.finish_reads=0;self.native_poll_result=None
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
   if self.triggers:
    self.finish_reads+=1
    self.status=0 if self.variant=='nonfinish' else 2 if self.variant=='stall' else 8 if self.variant=='error' else 1
   self.put(at,self.status)
  if at==0x01c10b90:self.put(at,self.count)
  if at==0x03900088 and self.variant=='later-fault' and self.get(self.base+self.a['copied'])>=32768:self.put(at,1)
 def mmwrite(self,u,access,at,size,value,unused):
  if 0x03900000<=at<0x03920000:self.io.append(('w',at-0x03900000))
  if not 0x01c10000<=at<0x01c11000:return
  off=at-0x01c10000;self.codec_writes.append((off,value))
  if off==0xbe4:self.quant.append(value)
  if off==0xb1c:self.status&=~value
  if off==0xb18 and value==0x10008:
   self.triggers+=1;self.count=4984+(65568*8 if self.variant=='later-fault' else 256)
   if self.variant=='empty':self.count=4984
   if self.variant=='overflow':self.count=1048576*8
   if self.variant=='unaligned':self.count+=1
   output=self.get(self.base+self.a['output']);self.u.mem_write(output+623,b'\x55'*(65568 if self.variant=='later-fault' else 32))
 def call(self,name,*values,limit=200000000):
  self.done=False;self.u.reg_write(UC_ARM64_REG_SP,STACK);self.u.reg_write(UC_ARM64_REG_LR,STOP)
  for i,v in enumerate(values):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  self.u.emu_start(self.image.address(name),0,count=limit);assert self.done,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  v=self.u.reg_read(UC_ARM64_REG_X0)&0xffffffff;return v if v<1<<31 else v-(1<<32)
 def pools(self):
  for pool,lo,hi in [('boot_pool','__device_pool_start','__device_pool_end'),('uncached_pool','__uncached_pool_start','__uncached_pool_end')]:
   start=self.image.address(lo);end=self.image.address(hi);assert end<=0x50000000 or start>=0x51000000
   assert self.call('device_memory_pool_init',self.image.address(pool),start,start,end-start)==1
  assert self.call('jpeg_encode_upload_allocate')==1
 def upload(self,raw,variant='nominal'):
  session=bytes(self.u.mem_read(self.base,856));packet=struct.pack('<4I2Q',0x55454a53,1,len(raw),0,*struct.unpack_from('<2Q',session,24))+raw
  if variant=='generation':packet=bytearray(packet);packet[24]^=1
  if variant=='hash':packet=bytearray(packet);packet[-1]^=1
  if variant=='truncated':packet=packet[:-1]
  # The real USB consumer bounds fragments at header and body boundaries.
  at=0;result=0
  while at<len(packet):
   left=self.call('jpeg_encode_input_remaining',self.s);n=min(left,197,len(packet)-at);assert n
   self.u.mem_write(SCRATCH,bytes(packet[at:at+n]));result=self.call('jpeg_encode_input_feed',self.s,self.base,self.image.address('jpeg_encode_now'),self.base,990000,SCRATCH,n);at+=n
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
  names=['jpeg_encode_now','authority','memory','copied','jpeg_encode_enter','jpeg_encode_leave','a733_encode_native_read','a733_encode_native_write']
  pointers=[self.image.address(n) if n not in ('authority','memory','copied') else self.image.local(n,'apps/jpeg-encode/native.c') for n in names]
  self.u.mem_write(SCRATCH,struct.pack('<8Q',*pointers));assert self.call('a733_encode_platform_init',self.base+self.a['platform'],c,SCRATCH,self.base)==1
  ops=self.call('a733_encode_platform_jpeg_ops');self.u.mem_write(SCRATCH,struct.pack('<QQQQII',self.get(self.base+self.a['input']),460800,0x10000000,0x10100000,462848,1048576))
  assert self.call('a733_jpeg_encode_begin',self.base+self.a['run'],ops,self.base+self.a['platform'],SCRATCH,990000)==1

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
 p.add_argument('--resume',type=Path)
 p.add_argument('--case',required=True,choices=['allocation','nominal','generation','hash','truncated','stale','nonfinish','stall','error','empty','overflow','unaligned','later-fault'])
 a=p.parse_args();a.output.mkdir(exist_ok=False);im=Image(a.image);m=Machine(im,a.session.read_bytes());raw=a.input.read_bytes();assert len(raw)==460800
 if a.resume:
  state=pickle.loads(gzip.decompress(a.resume.read_bytes()));assert state['image']==hashlib.sha256(a.image.read_bytes()).hexdigest() and state['session']==hashlib.sha256(a.session.read_bytes()).hexdigest()
  for at,data in state['memory']:m.u.mem_write(at,data)
  m.now=state['now'];r=1
 else:m.pools();r=m.upload(raw,a.case)
 if a.case in ('generation','hash','truncated'):
  assert r==(-1 if a.case!='truncated' else 0) and m.call('jpeg_encode_input_ready',m.s)==0 and not m.codec_writes
  report=dict(upload_result=r,device_writes=0)
 else:
  assert r==1
  if not a.resume:assert m.call('jpeg_encode_allocate')==1
  assert m.call('a733_iommu_validate',m.base+m.a['tables'])==1
  if not a.resume:
   state=dict(image=hashlib.sha256(a.image.read_bytes()).hexdigest(),session=hashlib.sha256(a.session.read_bytes()).hexdigest(),now=m.now,memory=[(at,bytes(m.u.mem_read(at,n))) for at,n in [(0x41000000,0x1000000),(0x50e00000,0x200000)]])
   (a.output/'allocated.pkl.gz').write_bytes(gzip.compress(pickle.dumps(state)))
  pt=im.layouts['device_memory_pool'];alloc=im.layouts['device_memory_allocation'];rows={}
  for name,count in [('boot_pool',2),('uncached_pool',3)]:
   base=im.address(name);items=[]
   for i in range(count):
    at=base+pt['allocations']+i*im.sizes['device_memory_allocation'];items.append(dict(first=m.get(at+alloc['first'],4),pages=m.get(at+alloc['pages'],4),state=m.get(at+alloc['state'],4)))
   rows[name]=items
  assert max(x['first']+x['pages'] for x in rows['boot_pool'])*4096==1511424
  assert max(x['first']+x['pages'] for x in rows['uncached_pool'])*4096==1560576
  assert im.sizes['jpeg_encode_application']<=65536
  report=dict(nc_bytes=1560576,wb_bytes=1511424,cpu_metadata_bytes=im.sizes['jpeg_encode_application'],allocations=rows,device_writes=0)
  if a.case!='allocation':
   m.configuration();m.variant=a.case
   if a.case=='stale':m.status=1
   result=0
   for i in range(2000):
    result=m.call('a733_jpeg_encode_poll',m.base+m.a['run'])
    if result:break
   if a.case=='nonfinish':
    assert result==0;m.now=990000;result=m.call('a733_jpeg_encode_poll',m.base+m.a['run']);assert result==-1
   else:assert result==(1 if a.case in ('nominal','stale') else -1),(a.case,result,i)
   assert m.triggers==1
   expected=struct.unpack('<128I',(Path.home()/'.cache/sunstoneos/evidence/jpeg-encode-trace-20261020/nominal-facts/quant-sram.bin').read_bytes());assert m.quant==list(expected)
   if result==1:
    n=m.get(m.base+m.a['jpeg_bytes']);out=m.get(m.base+m.a['payload']);jpeg=bytes(m.u.mem_read(out,n));assert jpeg[-2:]==b'\xff\xd9' and jpeg[623:-2]==b'\x55'*32
    (a.output/'copied.jpg').write_bytes(jpeg)
    state=dict(image=hashlib.sha256(a.image.read_bytes()).hexdigest(),session=hashlib.sha256(a.session.read_bytes()).hexdigest(),now=m.now,memory=[(at,bytes(m.u.mem_read(at,n))) for at,n in [(0x41000000,0x1000000),(0x50e00000,0x200000)]])
    (a.output/'completed.pkl.gz').write_bytes(gzip.compress(pickle.dumps(state)))
    assert m.call('a733_jpeg_encode_poll',m.base+m.a['run'])==1
   else:assert not any(m.u.mem_read(m.base+m.a['digest'],32))
   # Actual source skip behavior: only master3 microset, both global banks.
   master_micro=[0x400,0x600,0x800,0xa00,0x10400,0x10600,0x10800,0x10a00,0x10c00,0x10e00]
   for _,off in m.io:
    if off%0x10000>=0x400:assert 0xa00<=off<0xc00,hex(off)
   report.update(result=result,polls=i+1,triggers=m.triggers,programmed= m.codec_writes,io_accesses=m.io,device_writes=len(m.codec_writes))
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,case=a.case,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),input_sha256=hashlib.sha256(raw).hexdigest(),session_sha256=hashlib.sha256(a.session.read_bytes()).hexdigest(),physical_access=False,scope='Actual linked allocation/upload/controller/encoder provider/lowest native MMIO policy/IOMMU/copy. Memory readiness, translation attributes, lease status, locks, clocks, MMIO completion and32 entropy bytes modeled. No CPU startup, hardware arithmetic, physical burst/visibility/drain qualification.',**report),indent=2)+'\n');print('PASS',a.case)
if __name__=='__main__':main()
