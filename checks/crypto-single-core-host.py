#!/usr/bin/env python3
"""Check changed crypto profile packaging with explicitly modeled physical facts."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser()
for n in ('image','manifest','session','output'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument("--runtime-operation",type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[1]
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
prep=mod('rtc_single_host',r/'apps/crypto/prepare.py');tool=mod('rtc_single_tool',r/'tools/check-a733-crypto-supervised.py')
providers=mod('crypto_host_nominal_providers',r/'checks/a733-provider-fixture.py')
raw=providers.nominal_provider_session(a.session.read_bytes(),usb_reference_mode=2);image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest();facts=a.output/'MODEL-NOT-PHYSICAL';facts.mkdir()
def put(n,value):
 b=json.dumps(value,sort_keys=True).encode();p=(facts/(n+'.json')).resolve();p.write_bytes(b);return {'path':str(p),'sha256':hashlib.sha256(b).hexdigest()}
identity='CRYPTO-SOFTWARE-MODEL';port='NO-PHYSICAL-PORT'
common={'identity':identity,'port':port}
record={**common,'reservation':100}
required={'initial':['all_secure_nonsecure_ce_clients_excluded','remote_firmware_excluded','prior_ce_dma_independently_resolved','secure_bank_read_authorized','irq200_201_both_worlds_owned_masked','boot_lineage_reviewed'],
'memory':['private_cacheable_page','no_physical_aliases','ce_physical_ram_reachable','exclusive_cache_lines','permanent_page_reservation'],
'clocks':['observed_ce400_peri0','stable_ref_peri0_fabric','no_clock_reset_writers','ce_local_gates_resets_ready'],
'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse','one_attempt']}
for key,offset in zip(required,(632,664,696,728)):
 v={**common,**dict.fromkeys(required[key],True)}
 raw[offset:offset+32]=hashlib.sha256(json.dumps(v,sort_keys=True).encode()).digest();record[key]=v
if a.runtime_operation:
 struct.pack_into('<Q',raw,600,2);raw[760:792]=hashlib.sha256(a.runtime_operation.read_bytes()).digest();raw[792:824]=bytes(32)
 prep.operation.decode(a.runtime_operation.read_bytes(),raw)
record['session_template_sha256']=hashlib.sha256(raw).hexdigest()
template=a.output/'MODEL-session.bin';template.write_bytes(raw);admission=a.output/'MODEL-admission.json';admission.write_text(json.dumps(record,indent=2)+'\n')
bundle=a.output/'MODEL-NOT-DEPLOYABLE-bundle'
prepared=prep.prepare(a.image,None,admission,template,bundle,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',a.runtime_operation)
assert len(prepared['entry_evidence'])==4
prep.validate_bundle(bundle)
# Direct ownership remains required without a supporting record inventory.
bad={**record,'initial':{**record['initial'],next(iter(required['initial'])):False}}
try:prep.validate_admission(bad,image_hash,'unused-review-hash',raw,execution_profile='crypto-single-core-supervised-v1')
except ValueError as error:assert 'Required CRYPTO physical fact absent' in str(error)
else:raise AssertionError('Active or unowned device accepted')

if a.runtime_operation:
 assert prepared['_reference'] is None and prepared['_operation']==a.runtime_operation.read_bytes()
 operation=bundle/'operation.bin';original=operation.read_bytes();operation.write_bytes(original[:-1]+bytes([original[-1]^1]))
 try:prep.validate_bundle(bundle)
 except ValueError:pass
 else:raise AssertionError('Changed operation accepted')
 operation.write_bytes(original);assert prep.validate_bundle(bundle)['_operation']==original
 (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':{'runtime-session-and-request-commitment':True,'real-arm-launcher-link':True,'package-frozen-operation-integrity':True,'direct-ownership-conditions':True},'image_sha256':image_hash},indent=2)+'\n')
 print('PASS runtime crypto package, exact caller operation and retained admission; physical authority modeled')
 raise SystemExit(0)
# Reuse the accepted native crypto frame; only its new session binding changes.
original=Path.home()/'.cache/sunstoneos/evidence/crypto-native-20261018/native/native-frame.bin'
frame=bytearray(original.read_bytes());session=(bundle/'session.bin').read_bytes()
frame[16:32]=session[24:40];frame=bytes(frame)
capture=a.output/'capture';capture.mkdir();observed={}
tool.capture(lambda *_:frame,session,capture,observed,lambda:None,1,now=lambda:0,pause=lambda _:None)
assert observed['crypto_result']=='observed-success' and (capture/'output.bin').read_bytes()==frame[128:]
legacy=Path.home()/'.cache/sunstoneos/evidence/crypto-native-20261018/package/MODEL-bundle'
# Test native profile/type mismatches without re-running the accepted workload.
assert prep.image_manifest_check({},a.image.read_bytes())=='crypto-single-core-supervised-v1'
# Reuse the unchanged legacy image/profile check; its software-only bundle uses
# an old mocked authority schema and is intentionally not deployable.
legacy_manifest=json.loads((legacy/'image-input-manifest.json').read_text())
assert prep.image_manifest_check(legacy_manifest,(legacy/'image.elf').read_bytes())=='crypto-supervised-v1'
(a.output/'result.json').write_text(json.dumps({'physical_access':False,'cases':{'exact-profile-package-and-launcher':True,'direct-device-conditions':True,'no-external-review-records':True,'missing-ownership-condition-refused':True,'unchanged-native-crypto-output-new-session-capture':True,'legacy-frozen-bundle-compatible':True},'reused_frame':{'path':str(original),'sha256':hashlib.sha256(original.read_bytes()).hexdigest()},'limits':'All physical facts synthetic. Accepted native SHA/AES output reused without arithmetic or engine execution.'},indent=2)+'\n')
