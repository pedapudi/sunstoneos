#!/usr/bin/env python3
"""Execute compiled DI301 TNR ownership and copying with synthetic device writes.

The model supplies hardware completion and bytes, not temporal filtering or
physical DMA. Prepared authority, PMIC services and scheduler are modeled.
"""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('tnr_motion_helpers',ROOT/'checks/deinterlace-motion-native.py');motion=importlib.util.module_from_spec(s);s.loader.exec_module(motion)
base=motion.base
class Machine(motion.Machine):
 def checkpoint(self,path):
  value=super().checkpoint(path)
  # Preserve the interrupted native task, including all CPU registers and its
  # RAM stack, plus the explicit modeled providers and MMIO backing bytes.
  extra=dict(cpu=self.u.context_save(),mmio=[(at,bytes(self.u.mem_read(at,n))) for at,n in self.mmio],variant=self.variant)
  cpu=path.with_suffix('.cpu.pkl.gz');cpu.write_bytes(gzip.compress(pickle.dumps(extra),compresslevel=1,mtime=0))
  value['cpu_context']=dict(file=cpu.name,sha256=base.digest(cpu))
  path.write_text(json.dumps(value,indent=2)+'\n');return value
 def restore(self,path):
  super().restore(path);value=json.loads(path.read_text());item=value['cpu_context'];cpu=path.parent/item['file']
  assert base.digest(cpu)==item['sha256']
  extra=pickle.loads(gzip.decompress(cpu.read_bytes()))
  for at,data in extra['mmio']:self.u.mem_write(at,data)
  self.variant=extra['variant'];self.u.context_restore(extra['cpu'])
 def resume_to_yield(self):
  self.done=False
  self.u.emu_start(self.u.reg_read(base.UC_ARM64_REG_PC),0,count=200000000)
  assert self.done,'Interrupted task did not reach its modeled scheduler yield'
 def stop_after_allocation(self):
  self.allocation_stopped=False
  def returned(u,pc,size,unused):
   assert self.u.reg_read(base.UC_ARM64_REG_X0)==1
   self.allocation_stopped=True;self.done=True;u.emu_stop()
  def entered(u,pc,size,unused):
   target=self.u.reg_read(base.UC_ARM64_REG_LR)
   self.u.hook_add(base.UC_HOOK_CODE,returned,begin=target,end=target)
  target=self.im.address('deinterlace_allocate')
  self.u.hook_add(base.UC_HOOK_CODE,entered,begin=target,end=target)

 def weave_effect(self):
  r=self.regs;at=0x05400000
  assert r[at+0x20]==6 and r[at+0x24]==0x91001011 and r[at+0x34]==0x110
  assert r[at+0x4000]==0x3a3a3a and r[at+0x405c]==0
  assert not any(at+off in r for off in (0x1004,0x1008,0x100c,0x2000,0x3000))
  for off,iova in [(0xb0,0x10000000),(0xd0,0x10800000)]:
   assert r[at+off]==iova and r[at+off+4]==iova+307200 and r[at+off+16]==iova+640
   self.mapped(iova,460800,1)
  assert r[at+0x120]==0x10c00000 and r[at+0x124]==0x10c50000 and r[at+0x80]==160
  flags=[self.mapped(iova,307200,access) for iova,access in [(0x10c00000,1),(0x10c50000,2)]]
  assert all(bytes(self.u.mem_read(p,307200))==bytes(307200) for p in flags)
  for off,stride,delta,length in [(0xf0,640,0,307200),(0xf4,320,307200,76800),(0xf8,320,384000,76800)]:
   assert r[at+off]==0x10400000+delta and r[at+0x60+(off-0xf0)]==stride
   self.mapped(r[at+off],length,2)
  self.u.mem_write(flags[1],bytes([0x33])*307200)
  dest=self.mapped(r[at+0xf0],460800,2)
  self.u.mem_write(dest,bytes((i*13+7)&255 for i in range(460800)))
  self.events.append(('synthetic-TNR-device-writes-no-pixel-arithmetic',460800,307200))
  if self.variant=='tnr-flag-guard':self.put(flags[1]+307200,0,1)
