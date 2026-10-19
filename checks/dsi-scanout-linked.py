#!/usr/bin/env python3
"""Exercise private linked DSI admission/seal and copied observation boundaries."""
import argparse,hashlib,importlib.util,json,struct,time,subprocess
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class Machine:
 def __init__(self,image):
  self.image=image;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.mem_map(0x40000000,0x04000000);self.cpu.mem_map(0x50000000,0x01000000)
  for address,data in image.parts:self.cpu.mem_write(address,data)
  self.hooks={};self.finished=False;self.calls=[];self.cpu.hook_add(UC_HOOK_CODE,self.hook)
 def ret(self,value):self.cpu.reg_write(UC_ARM64_REG_X0,value);self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 def hook(self,cpu,address,size,unused):
  if address==0x50fff000:self.finished=True;cpu.emu_stop();return
  if address in self.hooks:
   self.calls.append(address);self.hooks[address]([cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)])
 def call(self,name,*args):
  self.finished=False;self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
  for i,value in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,value)
  self.cpu.emu_start(self.image.address(name),0,count=4000000);assert self.finished,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)));return self.cpu.reg_read(UC_ARM64_REG_X0)
 def set(self,base,layout,key,value,size=8):self.cpu.mem_write(base+layout[key],value.to_bytes(size,'little'))
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','application','output'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--seal-only',action='store_true');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=a.seal_only);image=module('dsi_image',ROOT/'checks/usb-host-hid-native.py').Image(a.image);prep=module('dsi_prep',ROOT/'apps/dsi-scanout/prepare.py');decoder=module('dsi_decode',ROOT/'apps/dsi-scanout/observe.py');capture=module('dsi_capture',ROOT/'apps/dsi-scanout/capture.py');cases={}
 for mode in (() if a.seal_only else ('720p','1080p')):
  raw=(a.application/(mode+'-session.bin')).read_bytes();observation=(a.application/(mode+'-observation.bin')).read_bytes();raw=bytearray(raw);struct.pack_into('<I',raw,532,2);raw=bytes(raw);prep.session_check(raw)
  model=a.output/(mode+'-session.bin');model.write_bytes(raw)
  r=subprocess.run([str(a.application/'native-check'),str(model),'success',str(a.output/(mode+'-observation.bin'))],capture_output=True,text=True)
  (a.output/(mode+'-composition.json')).write_text(json.dumps(dict(command=r.args,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr),indent=2)+'\n');assert r.returncode==0
  observation=(a.output/(mode+'-observation.bin')).read_bytes()
  for state in (0,1,2):
   m=Machine(image);cpu=m.cpu;board=image.address('board');dsi=image.address('dsi');bl=image.layouts['board_application'];dl=image.layouts['dsi_application'];cpu.mem_write(board+bl['session'],raw[:592]);cpu.mem_write(dsi+dl['session'],raw);cpu.mem_write(dsi+dl['profile_hash'],hashlib.sha256(raw).digest());cpu.mem_write(dsi+dl['observation'],observation);m.set(dsi,dl,'result',state,4)
   for name in ('board_live','dsi_live','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext'):
    m.hooks[image.address(name)]=lambda args:m.ret(1)
   available=[True]
   m.hooks[image.address('tud_vendor_n_available')]=lambda args:m.ret(int(available[0]))
   def read(args):cpu.mem_write(args[1],bytes(1));available[0]=False;m.ret(1)
   m.hooks[image.address('tud_vendor_n_read')]=read
   def delay(args):m.finished=True;cpu.emu_stop()
   m.hooks[image.address('vTaskDelay')]=delay
   # These provider boundaries must remain outside the private serializer fixture.
   m.call('board_usb0_task',0);count=int.from_bytes(cpu.mem_read(image.address('dsi_response_bytes','/apps/dsi-scanout/usb0.c'),4),'little');assert count==(384 if state==1 else 128)
   frame=bytes(cpu.mem_read(image.address('dsi_response','/apps/dsi-scanout/usb0.c'),count));(a.output/(mode+f'-{state}-frame.bin')).write_bytes(frame);decoded=decoder.decode(frame,raw);assert decoded['status']==state
   if state==1:
    case=a.output/mode;case.mkdir();record={};result=capture.capture(lambda packet,timeout:frame,raw,case,record,lambda:None,time.monotonic()+10);assert result['status']==1 and record['dsi_result']=='observed-success' and (case/'output.bin').read_bytes()==observation
    refused=[]
    for name,off in [('identity',24),('session',32),('payload-hash',64),('extent',12),('reserved',96),('geometry',252),('panel',376),('iommu',364)]:
     bad=bytearray(frame);bad[off]^=1
     if off>=128:bad[64:96]=hashlib.sha256(bad[128:]).digest()
     try:decoder.decode(bad,raw)
     except ValueError:refused.append(name)
     else:raise AssertionError(name)
    cases[mode]=dict(copied_bytes=256,refused=refused)
 if not a.seal_only:(a.output/'serializer-result.json').write_text(json.dumps(cases,indent=2)+'\n')
 else:cases=json.loads((a.output/'serializer-result.json').read_text())
 raw=(a.output/'720p-session.bin').read_bytes();base=0x41e00000;layout=image.layouts['a733_smp_contract'];buildid=raw[48:80]
 # The actual producer creates the288-byte contract. Only modeled monitor
 # arming fields are added; shared startup, cache setup and GIC are never called.
 request,contract=prep.request_bytes(image.address('sd_single_core_entry'),buildid,raw,*struct.unpack_from('<3Q',raw,24),100)
 now=2400;contract=bytearray(contract[:288])
 def put(key,value):struct.pack_into('<Q',contract,layout[key],value)
 for key,value in [('magic',prep.CONTRACT_MAGIC),('armed_at',now),('startup_deadline',now+24000000),('diagnostic_deadline',now+60*24000000),('expires_at',now+60*24000000)]:put(key,value)
 seal_cases=[]
 for kind in ('success','session-corrupt','entry-corrupt','extent','pool','expiry'):
  m=Machine(image);cpu=m.cpu;cpu.mem_write(0x41f00000,raw);c=bytearray(contract)
  if kind=='session-corrupt':c[224]^=1
  if kind=='entry-corrupt':c[256]^=1
  if kind=='extent':struct.pack_into('<Q',c,layout['bytes'],224)
  if kind=='pool':struct.pack_into('<Q',c,layout['reserved']+16,0x48100000)
  at=image.address('dsi_admission');cpu.mem_write(at,bytes(c));cpu.mem_write(base,bytes(c[:224]));m.hooks[image.address('a733_smp_counter')]=lambda args:m.ret(now+(24000000 if kind=='expiry' else 1))
  got=m.call('dsi_session_prepare',at);assert got==(1 if kind=='success' else 0),(kind,got)
  if kind=='success':
   saved=m.call('dsi_admitted_session');assert saved and bytes(cpu.mem_read(saved,4096))==raw
   cpu.mem_write(0x41f00000,bytes(4096));assert m.call('dsi_admitted_session')==saved and bytes(cpu.mem_read(saved,4096))==raw
   assert m.call('dsi_session_prepare',at)==0
  else:assert m.call('dsi_admitted_session')==0
  seal_cases.append(kind)
 (a.output/'contract-armed-MODEL-ONLY.bin').write_bytes(contract);(a.output/'request-MODEL-ONLY.bin').write_bytes(request)
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),cases=cases,private_seal_cases=seal_cases,scope='Actual linked private admission/seal and USB0 serializer; real host producer/decoder/durable capture. USB0, RTOS and counter boundaries modeled. Shared startup/cache/GIC/secondary execution excluded.'),indent=2)+'\n')
if __name__=='__main__':main()
