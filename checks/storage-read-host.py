#!/usr/bin/env python3
"""Actual SD0 producer/package/fragment decoder, with software-only authority."""
import argparse,copy,hashlib,importlib.util,json,struct,time
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def refused(call):
 try:call()
 except (ValueError,KeyError,OSError):return
 raise AssertionError('Expected refusal')
def run(image,manifest,fixture,frame,out,clang,lld):
 out.mkdir(parents=True,exist_ok=False);host=module('storage_read_host',ROOT/'tools/check-a733-storage-read-supervised.py');prep=host.preparation
 bundle=out/'MODEL-NOT-PHYSICAL-bundle';template=fixture/'session-template.bin';admission=fixture/'admission.json'
 prepared=prep.prepare(image,manifest,admission,template,bundle,clang,lld);frozen=host.freeze_bundle(bundle)
 assert frozen['_prepared_sha256']==hashlib.sha256((bundle/'prepared.json').read_bytes()).hexdigest()
 raw=frozen['_frozen']['session.bin'];body=bytearray(frame.read_bytes());struct.pack_into('<2Q',body,16,*struct.unpack_from('<2Q',raw,24));body=bytes(body)
 decoded=host.decoder.decode(body,raw);assert decoded['status']==1
 idle=bytearray(frame.with_name('host-idle-frame.bin').read_bytes());idle[16:32]=raw[24:40]
 # Fragment records are durable before another USB operation is permitted.
 class Device:
  def __init__(self):self.parts=[bytes(idle),body[:512],body[512:]];self.reads=0
  def read(self,ep,n,timeout):
   assert ep==0x81 and 0<timeout<=500 and (not self.reads or len(record['storage_read_fragments'])==self.reads)
   fragment=self.parts.pop(0);assert len(fragment)<=n;self.reads+=1;return fragment
  def write(self,ep,packet,timeout):assert ep==2;return len(packet)
 record={};device=Device();result=host.capture_device(device,lambda:None,time.monotonic()+20,raw,out,record,lambda:None,
                                                     [(decoded['first_sector'],decoded['blocks'])])
 assert result['status']==1 and device.reads==3 and (out/'output-001.bin').read_bytes()==body[host.decoder.HEADER:]
 # Cancelling invalidates the session before attempting the transfer.
 cancelled={};host.cancel_device(Device(),lambda:None,time.monotonic()+20,out,cancelled,lambda:None)
 refused(lambda:host.capture(lambda *a:body,raw,out,cancelled,lambda:None,time.monotonic()+20))
 assert cancelled['storage_read_result']=='cancelled'
 # A fragment durability failure prevents the second endpoint read.
 failed={};dev=Device()
 with patch.object(host.supervisor,'durable_frame',side_effect=OSError('modeled storage failure')):
  refused(lambda:host.read_frame(dev,lambda:None,time.monotonic()+20,out,failed,lambda:None))
 assert dev.reads==1 and not failed.get('storage_read_fragments')
 for offset in (0,4,16,24,32,208,host.decoder.HEADER,len(body)-1):
  bad=bytearray(body);bad[offset]^=1;refused(lambda:host.decoder.decode(bad,raw))
 for size in (0,255,len(body)-1,len(body)+1):refused(lambda:host.decoder.decode((body+b'\0')[:size],raw))
 # Every named upload is immutable, and runtime retains its first snapshot.
 for name in ('session.bin','request.bin','contract.bin','segment-0.bin','launch.bin'):
  p=bundle/name;original=p.read_bytes();p.write_bytes(bytes([original[0]^1])+original[1:])
  refused(lambda:prep.validate_bundle(bundle));assert frozen['_frozen'][name]==original;p.write_bytes(original)
 prep.validate_bundle(bundle)
 # Direct target input has no reviewed-fact dependency.
 base=json.loads(admission.read_bytes());source=bytearray(template.read_bytes())
 direct={k:base[k] for k in ('identity','port','reservation')}
 prep.validate_admission(direct,'unused','unused',source)
 authority_checks=0
 for offset,value in ((813,15),(856,0x80000000),(872,0)):
  changed=bytearray(source);struct.pack_into('<I' if offset!=813 else '<B',changed,offset,value)
  refused(lambda:prep.validate_admission(direct,'unused','unused',changed));authority_checks+=1
 # Software-only identity cannot match the future physical USB identity tuple.
 assert isinstance(base['identity'],str) and base['identity'].endswith('software-only-model')
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,fragmented_success=True,cancelled_session_rejected=True,fragment_durability_stops_reads=True,authority_refusals=authority_checks,sealed_file_refusals=5,prepared_sha256=frozen['_prepared_sha256'],launcher_sha256=frozen['files']['launch.bin'],scope='Actual producer, immutable ELF geometry, ARM launcher, transport integrity, fragment durability/decoder and native session limits. No board access or physical authority established.'),indent=2)+'\n')
 return bundle
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','manifest','fixture','frame','output','clang','lld'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.image,a.manifest,a.fixture,a.frame,a.output,a.clang,a.lld)
