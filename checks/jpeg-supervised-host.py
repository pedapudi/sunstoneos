#!/usr/bin/env python3
"""Focused JPEG host checks; no device discovery, FEL, native build or transport replay."""
import argparse
from contextlib import ExitStack
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sqlite3
import struct
from unittest.mock import patch
import types
ROOT=Path(__file__).resolve().parents[1]
def load(path):
    spec=importlib.util.spec_from_file_location('jpeg_host_checks',path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
class Clock:
    def __init__(self):self.value=100.
    def now(self):return self.value
    def pause(self,n):self.value+=max(.001,n)
def frame(session,status=1):
    output=bytes([128])*2304 if status==1 else b''
    return struct.pack('<4I2Q',0x4745504a,1,status,len(output),*struct.unpack_from('<2Q',session,24))+session[768:832]+(hashlib.sha256(output).digest() if output else bytes(32))+output

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle',type=Path,required=True,help='Existing offline packaging-model bundle; never deployed')
    parser.add_argument('--evidence',type=Path,required=True);args=parser.parse_args();e=args.evidence.resolve();e.mkdir(parents=True,exist_ok=True)
    host=load(ROOT/'tools/check-a733-jpeg-supervised.py');shared=host.supervisor
    # Native packaging geometry uses placeholder hashes; derive only this host
    # fixture's diagnostic identities, preserving the original software evidence.
    source_bundle=args.bundle.resolve();model=e/'host-model'
    if not model.exists():
        shutil.copytree(args.bundle,model,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
        metadata=json.loads((model/'prepared.json').read_bytes())
        for name in ('session-template.bin','session.bin'):
            value=bytearray((model/name).read_bytes());value[768:800]=bytes.fromhex(host.FIXTURE_SHA256)
            value[800:832]=hashlib.sha256(host.REFERENCE).digest();(model/name).write_bytes(value)
        segments,address,identity=host.preparation.base.image_parts(model/'image.elf')
        request,contract=host.preparation.request_bytes(address,identity,(model/'session.bin').read_bytes(),metadata['epoch'],metadata['generation'],metadata['lease'])
        (model/'request.bin').write_bytes(request);(model/'contract.bin').write_bytes(contract)
        metadata['files']={name:host.digest(model/name) for name in metadata['files']}
        shared.save_json(model/'prepared.json',metadata)
    args.bundle=model
    def fixture_freeze(bundle):
        # The preserved native model has no physical authority. Model only that
        # boundary; actual metadata, ELF, session and upload validation still run.
        with patch.object(host.preparation,'validate_admission',lambda *a,**kw:{}):
            return host.freeze_bundle(bundle)
    setup_errors=[]
    try:
        raw=(model/'session.bin').read_bytes();host.preparation.session_check(raw)
    except (ValueError,OSError,struct.error) as error:setup_errors.append('session size/type: '+str(error))
    try:
        assert raw[768:800].hex()==host.FIXTURE_SHA256 and raw[800:832]==hashlib.sha256(host.REFERENCE).digest()
    except (AssertionError,NameError):setup_errors.append('fixed fixture/reference hashes differ')
    try:
        metadata=json.loads((model/'prepared.json').read_bytes())
        segments,address,identity=host.preparation.base.image_parts(model/'image.elf')
        expected=host.preparation.request_bytes(address,identity,raw,metadata['epoch'],metadata['generation'],metadata['lease'])
        for name,value in zip(('request.bin','contract.bin'),expected):
            if (model/name).read_bytes()!=value:setup_errors.append('regenerated '+name+' differs')
    except (ValueError,OSError,KeyError,struct.error) as error:setup_errors.append('request/contract: '+str(error))
    try:
        if json.loads((model/'admission.json').read_bytes())!={}:setup_errors.append('unexpected modeled physical authority')
        original=fixture_freeze(model)
    except (ValueError,OSError,KeyError,struct.error) as error:setup_errors.append('bundle geometry/authority boundary: '+str(error))
    shared.save_json(e/'setup.json',{'physical_access':False,'model':str(model),'mismatches':setup_errors,
        'admission_boundary':'Explicit test-only validate_admission mock; empty record can never authorize physical entry.',
        'session_bytes':len(raw) if 'raw' in locals() else None,'fixture_sha256':host.FIXTURE_SHA256,
        'reference_sha256':hashlib.sha256(host.REFERENCE).hexdigest(),
        'metadata_sha256':host.digest(model/'prepared.json')})
    assert not setup_errors,'JPEG fixture setup mismatches: '+'; '.join(setup_errors)

    sources=[Path(__file__),*host.software_sources().values()]
    identities={str(p):host.digest(p) for p in dict.fromkeys(sources)}
    fingerprint=hashlib.sha256(json.dumps({'sources':identities,'bundle_files':original['files'],'metadata':original['_prepared_sha256']},sort_keys=True).encode()).hexdigest();state_path=e/'results.json'
    state=json.loads(state_path.read_text()) if state_path.exists() else {'physical_access':False,'cases':{},'source_identity':fingerprint,'sources':identities,'physical_admission_boundary':'Explicit fixture mock; admission.json={} is rejected by production validation.'}
    if state['source_identity']!=fingerprint:
        continuation=state.get('fixture_continuation')
        if continuation is None:
            assert set(state['cases'])=={'model-authority-refusal'},'Only the sole unaffected authority checkpoint may be reused.'
            assert all(identities[path]==identity for path,identity in state['sources'].items() if path!=str(Path(__file__)))
            (e/'original-source-manifest.json').write_bytes(state_path.read_bytes())
            state['cases']['model-authority-refusal']['input_identity']=state['source_identity']
            state['cases']['model-authority-refusal']['applies_to']={
                'original_model':str(source_bundle),'admission_sha256':host.digest(source_bundle/'admission.json'),
                'boundary':'Original admission={} refused unmocked; unchanged by diagnostic identity derivation.'}
            state['fixture_continuation']={'input_identity':fingerprint,'sources':identities,
                'derived_metadata_sha256':original['_prepared_sha256'],'derived_files':original['files'],
                'reason':'Only fixture/reference hashes corrected in copied model; request/contract regenerated. Failed capture attempt retained. Production source, native image and launcher unchanged.'}
            shared.save_json(state_path,state)
        else:assert continuation['input_identity']==fingerprint,'Changed resumed inputs.'

    inputs=e/'inputs'
    if not inputs.exists():
        for i,p in enumerate(dict.fromkeys(sources)):
            inputs.mkdir(exist_ok=True);shutil.copy2(p,inputs/(str(i)+'-'+p.name))
    session=original['_frozen']['session.bin']
    def case(name,operation):
        if name in state['cases']:return
        attempt=1
        while (e/(name+'-'+str(attempt))).exists():attempt+=1
        directory=e/(name+'-'+str(attempt));directory.mkdir()
        result=operation(directory);state['cases'][name]={'result':'pass','input_identity':fingerprint,**(result or {})};shared.save_json(state_path,state)
    def refused(operation):
        try:operation()
        except (ValueError,RuntimeError,OSError):return
        raise AssertionError('Expected refusal')
    case('model-authority-refusal',lambda d:refused(lambda:host.freeze_bundle(args.bundle.resolve())))
    def capture_case(directory,variation):
        clock=Clock();calls=[];received=[];committed=[];record={'complete':False};pending=[False]
        def exchange(packet,timeout):
            assert packet==bytes(1) and 0<timeout<=500;calls.append(packet)
            raw=frame(session,2 if variation=='failure' else 1)
            if variation=='pending-success' and not pending[0]:raw=frame(session,0);pending[0]=True
            if variation=='stale':
                value=bytearray(raw);struct.pack_into('<Q',value,16,99);raw=bytes(value)
            if variation=='malformed':raw=raw[:-1]
            if variation=='poison':raw=raw[:-1]+bytes([raw[-1]^1])
            if variation=='late':clock.value=200
            if variation=='read-failure':raise OSError('Fixture read failure')
            received.append(raw);return raw
        def save():
            if variation=='save-failure' and record.get('jpeg_result')=='observed-success':raise OSError('Fixture final save failure')
            committed.append(copy.deepcopy(record))
        error=None
        try:host.capture(exchange,session,directory,record,save,150,now=clock.now,pause=clock.pause)
        except (ValueError,RuntimeError,OSError) as caught:error=caught
        good=variation in ('pending-success','failure')
        assert bool(error)!=good and len(calls)==(2 if variation=='pending-success' else 1)
        assert record['jpeg_retention']=='whole-board-power-removal' and not record['complete']
        if variation=='pending-success':
            assert record['jpeg_result']=='observed-success' and (directory/'output.yu12').read_bytes()==host.REFERENCE
        elif variation=='failure':assert record['jpeg_result']=='observed-failure'
        else:
            assert record['jpeg_result']=='unobserved' and 'durable_result' not in record
            assert not any(r.get('jpeg_result')=='observed-success' for r in committed)
        if variation=='save-failure':assert (directory/'output.yu12').read_bytes()==host.REFERENCE and 'copied_jpeg_output' not in record
        for i,raw in enumerate(received,1):assert (directory/f'jpeg-{i:03}.bin').read_bytes()==raw
        return {'requests':len(calls),'observed':record['jpeg_result'],'error':str(error) if error else None}
    for variation in ('pending-success','failure','stale','malformed','poison','late','read-failure','save-failure'):
        case('capture-'+variation,lambda d,v=variation:capture_case(d,v))
    def transfer(directory,variation):
        clock=Clock();calls=[];actual=host.capture
        def write(endpoint,packet,timeout):
            assert endpoint==2 and packet==bytes(1);calls.append('write')
            if variation=='write-failure':raise OSError('Fixture write failure')
            if variation=='late':clock.value=200
            return 0 if variation=='partial' else 1
        def read(endpoint,length,timeout):
            calls.append('read');assert endpoint==0x81 and length==2432
            return frame(session)
        device=types.SimpleNamespace(write=write,read=read)
        with patch.object(host,'capture',lambda *a,**kw:actual(*a,**kw,now=clock.now,pause=clock.pause)),patch.object(host.time,'monotonic',clock.now):
            operation=lambda:host.capture_device(device,lambda:None,150,session,directory,{},lambda:None)
            if variation=='success':operation()
            else:refused(operation)
        assert calls==(['write','read'] if variation=='success' else ['write']);return {'calls':calls}
    for variation in ('success','partial','late','write-failure'):case('transfer-'+variation,lambda d,v=variation:transfer(d,v))
    def binding(directory):
        bundle=directory/'bundle';shutil.copytree(args.bundle,bundle,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
        prepared=fixture_freeze(bundle)
        def ref(p):return {'path':str(p.resolve()),'sha256':host.digest(p)}
        proof=directory/'fixture.txt';proof.write_text('Software fixture only; establishes no physical facts.\n')
        software={'schema':'a733-jpeg-supervised-software-v1','physical_access':False,
            **dict.fromkeys(('admission_cases','launcher_cases','host_capture','host_lifecycle'),True),
            'image':ref(bundle/'image.elf'),'launcher':ref(bundle/'launch.bin'),
            'sources':{name:ref(path) for name,path in host.software_sources().items()},'evidence':[ref(proof)]}
        software_path=directory/'software.json';shared.save_json(software_path,software)
        refs={'supervised-ram-recovery':ref(bundle/'admission.json'),'jpeg-supervised-software':ref(software_path)}
        submission={'prerequisites':list(refs),'artifacts':[ref(bundle/n) for n in ('prepared.json','image.elf','launch.bin')]}
        dbpath=directory/'queue.db';db=sqlite3.connect(dbpath)
        db.execute('CREATE TABLE settings(key TEXT,value TEXT)');db.execute('INSERT INTO settings VALUES(?,?)',('halted',''))
        db.execute('CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT)')
        db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('fixture','claimed','owner',json.dumps(submission),json.dumps({'prerequisites':refs})));db.commit()
        host.claimed(dbpath,'fixture','owner',bundle,prepared);refused(lambda:host.claimed(dbpath,'fixture','wrong',bundle,prepared))
        for name in ('prepared.json','admission.json','image.elf','launch.bin'):
            path=bundle/name;raw=path.read_bytes()
            try:
                path.write_bytes(raw+b'rebound')
                altered=copy.deepcopy(software)
                if name in ('image.elf','launch.bin'):altered['image' if name=='image.elf' else 'launcher']=ref(path)
                shared.save_json(software_path,altered);refs['supervised-ram-recovery']=ref(bundle/'admission.json');refs['jpeg-supervised-software']=ref(software_path)
                submission['artifacts']=[ref(bundle/n) for n in ('prepared.json','image.elf','launch.bin')]
                db.execute('UPDATE tests SET submission=?,review=?',(json.dumps(submission),json.dumps({'prerequisites':refs})));db.commit()
                refused(lambda:host.claimed(dbpath,'fixture','owner',bundle,prepared))
            finally:path.write_bytes(raw)
        altered=copy.deepcopy(software);name=next(iter(altered['sources']));altered['sources'][name]=ref(proof)
        refused(lambda:host.validate_software(altered,prepared));db.close()
        return {'positive':True,'wrong_owner':True,'rebound_files':4,'mutated_source':True}
    case('queue-frozen-binding',binding)
    def frozen(directory):
        bundle=directory/'bundle';shutil.copytree(args.bundle,bundle,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
        prepared=fixture_freeze(bundle);snapshot=copy.deepcopy(prepared['_frozen'])
        for name in ('session.bin','admission.json'):
            path=bundle/name;raw=path.read_bytes()
            try:path.write_bytes(raw+b'changed');refused(lambda:fixture_freeze(bundle))
            finally:path.write_bytes(raw)
        assert prepared['_frozen']==snapshot and prepared['_prepared_sha256']==host.digest(bundle/'prepared.json')
        return {'retained_initial_bytes':True}
    case('initial-snapshot',frozen)
    def routing(directory):
        calls=[]
        def runner(settings,bundle,prepared,ready,validate_entry,capture_device,**options):
            assert options['product']==host.PRODUCT and options['serial']==host.SERIAL and options['bcd_device']==0x0100
            assert options['capture_seconds']==59 and options['result_key']=='jpeg_result'
            assert prepared['_frozen']['session.bin']==session;calls.append(options['result_key']);return 0
        argv=['--bundle',str(args.bundle),'--fel',str(directory/'never-executed'),'--evidence',str(directory/'unused'),
            '--database',str(directory/'never-opened'),'--claim','fixture','--owner','fixture','--supervised-manual-reconnect']
        with patch.object(shared,'run_session',runner),patch.object(host.preparation,'validate_admission',lambda *a,**kw:{}):assert host.main(argv)==0
        assert calls==['jpeg_result'];return {'shared_transport':'Reused prior cases; only new callback routing checked.'}
    case('main-route',routing)
    shared.save_json(state_path,state);print(json.dumps({'cases':len(state['cases']),'results':str(state_path),'physical_access':False}))
if __name__=='__main__':main()
