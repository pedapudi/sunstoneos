#!/usr/bin/env python3
"""Check direct SD target input and native initialization limits without process records."""
import argparse,copy,hashlib,importlib.util,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--fixture',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
spec=importlib.util.spec_from_file_location('sd_initialization_prepare',ROOT/'apps/storage-read/prepare.py');prep=importlib.util.module_from_spec(spec);spec.loader.exec_module(prep)
template=(a.fixture/'session-template.bin').read_bytes()
a.output.mkdir(parents=True,exist_ok=False)
if not __debug__: raise SystemExit('checks require assertions')
record={'identity':'software-only-model','port':'no-physical-port','reservation':100}
assert prep.validate_admission(record,'unused','unused',template)=={}
import struct
results=[]
for name,offset,fmt,value in (
    ('unsupported-profile',860,'<I',4),('zero-read-count',872,'<I',0),
    ('range-overflow',864,'<Q',0xffffffff),('invalid-voltage',856,'<I',0x80000000),
    ('unbounded-identify',840,'<Q',10000001),('unbounded-read',848,'<Q',5000001),
    ('invalid-data-pin',813,'<B',15),('invalid-pull',817,'<B',15)):
    raw=bytearray(template);struct.pack_into(fmt,raw,offset,value)
    try:prep.validate_admission(record,'unused','unused',raw)
    except ValueError as error:results.append(dict(case=name,reason=str(error)))
    else:raise AssertionError(name)
(a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,
    'process_records_required':False,'native_limit_refusals':results},indent=2)+'\n')
print('PASS direct SD target and native range, timeout, voltage and pin limits')