def allocation(m,session,image):
 value=motion.allocation(m,session,image)
 value.update(md_bytes=307200,md_rounded=307200,scope='Actual compiled input/allocator/DI301 TNR generation/IOMMU construction; external authority modeled; no physical device execution.')
 return value
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','input','output'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--replay-original',type=Path);p.add_argument('--allocation-checkpoint',type=Path);p.add_argument('--allocation-only',action='store_true');p.add_argument('--resume',action='store_true');p.add_argument('--contract',type=Path)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=a.resume)
 im=base.helpers.Image(a.image);session=a.session.read_bytes();raw=a.input.read_bytes()
 assert len(session)==888 and len(raw)==921600 and struct.unpack_from('<I',session,876)[0]==3
 assert hashlib.sha256(raw).digest()==session[768:800]
 ih=base.digest(a.image);sh=hashlib.sha256(session).hexdigest();result=a.output/'result.json'
 report=json.loads(result.read_text()) if result.exists() else dict(image_sha256=ih,session_sha256=sh,cases={},physical_access=False,scope=__doc__,timing_scope='Prepared board deadline1000000 and monotonic counter1000 are explicitly modeled before allocation; bootstrap/launcher timing is excluded and is not patched after resume.')
 assert (report['image_sha256'],report['session_sha256'])==(ih,sh),'Refuse stale evidence identity'
 def passed(name,**values):
  report['cases'][name]=dict(passed=True,check_source_sha256=base.digest(Path(__file__)),**values)
  result.write_text(json.dumps(report,indent=2)+'\n');print('PASS',name,flush=True)
 def fresh():
  m=Machine(im,session);m.image_hash=ih;m.u.mem_write(base.SCRATCH,session)
  assert m.call('a733_deinterlace_session_valid',base.SCRATCH)==1
  m.pools();assert m.upload(raw)==1;return m
 if a.allocation_only:
  m=fresh();m.configure();m.stop_after_allocation()
  for _ in range(100):
   m.tick()
   if m.allocation_stopped:break
   assert not m.result(),m.events[-10:]
  else:raise AssertionError('Allocation breakpoint bound')
  m.checkpoint(a.output/'allocated.json')
  # A separate instance must load every saved byte and CPU register before
  # this checkpoint is admitted for later continuation.
  restored=Machine(im,session);restored.image_hash=ih;restored.restore(a.output/'allocated.json')
  assert restored.u.reg_read(base.UC_ARM64_REG_PC)==m.u.reg_read(base.UC_ARM64_REG_PC)
  assert allocation(restored,session,a.image)==allocation(m,session,a.image)
  (a.output/'allocation.json').write_text(json.dumps(allocation(m,session,a.image),indent=2)+'\n')
  (a.output/'page-table.bin').write_bytes(bytes(m.u.mem_read(m.get(m.base+m.fields['table']),32768)))
  if a.replay_original:
   for name in ('allocation.json','page-table.bin'):
    assert (a.output/name).read_bytes()==(a.replay_original/name).read_bytes(),('Replay differs from original',name)
   report.update(allocation_replay_count=1,allocation_replay_reason='Original allocation-only process exited without serializing continuation; one affected replay repairs the missing checkpoint.',original_allocation=str(a.replay_original.resolve()))
  passed('allocation',guarded_flag_bytes=307200,flag_output_iova='0x10c50000',checkpoint_reload_verified=True,original_artifacts_equal=bool(a.replay_original));return
 assert a.contract and a.contract.stat().st_size==288
 assert a.contract.read_bytes()[224:256]==hashlib.sha256(session).digest()
 checkpoint=a.output/'prepared.json'
 if 'nominal' not in report['cases']:
  assert a.allocation_checkpoint,'Require preserved allocation-return continuation'
  m=Machine(im,session);m.image_hash=ih;m.restore(a.allocation_checkpoint)
  assert m.get(m.base+m.fields['stage'],4)==0 and m.started==0
  m.resume_to_yield();assert m.get(m.base+m.fields['stage'],4)==1
  m.checkpoint(checkpoint);m.run()
  assert m.result()==1 and m.started==1 and m.get(m.base+m.fields['copied'])==460800
  output=bytes(m.u.mem_read(m.get(m.base+m.fields['payload']),460800))
  assert output==bytes((i*13+7)&255 for i in range(460800))
  (a.output/'output.yuv420').write_bytes(output);m.checkpoint(a.output/'copied.json')
  passed('nominal',submissions=1,copied=460800,pixel_arithmetic_checked=False)
 if 'tnr-flag-guard' not in report['cases']:
  m=Machine(im,session);m.image_hash=ih;m.restore(checkpoint);m.variant='tnr-flag-guard';m.run()
  assert m.result()==2 and m.started==1 and all(row['state']==2 for row in m.rows()['uncached_pool'])
  passed('tnr-flag-guard',retained=True,healthy_not_published=True)
 if 'native-reply-ack' not in report['cases']:
  m=Machine(im,session);m.image_hash=ih;m.restore(a.output/'copied.json');motion.replies(m,a.output)
  passed('native-reply-ack',native_serializer=True,vendor_fifo_and_transfer_completions_modeled=True)
if __name__=='__main__':main()
