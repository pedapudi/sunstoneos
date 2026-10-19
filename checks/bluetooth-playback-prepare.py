#!/usr/bin/env python3
"""Check new playback profile geometry and exact compiled codec-provider gating."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
for n in ('manifest','producer','session','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();spec=importlib.util.spec_from_file_location('pcm_prepare',ROOT/'apps/bluetooth/prepare.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
manifest=json.loads(a.manifest.read_text());original=bytearray((a.producer/'duplex-copied.pcm.profile.bin').read_bytes());template=a.session.read_bytes();results={}
for mode in (4,5):
 profile=bytearray(original);struct.pack_into('<I',profile,624,mode)
 if mode==4:profile=profile[:752];struct.pack_into('<2I',profile,8,2,752)
 session=bytearray(template);session[320:352]=hashlib.sha256(profile).digest();session[312:320]=profile[24:32];struct.pack_into('<I',session,400,2)
 assert m.profile_check(profile,session)
 bad=bytearray(profile);struct.pack_into('<I',bad,720,23999)
 try:m.profile_check(bad,session)
 except ValueError:pass
 else:raise AssertionError('Partial codec sample geometry admitted')
 for name in ('missing','changed','exact'):
  value=dict(manifest)
  if name=='missing':value.pop('wideband_provider',None)
  if name=='changed':value['wideband_provider']={**value['wideband_provider'],'revision':'unmatched'}
  try:m.bluetooth_admission({'radio_ddr':{}},profile,session,value,manifest['final_elf'],{})
  except ValueError as error:
   assert ('Exactly four' in str(error)) if name=='exact' else ('pinned mSBC provider' in str(error)),str(error)
  else:raise AssertionError('Incomplete component fixture unexpectedly admitted a launch')
  results[f'{mode}-{name}']={'passed':True}
a.output.write_text(json.dumps({'cases':results,'manifest_sha256':hashlib.sha256(a.manifest.read_bytes()).hexdigest(),'scope':'New profile/provider checks only, using actual producer profile and an existing modeled session with explicit SID/workload/profile binding. Incomplete input deliberately stops after codec gating. No complete physical launch admission or session authority minted.'},indent=2)+'\n')
print('PASS playback/duplex profile and exact native provider gate')
