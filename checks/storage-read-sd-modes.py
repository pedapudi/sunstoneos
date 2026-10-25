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
# SD physical layer 9.10 section 4.2.4.5: the four-bit CMD19 tuning block.
TUNING=bytes.fromhex('ff0fff00ffccc3ccc33cccfffefffeefffdfffddfffbfffbbfff7fff77f7bdef'
                     'fff0fff00ffccc3ccc33cccfffefffeefffdfffddfffbfffbbfff7fff77f7bde')
assert len(TUNING)==64
class UhsMachine(Machine):
 """A UHS-I card and the PF bank selector, for SD profile 3.

 The card answers S18R with S18A, holds DAT low after CMD11 until the clock
 restarts after the bank reports 1.8 V, then reports switch done; CMD6 offers
 SDR104; CMD19 returns the tuning block only for sample delays in WINDOW."""
 WINDOW=range(20,32)
 def __init__(self,image,session):
  super().__init__(image,session);self.phase=0;self.bank_reads=None;self.pf_writes=0;self.cmd19=0;self.sdr=0
  self.regs[0x02000040]=1<<10 # Adaptive PF withstand mode, as port 3-3 reports.
  gates=(1<<11)|(1<<9)|(1<<5)|(1<<4)|(1<<2)|(1<<1)
  self.regs.update({0x02003908:gates*0x10001,0x02003988:gates<<16})
 def read_mmio(self,cpu,access,a,size,value,unused):
  if a==0x04020200 and self.data_command==19:
   assert self.held and size==4 and self.fifo<self.total
   word=int.from_bytes(self.mode_data[self.fifo:self.fifo+4],'little');self.fifo+=4
   self.trace.append(['read',a,word]);self.write(a,word);return
  if a==0x02000048 and self.bank_reads is not None:
   self.bank_reads-=1
   if self.bank_reads<=0:self.regs[a]=3<<10
  super().read_mmio(cpu,access,a,size,value,unused)
  if a==0x0402003c:
   v=self.read(a)|((1<<9) if self.phase in (1,2) else (1<<8));self.write(a,v);self.trace[-1][2]=v
 def write_mmio(self,cpu,access,a,size,v,unused):
  if a==0x02000070:
   assert self.held and v==0 and self.phase==2 and not self.regs.get(0x04020004) and not self.pf_writes
   self.pf_writes+=1;self.bank_reads=3;self.trace.append(['write',a,v]);self.regs[a]=0;return
  if a==0x04020018 and v&(1<<21) and v&(1<<28):
   assert self.held and self.regs.get(0x04020004,0)&(1<<31);self.trace.append(['write',a,v]);self.regs[a]=0
   if self.phase==1:self.phase=2
   else:
    assert self.phase==2 and self.regs[0x02000048]==3<<10 and self.regs[0x04020004]==(1<<16)|(1<<31)
    self.phase=3;self.regs[0x04020038]=self.regs.get(0x04020038,0)|(1<<10)|4
   return
  if a==0x04020018 and not v&(1<<21) and v&63 in (11,19,41,6) and not (v&63==6 and not v&(1<<9)):
   n=v&63;arg=self.regs.get(0x0402001c,0)
   if n==41:
    super().write_mmio(cpu,access,a,size,v,unused)
    assert arg==0x40300000|(1<<24)
    if self.ocr>=2:self.regs[0x04020020]|=1<<24
    return
   assert self.held and not v&(1<<10);self.trace.append(['write',a,v]);self.commands.append([n,arg])
   self.regs[a]=v&~(1<<31);self.regs[0x04020038]=4;self.regs[0x04020020]=0x900;self.data_command=self.total=self.fifo=0
   if n==11:
    assert v==0x9000014b and arg==0 and self.state==1 and self.phase==0;self.regs[0x04020020]=0x200;self.phase=1;return
   if n==6 and arg in (0x00fffff3,0x80fffff3):
    assert self.phase==3 and self.width==4 and self.regs[0x04020010]==64
    data=bytearray(64);data[13]=0x0b;data[16]=3
    if arg>>31:self.sdr=3
    self.data_command=6;self.total=64;self.mode_data=bytes(data);return
   if n==6:
    self.regs[a]=0;self.commands.pop();self.trace.pop()
    return super().write_mmio(cpu,access,a,size,v,unused)
   assert n==19 and arg==0 and self.sdr==3 and self.regs[0x04020010]==64 and self.regs[0x04020144]&0x80
   assert (self.regs.get(0x04020100,0)&0x1fff0001)==(64<<16)|1 and not self.regs.get(0x0402005c,0)&(1<<31)
   self.cmd19+=1;self.commands.pop()
   self.data_command=19;self.total=64;self.mode_data=TUNING
   if self.regs[0x04020144]&63 not in self.WINDOW:self.regs[0x04020038]|=1<<7
   return
  super().write_mmio(cpu,access,a,size,v,unused)
