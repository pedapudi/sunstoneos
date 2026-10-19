#!/usr/bin/env python3
"""Run actual linked replay serializer/submit helper and real copied host capture."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from unicorn import UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','application','output'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--resume',action='store_true');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=a.resume);old=module('dsi_linked',ROOT/'checks/dsi-scanout-linked.py');image=module('dsi_replay_image',ROOT/'checks/usb-host-hid-native.py').Image(a.image);decode=module('dsi_replay_decode',ROOT/'apps/dsi-scanout/observe.py');host=module('dsi_replay_capture',ROOT/'apps/dsi-scanout/capture.py');cases={}
 for mode in ('720p','1080p'):
  raw=(a.application/mode/'session.bin').read_bytes();frames=[]
  for sequence in range(1,4):
   saved=a.output/(mode+f'-frame{sequence}.bin')
   if a.resume and saved.exists():frames.append(saved.read_bytes());continue
   payload=(a.application/(mode+'-success')/f'present-{sequence}.bin').read_bytes();m=old.Machine(image);cpu=m.cpu;board=image.address('board');dsi=image.address('dsi');bl=image.layouts['board_application'];dl=image.layouts['dsi_application'];il=image.layouts['dsi_input'];input_address=dsi+dl['input']
   cpu.mem_write(board+bl['session'],raw[:592]);cpu.mem_write(dsi+dl['session'],raw);cpu.mem_write(dsi+dl['profile_hash'],hashlib.sha256(raw).digest());cpu.mem_write(dsi+dl['replay_result'],payload);m.set(dsi,dl,'result',1,4);m.set(input_address,il,'allocated',1,4);m.set(input_address,il,'request',3,4)
   for name in ('board_live','dsi_live','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext'):m.hooks[image.address(name)]=lambda args:m.ret(1)
   available=[True];m.hooks[image.address('tud_vendor_n_available')]=lambda args:m.ret(int(available[0]))
   def read(args):cpu.mem_write(args[1],bytes(1));available[0]=False;m.ret(1)
   m.hooks[image.address('tud_vendor_n_read')]=read
   def delay(args):m.finished=True;cpu.emu_stop()
   m.hooks[image.address('vTaskDelay')]=delay
   m.call('board_usb0_task',0);count=int.from_bytes(cpu.mem_read(image.address('dsi_response_bytes','/apps/dsi-scanout/usb0.c'),4),'little');assert count==384
   frame=bytes(cpu.mem_read(image.address('dsi_response','/apps/dsi-scanout/usb0.c'),384));result=decode.decode(frame,raw);assert result['sequence']==sequence and result['slot']==(0,1,0)[sequence-1]
   (a.output/(mode+f'-frame{sequence}.bin')).write_bytes(frame);frames.append(frame)
   assert int.from_bytes(cpu.mem_read(input_address+il['request'],4),'little')==3
   # Actual copied transmission completion grants the next request credit.
   m.hooks[image.address('tud_vendor_n_write')]=lambda args:m.ret(args[2]);m.hooks[image.address('tud_vendor_n_write_flush')]=lambda args:m.ret(0)
   m.call('board_usb0_task',0);m.call('tud_vendor_tx_cb',0,384);assert int.from_bytes(cpu.mem_read(input_address+il['request'],4),'little')==0
  refused=[]
  for label,offset in [('sequence',144),('slot',152),('authority',176),('rcq-time',184),('frame-hash',200),('bank-extent',280),('frame-end-flag',304),('reserved',312)]:
   bad=bytearray(frames[0]);bad[offset]^=1
   if label=='rcq-time':struct.pack_into('<Q',bad,184,0)
   bad[64:96]=hashlib.sha256(bad[128:]).digest()
   try:decode.decode(bad,raw)
   except ValueError:refused.append(label)
   else:
    # Sequence has no intrinsic expected value; actual capture binds it to
    # its outstanding request, rather than rejecting valid later sequences.
    assert label=='sequence'
  uploads=[(a.application/mode/f'upload{slot}.bin').read_bytes() for slot in range(2)];written=[];selection=[0];record={};directory=a.output/mode
  if a.resume and (directory/'record.json').exists():
   cases[mode]=dict(copied_presentations=3,refused=refused,reused_completed_capture=True);continue
  directory.mkdir()
  class Device:
   def write(self,endpoint,data,timeout):assert endpoint==2 and timeout>0;written.append(bytes(data));return len(data)
   def read(self,endpoint,count,timeout):assert endpoint==0x81 and count==384;frame=frames[selection[0]];selection[0]+=1;return frame
  decoded=host.capture_device(Device(),lambda:None,time.monotonic()+30,raw,directory,record,lambda:None,frames=[data[96:] for data in uploads]);assert record['dsi_result']=='observed-success' and len(record['presentations'])==3
  expected=uploads[0]+uploads[1]+b''.join((a.application/mode/f'present{sequence}.bin').read_bytes()+bytes(1) for sequence in range(1,4));assert b''.join(written)==expected
  for sequence in range(1,4):assert (directory/f'present-{sequence:03}/output.bin').read_bytes()==frames[sequence-1][128:]
  (directory/'record.json').write_text(json.dumps(record,indent=2)+'\n');cases[mode]=dict(copied_presentations=3,refused=refused,transport_bytes=len(expected))
 # A valid later replay result must not satisfy the current host request.
 raw=(a.application/'720p/session.bin').read_bytes();wrong=(a.output/'720p-frame2.bin').read_bytes();directory=a.output/'wrong-sequence';directory.mkdir();record={}
 try:host.capture(lambda *args:wrong,raw,directory,record,lambda:None,time.monotonic()+10,expected=(1,0))
 except ValueError:assert record['dsi_result']=='unobserved' and 'copied_dsi_output' not in record and not (directory/'output.bin').exists()
 else:raise AssertionError('Wrong sequence became observed success')
 # Exercise actual native bounded submit code; only existing FreeRTOS critical
 # entry/exit primitives are boundaries. No startup/CPU/GIC implementation runs.
 submit=[]
 for busy in (0,1):
  m=old.Machine(image);cpu=m.cpu;cpu.mem_map(0x05000000,0x10000);cpu.mem_write(0x05008144,struct.pack('<I',busy*16));cpu.mem_write(0x05008150,struct.pack('<I',0x240));critical=[False];access=[]
  def enter(args):assert not critical[0];critical[0]=True;m.ret(0)
  def leave(args):assert critical[0];critical[0]=False;m.ret(0)
  m.hooks[image.address('vPortEnterCritical')]=enter;m.hooks[image.address('vPortExitCritical')]=leave
  def memory(uc,kind,address,size,value,unused):
   if 0x05000000<=address<0x05010000:assert critical[0] and size==4;access.append((kind,address,value))
  cpu.hook_add(UC_HOOK_MEM_READ,memory);cpu.hook_add(UC_HOOK_MEM_WRITE,memory)
  assert m.call('a733_display_native_rcq_submit')==busy and not critical[0]
  assert int.from_bytes(cpu.mem_read(0x05008150,4),'little')==(0x241 if busy else 0x240)
  assert len(access)==(3 if busy else 1);submit.append(dict(busy=busy,access=access))
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),cases=cases,actual_native_submit=submit,scope='Actual linked USB serializer and copied completion-credit release; real Python upload/present producer and durable host capture. Actual native bounded RCQ submit instructions with critical primitive boundaries modeled. No CPU startup, GIC, physical DMA, panel or retirement qualification.'),indent=2)+'\n')
if __name__=='__main__':main()
