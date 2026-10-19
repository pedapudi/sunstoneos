#!/usr/bin/env python3
"""Validate actual native runtime frames and finite uploads without USB access."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
p=argparse.ArgumentParser(description=__doc__)
for n in ('fixture','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
tool=module('host',ROOT/'tools/check-a733-crypto-supervised.py')
request=(a.fixture/'operation.bin').read_bytes();session=(a.fixture/'runtime-session.bin').read_bytes();frame=(a.fixture/'runtime-frame.bin').read_bytes()
nominal=tool.decoder.decode(frame,session,request=request);assert nominal['request_id']==0xfedcba9876543210 and not nominal['oracle_checked']
checks={}
def refused(name,fn):
 try:fn()
 except (ValueError,RuntimeError):checks[name]=True;return
 raise AssertionError('Accepted invalid '+name)
for name,at in (('id64',39),('operation',40),('request-digest',48),('session-digest',80),('output-digest',112),('copied-output',144)):
 bad=bytearray(frame);bad[at]^=1;refused(name,lambda:tool.decoder.decode(bytes(bad),session,request=request))
refused('truncated',lambda:tool.decoder.decode(frame[:-1],session,request=request))
for field,value in ((16,0),(28,1),(24,63),(12,4)):
 bad=bytearray(request);struct.pack_into('<Q' if field==16 else '<I',bad,field,value)
 changed=bytearray(session);changed[760:792]=hashlib.sha256(bad).digest()
 refused('request-shape-'+str(field),lambda:tool.preparation.operation.decode(bytes(bad),bytes(changed)))
# Optional oracle is an explicit separate qualification constraint.
qualified=bytearray(session);qualified[792:824]=frame[112:144]
bound=bytearray(frame);bound[80:112]=hashlib.sha256(qualified).digest()
assert tool.decoder.decode(bytes(bound),bytes(qualified),request=request)['oracle_checked']
qualified[792]^=1;bound[80:112]=hashlib.sha256(qualified).digest()
refused('oracle-mismatch',lambda:tool.decoder.decode(bytes(bound),bytes(qualified),request=request))
for variant in ('copied-success','interior-zlp','empty-read-budget','partial-read-fault','write-refusal','late-read','journal-failure'):
 dest=a.output/variant;dest.mkdir();record={};parts=[0];empty_sent=[False];writes=[];deadline=time.monotonic()+30
 class Device:
  def write(self,ep,data,timeout):
   assert ep==2 and data==b'\1'+struct.pack('<I',len(request))+request;writes.append(data)
   return len(data)-(variant=='write-refusal')
  def read(self,ep,count,timeout):
   assert ep==0x81
   if variant=='partial-read-fault' and parts[0]:raise OSError('modeled interrupted read')
   if variant=='late-read':raise tool.supervisor.CaptureWindowExpired('modeled expired read')
   if variant=='empty-read-budget':return b''
   if variant=='interior-zlp' and parts[0]>100 and not empty_sent[0]:empty_sent[0]=True;return b''
   pos=parts[0];part=frame[pos:pos+min(17,count)];parts[0]+=len(part);return part
 def save():
  if variant=='journal-failure' and record.get('crypto_result')=='observed-success':raise OSError('modeled durable record failure')
 try:tool.capture_device(Device(),lambda:None,deadline,session,dest,record,save,reference=None,request=request)
 except (ValueError,RuntimeError,OSError):assert variant not in ('copied-success','interior-zlp')
 else:assert variant in ('copied-success','interior-zlp') and (dest/'output.bin').read_bytes()==frame[144:]
 if variant not in ('copied-success','interior-zlp'):assert record.get('crypto_result')!='observed-success'
 assert len(writes)==1;checks[variant]=True
# The changed common host receive loop retains diagnostic v1 compatibility.
legacy=Path.home()/'.cache/sunstoneos/evidence/crypto-single-core-20261018/host/MODEL-NOT-DEPLOYABLE-bundle/session.bin'
old_session=legacy.read_bytes();old=Path.home()/'.cache/sunstoneos/evidence/crypto-native-20261018/native/native-frame.bin'
legacy_frame=bytearray(old.read_bytes());legacy_frame[16:32]=old_session[24:40]
dest=a.output/'diagnostic-host-regression';dest.mkdir();record={}
class Legacy:
 def write(self,ep,data,timeout):assert data==b'\0';return 1
 def read(self,ep,count,timeout):assert count==288;return bytes(legacy_frame)
tool.capture_device(Legacy(),lambda:None,time.monotonic()+30,old_session,dest,record,lambda:None)
assert record['crypto_result']=='observed-success';checks['diagnostic-host-regression']=True
(a.output/'result.json').write_text(json.dumps({'passed':True,'physical':False,'producer_frame_sha256':hashlib.sha256(frame).hexdigest(),'cases':checks},indent=2)+'\n')
print('PASS runtime crypto host: exact native producer, 64-bit identity, optional oracle, fragmented copy, late/error retention and diagnostic compatibility')
