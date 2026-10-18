#!/usr/bin/env python3
"""Check first SD initialization authority with actual session evidence binding."""
import argparse,copy,hashlib,importlib.util,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--fixture',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
spec=importlib.util.spec_from_file_location('sd_initialization_prepare',ROOT/'apps/storage-read/prepare.py');prep=importlib.util.module_from_spec(spec);spec.loader.exec_module(prep)
original=json.loads((a.fixture/'admission.json').read_text());template=(a.fixture/'session-template.bin').read_bytes()
initial=json.loads(Path(original['initial']['path']).read_bytes())
a.output.mkdir(parents=True,exist_ok=False)

def run(name,fact,accepted):
    record=copy.deepcopy(original);raw=bytearray(template);data=json.dumps(fact,sort_keys=True).encode()
    record['initial']['sha256']=hashlib.sha256(data).hexdigest();raw[632:664]=hashlib.sha256(data).digest()
    th=hashlib.sha256(raw).hexdigest();record['session_template_sha256']=th
    try:
        prep.validate_admission(record,record['image_sha256'],th,raw,{record['initial']['path']:data})
        assert accepted,name
    except ValueError:
        assert not accepted,name
    return dict(case=name,accepted=accepted)
new={**initial,'initialization_mode':'cmd0-once','cold_sd0_controller':False,'cold_card_protocol':False,
 'native_sd_33v_signalling':True,'no_failed_native_lifetime':True,
 'require_controller_reset_baseline':True,'authorize_one_cmd0_initialization':True}
results=[run('existing-cold-authority',initial,True),run('first-cmd0-attempt',new,True)]
for key in ('native_sd_33v_signalling','no_failed_native_lifetime','require_controller_reset_baseline',
            'authorize_one_cmd0_initialization','all_sd0_clients_excluded','irq193_owned_disabled_drained','no_dma_queue_encryption'):
    bad={**new,key:False};results.append(run('missing-'+key,bad,False))
results.append(run('unproved-cold-without-policy',{**initial,'cold_card_protocol':False},False))
results.append(run('mixed-cold-and-cmd0',{**new,'cold_card_protocol':True},False))
results.append(run('unsupported-policy',{**new,'initialization_mode':'retry'},False))
(a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'modeled_authority_only':True,'cases':results,'source_sha256':hashlib.sha256((ROOT/'apps/storage-read/prepare.py').read_bytes()).hexdigest()},indent=2)+'\n')
print('PASS: SD initialization policy, ownership and evidence/session binding; no device access.')
