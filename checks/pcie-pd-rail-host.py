#!/usr/bin/env python3
"""Image-bound PD/core/analog electrical admission for corrected PCIe profile."""
import hashlib,importlib.util,json,copy
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,p):
 s=importlib.util.spec_from_file_location(name,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def run(out,source):
 p=module('pcie_pd_host',source/'apps/pcie/prepare.py');folder=out/'fixture';raw=(folder/'session-template.bin').read_bytes();image_hash=hashlib.sha256((out/'build/sunstone-a733-pcie.elf').read_bytes()).hexdigest();record=json.loads((folder/'admission.json').read_text())
 p.validate_admission(record,image_hash,hashlib.sha256(raw).hexdigest(),raw)
 electrical=json.loads(Path(record['electrical']['path']).read_text());results=[]
 for label in ('old-bldo2-only','wrong-pd-rail','core-route','no-analog-route','analog-writer','analog-source'):
  changed=copy.deepcopy(electrical)
  if label=='old-bldo2-only':
   for key in ('pd_bank_bldo1_route_verified','pd_bank_bldo1_1v8_stable'):changed.pop(key)
   changed['pd_bank_1v8_stable']=True;changed['pd_bank_bldo2_route_verified']=True
  elif label=='wrong-pd-rail':changed['pd_bank_bldo1_route_verified']=False
  elif label=='core-route':changed['dcdc2_comb1_core_route_verified']=False
  elif label=='no-analog-route':changed.pop('comb1_analog_supply_routes')
  elif label=='analog-writer':changed['comb1_analog_supply_routes'][0]['lifetime_writers_excluded']=False
  else:changed['comb1_analog_supply_routes'][0]['source']=''
  data=json.dumps(changed,sort_keys=True).encode();path=out/(label+'-electrical.json');path.write_bytes(data);digest=hashlib.sha256(data).digest()
  altered=bytearray(raw);altered[680:712]=digest;modified=copy.deepcopy(record);modified['electrical']={'path':str(path),'sha256':digest.hex()};modified['session_template_sha256']=hashlib.sha256(altered).hexdigest()
  # Common board entry identity remains unchanged; bind each intentional
  # component mutation to its new actual full template hash before validation.
  try:p.validate_admission(modified,image_hash,hashlib.sha256(altered).hexdigest(),altered)
  except ValueError as error:
   expected='Required PCIe physical fact absent.' if label in ('old-bldo2-only','wrong-pd-rail','core-route') else 'Fitted COMB1 analog routes must be identified independently.' if label=='no-analog-route' else 'COMB1 analog route lacks actual source/stability/writer authority.'
   assert str(error)==expected,(label,str(error));results.append({'case':label,'refused':str(error)})
  else:raise AssertionError(label)
 (out/'pd-host-result.json').write_text(json.dumps({'passed':True,'cases':results,'physical_access':False,'scope':'Actual image-bound producer/electrical validators; modeled nondeployable physical facts.'},indent=2)+'\n')
