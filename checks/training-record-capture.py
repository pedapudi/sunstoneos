#!/usr/bin/env python3
"""Build both real reference records and reject changed capture identities.

Inputs are preserved ten-lifetime analysis, captures and pinned boot0.
Faults replace reads in memory; the original evidence is never changed.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import shutil
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--restore-set',type=Path,required=True)
p.add_argument('--boot0',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('training',ROOT/'tools/training-record.py')
t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
analysis=t.restore_set(a.restore_set)
references=[Path(c['path']) for c in analysis['captures'] if c['training_kind']=='pstate0'
            and ('board-session-20261025-27/' in c['path'] or 'board-session-20261025-35/' in c['path'])
            and c['path'].endswith('capture-1')]
assert len(references)==2
results=[]
for directory in references:
    source=json.loads((directory/'session.json').read_text())
    record=t.build(t.from_capture(directory,a.restore_set,a.boot0,1))
    parsed=t.parse(record)
    assert len(record)<=t.RECORD_LIMIT
    assert parsed['chip_id']==list(struct.unpack('<4I',(directory/'stage1-identity.bin').read_bytes()[:16]))
    assert struct.pack('<40H',*parsed['runs'][0]['values'])==struct.pack('<20I',*[int(w,16) for w in source['parameter_block'][32:52]])
    assert parsed['boot0_checksum']==0x2fce7bb3 and parsed['strap_crc']==0xf874a6a5
    assert parsed['temperature_mc']==-2147483648 and parsed['temperature_limit_mc']==0
    name='record-'+source['port']+'.bin';(a.output/name).write_bytes(record)
    results.append({'case':source['port'],'bytes':len(record),'sha256':hashlib.sha256(record).hexdigest(),'pass':True})

ref=references[0];session=json.loads((ref/'session.json').read_text())
real_read=Path.read_bytes;real_text=Path.read_text
cases={'sid':'Capture SID differs','stage-table':'Capture stage tables differ',
       'training-kind':'Capture training kind',
       'boot0':'boot0 differs','restore-set':'Restore set differs','incomplete':'Capture is incomplete'}
for case,message in cases.items():
    def read(path,*args,**kwargs):
        data=bytearray(real_read(path,*args,**kwargs))
        if ((case=='sid' and path==ref/'stage1-identity.bin') or
            (case=='boot0' and path==a.boot0) or (case=='restore-set' and path==a.restore_set)):
            data[0]^=1
        return bytes(data)
    def read_text(path,*args,**kwargs):
        text=real_text(path,*args,**kwargs)
        if path==ref/'session.json':
            data=json.loads(text)
            if case=='stage-table':data['manifest']['stages'][0]['entries'][0]['address']+=4
            if case=='incomplete':data['stages'].pop()
            return json.dumps(data)
        if case=='training-kind' and path==Path(session['ddr_evidence'])/'session.json':
            data=json.loads(text);data['manifest']['variant']='init-only';return json.dumps(data)
        return text
    with patch.object(Path,'read_bytes',read),patch.object(Path,'read_text',read_text):
        try:t.from_capture(ref,a.restore_set,a.boot0,1)
        except ValueError as error:assert message in str(error),(case,str(error))
        else:raise AssertionError('accepted '+case)
    results.append({'case':case,'pass':True})
# A capture may be moved and need not be listed in the historical analysis.
relocated = a.output / 'relocated-capture'
shutil.copytree(ref, relocated)
relocated_spec = t.from_capture(relocated, a.restore_set, a.boot0, 1)
assert t.build(relocated_spec) == t.build(t.from_capture(ref, a.restore_set, a.boot0, 1))
results.append({'case': 'relocated-capture', 'pass': True})
# A fresh capture supplies trained values; historical value equality is not an input contract.
stage = session['manifest']['stages'][3]
address = next(address for address, mask in t.profile(analysis) if address >= 0x0a920000)
entry = next(e for e in stage['entries'] if e['address'] <= address < e['address']+e['count']*e['width'])
path = relocated / session['stages'][3]['output']
data = bytearray(path.read_bytes()); data[entry['offset']+address-entry['address']] ^= 1; path.write_bytes(data)
assert t.from_capture(relocated, a.restore_set, a.boot0, 1) != relocated_spec
# New capture runs record live training state without a previous session path.
state = json.loads((relocated / 'session.json').read_text())
state.update(ddr_evidence=None, ddr_variant='pstate0', ddr_result={'dram_mib': 12288, 'board_result': 0})
(relocated / 'session.json').write_text(json.dumps(state))
t.from_capture(relocated, a.restore_set, a.boot0, 1)
results.append({'case': 'fresh-live-capture', 'pass': True})
(a.output/'results.json').write_text(json.dumps({'physical_access':False,'cases':results},indent=2)+'\n')
print('PASS capture builder: both board records preserve full parameters; six identity/data refusals, relocated and fresh live captures.')
