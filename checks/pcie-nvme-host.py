#!/usr/bin/env python3
"""Host admission, decoding and capture cases for the PCIe NVMe application.

The nominal frame comes from the linked image (checks/pcie-nvme-native.py).
USB transport is a modeled exchange; nothing here opens a USB device.
"""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def refused(call,message,name):
 try:call()
 except (ValueError,RuntimeError) as error:assert str(error)==message,(name,str(error))
 else:raise AssertionError(name)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('image','fixture','frame','output'):p.add_argument('--'+key,type=Path,required=True)
 a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=False)
 prep=module('pcie_nvme_host_prepare',ROOT/'apps/pcie-nvme/prepare.py');observe=module('pcie_nvme_host_observe',ROOT/'apps/pcie-nvme/observe.py')
 capture=module('pcie_nvme_host_capture',ROOT/'apps/pcie-nvme/capture.py')
 raw=(a.fixture/'session-template.bin').read_bytes();record=json.loads((a.fixture/'admission.json').read_text());ih=record['image_sha256']
 assert ih==hashlib.sha256(a.image.read_bytes()).hexdigest(),'Fixture belongs to another image'
 prep.validate_admission(record,ih,hashlib.sha256(raw).hexdigest(),raw);results={'nominal_admission':True}
 # Session geometry refusals before any evidence is read.
 for name,offset,fmt,value in [('version',600,'<Q',2),('function',808,'<I',8),('namespace',812,'<I',0),('timeout',792,'<Q',999)]:
  changed=bytearray(raw);struct.pack_into(fmt,changed,offset,value)
  message='Invalid PCIe NVMe session.' if name=='version' else 'Missing bounded NVMe read authority.'
  refused(lambda:prep.session_check(bytes(changed)),message,name);results[name+'_session_refused']=True
 # The reviewed NVMe record must name this exact read and every required fact.
 path=record['nvme']['path'];original=json.loads(Path(path).read_text())
 for name,key,value,message in [('read-authority','namespace_read_authorized',False,'NVMe controller or read authority is incomplete.'),
   ('cold-controller','controller_disabled_at_handoff',None,'NVMe controller or read authority is incomplete.'),
   ('other-lba','lba',9,'NVMe read authority differs.'),('other-schema','schema','a733-pcie-endpoint-test-v1','NVMe read authority differs.'),
   ('anonymous-controller','controller',dict(model='',serial='x',firmware='y'),'Actual NVMe controller identity required.')]:
  proof={**original,key:value}
  if value is None:proof.pop(key)
  blob=json.dumps(proof,sort_keys=True).encode();identity=hashlib.sha256(blob).hexdigest();changed=bytearray(raw);changed[816:848]=bytes.fromhex(identity)
  admission={**record,'nvme':{'path':path,'sha256':identity},'session_template_sha256':hashlib.sha256(changed).hexdigest()}
  refused(lambda:prep.validate_admission(admission,ih,admission['session_template_sha256'],changed,{path:blob}),message,name)
  results[name+'_refused']=True
 # Decoding the linked producer's frame, then refusing altered copies.
 frame=a.frame.read_bytes();decoded=observe.decode(frame,raw);assert decoded['status']==1 and decoded['lba']==8
 for name,offset,fmt,value,message in [('incomplete',12,'<I',1,'NVMe read or orderly stop incomplete'),
   ('other-lba',48,'<Q',9,'NVMe endpoint identity or read target differs'),
   ('small-bar',60,'<I',0x1000,'NVMe BAR0 placement differs'),
   ('geometry',40,'<I',13,'NVMe namespace geometry differs'),
   ('record-authority',144,'<I',0,'NVMe record authority differs')]:
  altered=bytearray(frame);struct.pack_into(fmt,altered,128+offset,value);altered[64:96]=hashlib.sha256(altered[128:]).digest()
  refused(lambda:observe.decode(bytes(altered),raw),message,name);results[name+'_frame_refused']=True
 altered=bytearray(frame);altered[128+192]^=1
 refused(lambda:observe.decode(bytes(altered),raw),'Copied NVMe output hash/extent differs','data');results['data_frame_refused']=True
 other=bytearray(raw);struct.pack_into('<Q',other,24,struct.unpack_from('<Q',raw,24)[0]^1)
 refused(lambda:observe.decode(frame,bytes(other)),'PCIe NVMe session differs','session');results['other_session_frame_refused']=True
 # Durable capture over a modeled exchange: one pending reply, then the result.
 pending=bytearray(frame[:128]);struct.pack_into('<2I',pending,8,0,0);pending[64:96]=bytes(32)
 replies=iter([bytes(pending),frame]);saved=[];model=out/'host-capture';model.mkdir();state={}
 result=capture.capture(lambda packet,timeout:next(replies),raw,model,state,lambda:saved.append(dict(state)),time.monotonic()+10,pause=lambda n:None)
 assert result['status']==1 and state['pcie_result']=='observed-success' and (model/'output.bin').read_bytes()==frame[128:]
 assert state['copied_pcie_output']['bytes']==observe.OUTPUT
 calls=0
 def lost(packet,timeout):
  nonlocal calls
  calls+=1;raise OSError('Modeled USB loss')
 try:capture.capture(lost,raw,model,{},lambda:None,time.monotonic()+10,pause=lambda n:None)
 except OSError:pass
 else:raise AssertionError('Transport loss hidden')
 assert calls==1;results.update(durable_capture=True,transport_no_retry=True)
 # The actual launcher build and a frozen bundle from the tested image.
 manifest=a.image.parent/'pcie-nvme-application/manifest.json'
 bundle=prep.prepare(a.image,manifest,a.fixture/'admission.json',a.fixture/'session-template.bin',out/'bundle',
  Path('/usr/bin/clang-21'),Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21')
 assert bundle['physical_access'] is False;prep.validate_bundle(out/'bundle');results['actual_launcher_build_and_frozen_bundle']=True
 results['physical_access']=False;(out/'host-results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results))
if __name__=='__main__':main()
