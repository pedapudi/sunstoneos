#!/usr/bin/env python3
"""Run changed mode2 serializer/ACK/security cases in the actual native ELF."""
import argparse,hashlib,importlib.util,json,shutil,struct,traceback
from pathlib import Path
from unicorn import UC_HOOK_MEM_READ
ROOT=Path(__file__).resolve().parents[1]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
p=argparse.ArgumentParser(description=__doc__)
for n in ('image','recognizer','output'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--msbc',action='store_true',help='Select mode3 with codec-bound version2 responses')
p.add_argument('--host-case',action='append',choices=['host-two-results','host-backlog','host-journal-before-ack','host-wrong-session','host-sample-gap','host-missing-disconnect','host-packet-read','host-wrong-codec','host-fallback'])
p.add_argument('--case',action='append',choices=['two-results','early-ack','stale-ack','revoked-copy','terminal-count-order','finished-status-order'])
a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=False)
base=module('bluetooth_usb_base_check',ROOT/'checks/bluetooth-usb-native.py');base.ELF_HASH=hashlib.sha256(a.image.read_bytes()).hexdigest()
shutil.copy2(a.image,out/'bluetooth.elf')
original=Path.home()/'.cache/sunstoneos/evidence/bluetooth-usb-native-20261018'
profile=bytearray((original/'profile.bin').read_bytes());assert len(profile)==832;struct.pack_into('<I',profile,624,3 if a.msbc else 2);struct.pack_into('<I',profile,720,24000)
session=bytearray((original/'admitted-session.bin').read_bytes());session[320:352]=hashlib.sha256(profile).digest()
(out/'profile.bin').write_bytes(profile);(out/'admitted-session.bin').write_bytes(session)
shutil.copy2(a.recognizer/'segment-1.txt',out/'transcript.bin');(out/'snapshot.bin').write_bytes((a.recognizer/'segment-1.snapshot.bin').read_bytes()[:144])
observe=module('repeated_usb_observe',ROOT/'apps/bluetooth/observe.py')
def packet(op,ticket=0,offset=0,count=0):return struct.pack('<BBHIQ',0x80,op,count,offset,ticket)
def adopt(m,n):
 source=a.recognizer
 if m.variant=='host-fallback':source=Path.home()/'.cache/sunstoneos/evidence/bluetooth-repeated-recognition-20261019/recognizer'
 data=(source/f'segment-{n}.txt').read_bytes();snapshot=bytearray((source/f'segment-{n}.snapshot.bin').read_bytes());assert len(snapshot)==176
 if m.variant=='host-fallback':struct.pack_into('<I',snapshot,44,1)
 assert m.word(m.variables['terminal']['address'])==0
 m.cpu.mem_write(m.service+m.sf['log'],data);m.cpu.mem_write(m.service+m.sf['segment'],bytes(snapshot));m.write(m.service+m.sf['used'],len(data),4)
 m.write(m.voice+m.vf['segment_tail'],n,8);m.write(m.variables['terminal']['address'],1);return data

def create(name):
 m=base.Machine(out,name);m.voice=m.variables['voice']['address'];m.vf=m.variables['voice']['members']
 m.write(m.voice+m.vf['segment_app'],m.s['bluetooth'],8);m.write(m.voice+m.vf['app'],m.s['bluetooth'],8)
 m.write(m.voice+m.vf['segment_head'],2,8);m.write(m.voice+m.vf['segment_tail'],1,8)
 m.enumerate();return m
