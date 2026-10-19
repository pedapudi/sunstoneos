#!/usr/bin/env python3
"""Check the SS source-capacity bound on a copied native-model session, offline."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--session',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    spec=importlib.util.spec_from_file_location('ss_prepare',ROOT/'apps/board/prepare.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    session=bytearray(a.session.read_bytes());assert m.session_check(session)==6
    path='/model-only/ss-capacity.json';record={'superspeed':{'path':path}}
    passed=[]
    for name,source,capacity,accepted in [('900',900,900,True),('1500',1500,1500,True),('underqualified',1500,900,False),('under900',500,900,False),('boolean',900,True,False),('string',900,'900',False),('missing',900,None,False)]:
        struct.pack_into('<I',session,552,source);cache={path:json.dumps({'qualified_source_ma':capacity}).encode()}
        try:m.superspeed_source_check(record,cache,session)
        except ValueError:assert not accepted,name
        else:assert accepted,name
        passed.append(name)
    try:m.superspeed_source_check({}, {},session)
    except ValueError:passed.append('missing-record')
    else:raise AssertionError('missing physical record accepted')
    a.output.mkdir(parents=True,exist_ok=False);(a.output/'session-input.bin').write_bytes(a.session.read_bytes())
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=passed,session_sha256=hashlib.sha256(a.session.read_bytes()).hexdigest(),physical_access=False,scope='Only source-capacity helper after existing authenticated evidence snapshot boundary. Model capacity values are not physical admission and no prepared bundle is created.'),indent=2)+'\n')
if __name__=='__main__':main()
