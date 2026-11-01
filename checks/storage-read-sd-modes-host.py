#!/usr/bin/env python3
"""Production SD mode package, wire discriminator and copied capture checks."""
import argparse,hashlib,importlib.util,json,struct,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'checks'))
import harness
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def refused(call):
 try:call()
 except (ValueError,RuntimeError):return
 raise AssertionError('Expected mode refusal')
def authority_refusals(prep,fixture):
 original=(fixture/'session-template.bin').read_bytes()
 record={'identity':'software-only-model','port':'no-physical-port','reservation':100}
 assert prep.validate_admission(record,'unused','unused',original)=={}
 for offset,value in ((813,0),(817,15),(856,0x80000000)):
  raw=bytearray(original);struct.pack_into('<I' if offset==856 else '<B',raw,offset,value)
  refused(lambda:prep.validate_admission(record,'unused','unused',raw))
def run(image,manifest,evidence):
 host=module('sd_modes_host',ROOT/'tools/check-a733-storage-read-supervised.py');prep=host.preparation
 out=evidence/'host';out.mkdir(parents=True,exist_ok=False);results={}
 for profile,label in ((1,'default'),(2,'hs')):
  fixture=evidence/('fixture-'+label);bundle=out/f'bundle-{profile}'
  prep.prepare(image,manifest,fixture/'admission.json',fixture/'session-template.bin',bundle,str(harness.CLANG),str(harness.LLD))
  frozen=host.freeze_bundle(bundle);session=frozen['_frozen']['session.bin'];raw_fixture=(fixture/'session-template.bin').read_bytes()
  frames=[]
  for name in ('idle-frame.bin','read-frame.bin','second-frame.bin'):
   frame=bytearray((evidence/'native'/f'profile-{profile}'/name).read_bytes());frame[16:32]=session[24:40];frames.append(bytes(frame))
  value=host.decoder.decode(frames[1],session);assert value['sd_profile']==profile and value['bus_width']==4
  for offset in (4,280,281,282,283,284):
   bad=bytearray(frames[1]);bad[offset]^=1;refused(lambda:host.decoder.decode(bad,session))
  if profile==1:
   bad=bytearray(frames[1]);bad[80]=0x22;refused(lambda:host.decoder.decode(bad,session))
  bad=bytearray(session);struct.pack_into('<I',bad,860,4);refused(lambda:prep.session_check(bad))
  bad=bytearray(session);bad[813]=0;refused(lambda:prep.session_check(bad))
  authority_refusals(prep,fixture)
  expected=[b'\0',host.decoder.read_request(19,2,1),host.decoder.read_request(21,1,2)]
  class Device:
   def __init__(self):self.frames=list(frames);self.pending=b'';self.reads=0
   def write(self,ep,packet,timeout):
    assert ep==2 and not self.pending and packet==expected.pop(0);self.pending=self.frames.pop(0);return len(packet)
   def read(self,ep,n,timeout):
    assert ep==0x81 and self.pending and self.reads==len(record.get('storage_read_fragments',[]))
    part=self.pending[:min(n,37)];self.pending=self.pending[len(part):];self.reads+=1;return part
  dest=out/f'capture-{profile}';dest.mkdir();record={};device=Device()
  result=host.capture_device(device,lambda:None,time.monotonic()+20,session,dest,record,lambda:None,[(19,2),(21,1)])
  assert result['status']==1 and record['storage_read_application_reads']==2 and len(record['storage_read_reads'])==2
  assert (dest/'output-001.bin').read_bytes()==frames[1][288:] and (dest/'output-002.bin').read_bytes()==frames[2][288:]
  results[str(profile)]={'passed':True,'wire_version':3,'card_hz':value['card_hz'],'module_hz':value['module_hz'],'native_reads':2,'copied_usb_reads':2,'durable_fragments':device.reads,'launcher_sha256':frozen['files']['launch.bin']}
 (out/'result.json').write_text(json.dumps({'physical_access':False,'cases':results,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest()},indent=2)+'\n')
 print('SD modes production preparation, explicit wire3/CSD/mode refusals and fragmented durable host capture passed for both profiles.')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--authority-only',action='store_true');a=p.parse_args()
 if a.authority_only:
  host=module('sd_modes_authority_host',ROOT/'tools/check-a733-storage-read-supervised.py')
  for label in ('default','hs'):authority_refusals(host.preparation,a.evidence/('fixture-'+label))
  (a.evidence/'host/authority-result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':6,'scope':'Direct target preparation and native pin-route/voltage limits for default and high-speed sessions.'},indent=2)+'\n')
  print('Six direct SD mode pin and voltage limits passed without process records.')
 else:run(a.image,a.manifest,a.evidence)
