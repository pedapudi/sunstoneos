#!/usr/bin/env python3
"""Check the opt-in profile/provider gate using native-produced manifest/identities."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
for n in ('manifest','profile','session','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();spec=importlib.util.spec_from_file_location('msbc_prepare',ROOT/'apps/bluetooth/prepare.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
profile=a.profile.read_bytes();session=a.session.read_bytes();manifest=json.loads(a.manifest.read_text());assert struct.unpack_from('<I',profile,624)[0]==3
assert m.profile_check(profile,session)
changed=bytearray(profile);struct.pack_into('<I',changed,720,23999)
try:m.profile_check(changed,session)
except ValueError:pass
else:raise AssertionError('Partial codec frame admitted')
results={}
for name in ('missing','changed','exact'):
 value=dict(manifest)
 if name=='missing':value.pop('wideband_provider',None)
 if name=='changed':value['wideband_provider']={**value['wideband_provider'],'revision':'unmatched'}
 try:m.bluetooth_admission({'radio_ddr':{}},profile,session,value,manifest['final_elf'],{})
 except ValueError as error:
  if name=='exact':assert 'Exactly four' in str(error),str(error)
  else:assert 'pinned mSBC provider' in str(error),str(error)
 else:raise AssertionError('Incomplete modeled admission unexpectedly passed')
 results[name]={'passed':True}
a.output.write_text(json.dumps({'cases':results,'partial_frame_rejected':True,'manifest_sha256':hashlib.sha256(a.manifest.read_bytes()).hexdigest(),'scope':'New profile geometry and provider gate only. Existing producer-derived identity fixtures; no complete launch admission or physical authority established.'},indent=2)+'\n')
print('PASS opt-in geometry and exact native provider gate')
