#!/usr/bin/env python3
"""Check changed thermal profile packaging with explicitly modeled physical facts."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser()
for n in ('image','manifest','session','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[1]
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
prep=mod('rtc_single_host',r/'apps/thermal/prepare.py');tool=mod('rtc_single_tool',r/'apps/thermal/capture.py')
raw=bytearray(a.session.read_bytes());image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest();facts=a.output/'MODEL-NOT-PHYSICAL';facts.mkdir()
def put(n,value):
 b=json.dumps(value,sort_keys=True).encode();p=(facts/(n+'.json')).resolve();p.write_bytes(b);return {'path':str(p),'sha256':hashlib.sha256(b).hexdigest()}
identity='THERMAL-SOFTWARE-MODEL';port='NO-PHYSICAL-PORT'
common={'identity':identity,'port':port}
record={**common,'reservation':100}
required={'initial':['exclusive_ths_and_irq71','remote_writers_excluded','no_inherited_thermal_service','security_access_authorized','gic_route_owned','boot_lineage_reviewed'],
'electrical':['avcc_adc_digital_supplies_stable','sid_shadows_readable','device_mappings_verified','no_physical_aliases'],
'clocks':['stable_apb1_ref_osc','no_clock_reset_writers','gpadc_consumers_excluded' if raw[768] else 'stable_enabled_gpadc24m'],
'supervision':['whole_board_power_removal','permanent_retention','no_reuse','one_attempt']}
for key,offset in zip(required,(632,664,696,728)):
 v={**common,**dict.fromkeys(required[key],True)}
 raw[offset:offset+32]=hashlib.sha256(json.dumps(v,sort_keys=True).encode()).digest();record[key]=v
record['session_template_sha256']=hashlib.sha256(raw).hexdigest()
template=a.output/'MODEL-session.bin';template.write_bytes(raw);admission=a.output/'MODEL-admission.json';admission.write_text(json.dumps(record,indent=2)+'\n')
bundle=a.output/'MODEL-NOT-DEPLOYABLE-bundle'
prepared=prep.prepare(a.image,None,admission,template,bundle,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21')
assert len(prepared['entry_evidence'])==4
prep.validate_bundle(bundle)
# Direct ownership remains required without a supporting record inventory.
bad={**record,'initial':{**record['initial'],next(iter(required['initial'])):False}}
try:prep.validate_admission(bad,image_hash,'unused-review-hash',raw,execution_profile='thermal-single-core-supervised-v1')
except ValueError as error:assert 'Required THERMAL physical fact absent' in str(error)
else:raise AssertionError('Active or unowned device accepted')

# Reuse the accepted native thermal frame; only its new session binding changes.
original=Path.home()/'.cache/sunstoneos/evidence/thermal-application-20261018/native-cases/native-frame.bin'
frame=bytearray(original.read_bytes());session=(bundle/'session.bin').read_bytes()
frame[16:32]=session[24:40];frame[32:64]=hashlib.sha256(session).digest();frame=bytes(frame)
capture=a.output/'capture';capture.mkdir();observed={}
tool.capture(lambda *_:frame,session,capture,observed,lambda:None,1,now=lambda:0,pause=lambda _:None)
assert observed['thermal_result']=='observed-success' and (capture/'output.bin').read_bytes()==frame[128:]
legacy=Path.home()/'.cache/sunstoneos/evidence/thermal-application-20261018/MODEL-NOT-DEPLOYABLE-bundle'
assert prep.validate_bundle(legacy).get('execution_profile','thermal-supervised-v1')=='thermal-supervised-v1'
(a.output/'result.json').write_text(json.dumps({'physical_access':False,'cases':{'exact-profile-package-and-launcher':True,'direct-device-conditions':True,'no-external-review-records':True,'missing-ownership-condition-refused':True,'unchanged-native-thermal-output-new-session-capture':True,'legacy-frozen-bundle-compatible':True},'reused_frame':{'path':str(original),'sha256':hashlib.sha256(original.read_bytes()).hexdigest()},'limits':'All physical facts synthetic. Accepted native thermal calibration/two-set payload reused without sensor execution.'},indent=2)+'\n')