results={}
for case in ([] if a.host_case else a.case or ['two-results','early-ack','stale-ack','revoked-copy','terminal-count-order','finished-status-order']):
 m=create(case)
 try:
  if case=='terminal-count-order':
   m.write(m.voice+m.vf['segment_head'],1,8);m.write(m.voice+m.vf['segment_tail'],1,8)
   seen=[]
   def ordering(cpu,access,address,size,value,unused):
    if address in [m.voice+m.vf[v] for v in ('segment_head','segment_tail','segment_disconnected')]:
     seen.append(address)
     if len(seen)==1:assert address==m.voice+m.vf['segment_disconnected']
     if address==m.voice+m.vf['segment_disconnected']:
      m.write(m.voice+m.vf['segment_disconnected'],1,4);m.write(m.voice+m.vf['segment_head'],2,8)
   m.cpu.hook_add(UC_HOOK_MEM_READ,ordering);ptr=0x50df0000;assert m.call('bluetooth_headset_segment_status',m.s['bluetooth'],ptr)==1
   assert struct.unpack('<QQII',m.cpu.mem_read(ptr,24))==(2,1,0,1);results[case]={'passed':True,'actual_first_read':'disconnected'};continue
  if case=='finished-status-order':
   seen=[]
   def finish_order(cpu,access,address,size,value,unused):
    if address in (m.variables['finished']['address'],m.voice+m.vf['segment_disconnected']):
     seen.append(address)
     if len(seen)==1:assert address==m.variables['finished']['address']
     if address==m.variables['finished']['address']:
      m.write(m.voice+m.vf['segment_disconnected'],1);m.write(m.variables['finished']['address'],1)
   m.cpu.hook_add(UC_HOOK_MEM_READ,finish_order);m.request(packet(0));frame=m.drain(272)
   value,_=observe.decode_repeated(frame,session,profile);assert value['finished']==1 and value['disconnected']==1
   results[case]={'passed':True,'actual_first_read':'finished'};continue
  data=adopt(m,1)
  if case=='revoked-copy':
   poisoned=[]
   def revoke(cpu,access,address,size,value,unused):
    if address==m.voice+m.vf['segment_disconnected'] and not poisoned:
     poisoned.append(True);m.field('bluetooth_application','bluetooth','failed',1)
   m.cpu.hook_add(UC_HOOK_MEM_READ,revoke);m.request(packet(1,1,0,min(1024,len(data))));frame=m.drain(272)
   value,body=observe.decode_repeated(frame,session,profile);assert poisoned and value['state']==3 and not value['ticket'] and not body
   assert m.word(m.variables['transcript_delivered']['address'],8)==0 and m.word(m.variables['terminal']['address'])==1
  else:
   if case=='early-ack':
    m.request(packet(1,1,0,100));m.drain(372)
    assert m.word(m.variables['terminal']['address'])==1;m.complete(2,packet(2,1));assert not m.tick()
    assert m.word(m.variables['terminal']['address'])==1
   else:
    for n in (1,2) if case=='two-results' else (1,):
     if n==2:data=adopt(m,2)
     copied=bytearray()
     for offset in range(0,len(data),1024):
      count=min(1024,len(data)-offset);m.request(packet(1,n,offset,count))
      assert m.word(m.variables['terminal']['address'])==1
      frame=m.drain(272+count);value,body=observe.decode_repeated(frame,session,profile,offset)
      assert value['ticket']==n and body==data[offset:offset+count];copied.extend(body);(out/f'{case}-{n}-{offset}.bin').write_bytes(frame)
     assert bytes(copied)==data;m.request(packet(2,n));ack=m.drain(272);value,_=observe.decode_repeated(ack,session,profile)
     assert value['ticket']==n and m.word(m.variables['terminal']['address'])==0
    if case=='stale-ack':m.complete(2,packet(2,1));assert not m.tick()
    else:
     m.write(m.voice+m.vf['segment_disconnected'],1);m.write(m.variables['finished']['address'],1)
     m.request(packet(0));frame=m.drain(272);value,_=observe.decode_repeated(frame,session,profile)
     assert value['state']==2 and value['published']==value['consumed']==2 and not value['ticket'];(out/'terminal.bin').write_bytes(frame)
  results[case]={'passed':True,**m.observation()}
 except BaseException:
  (out/(case+'-failed.json')).write_text(json.dumps(m.observation(),indent=2)+'\n');(out/(case+'-failure.log')).write_text(traceback.format_exc());raise
 (out/'results.json').write_text(json.dumps({'image_sha256':base.ELF_HASH,'physical_access':False,'cases':results,'scope':'Actual new native recognition copy/status/ACK, Bluetooth USB task/serializer and TinyUSB; scheduler, DCD, provider-ready state and adopted actual recognizer outputs modeled.'},indent=2)+'\n')
