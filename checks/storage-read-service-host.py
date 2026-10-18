#!/usr/bin/env python3
"""New reusable read-range capture, package and stale-frame refusal cases."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def refused(operation):
 try:operation()
 except (ValueError,KeyError,OSError,RuntimeError):return
 raise AssertionError('Expected refusal')
def run(image,manifest,fixture,native,out,clang,lld):
 out.mkdir(parents=True,exist_ok=False);host=module('storage_read_service_host',ROOT/'tools/check-a733-storage-read-supervised.py');prep=host.preparation
 bundle=out/'MODEL-NOT-PHYSICAL-bundle';prep.prepare(image,manifest,fixture/'admission.json',fixture/'session-template.bin',bundle,clang,lld)
 frozen=host.freeze_bundle(bundle);raw=frozen['_frozen']['session.bin']
 frames=[]
 for name in ('idle-frame.bin','pending-frame.bin','native-frame.bin','pending-last-frame.bin','last-frame.bin'):
  body=bytearray((native/name).read_bytes());struct.pack_into('<2Q',body,16,*struct.unpack_from('<2Q',raw,24));frames.append(bytes(body))
 for body in frames:host.decoder.decode(body,raw)
 reads=[(19,2),(33554424,8)];packets=[b'\0',host.decoder.read_request(19,2,1),b'\0',host.decoder.read_request(33554424,8,2),b'\0']
 class Device:
  def __init__(self):self.frames=list(frames);self.expected=list(packets);self.pending=b'';self.reads=0;self.writes=0
  def write(self,ep,packet,timeout):
   assert ep==2 and not self.pending and packet==self.expected.pop(0) and 0<timeout<=500
   self.pending=self.frames.pop(0);self.writes+=1;return len(packet)
  def read(self,ep,n,timeout):
   assert ep==0x81 and self.pending and 0<timeout<=500
   assert self.reads==len(record.get('storage_read_fragments',[]))
   fragment=self.pending[:min(n,73)];self.pending=self.pending[len(fragment):];self.reads+=1;return fragment
 record={};device=Device();result=host.capture_device(device,lambda:None,time.monotonic()+20,raw,out,record,lambda:None,reads)
 assert result['status']==1 and len(record['storage_read_reads'])==2 and device.writes==5
 assert (out/'output-001.bin').read_bytes()==frames[2][288:] and (out/'output-002.bin').read_bytes()==frames[4][288:]
 # Strong request binding uses actual completed native frame bytes.
 value=host.decoder.decode(frames[2],raw)
 for field in ('ticket','request_id','first_sector','blocks','client'):
  refused(lambda field=field:host.decoder.decode(frames[2],raw,{field:value[field]+1}))
 for index in (240,248,256,264,268,280,288,len(frames[2])-1):
  bad=bytearray(frames[2]);bad[index]^=1
  expected={k:value[k] for k in ('ticket','request_id','first_sector','blocks','client')}
  refused(lambda:host.decoder.decode(bad,raw,expected))
 for first,blocks,rid in [(-1,1,1),(0,0,1),(0,9,1),(2**32-1,2,1),(0,1,0)]:refused(lambda:host.decoder.read_request(first,blocks,rid))
 # Failed persistence must stop before the next endpoint read.
 record={};device=Device();device.write(2,b'\0',500)
 with patch.object(host.supervisor,'durable_frame',side_effect=OSError('modeled durable-write refusal')):
  refused(lambda:host.read_frame(device,lambda:None,time.monotonic()+20,out,record,lambda:None))
 assert device.reads==1 and not record.get('storage_read_fragments')
 # Newly required lifetime premises are bound to the session evidence hash.
 admission=json.loads((fixture/'admission.json').read_text());template=(fixture/'session-template.bin').read_bytes()
 for key,offset,field in [('topology',728,'no_card_substitution'),('supervision',760,'no_buffer_reuse_after_uncertainty')]:
  import copy
  changed=copy.deepcopy(admission);path=changed[key]['path'];fact=json.loads(Path(path).read_bytes());fact[field]=False
  data=json.dumps(fact,sort_keys=True).encode();changed[key]['sha256']=hashlib.sha256(data).hexdigest();session=bytearray(template);session[offset:offset+32]=hashlib.sha256(data).digest()
  digest=hashlib.sha256(session).hexdigest();changed['session_template_sha256']=digest
  refused(lambda:prep.validate_admission(changed,frozen['image_sha256'],digest,session,{path:data}))
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,actual_frames=5,read_ranges=reads,client_ticket_range_binding=True,fragment_durability=True,new_lifetime_premises=True,prepared_sha256=frozen['_prepared_sha256'],launcher_sha256=frozen['files']['launch.bin'],scope='New copied multi-range native-frame capture and immutable version2 producer/package; no device access and no old cases replayed.'),indent=2)+'\n')
 return bundle
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','manifest','fixture','native','output','clang','lld'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.image,a.manifest,a.fixture,a.native,a.output,a.clang,a.lld)
