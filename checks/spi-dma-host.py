#!/usr/bin/env python3
"""Offline DMA session authority, frozen bundle and fragmented copied capture."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def refuses(fn):
 try:fn()
 except (ValueError,AssertionError):return
 raise AssertionError('Invalid input accepted')
def launcher(bundle):
 from elftools.elf.elffile import ELFFile
 from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
 from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_PC,UC_ARM_REG_SP,UC_ARM_REG_LR
 with (bundle/'launch.elf').open('rb') as f:
  elf=ELFFile(f);symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
 results={}
 for label,offset,value in [('nominal',None,None),('legacy-version',600,2),('short-session',608,832),('mixed-generation',32,0)]:
  cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM)
  for a,n in ((0x50000,0x30000),(0x41f00000,0x1000)):cpu.mem_map(a,n)
  cpu.mem_write(0x52000,(bundle/'launch.bin').read_bytes());cpu.mem_write(0x52c00,(bundle/'request.bin').read_bytes())
  raw=bytearray((bundle/'session.bin').read_bytes())
  if offset is not None:struct.pack_into('<Q',raw,offset,value)
  cpu.mem_write(0x41f00000,bytes(raw));terminal=[]
  def hook(uc,at,size,unused):
   if at==symbols['spi_supervised_base_prepare']:terminal.append('common-launcher');uc.emu_stop()
   elif at==symbols['refused']:terminal.append(uc.reg_read(UC_ARM_REG_R0));uc.emu_stop()
  cpu.hook_add(UC_HOOK_CODE,hook);cpu.reg_write(UC_ARM_REG_SP,0x70000);cpu.reg_write(UC_ARM_REG_LR,0x60000)
  cpu.emu_start(symbols['a733_smp_launch_prepare'],0x60000,count=10000)
  assert terminal==(['common-launcher'] if label=='nominal' else [14]),(label,terminal)
  results[label]=terminal
 return results
def run(bundle,frame_path,out):
 out.mkdir(parents=True,exist_ok=False);host=module('spi_dma_capture',ROOT/'apps/spi/capture.py');prep=host.preparation
 frozen=host.freeze_bundle(bundle);raw=frozen['_frozen']['session.bin'];frame=frame_path.read_bytes()
 assert host.decoder.decode(frame,raw)['bytes']==4192
 record=json.loads((bundle/'admission.json').read_text());template=(bundle/'session-template.bin').read_bytes()
 ih=hashlib.sha256((bundle/'image.elf').read_bytes()).hexdigest()
 for label,offset,fmt,value in [('channels',918,'H',1),('routing',864,'I',1),('fabric',832,'I',1),('command',800,'B',0),('count',796,'I',4095),('profile',600,'Q',2)]:
  bad=bytearray(template);struct.pack_into('<'+fmt,bad,offset,value)
  refuses(lambda:prep.validate_admission(record,ih,hashlib.sha256(bad).hexdigest(),bytes(bad)))
 refuses(lambda:prep.session_check(template[:-1]))
 refuses(lambda:prep.image_profile((bundle/'image.elf').read_bytes(),template[:832]))
 for offset in (128+28,128+64,128+72,128+76,128+80):
  bad=bytearray(frame);struct.pack_into('<I',bad,offset,0);bad[64:96]=hashlib.sha256(bad[128:]).digest()
  # Request-off is zero on success; use one for that field.
  if offset==200:struct.pack_into('<I',bad,offset,1);bad[64:96]=hashlib.sha256(bad[128:]).digest()
  refuses(lambda:host.decoder.decode(bytes(bad),raw))
 pending=bytearray(frame[:128]);struct.pack_into('<2I',pending,8,0,0);pending[64:96]=bytes(32)
 class Device:
  def __init__(self,mode):self.mode=mode;self.requests=0;self.reads=0;self.remaining=b'';self.clock=1.
  def write(self,ep,data,timeout):
   assert ep==2 and data==bytes(1) and not self.remaining;self.requests+=1
   self.remaining=bytes(pending) if self.requests==1 else frame
   return 1
  def read(self,ep,n,timeout):
   assert ep==0x81 and timeout>0;self.reads+=1
   if self.mode=='deadline' and self.requests==2:self.clock=61.
   if self.mode=='truncated' and self.requests==2 and self.reads>5:return b''
   count=min(n, [1,31,97,509][self.reads%4],len(self.remaining));data=self.remaining[:count];self.remaining=self.remaining[count:];return data
 results={}
 for mode in ('fragmented','truncated','deadline'):
  directory=out/mode;directory.mkdir();device=Device(mode);record={}
  # Capture's default clock was bound at import; replace only this dependency.
  capture=host.capture
  def capture_clock(*args,**kwargs):return capture(*args,**kwargs,now=lambda:device.clock,pause=lambda _:None)
  with patch.object(host,'capture',capture_clock),patch.object(host.time,'monotonic',lambda:device.clock):
   action=lambda:host.capture_device(device,lambda:None,60.,raw,directory,record,lambda:None)
   if mode=='fragmented':
    assert action()['status']==1 and (directory/'output.bin').read_bytes()==frame[128:]
    assert device.requests==2 and device.reads>8
   else:refuses(action);assert record['spi_result']=='unobserved' and not (directory/'output.bin').exists()
  results[mode]=dict(passed=True,requests=device.requests,reads=device.reads)
 results['authority-and-freeze']=dict(passed=True,session_bytes=len(raw),frozen_files=len(frozen['_frozen']))
 results['private-launcher']=dict(passed=True,cases=launcher(bundle),scope='Actual SPI admission instructions up to the reused common launcher; no physical execution')
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=results,physical_access=False),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('bundle','frame','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();run(a.bundle,a.frame,a.output)
