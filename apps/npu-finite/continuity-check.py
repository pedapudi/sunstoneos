#!/usr/bin/env python3
"""Check actual captured continuity and refuse incomplete, corrupt or restored-differently inputs."""
import argparse,hashlib,importlib.util,json,tempfile
from pathlib import Path
from unittest.mock import patch
p=argparse.ArgumentParser(description=__doc__)
for name in ('bundle','session','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('npu_continuity',Path(__file__).with_name('continuity.py'));c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
original=a.session.read_bytes();before=sha(a.session);cases=[]
result=c.derive(a.bundle,a.session,a.output/'continuity.json')
assert result['result']['status']==1 and result['npu_result']['functional_success'] is False
assert {k:v for k,v in result.items() if k not in ('controlled','continuity_derivation')}==json.loads(original)
cases.append({'case':'actual-captured-numerical-failure-restored-continuity','passed':True})
with tempfile.TemporaryDirectory(prefix='npu-continuity-negative-') as temporary:
    for variant in ('incomplete','restoration-differs','invalid-receipt'):
        path=a.session.resolve();changed=json.loads(original);changed['dram_restored']=False;raw=json.dumps(changed).encode()
        expected='Incomplete'
        if variant=='restoration-differs':path=a.session.parent/'DRAM-restored.bin';raw=path.read_bytes();raw=bytes([raw[0]^1])+raw[1:];expected='Full DRAM'
        if variant=='invalid-receipt':path=a.session.parent/'receipt.bin';raw=path.read_bytes();raw=bytes([raw[0]^1])+raw[1:];expected='Receipt identity'
        output=Path(temporary)/variant
        with patch.object(c,'read_input',lambda p,at=path.resolve(),value=raw:value if p==at else p.read_bytes()):
            try:c.derive(a.bundle,a.session,output)
            except ValueError as error:assert expected in str(error),(variant,str(error))
            else:raise AssertionError(variant+' accepted')
        assert not output.exists();cases.append({'case':variant,'passed':True})
assert sha(a.session)==before
(a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'original_session':{'path':str(a.session.resolve()),'sha256':before},'producer_sha256':sha(Path(__file__).with_name('continuity.py')),'checker_sha256':sha(Path(__file__)),'derived_sha256':sha(a.output/'continuity.json'),'cases':cases,'scope':'Actual frozen bundle and captured reports/restoration validated read-only. Refusals inject bytes only into the offline reader; no original file edits, device operations or queue authority.'},indent=2)+'\n')
print('PASS continuity:',len(cases),'cases')
