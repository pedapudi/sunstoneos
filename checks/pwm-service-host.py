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
 out.mkdir(parents=True,exist_ok=False);host=module('pwm_host',ROOT/'tools/check-a733-pwm-supervised.py');prep=host.preparation
 bundle=out/'MODEL-NOT-PHYSICAL-bundle';template=fixture/'session-template.bin';admission=fixture/'admission.json'
 prepared=prep.prepare(image,manifest,admission,template,bundle,clang,lld);frozen=host.freeze_bundle(bundle)
 assert frozen['_prepared_sha256']==hashlib.sha256((bundle/'prepared.json').read_bytes()).hexdigest()
 raw=frozen['_frozen']['session.bin'];body=bytearray(frame.read_bytes());struct.pack_into('<2Q',body,16,*struct.unpack_from('<2Q',raw,24));body[64:96]=hashlib.sha256(raw).digest();body=bytes(body)
 # Bind the modeled copied frame to the producer's freshly minted session.
 assert host.decoder.decode(body,raw)['status']==1
 # Fragment records are durable before another USB operation is permitted.
 class Device:
  def __init__(self):self.parts=[body[:256],body[256:]];self.reads=0
  def read(self,ep,n,timeout):
   assert ep==0x81 and 0<timeout<=500 and (not self.reads or len(record['pwm_fragments'])==self.reads)
   fragment=self.parts.pop(0);assert len(fragment)<=n;self.reads+=1;return fragment
  def write(self,ep,packet,timeout):assert ep==2 and packet in (b'\0',b'\1');return 1
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
 for offset in (0,4,12,16,24,32,64,96,100,132,136,176,180,184,188,192,200,205,212,216,220,232,236,248,255,272,276,300,304,352,511):
  bad=bytearray(body);bad[offset]^=1;refused(lambda:host.decoder.decode(bad,raw))
 for size in (0,255,511,513):refused(lambda:host.decoder.decode((body+b'\0')[:size],raw))
 # A returned native failure is terminal: one command, one query, two reads.
 failure=bytearray((frame.parent/'command-deadline-frame.bin').read_bytes());struct.pack_into('<2Q',failure,16,*struct.unpack_from('<2Q',raw,24));failure[64:96]=hashlib.sha256(raw).digest();failure=bytes(failure)
 assert host.decoder.decode(failure,raw)['status']==2
 class FailedDevice(Device):
  def __init__(self):self.parts=[failure[:256],failure[256:]];self.reads=0;self.operations=[]
  def read(self,ep,n,timeout):self.operations.append('read');return super().read(ep,n,timeout)
  def write(self,ep,packet,timeout):
   self.operations.append('command' if packet.startswith(b'\2') else 'query');return len(packet)
 failed_dir=out/'returned-native-failure';failed_dir.mkdir();failed_record={};failed_device=FailedDevice()
 # Failure-fragment durability uses this record, distinct from the successful frame.
 record=failed_record
 refused(lambda:host.command_device(failed_device,lambda:None,time.monotonic()+20,raw,failed_dir,failed_record,lambda:None,1,1,300))
 assert failed_device.operations==['command','query','read','read'] and failed_record['durable_result']['status']==2
 initial_dir=out/'initial-native-failure';initial_dir.mkdir();initial_record={};record=initial_record;initial_device=FailedDevice()
 refused(lambda:host.run_program(initial_device,lambda:None,time.monotonic()+20,raw,initial_dir,initial_record,lambda:None,[(1,300)]))
 assert initial_device.operations==['query','read','read'] and initial_record['pwm_command_failure']['status']==2 and initial_record['pwm_result']=='unobserved'
 # No command replay when a valid acknowledged record is received.
 class AckDevice(Device):
  def write(self,ep,packet,timeout):return len(packet)
 ack_dir=out/'acknowledgement';ack_dir.mkdir();ack_record={};record=ack_record;ack_device=AckDevice()
 sequence=host.decoder.decode(body,raw)['command']['sequence']
 result=host.command_device(ack_device,lambda:None,time.monotonic()+20,raw,ack_dir,ack_record,lambda:None,sequence,3,0)
 assert result['command']['ack_state']==2 and len(ack_record['pwm_commands'])==1
 # Every named upload is immutable, and runtime retains its first snapshot.
 for name in ('session.bin','request.bin','contract.bin','segment-0.bin','launch.bin'):
  p=bundle/name;original=p.read_bytes();p.write_bytes(bytes([original[0]^1])+original[1:])
  refused(lambda:prep.validate_bundle(bundle));assert frozen['_frozen'][name]==original;p.write_bytes(original)
 prep.validate_bundle(bundle)
 # Required physical facts are checked using the actual extension validator.
 base=json.loads(admission.read_bytes());source=bytearray(template.read_bytes());fixture_module=module('pwm_authority_facts',ROOT/'checks/pwm-service-fixture.py')
 authority_checks=0
 for key,offset,fields in [(k,632+32*i,v) for i,(k,v) in enumerate(fixture_module.FACTS.items())]+[('led',832,fixture_module.LED_FACTS)]:
  for field in fields:
   record_copy=copy.deepcopy(base);cache={};original=Path(base[key]['path']).read_bytes();fact=json.loads(original);fact[field]=False;changed=json.dumps(fact,sort_keys=True).encode()
   record_copy[key]['sha256']=hashlib.sha256(changed).hexdigest();cache[base[key]['path']]=changed
   changed_raw=bytearray(source);changed_raw[offset:offset+32]=hashlib.sha256(changed).digest();th=hashlib.sha256(changed_raw).hexdigest();record_copy['session_template_sha256']=th
   refused(lambda:prep.validate_admission(record_copy,prepared['image_sha256'],th,changed_raw,cache));authority_checks+=1
 # Software-only identity cannot match the future physical USB identity tuple.
 assert isinstance(base['identity'],str) and base['identity'].endswith('software-only-model')
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,fragmented_success=True,returned_failure_stops_operations=True,command_acknowledgement=True,cancelled_session_rejected=True,fragment_durability_stops_reads=True,authority_refusals=authority_checks,sealed_file_refusals=5,prepared_sha256=frozen['_prepared_sha256'],launcher_sha256=frozen['files']['launch.bin'],scope='Actual producer, immutable ELF geometry, ARM launcher, frozen package, fragment durability/decoder and authority extension. No board access or physical authority established.'),indent=2)+'\n')
 return bundle
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','manifest','fixture','frame','output','clang','lld'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.image,a.manifest,a.fixture,a.frame,a.output,a.clang,a.lld)
