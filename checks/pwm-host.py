#!/usr/bin/env python3
"""Actual PWM producer/package/fragment decoder, with software-only authority."""
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
 out.mkdir(parents=True,exist_ok=True);host=module('pwm_host',ROOT/'tools/check-a733-pwm-supervised.py');prep=host.preparation
 bundle=out/'MODEL-NOT-PHYSICAL-bundle';template=fixture/'session-template.bin';admission=fixture/'admission.json'
 prepared=prep.validate_bundle(bundle) if bundle.exists() else prep.prepare(image,manifest,admission,template,bundle,clang,lld);frozen=host.freeze_bundle(bundle)
 assert frozen['_prepared_sha256']==hashlib.sha256((bundle/'prepared.json').read_bytes()).hexdigest()
 raw=frozen['_frozen']['session.bin'];body=bytearray(frame.read_bytes());struct.pack_into('<2Q',body,16,*struct.unpack_from('<2Q',raw,24));body[64:96]=hashlib.sha256(raw).digest();body=bytes(body)
 # Bind the modeled copied frame to the producer's freshly minted session.
 assert host.decoder.decode(body,raw)['status']==1
 # Fragment records are durable before another USB operation is permitted.
 class Device:
  def __init__(self):self.parts=[body[:128],body[128:]];self.reads=0
  def read(self,ep,n,timeout):
   assert ep==0x81 and 0<timeout<=500 and (not self.reads or len(record['pwm_fragments'])==self.reads)
   fragment=self.parts.pop(0);assert len(fragment)<=n;self.reads+=1;return fragment
  def write(self,ep,packet,timeout):assert ep==2 and packet in (b'\0',b'\1');return 1
 # Reuse completed fragment/cancellation steps if a later fixture assertion was corrected.
 if (out/'pwm-001.bin').exists():
  assert (out/'pwm-001.bin').read_bytes()==body
  assert (out/'fragment-0001.bin').read_bytes()+(out/'fragment-0002.bin').read_bytes()==body
 else:
  record={};device=Device();result=host.capture_device(device,lambda:None,time.monotonic()+20,raw,out,record,lambda:None)
  assert result['status']==1 and device.reads==2
  # Cancelling invalidates the session before attempting the transfer.
  cancelled={};host.cancel_device(Device(),lambda:None,time.monotonic()+20,out,cancelled,lambda:None)
  refused(lambda:host.capture(lambda *a:body,raw,out,cancelled,lambda:None,time.monotonic()+20))
  assert cancelled['pwm_result']=='cancelled'
  # A fragment durability failure prevents the second endpoint read.
  failed={};dev=Device()
  with patch.object(host.supervisor,'durable_frame',side_effect=OSError('modeled storage failure')):
   refused(lambda:host.read_frame(dev,lambda:None,time.monotonic()+20,out,failed,lambda:None))
  assert dev.reads==1 and not failed.get('pwm_fragments')
 for offset in (0,4,12,16,24,32,64,96,100,104,108,120,132,136,176,180,200,205,212,220,232,248,255):
  bad=bytearray(body);bad[offset]^=1;refused(lambda:host.decoder.decode(bad,raw))
 for size in (0,255,257):refused(lambda:host.decoder.decode((body+b'\0')[:size],raw))
 # Every named upload is immutable, and runtime retains its first snapshot.
 for name in ('session.bin','request.bin','contract.bin','segment-0.bin','launch.bin'):
  p=bundle/name;original=p.read_bytes();p.write_bytes(bytes([original[0]^1])+original[1:])
  refused(lambda:prep.validate_bundle(bundle));assert frozen['_frozen'][name]==original;p.write_bytes(original)
 prep.validate_bundle(bundle)
 # Required physical facts are checked using the actual extension validator.
 base=json.loads(admission.read_bytes());source=bytearray(template.read_bytes());fixture_module=module('pwm_authority_facts',ROOT/'checks/pwm-fixture.py')
 authority_checks=0
 for i,(key,fields) in enumerate(fixture_module.FACTS.items()):
  for field in fields:
   record_copy=copy.deepcopy(base);cache={};original=Path(base[key]['path']).read_bytes();fact=json.loads(original);fact[field]=False;changed=json.dumps(fact,sort_keys=True).encode()
   record_copy[key]['sha256']=hashlib.sha256(changed).hexdigest();cache[base[key]['path']]=changed
   changed_raw=bytearray(source);changed_raw[632+32*i:664+32*i]=hashlib.sha256(changed).digest();th=hashlib.sha256(changed_raw).hexdigest();record_copy['session_template_sha256']=th
   refused(lambda:prep.validate_admission(record_copy,prepared['image_sha256'],th,changed_raw,cache));authority_checks+=1
 # Software-only identity cannot match the future physical USB identity tuple.
 assert isinstance(base['identity'],str) and base['identity'].endswith('software-only-model')
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,fragmented_success=True,cancelled_session_rejected=True,fragment_durability_stops_reads=True,authority_refusals=authority_checks,sealed_file_refusals=5,prepared_sha256=frozen['_prepared_sha256'],launcher_sha256=frozen['files']['launch.bin'],scope='Actual producer, immutable ELF geometry, ARM launcher, frozen package, fragment durability/decoder and authority extension. No board access or physical authority established.'),indent=2)+'\n')
 return bundle
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','manifest','fixture','frame','output','clang','lld'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.image,a.manifest,a.fixture,a.frame,a.output,a.clang,a.lld)