(out/'results.json').write_text(json.dumps({'image_sha256':base.ELF_HASH,'physical_access':False,'cases':results,'scope':'Actual new native recognition copy/status/ACK, Bluetooth USB task/serializer and TinyUSB; scheduler, DCD, provider-ready state and adopted actual recognizer outputs modeled.'},indent=2)+'\n')
print('PASS selected repeated Bluetooth native USB cases')

if a.host_case:
 host=module('repeated_bluetooth_host',ROOT/'tools/check-a733-bluetooth-supervised.py')
 for name in a.host_case:
  m=create(name);adopt(m,1);requests=[];record={};dest=out/name;dest.mkdir();cancelled=False
  class Clock:
   value=100.
   def now(self):self.value+=.001;return self.value
   def pause(self,n):self.value+=max(.005,n)
  clock=Clock()
  def exchange(data,timeout):
   global cancelled
   mark,op,count,offset,ticket=struct.unpack('<BBHIQ',data);assert mark==0x80;requests.append(op)
   m.request(data);frame=bytearray(m.drain(272+(count if op==1 else 0)))
   if op==2 and ticket==1:adopt(m,2)
   if op==3:
    cancelled=True
    if name!='host-missing-disconnect':
     m.write(m.voice+m.vf['segment_disconnected'],1);m.write(m.voice+m.vf['segment_flags'],4 if name=='host-backlog' else 1)
   if op==3 and 2 in requests and name!='host-missing-disconnect':m.write(m.variables['finished']['address'],1)
   if name=='host-wrong-session':frame[32]^=1
   if name=='host-sample-gap':frame[104]^=1
   if name=='host-wrong-codec' and ticket:frame[124]=1
   return bytes(frame)
  if name=='host-backlog':m.write(m.voice+m.vf['segment_flags'],4)
  def save():
   if name=='host-journal-before-ack' and record.get('transcripts'):raise OSError('modeled durable transcript journal failure')
  if name=='host-packet-read':
   import time
   class Device:
    pending=b''
    reads=[]
    def write(self,endpoint,data,timeout):
     self.pending=exchange(data,timeout);return len(data)
    def read(self,endpoint,size,timeout):
     self.reads.append(size);part=self.pending[:512];assert size>=len(part)
     self.pending=self.pending[len(part):];return part
   device=Device();saved_capture=host.capture
   def receive(read,*args,**kwargs):return read(packet(1,1,0,1024),1000)
   host.capture=receive
   try:frame=host.capture_device(device,lambda:None,time.monotonic()+10,session,profile,dest,record,save)
   finally:host.capture=saved_capture
   value,body=observe.decode_repeated(frame,session,profile);assert len(body)==1024 and all(n>=512 for n in device.reads)
   results[name]={'passed':True,'requested_read_sizes':device.reads,'first_packet_bytes':512}
   (out/'host-results.json').write_text(json.dumps({'image_sha256':base.ELF_HASH,'physical_access':False,'cases':results},indent=2)+'\n');continue
  try:host.capture_repeated(exchange,session,profile,dest,record,save,100.4 if name=='host-missing-disconnect' else 130,2,clock.now,clock.pause)
  except (ValueError,RuntimeError,OSError):assert name not in ('host-two-results','host-backlog'),name
  else:assert record['bluetooth_result']==('observed-failure' if name in ('host-backlog','host-fallback') else 'observed-success')
  if name=='host-fallback':assert not record['repeated_recognition']['wideband'] and record['repeated_recognition']['wideband_fallback']
  if name=='host-journal-before-ack':assert 2 not in requests and m.word(m.variables['terminal']['address'])==1
  if name=='host-missing-disconnect':assert record['bluetooth_result']!='observed-success' and not m.word(m.voice+m.vf['segment_disconnected'])
  results[name]={'passed':True,'operations':requests,'record':record,**m.observation()}
  (out/'host-results.json').write_text(json.dumps({'image_sha256':base.ELF_HASH,'physical_access':False,'cases':results,'scope':'Host driven by actual new native USB/TinyUSB producer; adopted exact two real recognizer outputs and modeled input/disconnect progress.'},indent=2)+'\n')
 print('PASS selected host transcript collection and validation cases')
