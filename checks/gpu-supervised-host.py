#!/usr/bin/env python3
"""Only the GPU supervised host adapter; every device and transport is a fixture."""
import argparse
import copy
import hashlib
import importlib.util
import inspect
import json
from pathlib import Path
import shutil
import sqlite3
import struct
import types
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def load(path):
    spec=importlib.util.spec_from_file_location('gpu_host_adapter_checks',path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
class Clock:
    def __init__(self):self.value=100.
    def now(self):return self.value
    def pause(self,n):self.value+=max(.001,n)
def session():
    raw=bytearray(592);struct.pack_into('<6Q',raw,0,0x4153373342535332,2,592,31,37,41)
    struct.pack_into('<I',raw,400,2);struct.pack_into('<2Q',raw,456,20,21);return bytes(raw)
def frame(chunk,state=1,epoch=31):
    return struct.pack('<7IQI',0x52555047,1,state,chunk,64 if state==1 else 0,64,64,epoch,4096 if state==1 else 0)+(bytes.fromhex('2080c0ff')*64 if state==1 else b'')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--image',type=Path,required=True,help='Existing reviewed GPU ELF; never rebuilt or executed')
    a=parser.parse_args();out=a.evidence;out.mkdir(parents=True,exist_ok=True)
    host=load(ROOT/'tools/check-a733-gpu-supervised.py');sup=host.supervisor
    sources=[Path(__file__),Path(host.__file__),Path(sup.__file__),Path(sup.base.__file__),Path(sup.observer.__file__),Path(host.preparation.__file__),Path(host.preparation.board.__file__),Path(host.decoder.__file__)]
    identities={str(p.relative_to(ROOT)):host.digest(p) for p in sources}
    state_path=out/'results.json';state=json.loads(state_path.read_text()) if state_path.exists() else {'physical_access':False,'cases':{}}
    def case(name,fn):
        fingerprint=hashlib.sha256((inspect.getsource(fn)+inspect.getsource(Clock)+inspect.getsource(session)+inspect.getsource(frame)+json.dumps(identities,sort_keys=True)+host.digest(a.image)).encode()).hexdigest()
        if name in state['cases']:
            assert state['cases'][name]['inputs']==fingerprint,'Changed completed inputs: '+name
            return
        d=out/name
        if d.exists():d=out/(name+'-resume-'+str(len(list(out.glob(name+'*')))))
        d.mkdir();result=fn(d);state['cases'][name]={'inputs':fingerprint,'result':'pass',**result};sup.save_json(state_path,state)
    snapshot=out/('inputs-'+str(len(list(out.glob('inputs-*')))+1))
    for p in sources:
        target=snapshot/p.relative_to(ROOT);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)

    def captures(d,variation):
        clock=Clock();calls=[];received=[];committed=[];record={'gpu_result':'unobserved','complete':False};pending=[False]
        def exchange(packet,timeout):
            chunk=struct.unpack('<I',packet)[0];calls.append(chunk);assert 0<timeout<=500
            data=frame(chunk)
            if variation=='pending-success' and not pending[0]:data=frame(0,0);pending[0]=True
            if variation=='wrong-epoch':data=frame(chunk,epoch=99)
            if variation=='wrong-chunk':data=frame(chunk+1)
            if variation=='poison':data=data[:40]+bytes([0xa5])*256
            if variation=='failure':data=frame(chunk,2)
            if variation=='short':data=b'partial'
            if variation=='pending-output':data=frame(chunk,0)+b'x'
            if variation=='regression' and chunk==1:data=frame(chunk,0)
            if variation=='late':clock.value=200
            if variation=='read-failure':raise OSError('Software fixture transport failure')
            received.append(data);return data
        def save():
            if variation=='save-failure' and record['gpu_result']=='observed-success':raise OSError('Software fixture journal failure')
            committed.append(copy.deepcopy(record));sup.save_json(d/'session.json',record)
        error=None
        try:host.capture(exchange,session(),d,record,save,150,now=clock.now,pause=clock.pause)
        except (ValueError,OSError,RuntimeError,struct.error) as ex:error=ex
        if variation=='pending-success':
            assert not error and calls==[0]+list(range(64)) and record['gpu_result']=='observed-success'
            assert (d/'rgba8.bin').read_bytes()==bytes.fromhex('2080c0ff')*4096
            assert host.digest(d/'rgba8.bin')==record['copied_gpu_output']['sha256']
        elif variation=='failure':assert not error and calls==[0] and record['gpu_result']=='observed-failure'
        else:
            assert error and calls==(list(range(64)) if variation=='save-failure' else [0,1] if variation=='regression' else [0])
            assert not any(x.get('gpu_result')=='observed-success' for x in committed)
            if variation=='save-failure':assert record['gpu_result']=='unobserved' and 'durable_result' not in record and (d/'rgba8.bin').exists()
        assert not record['complete']
        for i,data in enumerate(received,1):assert (d/f'gpu-{i:03}.bin').read_bytes()==data
        return {'requests':len(calls),'error':str(error) if error else None,'observed':record['gpu_result'],'limits':'Only copied fixture USB bytes; no GPU render or drain claim.'}
    for variation in ['pending-success','wrong-epoch','wrong-chunk','poison','failure','short','pending-output','regression','late','read-failure','save-failure']:
        case('capture-'+variation,lambda d,v=variation:captures(d,v))

    def transfers(d,variation):
        clock=Clock();calls=[];record={'complete':False};actual=host.capture
        def write(endpoint,packet,timeout):
            calls.append('write');assert endpoint==2 and packet==struct.pack('<I',0)
            if variation=='write-failure':raise OSError('Software fixture failed write')
            if variation=='expired-write':clock.value=200
            return 3 if variation=='short-write' else 4
        def read(endpoint,length,timeout):
            calls.append('read');assert endpoint==0x81 and length==296
            raise OSError('Software fixture failed read')
        device=types.SimpleNamespace(write=write,read=read)
        with patch.object(host.time,'monotonic',clock.now),patch.object(host,'capture',lambda *a,**kw:actual(*a,**kw,now=clock.now,pause=clock.pause)):
            try:host.capture_device(device,lambda:calls.append('same'),150,session(),d,record,lambda:None)
            except (ValueError,OSError):pass
            else:raise AssertionError('Invalid transfer accepted')
        assert calls==(['same','write','same','read'] if variation=='read-failure' else ['same','write','same'] if variation=='expired-write' else ['same','write'])
        assert not record['complete'];return {'calls':calls}
    for variation in ['short-write','write-failure','expired-write','read-failure']:
        case('transfer-'+variation,lambda d,v=variation:transfers(d,v))

    def bundle(d):
        prepared={'files':{}};raw=session();p=d/'session.bin';p.write_bytes(raw);prepared['files'][p.name]=host.digest(p)
        frozen=host.bundle_bytes(d,prepared,p.name);p.write_bytes(b'changed');assert frozen==raw
        try:host.bundle_bytes(d,prepared,p.name)
        except ValueError:pass
        else:raise AssertionError('Changed session accepted')
        return {'frozen_session':True}
    case('bundle-byte-binding',bundle)

    def software(d):
        shutil.copy2(a.image,d/'image.elf');(d/'launch.bin').write_bytes(b'Software fixture, never executable')
        def ref(path):return {'path':str(path.absolute()),'sha256':host.digest(path)}
        paths=dict(host.source_files());paths.update({'supervised-host-tool.py':Path(sup.__file__),'base-host-tool.py':Path(sup.base.__file__),'observer-host-tool.py':Path(sup.observer.__file__)})
        note=d/'model.txt';note.write_text('Software fixture only; never physical admission.\n')
        value={'schema':'a733-gpu-supervised-software-v1','physical_access':False,'admission_cases':True,'launcher_cases':True,'host_capture':True,'host_lifecycle':True,
            'image':ref(d/'image.elf'),'launcher':ref(d/'launch.bin'),'sources':{k:ref(v) for k,v in paths.items()},'evidence':[ref(note)]}
        prepared={'files':{n:host.digest(d/n) for n in ['image.elf','launch.bin']},'_source_hashes':{k:host.digest(v) for k,v in paths.items()}}
        host.require_embedded_firmware((d/'image.elf').read_bytes())
        host.validate_software(value,prepared)
        wrong=copy.deepcopy(value);wrong['sources']['gpu-host-tool.py']=ref(note)
        try:host.validate_software(wrong,prepared)
        except ValueError:pass
        else:raise AssertionError('Different host source accepted')
        # Change actual embedded firmware bytes in a copy of the retained ELF.
        from elftools.elf.elffile import ELFFile
        with (d/'image.elf').open('rb') as f:
            e=ELFFile(f);symbols={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()};first=symbols['gpu_firmware']
            offset=next(s['p_offset']+first-s['p_vaddr'] for s in e.iter_segments() if s['p_type']=='PT_LOAD' and s['p_vaddr']<=first<s['p_vaddr']+s['p_filesz'])
        data=bytearray((d/'image.elf').read_bytes());data[offset]^=1;(d/'image.elf').write_bytes(data)
        try:host.require_embedded_firmware((d/'image.elf').read_bytes())
        except ValueError:pass
        else:raise AssertionError('Different embedded firmware accepted')
        return {'validated':['exact source/executable records','actual pinned embedded firmware'],'refused':['changed adapter source','changed firmware']}
    case('software-image-firmware-binding',software)

    def queue(d):
        for name in ['admission.json','prepared.json','launch.bin']:(d/name).write_bytes(b'Software queue fixture '+name.encode())
        shutil.copy2(a.image,d/'image.elf')
        def ref(path):return {'path':str(path.absolute()),'sha256':host.digest(path)}
        paths=host.software_sources();note=d/'model.txt';note.write_text('Software queue model only; never physical admission.\n')
        software={'schema':'a733-gpu-supervised-software-v1','physical_access':False,'admission_cases':True,'launcher_cases':True,'host_capture':True,'host_lifecycle':True,
            'image':ref(d/'image.elf'),'launcher':ref(d/'launch.bin'),'sources':{k:ref(v) for k,v in paths.items()},'evidence':[ref(note)]}
        prepared={'files':{n:host.digest(d/n) for n in ['image.elf','launch.bin','admission.json']},
            '_prepared_sha256':host.digest(d/'prepared.json'),'_source_hashes':{k:host.digest(v) for k,v in paths.items()}}
        value=d/'software.json';value.write_text(json.dumps(software))
        prerequisites={'supervised-ram-recovery':ref(d/'admission.json'),'gpu-supervised-software':ref(value)}
        submission={'prerequisites':list(prerequisites),'artifacts':[ref(d/n) for n in ['prepared.json','image.elf','launch.bin']]};review={'prerequisites':prerequisites}
        database=d/'queue.sqlite';db=sqlite3.connect(database);db.executescript('CREATE TABLE settings(key TEXT,value INTEGER);INSERT INTO settings VALUES("halted",0);CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT);')
        db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('gpu-test','claimed','coordinator',json.dumps(submission),json.dumps(review)));db.commit()
        host.claimed(database,'gpu-test','coordinator',d,prepared)
        try:host.claimed(database,'gpu-test','another owner',d,prepared)
        except ValueError:pass
        else:raise AssertionError('Foreign claim accepted')
        db.execute('UPDATE settings SET value=1');db.commit()
        try:host.claimed(database,'gpu-test','coordinator',d,prepared)
        except ValueError:pass
        else:raise AssertionError('Halted queue accepted')
        # Replacement refs match every current replacement file, but not the
        # initial snapshots whose bytes would be uploaded by shared preload.
        db.execute('UPDATE settings SET value=0')
        (d/'image.elf').write_bytes(b'Replacement software-only image, never executed')
        (d/'prepared.json').write_bytes(b'Replacement software-only metadata')
        software['image']=ref(d/'image.elf');value.write_text(json.dumps(software))
        review['prerequisites']['gpu-supervised-software']=ref(value)
        submission['artifacts']=[ref(d/n) for n in ['prepared.json','image.elf','launch.bin']]
        db.execute('UPDATE tests SET submission=?,review=?',(json.dumps(submission),json.dumps(review)));db.commit()
        try:host.claimed(database,'gpu-test','coordinator',d,prepared)
        except ValueError:pass
        else:raise AssertionError('Replacement review authorized earlier upload bytes')
        db.close();return {'claim':'exact owner and frozen reviewed executable/admission identities','refused':['foreign owner','halted queue','post-validation replacement metadata/image with matching replacement refs']}
    case('queue-admission',queue)

    def wiring(d):
        bundle=d/'bundle';bundle.mkdir();raw=session()
        admission={'identity':'software-only-target','port':'software-only-port'};entry=b'Software-only prerequisite'
        values={'session.bin':raw,'session-template.bin':raw,'admission.json':json.dumps(admission).encode(),'entry.bin':entry,'image.elf':a.image.read_bytes()}
        for name,data in values.items():(bundle/name).write_bytes(data)
        prepared={'files':{n:host.digest(bundle/n) for n in values},'entry_evidence':{'original-evidence':{'name':'entry.bin'}},'image_sha256':host.digest(bundle/'image.elf'),'retention':'whole-board-power-removal'}
        (bundle/'prepared.json').write_text(json.dumps(prepared))
        calls=[]
        def validate(value,image_hash,template_hash,evidence,frozen):
            assert value==admission and image_hash==prepared['image_sha256'] and evidence=={'original-evidence':entry} and frozen
            calls.append('frozen-entry-check')
        def run(args,source,p,ready,validate_entry,capture,**kw):
            assert source==bundle.resolve() and p['_sizes']=={n:len(v) for n,v in values.items()}
            assert (kw['product'],kw['serial'],kw['bcd_device'],kw['capture_seconds'],kw['result_key'])==('sunstoneos native GPU','sunstoneos-a733-gpu',0x100,59,'gpu_result')
            ready();(bundle/'entry.bin').write_bytes(b'changed after snapshot')
            validate_entry(admission['identity'],admission['port'])
            try:validate_entry('another target',admission['port'])
            except ValueError:pass
            else:raise AssertionError('Different target accepted')
            assert 'whole-board power removal' in kw['description'];calls.append('shared-run-session');return 2
        with patch.object(host.preparation,'validate_bundle',lambda b:prepared),patch.object(host.preparation,'validate_admission',validate),patch.object(host,'claimed',lambda *a:calls.append('claim-check')),patch.object(sup,'run_session',run):
            code=host.main(['--bundle',str(bundle),'--fel',str(d/'never-executed-fel'),'--evidence',str(d/'never-used-device-evidence'),'--database',str(d/'queue'),'--claim','fixture','--owner','fixture','--supervised-manual-reconnect'])
        assert code==2 and calls==['claim-check','frozen-entry-check','frozen-entry-check','shared-run-session'];return {'calls':calls,'limits':'Shared transport mocked; existing per-port lock/retention evidence reused.'}
    case('shared-session-wiring',wiring)
    state['result']='PASS new GPU host adapter cases; no physical access or GPU execution';sup.save_json(state_path,state);print(state['result'])
if __name__=='__main__':main()
