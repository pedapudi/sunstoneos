#!/usr/bin/env python3
"""Changed board profile, linked-identity packaging and frozen-evidence checks."""
import argparse,hashlib,importlib.util,json,struct,shutil
from pathlib import Path
p=argparse.ArgumentParser()
for n in ('image','manifest','session','profile','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[1]
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
prep=mod('board_single_host',r/'apps/board/prepare.py');tool=mod('board_single_tool',r/'tools/check-a733-board-supervised.py')
raw=bytearray(a.session.read_bytes());image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest();facts=a.output/'MODEL-NOT-PHYSICAL';facts.mkdir()
def put(n,value):
 b=json.dumps(value,sort_keys=True).encode();p=(facts/(n+'.json')).resolve();p.write_bytes(b);return {'path':str(p),'sha256':hashlib.sha256(b).hexdigest()}
identity='BOARD-SOFTWARE-MODEL';port='NO-PHYSICAL-PORT'
common={'identity':identity,'port':port,'image_sha256':image_hash,'reviewed':True}
model=put('MODEL',{'physical_access':False,'warning':'Synthetic fixture only. Never physical authority.'})
conditions={n:model for n in ('primary-cache-transition-and-secondary-off','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','physical-code-publication','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity','rom-usb0-pio-takeover','usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded')}
platform=put('platform',{**common,'schema':'a733-board-single-core-platform-v1','conditions':conditions,'entry_conditions_established':True,'physical_coherence_result':'under-test'})
def memory(n,ranges):return put(n,{**common,'schema':'a733-ddr-reservation-v1','exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'ranges':ranges,'evidence':[model]})
record={**common,'schema':'a733-board-supervised-entry-v2','reservation':100,'ddr':memory('ddr',[[0x41000000,0x42200000]]),'radio_ddr':memory('radio-ddr',[[0x43000000,0x43100000]]),'platform':platform,'whole_board_power_removal':True,'no_resource_reuse_before_power_removal':True,'recovery':{'mode':'manual-usb-reconnect','demonstrated_fel':True,'explicit_reconnect_before_resumption':True,'preserve_microsd':True,'one_attempt':True,'evidence':[model]}}
firmware=Path.home()/'.cache/sunstoneos/sources/radio/firmware/aic8800d80'
names=['fw_adid_8800d80_u02.bin','fw_patch_8800d80_u02.bin','fw_patch_table_8800d80_u02.bin','fmacfw_8800d80_h_u02.bin']
hashes=['a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56','4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0','5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60','ae8d548df96c132c445f888b91eab5ef03c19991b8bdef4fc12ebfcbf9f953e7']
profile=a.profile.read_bytes();payloads=[]
for i,(name,h) in enumerate(zip(names,hashes)):
 f=firmware/name;assert hashlib.sha256(f.read_bytes()).hexdigest()==h
 payloads.append({'path':str(f),'sha256':h,'address':struct.unpack_from('<Q',profile,432+8*i)[0]})
record['payloads']=payloads
record['session_template_sha256']=hashlib.sha256(raw).hexdigest()
template=a.output/'MODEL-session.bin';template.write_bytes(raw);admission=a.output/'MODEL-admission.json';admission.write_text(json.dumps(record,indent=2)+'\n')
bundle=a.output/'MODEL-NOT-DEPLOYABLE-bundle'
prepared=prep.prepare(a.image,a.manifest,admission,template,bundle,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',a.profile,payloads)
assert (bundle/'session.bin').read_bytes()[48:80].hex()==prepared['build_id']!=prepared['image_sha256']
# Exact producer binding is checked without replaying device protocols.
nominal_session=(bundle/'session.bin').read_bytes()
bad=bytearray(nominal_session);bad[48:80]=bytes.fromhex(prepared['image_sha256'])
assert bad!=(bundle/'session.bin').read_bytes()
(bundle/'session.bin').write_bytes(bad)
try:prep.validate_bundle(bundle)
except ValueError:pass
else:raise AssertionError('ELF hash accepted as linked identity')
(bundle/'session.bin').write_bytes(nominal_session)
# The legacy producer uses the same linked-identity consumer contract.
legacy=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/build/sunstone-a733-board.elf'
legacy_hash=hashlib.sha256(legacy.read_bytes()).hexdigest()
legacy_manifest={'target':'sunstone-a733-board','execution_profile':'board-supervised-v2','final_elf':legacy_hash}
legacy_mf=a.output/'legacy-image-manifest.json';legacy_mf.write_text(json.dumps(legacy_manifest))
legacy_record={**record,'image_sha256':legacy_hash}
for key in ('ddr','radio_ddr','platform'):
 value=json.loads(Path(record[key]['path']).read_text());value['image_sha256']=legacy_hash
 if key=='platform':
  value['schema']='a733-smp-observer-platform-v1'
  value['conditions']={k:model for k in ('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity')}
 legacy_record[key]=put('legacy-'+key,value)
legacy_raw=bytearray(raw);struct.pack_into('<I',legacy_raw,532,1)
legacy_record['session_template_sha256']=hashlib.sha256(legacy_raw).hexdigest()
legacy_template=a.output/'MODEL-legacy-template.bin';legacy_template.write_bytes(legacy_raw)
legacy_admission=a.output/'MODEL-legacy-admission.json';legacy_admission.write_text(json.dumps(legacy_record))
legacy_bundle=a.output/'MODEL-legacy-bundle'
legacy_prepared=prep.prepare(legacy,legacy_mf,legacy_admission,legacy_template,legacy_bundle,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',a.profile,payloads)
assert (legacy_bundle/'session.bin').read_bytes()[48:80].hex()==legacy_prepared['build_id']!=legacy_hash
# Every original platform condition can disappear after freezing.
for f in facts.iterdir():f.unlink()
prep.validate_bundle(bundle)
frozen={path:(bundle/item['name']).read_bytes() for path,item in prepared['entry_evidence'].items()};del frozen[model['path']]
try:prep.validate_admission(record,image_hash,record['session_template_sha256'],frozen,True,'board-single-core-supervised-v1')
except ValueError:pass
else:raise AssertionError('Missing frozen condition accepted')
# Selected completed response requires the actual native client's verified-byte count.
session=(bundle/'session.bin').read_bytes();epoch,generation=struct.unpack_from('<2Q',session,24)
frame=struct.pack('<6I2Q',0x52414f42,1,3,0,2,5,epoch,generation)+session[368:400]
capture=a.output/'capture';capture.mkdir();observed={}
tool.capture(lambda *_:frame,session,profile,capture,observed,lambda:None,1,now=lambda:0,pause=lambda _:None)
assert observed['board_result']=='observed-success'
# Legacy profile receives the same linked-identity correction, with its old mode1.
legacy=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/build/sunstone-a733-board.elf'
legacy_manifest={'target':'sunstone-a733-board','execution_profile':'board-supervised-v2','final_elf':hashlib.sha256(legacy.read_bytes()).hexdigest()}
assert prep.image_manifest_check(legacy_manifest,legacy.read_bytes())=='board-supervised-v2'
(a.output/'result.json').write_text(json.dumps({'physical_access':False,'cases':{'exact-profile-radio-firmware-package-and-launcher':True,'session-uses-linked-build-identity':True,'elf-hash-session-mutation-refused':True,'platform-condition-bytes-frozen':True,'frozen-original-path-removal':True,'missing-frozen-condition-refused':True,'selected-radio-client-completion-capture':True,'legacy-elf-profile-retained':True,'fresh-legacy-session-uses-linked-identity':True},'limits':'All physical facts and completion framing synthetic. Actual pinned firmware bytes uploaded only into protected software bundle. No physical or protocol replay.'},indent=2)+'\n')
