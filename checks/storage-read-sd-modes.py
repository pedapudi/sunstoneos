#!/usr/bin/env python3
"""Selected negotiated-mode reads through the actual linked native service."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
service=module('sd_modes_service',ROOT/'checks/storage-read-service.py')
class Machine(service.Machine):
 def __init__(self,image,session):
  self.width=1;self.card_hs=0;self.mode_app=False;self.mode_data=bytes();self.module_changes=[]
  super().__init__(image,session);self.reject_mapping=False
  self.regs.update({0x020020a0:0xfe000000|(99<<8)|(1<<20),0x02003908:((1<<11)|(1<<2)|(1<<1))*0x10001,0x02003988:((1<<11)|(1<<2)|(1<<1))<<16})
  csd=(1<<126)|(9<<80)|((5|(1<<10))<<84)|(0x7fff<<48)|(0x32<<96)
  self.csd=[(csd>>(96-32*i))&0xffffffff for i in range(4)]
 def code(self,cpu,a,size,unused):
  if self.reject_mapping and self.hooks.get(a)=='aarch64_device_translate':self.ret(0)
  else:super().code(cpu,a,size,unused)
 def prepare_only(self):
  # Reuse the accepted admission/start sequence, stopping before the first
  # owner iteration so the real copied API observes unconstructed state.
  self.reject_mapping=False
  original=self.owner
  def stop():raise StopIteration
  self.owner=stop
  try:self.establish()
  except StopIteration:pass
  finally:self.owner=original
 def read_mmio(self,cpu,access,a,size,value,unused):
  if a==0x04020200 and self.data_command in (6,51):
   assert self.held and size==4 and self.fifo<self.total
   word=int.from_bytes(self.mode_data[self.fifo:self.fifo+4],'little');self.fifo+=4
   self.trace.append(['read',a,word]);self.write(a,word);return
  super().read_mmio(cpu,access,a,size,value,unused)
 def write_mmio(self,cpu,access,a,size,v,unused):
  if a==0x02002d00 and self.width==4:
   assert self.held and self.regs.get(0x04020004)==0 and not self.regs.get(0x04020018,0)&(1<<31)
   self.module_changes.append(v)
  if a==0x04020018 and not v&(1<<21) and self.state==4 and v&63 in (55,51,6):
   assert self.held and size==4 and not v&(1<<10)
   n=v&63;arg=self.regs.get(0x0402001c,0);self.commands.append([n,arg]);self.trace.append(['write',a,v])
   self.regs[a]=v&~(1<<31);self.regs[0x04020038]=4;self.regs[0x04020020]=0x900
   self.data_command=self.total=self.fifo=0
   if n==55:assert arg==0x12340000;self.mode_app=True;self.regs[0x04020020]|=1<<5
   elif n==51:
    assert self.mode_app and arg==0 and self.regs[0x04020010]==8
    self.mode_app=False;self.data_command=51;self.total=8;self.mode_data=bytes([2,5])+bytes(6)
   elif not v&(1<<9):assert self.mode_app and arg==2;self.mode_app=False;self.width=4
   else:
    assert self.width==4 and self.regs[0x0402000c]==1 and self.regs[0x04020010]==64
    assert arg in (0x00fffff1,0x80fffff1);self.data_command=6;self.total=64
    data=bytearray(64);data[13]=2
    if arg>>31:self.card_hs=1;data[16]=1
    self.mode_data=bytes(data)
   return
  super().write_mmio(cpu,access,a,size,v,unused)
def run(image,sessions,out,early_only=False):
 out.mkdir(parents=True,exist_ok=False);e=service.base.Image(image);e.sizes={};e.symbol_sizes={}
 with image.open('rb') as f:
  elf=ELFFile(f)
  for s in elf.get_section_by_name('.symtab').iter_symbols():
   if s.name in ('storage_read','storage_read_consumer','board','providers'):e.symbol_sizes[s.name]=int(s['st_size'])
  for cu in elf.get_dwarf_info().iter_CUs():
   for die in cu.iter_DIEs():
    if die.tag=='DW_TAG_structure_type' and 'DW_AT_name' in die.attributes and 'DW_AT_byte_size' in die.attributes:e.sizes[die.attributes['DW_AT_name'].value.decode()]=int(die.attributes['DW_AT_byte_size'].value)
 decoder=module('sd_modes_decoder',ROOT/'apps/storage-read/observe.py');results={}
 for session in sessions:
  raw=session.read_bytes();profile=struct.unpack_from('<I',raw,860)[0];assert profile in (1,2)
  dest=out/f'profile-{profile}';dest.mkdir();m=Machine(e,raw)
  if early_only:
   m.prepare_only();pending=m.frame();value=decoder.decode(pending,raw)
   assert value['status']==0 and struct.unpack_from('<I',pending,4)[0]==3 and m.field('initialized')==0
   (dest/'pending-frame.bin').write_bytes(pending)
   m.reject_mapping=True;m.owner();retained=m.frame();value=decoder.decode(retained,raw)
   assert value['status']==2 and m.field('reason')==2 and not m.commands
   assert struct.unpack_from('<I',retained,4)[0]==3
   (dest/'retained-frame.bin').write_bytes(retained)
   results[str(profile)]={'preconstruction_pending':True,'early_mapping_failure_retained':True,'physical_access':False}
   continue
  m.establish();assert m.width==4 and m.card_hs==(profile==2)
  assert len(m.module_changes)==3 and m.module_changes[-1]==(0x81000007 if profile==1 else 0x81000003)
  (dest/'idle-frame.bin').write_bytes(m.frame())
  accepted,ticket=m.submit(2,19,2,1);assert accepted==1
  m.native_client();m.complete(0,True);frame=m.frame();value=decoder.decode(frame,raw)
  assert value['card_hz']==(25000000 if profile==1 else 50000000) and value['bus_width']==4 and value['timing']==(profile==2)
  assert frame[288:]==service.payload(19,2);(dest/'read-frame.bin').write_bytes(frame)
  for _ in range(500):
   m.native_client();m.owner()
   if m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['result'])==1:break
  assert m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['completed'])==2
  assert m.frame()[288:]==frame[288:] and m.invoke(2,'storage_read_ack',ticket)==1
  accepted,ticket=m.submit(2,21,1,2);assert accepted==1;m.complete(0);second=m.frame();decoder.decode(second,raw)
  assert second[288:]==service.payload(21,1) and m.invoke(2,'storage_read_ack',ticket)==1
  (dest/'second-frame.bin').write_bytes(second)
  (dest/'trace.json').write_text(json.dumps(m.trace)+'\n')
  results[str(profile)]={'passed':True,'commands':m.commands,'module_changes':m.module_changes,'native_reads':2,'usb_copied_reads':2,'physical_access':False}
 (out/'result.json').write_text(json.dumps({'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'cases':results,'scope':('Actual linked session/start and copied API before construction and after early mapping failure; board/provider/time/scheduling hooks modeled. No card commands, startup or physical qualification.' if early_only else 'Actual linked owner/core/native filter, session/parser and two copied clients; shared provider returns, MMIO/card/time/scheduling modeled. No startup or physical qualification.')},indent=2)+'\n')
 print('Actual native early copied diagnostics passed.' if early_only else 'Actual native SD4 default/HS negotiation and native/USB repeated copied reads passed.')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--session',type=Path,action='append',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--early-copy-only',action='store_true');a=p.parse_args();run(a.image,a.session,a.output,a.early_copy_only)
