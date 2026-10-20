#!/usr/bin/env python3
"""Check retained USB authority and response identity in the linked GPU/display image."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_LR,UC_ARM64_REG_SP,UC_ARM64_REG_PSTATE
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
for name in ('image','session','contract','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();P=a.image;O=a.output;O.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('gd_usb_admission_image',R/'checks/storage-read-native.py')
n=importlib.util.module_from_spec(spec);spec.loader.exec_module(n)
e=n.Image(P,layout_names=('a733_smp_contract',))
raw=a.session.read_bytes();c=bytearray(a.contract.read_bytes()[:288]);upload=0x41f00000
assert struct.unpack_from('<3Q',raw)==(0x4153373347444131,2,len(raw)) and len(raw)>=984
assert len(c)==288 and c[224:256]==hashlib.sha256(raw).digest() and c[256:288]==raw[104:136]
struct.pack_into('<Q',c,0,0x4153373347444331)
for name,value in dict(armed_at=100,startup_deadline=24000100,diagnostic_deadline=480000100,expires_at=480000100).items():
 struct.pack_into('<Q',c,e.layouts['a733_smp_contract'][name],value)
c=bytes(c);(O/'session.bin').write_bytes(raw);(O/'contract.bin').write_bytes(c)
class Machine:
 def __init__(self,publish=1):
  self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.reg_write(UC_ARM64_REG_PSTATE,13)
  self.u.mem_map(0x41000000,0x1200000);self.u.mem_map(0x50000000,0x200000)
  for a,b in e.parts:
   if b:self.u.mem_write(a,b)
  self.u.mem_write(upload,raw);self.u.mem_write(0x42004000,c);self.rc=e.address('retained_contract');self.u.mem_write(self.rc,c)
  self.publications=[]
  def ret(v):self.u.reg_write(UC_ARM64_REG_X0,v);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
  def clock(*_):ret(200)
  def publication(*_):
   self.publications.append([self.u.reg_read(x) for x in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2)]);ret(publish)
  def stop(*_):self.u.emu_stop()
  for name,fn in [('a733_smp_counter',clock),('a733_cpu_coherence_publish',publication)]:
   a=e.address(name);self.u.hook_add(UC_HOOK_CODE,fn,begin=a,end=a)
  self.u.hook_add(UC_HOOK_CODE,stop,begin=0x501ff000,end=0x501ff000)
 def call(self,name,*args):
  for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  self.u.reg_write(UC_ARM64_REG_SP,0x501fe000);self.u.reg_write(UC_ARM64_REG_LR,0x501ff000)
  self.u.emu_start(e.address(name),0x501ff004,count=150000000)
  assert self.u.reg_read(UC_ARM64_REG_PC)==0x501ff000,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  return self.u.reg_read(UC_ARM64_REG_X0)
results=[]
m=Machine()
assert m.call('gpu_display_usb_get')==0 and m.call('gpu_display_session_digest_get')==0
assert m.call('gpu_display_session_prepare',m.rc)==1
usb=m.call('gpu_display_usb_get');digest=m.call('gpu_display_session_digest_get');inputs=m.call('gpu_display_inputs_get')
assert usb and digest and inputs and len(m.publications)==1
assert bytes(m.u.mem_read(usb,136))==raw[704:840]
assert bytes(m.u.mem_read(digest,32))==hashlib.sha256(raw).digest()
assert bytes(m.u.mem_read(inputs,len(raw)-840))==raw[840:]
m.u.mem_write(upload,bytes(len(raw)))
assert bytes(m.u.mem_read(usb,136))==raw[704:840]
assert bytes(m.u.mem_read(digest,32))==hashlib.sha256(raw).digest()
assert bytes(m.u.mem_read(inputs,len(raw)-840))==raw[840:]
results.append('authenticated-private-USB-input-and-digest-publication')
for name,offset,value in [('owner-conflict',704,raw[240:248]),('missing-USB-proof',720,bytes(32))]:
 m=Machine();m.u.mem_write(upload+offset,value)
 assert m.call('gpu_display_session_valid',upload)==0,name
 results.append(name)
m=Machine();m.u.mem_write(upload+712,bytes([raw[712]^1]))
assert m.call('a733_smp_contract_valid',m.rc,200)==0
results.append('unsealed-USB-handoff-change')
report={'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(P.read_bytes()).hexdigest(),
 'session_sha256':hashlib.sha256(raw).hexdigest(),'cases':results,
 'scope':'Actual linked session/hash/private-copy/getter code. Counter, cache publication and physical authority modeled. No provider, CPU startup, USB or peripheral execution. Earlier unchanged memory/layout and publication-failure cases are not replayed.'}
(O/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS GPU/display USB authority and immutable capture identity')
