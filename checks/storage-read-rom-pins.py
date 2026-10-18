#!/usr/bin/env python3
"""Check exact inherited ROM pins and their sealed read-only handoff authority."""
import argparse,copy,hashlib,importlib.util,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--fixture',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
s=importlib.util.spec_from_file_location('sd_rom_prepare',ROOT/'apps/storage-read/prepare.py');prep=importlib.util.module_from_spec(s);s.loader.exec_module(prep)
record=json.loads((a.fixture/'admission.json').read_text());template=bytearray((a.fixture/'session-template.bin').read_bytes())
fact=json.loads(Path(record['initial']['path']).read_bytes());fact.update(initialization_mode='cmd0-once',cold_sd0_controller=False,cold_card_protocol=False,native_sd_33v_signalling=True,no_failed_native_lifetime=True,require_controller_reset_baseline=True,authorize_one_cmd0_initialization=True,rom_jtag_handoff_authorized=True,jtag_debug_clients_excluded=True)
for i,fn in enumerate((3,3,15,3,15,3,15)):template[792+3*i]=fn
results=[]
for case,changed,pin,accepted in [('authorized',{},None,True),('debug-client-not-excluded',{'jtag_debug_clients_excluded':False},None,False),('not-authorized',{'rom_jtag_handoff_authorized':False},None,False),('cold-policy',{'initialization_mode':'cold','cold_sd0_controller':True,'cold_card_protocol':True},None,False),('unexpected-uart',{},2,False)]:
 raw=bytearray(template);r=copy.deepcopy(record);data=json.dumps({**fact,**changed},sort_keys=True).encode()
 if pin is not None:raw[792+3*pin]=3
 raw[632:664]=hashlib.sha256(data).digest();h=hashlib.sha256(raw).hexdigest();r['initial']['sha256']=hashlib.sha256(data).hexdigest();r['session_template_sha256']=h
 try:prep.session_check(raw);prep.validate_admission(r,r['image_sha256'],h,raw,{r['initial']['path']:data});assert accepted,case
 except ValueError:assert not accepted,case
 if accepted:(a.output/'MODEL-session-template.bin').write_bytes(raw)
 results.append(dict(case=case,accepted=accepted))
(a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results,source_sha256=hashlib.sha256((ROOT/'apps/storage-read/prepare.py').read_bytes()).hexdigest()),indent=2)+'\n')
print('PASS exact ROM pin policy and bound authority; model only.')
