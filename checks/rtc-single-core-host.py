#!/usr/bin/env python3
"""Check changed RTC profile packaging with explicitly modeled physical facts."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser()
for n in ('image','manifest','session','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[1]
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
prep=mod('rtc_single_host',r/'apps/rtc/prepare.py');tool=mod('rtc_single_tool',r/'tools/check-a733-rtc-supervised.py')
raw=bytearray(a.session.read_bytes());image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest();facts=a.output/'MODEL-NOT-PHYSICAL';facts.mkdir()
def put(n,value):
 b=json.dumps(value,sort_keys=True).encode();p=(facts/(n+'.json')).resolve();p.write_bytes(b);return {'path':str(p),'sha256':hashlib.sha256(b).hexdigest()}
identity='RTC-SOFTWARE-MODEL';port='NO-PHYSICAL-PORT'
common={'identity':identity,'port':port,'image_sha256':image_hash,'reviewed':True}
model=put('MODEL',{'physical_access':False,'warning':'Synthetic fixture only. Never physical authority.'})
conditions={n:model for n in ('primary-cache-transition-and-secondary-off','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','physical-code-publication','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity','rom-usb0-pio-takeover','usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded')}
platform=put('platform',{**common,'schema':'a733-rtc-single-core-platform-v1','conditions':conditions,'entry_conditions_established':True,'physical_coherence_result':'under-test'})
ddr=put('ddr',{**common,'schema':'a733-ddr-reservation-v1','exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'ranges':[[0x41000000,0x42200000]],'evidence':[model]})
record={**common,'schema':'a733-rtc-supervised-entry-v1','reservation':100,'ddr':ddr,'platform':platform,'whole_board_power_removal':True,'no_resource_reuse_before_power_removal':True,'recovery':{'mode':'manual-usb-reconnect','demonstrated_fel':True,'explicit_reconnect_before_resumption':True,'preserve_microsd':True,'one_attempt':True,'evidence':[model]}}
required={'initial':['exclusive_rtc_and_rccu_word','irq228_owned_disabled_inactive_unpending','remote_writers_excluded','other_cores_excluded','inherited_alarm_disabled_unpending'],'clock':['external_32768_stable','source_writers_excluded','exclusive_calendar_writer_lifetime','remote_calendar_writers_excluded','registered_native_calendar_clients_authorized','calendar_write_authorized'],'electrical':['fitted_board_identified','vcc_rtc_power_stable'],'supervision':['whole_board_power_removal','permanent_ownership','no_reuse','one_attempt']}
for key,offset in zip(required,(632,664,696,728)):
 v={**common,'schema':'a733-rtc-'+key+'-v1',**dict.fromkeys(required[key],True)}
 if key=='clock':v.update(requested_day=struct.unpack_from('<I',raw,772)[0],requested_hms=struct.unpack_from('<I',raw,776)[0])
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
try:prep.validate_admission(record,image_hash,record['session_template_sha256'],raw,frozen,True,'rtc-single-core-supervised-v1')
except ValueError:pass
else:raise AssertionError('Missing frozen condition accepted')
# Existing native service output is reused; only changed session framing/capture is exercised.
original=Path.home()/'.cache/sunstoneos/evidence/rtc-service-native-20261018/composition-service-output.bin'
payload=original.read_bytes();session=(bundle/'session.bin').read_bytes();epoch,generation=struct.unpack_from('<2Q',session,24)
frame=struct.pack('<4I2Q',0x31435452,4,1,2416,epoch,generation)+hashlib.sha256(payload).digest()+payload
capture=a.output/'capture';capture.mkdir();observed={}
tool.capture(lambda *_:frame,session,capture,observed,lambda:None,1,now=lambda:0,pause=lambda _:None)
assert observed['rtc_result']=='observed-success'
(a.output/'result.json').write_text(json.dumps({'physical_access':False,'cases':{'exact-profile-package-and-launcher':True,'platform-condition-bytes-frozen':True,'frozen-original-path-removal':True,'missing-frozen-condition-refused':True,'unchanged-native-service-output-new-session-capture':True},'limits':'All physical facts synthetic. Accepted native service payload reused without service execution.'},indent=2)+'\n')
