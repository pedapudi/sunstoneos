#!/usr/bin/env python3
"""Only new LRADC electrical/source prerequisites and six-bit report refusals."""
import argparse,copy,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def digest(data):return hashlib.sha256(data).hexdigest()
def run(bundle,reply,out):
 out.mkdir(parents=True,exist_ok=False);p=module('lradc_host_prepare',ROOT/'apps/adc/prepare.py');d=module('lradc_host_decode',ROOT/'apps/adc/observe.py')
 prepared=p.validate_bundle(bundle);session=(bundle/'session.bin').read_bytes();frame=reply.read_bytes();assert d.decode(frame,session)['controller']=='LRADC0'
 source=json.loads((bundle/'admission.json').read_text());template=(bundle/'session-template.bin').read_bytes();results={}
 for name,key,change in [('missing-idle-electrical-authority','electrical','remove'),('incorrect-overrange-rating','electrical','range'),('legacy-rtc-writers-not-excluded','clocks','rtc')]:
  record=copy.deepcopy(source);raw=bytearray(template);value=json.loads(Path(record[key]['path']).read_bytes())
  if change=='remove':value.pop('lradc_idle_electrical_authority')
  elif change=='rtc':value['all_legacy_rtc_source_calibration_oscillator_writers_excluded']=False
  else:
   ref=value['lradc_idle_electrical_authority'];determination=json.loads(Path(ref['path']).read_bytes());determination['nominal_pullup_uv']=1350000
   path=(out/(name+'-determination.json')).resolve();data=json.dumps(determination,sort_keys=True).encode();path.write_bytes(data);value['lradc_idle_electrical_authority']=dict(path=str(path),sha256=digest(data))
  path=(out/(name+'.json')).resolve();data=json.dumps(value,sort_keys=True).encode();path.write_bytes(data);record[key]=dict(path=str(path),sha256=digest(data));offset={'electrical':664,'clocks':696}[key];raw[offset:offset+32]=hashlib.sha256(data).digest();record['session_template_sha256']=digest(raw)
  try:p.validate_admission(record,prepared['image_sha256'],digest(raw),raw)
  except ValueError as error:
   assert ('LRADC' in str(error)) if change!='rtc' else ('physical fact absent' in str(error));results[name]=str(error)
  else:raise AssertionError(name+' accepted')
 for name in ['code64','nonzero-unused-slot','repeated-observation-time','repeated-sequence']:
  changed=bytearray(frame)
  if name=='code64':struct.pack_into('<H',changed,128+40,64)
  elif name=='nonzero-unused-slot':changed[128+48]=1
  elif name=='repeated-observation-time':changed[128+96:128+104]=changed[128+32:128+40]
  else:changed[128+108:128+112]=changed[128+44:128+48]
  changed[64:96]=hashlib.sha256(changed[128:]).digest()
  try:d.decode(bytes(changed),session)
  except ValueError as error:results[name]=str(error)
  else:raise AssertionError(name+' accepted')
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical=False,image_sha256=prepared['image_sha256'],session_sha256=digest(session),reply_sha256=digest(frame),cases=results),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('bundle','reply','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();run(a.bundle,a.reply,a.output)
