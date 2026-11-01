#!/usr/bin/env python3
"""Check direct board packaging, native identity and bounded result capture."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser()
for n in ('image','session','profile','output'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--manifest',type=Path,help=argparse.SUPPRESS)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[1]
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
prep=mod('board_single_host',r/'apps/board/prepare.py');tool=mod('board_single_tool',r/'tools/check-a733-board-supervised.py')
raw=bytearray(a.session.read_bytes());image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest()
record={'identity':'BOARD-SOFTWARE-MODEL','port':'NO-PHYSICAL-PORT','reservation':100}
firmware=Path.home()/'.cache/sunstoneos/sources/radio/firmware/aic8800d80'
names=['fw_adid_8800d80_u02.bin','fw_patch_8800d80_u02.bin','fw_patch_table_8800d80_u02.bin','fmacfw_8800d80_h_u02.bin']
hashes=['a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56','4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0','5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60','ae8d548df96c132c445f888b91eab5ef03c19991b8bdef4fc12ebfcbf9f953e7']
profile=a.profile.read_bytes();payloads=[]
for i,(name,h) in enumerate(zip(names,hashes)):
 f=firmware/name;assert hashlib.sha256(f.read_bytes()).hexdigest()==h
 payloads.append({'path':str(f),'sha256':h,'address':struct.unpack_from('<Q',profile,432+8*i)[0]})
record['payloads']=payloads
template=a.output/'session-template.bin';template.write_bytes(raw)
inputs=a.output/'direct-inputs.json';inputs.write_text(json.dumps(record,indent=2)+'\n')
bundle=a.output/'bundle'
compiler='/usr/bin/clang-21';linker='/home/sunil/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
prepared=prep.prepare(a.image,None,inputs,template,bundle,compiler,linker,a.profile,payloads)
assert (bundle/'session.bin').read_bytes()[48:80].hex()==prepared['build_id']!=prepared['image_sha256']
assert not prepared['entry_evidence']
# Even recomputing an ordinary file hash cannot change the native identity.
nominal_session=(bundle/'session.bin').read_bytes();metadata=json.loads((bundle/'prepared.json').read_text())
bad=bytearray(nominal_session);bad[48:80]=bytes.fromhex(prepared['image_sha256'])
(bundle/'session.bin').write_bytes(bad);metadata['files']['session.bin']=hashlib.sha256(bad).hexdigest()
(bundle/'prepared.json').write_text(json.dumps(metadata))
try:prep.validate_bundle(bundle)
except ValueError:pass
else:raise AssertionError('ELF hash accepted as linked identity')
(bundle/'session.bin').write_bytes(nominal_session);metadata['files']['session.bin']=hashlib.sha256(nominal_session).hexdigest()
(bundle/'prepared.json').write_text(json.dumps(metadata));prep.validate_bundle(bundle)
# Both native profiles select their behavior from ELF geometry.
legacy=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/build/sunstone-a733-board.elf'
assert prep.image_manifest_check({},legacy.read_bytes())=='board-supervised-v2'
legacy_raw=bytearray(raw);struct.pack_into('<I',legacy_raw,532,1)
legacy_template=a.output/'legacy-template.bin';legacy_template.write_bytes(legacy_raw)
legacy_bundle=a.output/'legacy-bundle'
legacy_prepared=prep.prepare(legacy,None,inputs,legacy_template,legacy_bundle,compiler,linker,a.profile,payloads)
assert (legacy_bundle/'session.bin').read_bytes()[48:80].hex()==legacy_prepared['build_id']
# Keep real identity and reservation requirements without an evidence inventory.
for name,value in [('identity',''),('port',''),('reservation',0)]:
 wrong={**record,name:value}
 try:prep.validate_admission(wrong,image_hash,'unused-review-hash')
 except ValueError:pass
 else:raise AssertionError('Invalid direct '+name+' accepted')
# Completion requires the selected native client's verified-byte count.
session=(bundle/'session.bin').read_bytes();epoch,generation=struct.unpack_from('<2Q',session,24)
frame=struct.pack('<6I2Q',0x52414f42,1,3,0,2,5,epoch,generation)+session[368:400]
capture=a.output/'capture';capture.mkdir();observed={}
tool.capture(lambda *_:frame,session,profile,capture,observed,lambda:None,1,now=lambda:0,pause=lambda _:None)
assert observed['board_result']=='observed-success'
(a.output/'result.json').write_text(json.dumps({'physical_access':False,'cases':{'radio-package-without-review-records':True,'session-uses-linked-build-identity':True,'elf-hash-session-mutation-refused':True,'target-and-reservation-required':True,'selected-radio-client-completion-capture':True,'legacy-profile-direct-packaging':True},'limits':'Target and completion are modeled. Firmware bytes are packaged on the host; no device or protocol replay.'},indent=2)+'\n')
