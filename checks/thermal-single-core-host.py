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
common={'identity':identity,'port':port,'image_sha256':image_hash,'reviewed':True}
model=put('MODEL',{'physical_access':False,'warning':'Synthetic fixture only. Never physical authority.'})
conditions={n:model for n in ('primary-cache-transition-and-secondary-off','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','physical-code-publication','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity','rom-usb0-pio-takeover','usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded')}
platform=put('platform',{**common,'schema':'a733-board-single-core-platform-v1','conditions':conditions,'entry_conditions_established':True,'physical_coherence_result':'under-test'})
ddr=put('ddr',{**common,'schema':'a733-ddr-reservation-v1','exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'ranges':[[0x41000000,0x42200000]],'evidence':[model]})
record={**common,'schema':'a733-thermal-supervised-entry-v1','reservation':100,'ddr':ddr,'platform':platform,'whole_board_power_removal':True,'no_resource_reuse_before_power_removal':True,'recovery':{'mode':'manual-usb-reconnect','demonstrated_fel':True,'explicit_reconnect_before_resumption':True,'preserve_microsd':True,'one_attempt':True,'evidence':[model]}}
required={'initial':['exclusive_ths_and_irq71','remote_writers_excluded','no_inherited_thermal_service','security_access_authorized','gic_route_owned','boot_lineage_reviewed'],
'electrical':['avcc_adc_digital_supplies_stable','sid_shadows_readable','device_mappings_verified','no_physical_aliases'],
'clocks':['stable_apb1_ref_osc','no_clock_reset_writers','gpadc_consumers_excluded' if raw[768] else 'stable_enabled_gpadc24m'],
'supervision':['whole_board_power_removal','permanent_retention','no_reuse','one_attempt']}
for key,offset in zip(required,(632,664,696,728)):
 v={**common,'schema':'a733-thermal-'+key+'-v1',**dict.fromkeys(required[key],True)}
 ref=put(key,v);raw[offset:offset+32]=bytes.fromhex(ref['sha256']);record[key]=ref
record['session_template_sha256']=hashlib.sha256(raw).hexdigest()
template=a.output/'MODEL-session.bin';template.write_bytes(raw);admission=a.output/'MODEL-admission.json';admission.write_text(json.dumps(record,indent=2)+'\n')
bundle=a.output/'MODEL-NOT-DEPLOYABLE-bundle'
prepared=prep.prepare(a.image,a.manifest,admission,template,bundle,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21')
assert any(item['sha256']==model['sha256'] for item in prepared['entry_evidence'].values())
# Bundle validation must consume retained bytes after every original condition disappears.
for f in facts.iterdir():f.unlink()
prep.validate_bundle(bundle)
frozen={path:(bundle/item['name']).read_bytes() for path,item in prepared['entry_evidence'].items()}
del frozen[model['path']]
try:prep.validate_admission(record,image_hash,record['session_template_sha256'],raw,frozen,True,'thermal-single-core-supervised-v1')
except ValueError:pass
else:raise AssertionError('Missing frozen condition accepted')
# Reuse the accepted native thermal frame; only its new session binding changes.
original=Path.home()/'.cache/sunstoneos/evidence/thermal-application-20261018/native-cases/native-frame.bin'
frame=bytearray(original.read_bytes());session=(bundle/'session.bin').read_bytes()
frame[16:32]=session[24:40];frame[32:64]=hashlib.sha256(session).digest();frame=bytes(frame)
capture=a.output/'capture';capture.mkdir();observed={}
tool.capture(lambda *_:frame,session,capture,observed,lambda:None,1,now=lambda:0,pause=lambda _:None)
assert observed['thermal_result']=='observed-success' and (capture/'output.bin').read_bytes()==frame[128:]
legacy=Path.home()/'.cache/sunstoneos/evidence/thermal-application-20261018/MODEL-NOT-DEPLOYABLE-bundle'
assert prep.validate_bundle(legacy).get('execution_profile','thermal-supervised-v1')=='thermal-supervised-v1'
(a.output/'result.json').write_text(json.dumps({'physical_access':False,'cases':{'exact-profile-package-and-launcher':True,'platform-condition-bytes-frozen':True,'frozen-original-path-removal':True,'missing-frozen-condition-refused':True,'unchanged-native-thermal-output-new-session-capture':True,'legacy-frozen-bundle-compatible':True},'reused_frame':{'path':str(original),'sha256':hashlib.sha256(original.read_bytes()).hexdigest()},'limits':'All physical facts synthetic. Accepted native thermal calibration/two-set payload reused without sensor execution.'},indent=2)+'\n')
