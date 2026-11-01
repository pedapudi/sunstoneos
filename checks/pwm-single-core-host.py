#!/usr/bin/env python3
"""Check the boot-CPU PWM producer and capture with explicitly modeled authority."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('image','manifest','fixture','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);r=Path(__file__).resolve().parents[1]
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def refused(call):
 try:call()
 except (ValueError,OSError):return
 raise AssertionError('Invalid profile or authority accepted')
host=mod('pwm_single_host',r/'tools/check-a733-pwm-supervised.py');prep=host.preparation
bundle=a.output/'MODEL-NOT-DEPLOYABLE-bundle';template=a.fixture/'session-template.bin';admission=a.fixture/'admission.json'
record=json.loads(admission.read_bytes())
metadata=json.loads((a.fixture/'prepared.json').read_bytes())
cache={path:(a.fixture/item['name']).read_bytes() for path,item in metadata['entry_evidence'].items()}
direct={key:record[key] for key in ('identity','port','reservation')}
for key in ('initial','memory','power','supervision','led'):
 if key in record:
  value=json.loads(cache[record[key]['path']])
  direct[key]={k:v for k,v in value.items() if k not in ('schema','reviewed','image_sha256')}
direct['power']['fan_5v_topology_safe']=direct['power'].pop('fan_5v_topology_reviewed')
direct['power']['q2_r89_loading_safe']=direct['power'].pop('q2_r89_loading_reviewed')
inputs=a.output/'direct-inputs.json';inputs.write_text(json.dumps(direct))
prepared=prep.prepare(a.image,None,inputs,template,bundle,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21')
assert prepared['execution_profile']=='pwm-single-core-supervised-v1'
frozen=host.freeze_bundle(bundle);raw=frozen['_frozen']['session.bin'];assert struct.unpack_from('<I',raw,532)[0]==2
results={}
def passed(n):results[n]=True;(a.output/'completed.json').write_text(json.dumps(results,indent=2)+'\n')
passed('actual-producer-package-launcher-and-rom-takeover-mode2')
assert prep.image_manifest_check({},a.image.read_bytes())=='pwm-single-core-supervised-v1'
passed('profile-inferred-without-review-manifest')
bad={**direct,'power':{**direct['power'],'bldo1_pd_1800000':False}}
refused(lambda:prep.validate_admission(bad,prepared['image_sha256'],'',template.read_bytes(),execution_profile=prepared['execution_profile']))
passed('invalid-supply-condition-refused')
bad={**direct,'power':{**direct['power'],'q2_r89_loading_safe':False}}
refused(lambda:prep.validate_admission(bad,prepared['image_sha256'],'',template.read_bytes(),execution_profile=prepared['execution_profile']))
passed('unsafe-external-fan-circuit-refused')
# Preserve all old service/command evidence; only bind the accepted frame to this session.
original=Path.home()/'.cache/sunstoneos/evidence/pwm-service-20261018/native/native-frame.bin'
frame=bytearray(original.read_bytes());frame[16:32]=raw[24:40];frame[32:64]=raw[48:80];frame[64:96]=hashlib.sha256(raw).digest();frame=bytes(frame)
observed={};capture=a.output/'capture';capture.mkdir()
host.capture(lambda *_:frame,raw,capture,observed,lambda:None,1,now=lambda:0,pause=lambda _:None)
assert observed['pwm_result']=='observed-success' and observed['durable_result']['sha256']==hashlib.sha256(frame).hexdigest()
passed('existing-native-service-frame-bound-to-new-session-and-captured')
for name in ('session.bin','launch.bin'):
 path=bundle/name;old=path.read_bytes();path.write_bytes(old+b'\0');refused(lambda:prep.validate_bundle(bundle));assert frozen['_frozen'][name]==old;path.write_bytes(old)
passed('frozen-host-input-binding')
legacy=Path.home()/'.cache/sunstoneos/evidence/pwm-service-20261018/host/MODEL-NOT-PHYSICAL-bundle'
prep.validate_bundle(legacy);passed('existing-frozen-service-bundle-compatible')
(a.output/'result.json').write_text(json.dumps(dict(physical_access=False,cases=results,reused_frame=dict(path=str(original),sha256=hashlib.sha256(original.read_bytes()).hexdigest()),limits='Entry and peripheral facts modeled. Accepted PWM frame reused; no waveform, service or command execution repeated.'),indent=2)+'\n')