def run(image,sessions,out,early_only=False,layout_only=False,board_live_only=False):
 out.mkdir(parents=True,exist_ok=False);e=service.base.sd0_image(image);e.sizes={};e.symbol_sizes={}
 with image.open('rb') as f:
  elf=ELFFile(f)
  for s in elf.get_section_by_name('.symtab').iter_symbols():
   if s.name in ('storage_read','storage_read_consumer','board','providers'):e.symbol_sizes[s.name]=int(s['st_size'])
  for cu in elf.get_dwarf_info().iter_CUs():
   for die in cu.iter_DIEs():
    if die.tag=='DW_TAG_structure_type' and 'DW_AT_name' in die.attributes and 'DW_AT_byte_size' in die.attributes:e.sizes[die.attributes['DW_AT_name'].value.decode()]=int(die.attributes['DW_AT_byte_size'].value)
 decoder=module('sd_modes_decoder',ROOT/'apps/storage-read/observe.py');results={}
 for session in sessions:
  raw=session.read_bytes();profile=struct.unpack_from('<I',raw,860)[0];assert profile in (1,2,3)
  dest=out/f'profile-{profile}';dest.mkdir();m=(UhsMachine if profile==3 else Machine)(e,raw)
  if board_live_only:
   layout=e.layouts['board_application'];address=e.address('board_live');hook=m.hooks.pop(address)
   m.write(m.board+layout['fatal'],0);m.write(m.board+layout['live_until'],m.time+1000000,8)
   assert m.call('board_live',0)==1
   m.write(m.board+layout['fatal'],1);assert m.call('board_live',0)==0
   m.write(m.board+layout['fatal'],0);m.write(m.board+layout['live_until'],m.time,8)
   assert m.call('board_live',0)==0;m.hooks[address]=hook
   results[str(profile)]={'actual_board_live_healthy':True,'actual_board_live_fatal_refused':True,'actual_board_live_expired_refused':True,'field_offsets':{k:layout[k] for k in ('fatal','live_until')},'physical_access':False}
   continue
  if early_only:
   m.prepare_only();pending=m.frame();value=decoder.decode(pending,raw)
   assert value['status']==0 and struct.unpack_from('<I',pending,4)[0]==4 and m.field('initialized')==0
   (dest/'pending-frame.bin').write_bytes(pending)
   m.reject_mapping=True;m.owner();retained=m.frame();value=decoder.decode(retained,raw)
   assert value['status']==2 and m.field('reason')==2 and not m.commands
   assert struct.unpack_from('<I',retained,4)[0]==4
   (dest/'retained-frame.bin').write_bytes(retained)
   results[str(profile)]={'preconstruction_pending':True,'early_mapping_failure_retained':True,'physical_access':False}
   continue
  m.establish();assert m.width==4 and m.card_hs==(profile==2)
  assert len(m.module_changes)==3 and m.module_changes[-1]==(0x81000007 if profile==1 else 0x81000003 if profile==2 else 0x82000000)
  if profile==3:
   # One selector write, 12 passing delays of 20 CMD19 each plus 52 failing ones.
   assert m.pf_writes==1 and m.phase==3 and m.sdr==3 and m.cmd19==12*20+52 and (m.regs[0x04020144]&0xbf)==0x80|26
   assert [c[0] for c in m.commands[:7]]==[0,8,55,41,55,41,11]
  if layout_only:
   assert profile==1 and len(sessions)==1
   packet=decoder.read_request(19,2,1)
   m.usb_step(packet[:1]);m.usb_step(packet[1:]);assert len(m.tx)==1
   pending=m.tx[-1];assert decoder.decode(pending,raw)['status']==0
   (dest/'pending-frame.bin').write_bytes(pending)
   m.invoke(2,'tud_vendor_tx_cb',0,len(pending));m.complete(0,True)
   for _ in range(500):
    m.native_client();m.owner()
    if m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['result'])==1:break
   m.usb_step(b'\0');frame=m.tx[-1];value=decoder.decode(frame,raw)
   assert frame[288:]==service.payload(19,2) and value['bus_width']==4 and value['card_hz']==25000000
   assert m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['result'])==1
   assert m.read(e.address('storage_read_consumer')+e.layouts['storage_read_consumer']['completed'])==2
   m.invoke(2,'tud_vendor_tx_cb',0,len(frame));m.usb_step();assert m.state_of(0)==3
   (dest/'read-frame.bin').write_bytes(frame);(dest/'trace.json').write_text(json.dumps(m.trace)+'\n')
   results[str(profile)]={'passed':True,'actual_native_client_reads':2,'actual_usb0_copied_reads':1,'fragmented_request':True,'completed_tx_callback_and_ack':True,'physical_access':False}
   continue
  (dest/'idle-frame.bin').write_bytes(m.frame())
  accepted,ticket=m.submit(2,19,2,1);assert accepted==1
  m.native_client();m.complete(0,True);frame=m.frame();value=decoder.decode(frame,raw)
  assert value['card_hz']==(25000000 if profile==1 else 50000000 if profile==2 else 150000000) and value['bus_width']==4
  # Model time does not advance inside one owner call, so elapsed_us may be 0.
  assert value['timing']==(1 if profile==2 else 3 if profile==3 else 0) and value['elapsed_us'] is not None
  if profile==3:assert (value['uhs_outcome'],value['signal_1v8'],value['tuning_start'],value['tuning_width'],value['sample_delay'])==(1,True,20,12,26)
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
 (out/'result.json').write_text(json.dumps({'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'cases':results,'scope':('Actual linked board_live loads with healthy, fatal and expired shared-board fields, using current DWARF layout. Counter hooked; no controller/protocol tests repeated.' if board_live_only else 'Current linked board embedding layout, actual native client and USB0 fragmented request/copy/TX callback/ack; provider returns, card/MMIO/time/scheduling modeled. No host HID execution, CPU/startup or physical qualification.' if layout_only else 'Actual linked session/start and copied API before construction and after early mapping failure; board/provider/time/scheduling hooks modeled. No card commands, startup or physical qualification.' if early_only else 'Actual linked owner/core/native filter, session/parser and two copied clients; shared provider returns, MMIO/card/time/scheduling modeled. No startup or physical qualification.')},indent=2)+'\n')
 print('Current linked board liveness layout passed.' if board_live_only else 'Current linked SD/client/USB0 layout composition passed.' if layout_only else 'Actual native early copied diagnostics passed.' if early_only else 'Actual native SD4 default/HS negotiation and native/USB repeated copied reads passed.')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--session',type=Path,action='append',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--early-copy-only',action='store_true');p.add_argument('--layout-only',action='store_true');p.add_argument('--board-live-only',action='store_true');a=p.parse_args();assert sum((a.layout_only,a.early_copy_only,a.board_live_only))<=1;run(a.image,a.session,a.output,a.early_copy_only,a.layout_only,a.board_live_only)
