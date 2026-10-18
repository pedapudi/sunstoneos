#!/usr/bin/env python3
"""Check direct review-to-frozen-bundle identity for observer and board adapters.

Fixtures model already validated bytes. Native image/protocol/transport checks
are reused; this script never imports USB, invokes FEL, or acquires a board lock.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def sha(data):return hashlib.sha256(data).hexdigest()
def ref(path):return {'path':str(path.resolve()),'sha256':sha(path.read_bytes())}

def checks(host,kind,directory):
    bundle=directory/'bundle';bundle.mkdir()
    for name in ('image.elf','launch.bin','admission.json'):
        (bundle/name).write_bytes(('SOFTWARE FIXTURE: '+name+'; never executable or physical evidence.\n').encode())
    initial={'files':{name:ref(bundle/name)['sha256'] for name in ('image.elf','launch.bin','admission.json')}}
    metadata=json.dumps(initial,sort_keys=True).encode();(bundle/'prepared.json').write_bytes(metadata)
    prepared={**initial,'_prepared_sha256':sha(metadata)}
    proof=directory/'fixture.txt';proof.write_text('Software-only approval fixture; establishes no hardware facts.\n')
    shared=host if kind=='observer' else host.supervisor
    software={'schema':'a733-smp-supervised-software-v1' if kind=='observer' else 'a733-board-supervised-software-v1',
        'physical_access':False,**dict.fromkeys(('admission_cases','launcher_cases','host_lifecycle','host_capture'),True),
        'image':ref(bundle/'image.elf'),'launcher':ref(bundle/'launch.bin'),'evidence':[ref(proof)]}
    if kind=='observer':
        sources={'host_tool':Path(host.__file__),'base_host_tool':Path(host.base.__file__),'observer_host_tool':Path(host.observer.__file__)}
        software.update({key:ref(path) for key,path in sources.items()})
    else:
        sources=dict(host.source_files());sources.update({'supervised-host-tool.py':Path(shared.__file__),
            'base-host-tool.py':Path(shared.base.__file__),'observer-host-tool.py':Path(shared.observer.__file__)})
        software['sources']={key:ref(path) for key,path in sources.items()}
    software_path=directory/'software.json';shared.save_json(software_path,software)
    software_key='supervised-observer-software' if kind=='observer' else 'board-supervised-software'
    prerequisites={'supervised-ram-recovery':ref(bundle/'admission.json'),software_key:ref(software_path)}
    submission={'prerequisites':list(prerequisites),'artifacts':[ref(bundle/n) for n in ('prepared.json','image.elf','launch.bin')]}
    database=directory/'queue.db';db=sqlite3.connect(database)
    db.execute('CREATE TABLE settings(key TEXT,value TEXT)');db.execute('INSERT INTO settings VALUES(?,?)',('halted',''))
    db.execute('CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT)')
    db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('fixture','claimed','fixture-owner',json.dumps(submission),json.dumps({'prerequisites':prerequisites})));db.commit()
    def claim():
        args=(database,'fixture','fixture-owner',bundle)
        return host.claimed(*args,bundle/'admission.json',prepared) if kind=='observer' else host.claimed(*args,prepared)
    results={}
    claim();results['unchanged-frozen-claim']='pass'
    expected={**prepared['files'],'prepared.json':prepared['_prepared_sha256']}
    real_digest=host.digest
    for name in ('prepared.json','admission.json','image.elf','launch.bin'):
        path=bundle/name;original=path.read_bytes();changed_software=copy.deepcopy(software)
        try:
            path.write_bytes(original+b'changed after initial validation\n')
            if name in ('image.elf','launch.bin'):changed_software['image' if name=='image.elf' else 'launcher']=ref(path)
            shared.save_json(software_path,changed_software)
            prerequisites['supervised-ram-recovery']=ref(bundle/'admission.json');prerequisites[software_key]=ref(software_path)
            submission['artifacts']=[ref(bundle/n) for n in ('prepared.json','image.elf','launch.bin')]
            db.execute('UPDATE tests SET submission=?,review=?',(json.dumps(submission),json.dumps({'prerequisites':prerequisites})));db.commit()
            # Model replacement after the preliminary scan. Real artifact()
            # validation still sees current bytes and rebound review hashes.
            def scanned_digest(p):
                p=Path(p)
                return expected[p.name] if p.parent==bundle and p.name in expected else real_digest(p)
            with patch.object(host,'digest',scanned_digest):
                try:claim()
                except (ValueError,RuntimeError) as error:results['rebound-'+name]={'result':'pass','refusal':str(error)}
                else:raise AssertionError('Accepted changed frozen '+name)
        finally:path.write_bytes(original)
    db.close();return {'cases':results,'initial_prepared_sha256':prepared['_prepared_sha256'],
        'source_identities':{str(path):sha(path.read_bytes()) for path in sources.values()}}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--evidence',type=Path,required=True);a=parser.parse_args()
    evidence=a.evidence.resolve();evidence.mkdir(parents=True,exist_ok=False)
    state={'physical_access':False,'runner_sha256':sha(Path(__file__).read_bytes()),'consumers':{}}
    for kind,name in [('observer','check-a733-smp-supervised.py'),('board','check-a733-board-supervised.py')]:
        directory=evidence/kind;directory.mkdir();host=load('binding_'+kind,ROOT/'tools'/name)
        state['consumers'][kind]=checks(host,kind,directory)
        shared=host if kind=='observer' else host.supervisor
        shared.save_json(evidence/'results.json',state)
    print(json.dumps({'binding_cases':10,'physical_access':False,'results':str(evidence/'results.json')}))
if __name__=='__main__':main()
