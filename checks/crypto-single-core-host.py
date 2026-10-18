#!/usr/bin/env python3
"""Check changed crypto profile packaging with explicitly modeled physical facts."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser()
for n in ('image','manifest','session','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[1]
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
prep=mod('rtc_single_host',r/'apps/crypto/prepare.py');tool=mod('rtc_single_tool',r/'tools/check-a733-crypto-supervised.py')
raw=bytearray(a.session.read_bytes());image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest();facts=a.output/'MODEL-NOT-PHYSICAL';facts.mkdir()
def put(n,value):
 b=json.dumps(value,sort_keys=True).encode();p=(facts/(n+'.json')).resolve();p.write_bytes(b);return {'path':str(p),'sha256':hashlib.sha256(b).hexdigest()}
identity='CRYPTO-SOFTWARE-MODEL';port='NO-PHYSICAL-PORT'
common={'identity':identity,'port':port,'image_sha256':image_hash,'reviewed':True}
model=put('MODEL',{'physical_access':False,'warning':'Synthetic fixture only. Never physical authority.'})
conditions={n:model for n in ('primary-cache-transition-and-secondary-off','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','physical-code-publication','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity','rom-usb0-pio-takeover','usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded')}
platform=put('platform',{**common,'schema':'a733-board-single-core-platform-v1','conditions':conditions,'entry_conditions_established':True,'physical_coherence_result':'under-test'})
ddr=put('ddr',{**common,'schema':'a733-ddr-reservation-v1','exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'ranges':[[0x41000000,0x42200000]],'evidence':[model]})
record={**common,'schema':'a733-crypto-supervised-entry-v1','reservation':100,'ddr':ddr,'platform':platform,'whole_board_power_removal':True,'no_resource_reuse_before_power_removal':True,'recovery':{'mode':'manual-usb-reconnect','demonstrated_fel':True,'explicit_reconnect_before_resumption':True,'preserve_microsd':True,'one_attempt':True,'evidence':[model]}}
required={'initial':['all_secure_nonsecure_ce_clients_excluded','remote_firmware_excluded','prior_ce_dma_independently_resolved','secure_bank_read_authorized','irq200_201_both_worlds_owned_masked','boot_lineage_reviewed'],
'memory':['private_cacheable_page','no_physical_aliases','ce_physical_ram_reachable','exclusive_cache_lines','permanent_page_reservation'],
'clocks':['observed_ce400_peri0','stable_ref_peri0_fabric','no_clock_reset_writers','ce_local_gates_resets_ready'],
'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse','one_attempt']}
for key,offset in zip(required,(632,664,696,728)):
 v={**common,'schema':'a733-crypto-'+key+'-v1',**dict.fromkeys(required[key],True)}
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
try:prep.validate_admission(record,image_hash,record['session_template_sha256'],raw,frozen,True,'crypto-single-core-supervised-v1')
except ValueError:pass
else:raise AssertionError('Missing frozen condition accepted')
# Reuse the accepted native crypto frame; only its new session binding changes.
original=Path.home()/'.cache/sunstoneos/evidence/crypto-native-20261018/native/native-frame.bin'
frame=bytearray(original.read_bytes());session=(bundle/'session.bin').read_bytes()
frame[16:32]=session[24:40];frame=bytes(frame)
capture=a.output/'capture';capture.mkdir();observed={}
tool.capture(lambda *_:frame,session,capture,observed,lambda:None,1,now=lambda:0,pause=lambda _:None)
assert observed['crypto_result']=='observed-success' and (capture/'output.bin').read_bytes()==frame[128:]
legacy=Path.home()/'.cache/sunstoneos/evidence/crypto-native-20261018/package/MODEL-bundle'
# Test native profile/type mismatches without re-running the accepted workload.
manifest=json.loads(a.manifest.read_text())
for key,value in [('target','sunstone-a733-crypto'),('execution_profile','crypto-supervised-v1')]:
 changed={**manifest,key:value}
 try:prep.image_manifest_check(changed,a.image.read_bytes())
 except ValueError:pass
 else:raise AssertionError(key)
# Reuse the unchanged legacy image/profile check; its software-only bundle uses
# an old mocked authority schema and is intentionally not deployable.
legacy_manifest=json.loads((legacy/'image-input-manifest.json').read_text())
assert prep.image_manifest_check(legacy_manifest,(legacy/'image.elf').read_bytes())=='crypto-supervised-v1'
(a.output/'result.json').write_text(json.dumps({'physical_access':False,'cases':{'exact-profile-package-and-launcher':True,'platform-condition-bytes-frozen':True,'frozen-original-path-removal':True,'missing-frozen-condition-refused':True,'unchanged-native-crypto-output-new-session-capture':True,'legacy-frozen-bundle-compatible':True},'reused_frame':{'path':str(original),'sha256':hashlib.sha256(original.read_bytes()).hexdigest()},'limits':'All physical facts synthetic. Accepted native SHA/AES output reused without arithmetic or engine execution.'},indent=2)+'\n')
