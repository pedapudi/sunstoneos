#!/usr/bin/env python3
"""Execute initial DI301 MOTION ownership/control; synthetic device writes are not pixel arithmetic."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('motion_base',ROOT/'checks/deinterlace-native.py');base=importlib.util.module_from_spec(s);s.loader.exec_module(base)
SCRATCH=base.SCRATCH
class Machine(base.Machine):
 def rows(self):
  result={};pf=self.im.layouts['device_memory_pool'];af=self.im.layouts['device_memory_allocation'];size=self.im.sizes['device_memory_allocation']
  for name,count in [('boot_pool',2),('uncached_pool',6)]:
   at=self.im.address(name);back=at+pf['backing'];rows=[]
   for i in range(count):
    p=at+pf['allocations']+i*size;first=self.get(p+af['first'],4);pages=self.get(p+af['pages'],4)
    if pages:rows.append(dict(slot=i,cpu=self.get(back)+first*4096,physical=self.get(back+8)+first*4096,bytes=pages*4096,state=self.get(p+af['state'],4)))
   result[name]=rows
  return result
 def weave_effect(self):
  # Opaque hardware-effect substitute, explicitly NOT a DI numerical model.
  r=self.regs;at=0x05400000
  assert r[at+0x20]==3 and r[at+0x24]==0x90101011 and r[at+0x80]==160
  assert (r[at+0x2000]&0x01110011)==0x01010011 and r[at+0x38]==0
  for off,iova in [(0xb0,0x10000000),(0xd0,0x10800000)]:
   assert r[at+off]==iova and r[at+off+4]==iova+307200 and r[at+off+16]==iova+640
   self.mapped(iova,460800,1)
   try:self.mapped(iova,1,2)
   except AssertionError:pass
   else:raise AssertionError('Input writable')
  assert r[at+0x120]==0x10c00000 and r[at+0x124]==0x10c20000
  initial=self.mapped(0x10c00000,76800,1);flags=self.mapped(0x10c20000,76800,2)
  assert bytes(self.u.mem_read(initial,76800))==bytes(76800) and bytes(self.u.mem_read(flags,76800))==bytes(76800)
  assert not any(at+off in r for off in (0x1004,0x1008,0x100c))
  self.u.mem_write(flags,bytes([0x33])*76800)
  dest=self.mapped(r[at+0x110],460800,2)
  self.u.mem_write(dest,bytes((i*13+7)&255 for i in range(460800)))
  self.events.append(('synthetic-device-writes-not-DI-arithmetic',460800,76800))
  if self.variant=='current-mutated':self.put(self.mapped(0x10800000,1,1),1,1)
 def read_mmio(self,u,access,at,size,value,unused):
  if self.variant=='late-flag-guard' and self.get(self.base+self.fields['copied']):
   self.put(self.get(self.base+self.fields['flag_output'])+76800,0,1)
  return super().read_mmio(u,access,at,size,value,unused)
def allocation(m,session,image):
 f=m.fields;b=m.base;table=m.get(b+f['table']);t=b+f['tables'];tf=m.im.layouts['a733_iommu_tables'];sf=m.im.layouts['a733_iommu_span'];size=m.im.sizes['a733_iommu_span']
 assert m.get(t+tf['count'],4)==5 and m.call('a733_iommu_validate',t)==1
 spans=[{key:m.get(t+tf['spans']+i*size+sf[key],4 if key=='access' else 8) for key in ['iova','physical','bytes','access']} for i in range(5)]
 return dict(physical_authority=False,modeled=True,image=base.digest(image),session=hashlib.sha256(session).hexdigest(),rounded=462848,input_bytes=921600,input_rounded=921600,md_bytes=76800,md_rounded=77824,table_bytes=32768,allocations=m.rows(),spans=spans,table_physical=table,source_physical=m.get(b+f['input']),destination_physical=m.get(b+f['output']),current_physical=m.get(b+f['current']),flag_input_physical=m.get(b+f['flag_input']),flag_output_physical=m.get(b+f['flag_output']),table_sha256=hashlib.sha256(bytes(m.u.mem_read(table,32768))).hexdigest(),scope='Actual input/allocator/DI301 generation/IOMMU construction; external prepared authority modeled; no device execution.')
def replies(m,out):
 # Execute the actual mode-dependent native USB serializer/ACK logic. TinyUSB
 # FIFO operations and completion callbacks are modeled, not its unchanged DCD.
 functions={}
 for name in ('tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','tud_speed_get','tud_task_ext','tusb_rhport_init','a733_native_usb_step','a733_native_usb_health'):
  if name in m.im.symbols:functions[m.im.address(name)]=name
 incoming=bytearray();wire=bytearray();pending=[]
 def hook(u,pc,size,unused):
  a=m.args();name=functions[pc]
  if name in ('tud_speed_get','tusb_rhport_init','a733_native_usb_step','a733_native_usb_health'):m.ret(1)
  elif name=='tud_vendor_n_available':m.ret(len(incoming))
  elif name=='tud_vendor_n_read':
   n=min(a[2],len(incoming));u.mem_write(a[1],bytes(incoming[:n]));del incoming[:n];m.ret(n)
  elif name=='tud_vendor_n_write':
   n=a[2];wire.extend(u.mem_read(a[1],n));pending.append(n);m.ret(n)
  else:m.ret(0)
 for pc in functions:m.u.hook_add(base.UC_HOOK_CODE,hook,begin=pc,end=pc)
 def exchange(data):
  incoming.extend(data);start=len(wire)
  # A query first builds its snapshot; the next owner iteration queues it.
  m.call('board_usb0_task',0);m.call('board_usb0_task',0)
  while pending:
   n=pending.pop(0);m.call('tud_vendor_tx_cb',0,n)
   m.call('board_usb0_task',0)
  # The exact460992 response is64-aligned and needs terminal ZLP.
  if len(wire)>start and (len(wire)-start)%64==0:m.call('tud_vendor_tx_cb',0,0)
  return bytes(wire[start:])
 response=exchange(b'\0');assert len(response)==460992
 (out/'response.bin').write_bytes(response)
 receipt=exchange(b'\1'+m.session[32:40]+response[96:128]);assert not receipt
 receipt=exchange(b'\0');assert len(receipt)==192 and struct.unpack_from('<I',receipt,8)[0]==3
 (out/'acknowledged.bin').write_bytes(receipt)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('image','session','input','output'):p.add_argument('--'+key,type=Path,required=True)
 p.add_argument('--allocation-only',action='store_true');p.add_argument('--resume',action='store_true');p.add_argument('--contract',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=a.resume)
 im=base.helpers.Image(a.image);session=a.session.read_bytes();raw=a.input.read_bytes();assert len(raw)==921600 and struct.unpack_from('<I',session,876)[0]==2 and hashlib.sha256(raw).digest()==session[768:800]
 ih=base.digest(a.image);sh=hashlib.sha256(session).hexdigest();result=a.output/'result.json'
 report=json.loads(result.read_text()) if result.exists() else dict(image_sha256=ih,session_sha256=sh,cases={},physical_access=False,scope=__doc__,device_model='Synthetic bounded device writes exercise ownership/control and copying only; no numerical or physical DI claim.')
 assert (report['image_sha256'],report['session_sha256'])==(ih,sh),'Refuse stale evidence identity'
 def passed(name,**kwargs):
  report['cases'][name]=dict(passed=True,check_source_sha256=base.digest(Path(__file__)),**kwargs);result.write_text(json.dumps(report,indent=2)+'\n');print('PASS',name,flush=True)
 def fresh():
  m=Machine(im,session);m.image_hash=ih;m.u.mem_write(SCRATCH,session);assert m.call('a733_deinterlace_session_valid',SCRATCH)==1;m.pools();assert m.upload(raw)==1;return m
 if a.allocation_only:
  m=fresh();assert m.call('deinterlace_allocate')==1;(a.output/'allocation.json').write_text(json.dumps(allocation(m,session,a.image),indent=2)+'\n');(a.output/'page-table.bin').write_bytes(bytes(m.u.mem_read(m.get(m.base+m.fields['table']),32768)));print('PASS actual motion allocation and guarded master9 tables');return
 assert a.contract and a.contract.stat().st_size==288,'Require root actual launcher published runtime contract'
 contract=a.contract.read_bytes();assert contract[224:256]==hashlib.sha256(session).digest()
 checkpoint=a.output/'prepared.json'
 if 'nominal' not in report['cases']:
  m=fresh();m.checkpoint(a.output/'uploaded.json');m.configure()
  for _ in range(100):
   m.tick()
   if m.get(m.base+m.fields['stage'],4)==1:break
   assert not m.result(),m.events[-10:]
  else:raise AssertionError('Setup bound')
  m.checkpoint(checkpoint);m.run();assert m.result()==1 and m.started==1 and m.get(m.base+m.fields['copied'])==460800
  output=bytes(m.u.mem_read(m.get(m.base+m.fields['payload']),460800));assert output==bytes((i*13+7)&255 for i in range(460800))
  (a.output/'output.nv12').write_bytes(output);m.checkpoint(a.output/'copied.json');passed('nominal',submissions=1,copied=460800,pixel_arithmetic_checked=False)
 for variant in ('reset-deasserted','bus-running','module-running'):
  if variant in report['cases']:continue
  m=Machine(im,session);m.image_hash=ih;m.restore(a.output/'uploaded.json');m.configure();m.regs[0x02002a24]=0x10000 if variant=='reset-deasserted' else 1 if variant=='bus-running' else 0;m.regs[0x02002a20]=0x80000000 if variant=='module-running' else 0;m.run();assert m.result()==2 and not m.started and not m.rows()['uncached_pool'];assert not any(e[0]=='write' and e[1]==0x02002a24 for e in m.events);passed(variant,no_reset_release=True,no_submission=True)
 for variant in ('late-flag-guard','current-mutated'):
  if variant in report['cases']:continue
  m=Machine(im,session);m.image_hash=ih;m.restore(checkpoint);m.variant=variant;m.run();assert m.result()==2 and m.started==1 and all(row['state']==2 for row in m.rows()['uncached_pool']);passed(variant,retained=True,healthy_not_published=True)
 if 'second-frame-auth' not in report['cases']:
  m=Machine(im,session);m.pools();assert m.upload(raw,'hash')==-1 and not m.events and not m.rows()['uncached_pool'];passed('second-frame-auth',no_provider_or_device=True)
 if 'native-reply-ack' not in report['cases']:
  m=Machine(im,session);m.image_hash=ih;m.restore(a.output/'copied.json');replies(m,a.output);passed('native-reply-ack',native_serializer=True,vendor_fifo_and_transfer_completions_modeled=True)
 report['passed']=True;result.write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
