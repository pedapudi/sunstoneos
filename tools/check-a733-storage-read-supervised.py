#!/usr/bin/env python3
"""Capture one admitted supervised SD0 read decode; retain resources until full power removal."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
supervisor=module('storage_read_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('storage_read_capture_preparation',ROOT/'apps/storage-read/prepare.py')
decoder=module('storage_read_capture_decoder',ROOT/'apps/storage-read/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','storage-read-supervised-software'}
PRODUCT='sunstoneos native SD0 read'
SERIAL='sunstoneos-a733-storage-read'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep):
    preparation.session_check(session)
    require(not record.get('storage_read_cancelled'),'Cancelled SD0 session output is invalid.')
    record.update(storage_read_retention='whole-board-power-removal',storage_read_result='unobserved');save()
    for sequence in range(1,2401):
        require(now()<deadline,'SD0 read capture deadline expired.')
        frame=exchange(bytes(1),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'storage_read-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('storage_read_frames',[]).append(entry);save()
        result=decoder.decode(frame,session);entry.update(result)
        require(now()<deadline,'SD0 read response arrived after capture deadline.')
        if result['status']==0:
            pause(min(.025,max(0,deadline-now())));continue
        require(not record.get('storage_read_cancelled'),'Cancelled SD0 session output is invalid.')
        if result['status']==1:
            output=evidence/'output.bin';supervisor.durable_frame(output,frame[256:])
            require(now()<deadline,'SD0 read output became durable after capture deadline.')
            record['copied_storage_read_output']={'path':output.name,'bytes':512,'sha256':hashlib.sha256(frame[256:]).hexdigest()}
        record['storage_read_result']='observed-success' if result['status']==1 else 'observed-failure'
        record['durable_result']=entry.copy()
        try:save()
        except BaseException:
            record['storage_read_result']='unobserved';record.pop('durable_result',None);record.pop('copied_storage_read_output',None);raise
        return result
    raise supervisor.CaptureWindowExpired('SD0 read exhausted bounded pending-query budget.')


def read_frame(device,same,deadline,evidence,record,save,now=time.monotonic):
    frame=bytearray();expected=None
    while expected is None or len(frame)<expected:
        same();remaining=int((deadline-now())*1000)
        require(remaining>0,'SD0 frame deadline expired.')
        maximum=(768 if expected is None else expected)-len(frame)
        fragment=bytes(device.read(0x81,maximum,timeout=min(500,remaining)))
        path=evidence/f"fragment-{len(record.get('storage_read_fragments',[]))+1:04}.bin"
        supervisor.durable_frame(path,fragment)
        record.setdefault('storage_read_fragments',[]).append(dict(path=path.name,sha256=hashlib.sha256(fragment).hexdigest(),bytes=len(fragment)));save()
        require(fragment and len(fragment)<=maximum and now()<deadline,'Incomplete/late SD0 fragment.')
        frame.extend(fragment)
        if expected is None and len(frame)>=256:
            magic,version,status,count=struct.unpack_from('<4I',frame)
            require((magic,version)==(0x44524453,1) and status in (0,1,2) and count==(512 if status==1 else 0),'Invalid SD0 frame extent.')
            expected=256+count;require(len(frame)<=expected,'SD0 frame exceeds extent.')
    return bytes(frame)


def cancel_device(device,same,deadline,evidence,record,save,now=time.monotonic):
    # Mark cancellation before any I/O; this session can never publish output again.
    record['storage_read_cancelled']=True;record['storage_read_result']='cancelled';save()
    same();require(now()<deadline,'SD0 cancel deadline expired.')
    require(device.write(0x02,b'\1',timeout=min(500,max(1,int((deadline-now())*1000))))==1,'Incomplete SD0 cancellation.')


def capture_device(device,same,deadline,session,evidence,record,save):
    def exchange(packet,timeout):
        require(not record.get('storage_read_cancelled'),'Cancelled SD0 session output is invalid.')
        same();require(device.write(0x02,packet,timeout=timeout)==1,'Incomplete SD0 request.')
        return read_frame(device,same,deadline,evidence,record,save)
    return capture(exchange,session,evidence,record,save,deadline)


def source_files():
    return [('storage_read-host-tool.py',Path(__file__)),('storage_read-preparation.py',Path(preparation.__file__)),
        ('storage_read-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    prepared=preparation.validate_bundle(bundle)
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    return prepared


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid SD0 read review reference.')
    path=Path(value['path']);require(path.is_absolute(),'SD0 read review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'SD0 read review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-storage-read-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('session_binding','read_only_trace','revocation_retention','host_capture')),
            'SD0 read software evidence is incomplete.')
    for key,name in [('image','image.elf'),('launcher','launch.bin')]:
        _,data=reference_bytes(value[key])
        require(hashlib.sha256(data).hexdigest()==prepared['files'][name],
                'Software evidence covers different initially admitted executable bytes.')
    require(set(value.get('sources',{}))==set(prepared['_source_hashes']),'Software source identities are incomplete.')
    for name,expected in prepared['_source_hashes'].items():
        _,data=reference_bytes(value['sources'][name])
        require(hashlib.sha256(data).hexdigest()==expected,'Software evidence covers another implementation: '+name)
    require(value.get('evidence'),'Software result evidence is missing.')
    for item in value['evidence']:reference_bytes(item)


def claimed(database,test_id,owner,bundle,prepared):
    db=sqlite3.connect(database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'SD0 read queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'SD0 read prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted SD0 read entry.')
        _,data=reference_bytes(refs['storage-read-supervised-software']);validate_software(json.loads(data),prepared)
        covered={}
        for value in submission['artifacts']:
            path,data=reference_bytes(value);covered[path]=hashlib.sha256(data).hexdigest()
        expected={'prepared.json':prepared['_prepared_sha256'],'image.elf':prepared['files']['image.elf'],
                  'launch.bin':prepared['files']['launch.bin']}
        require(all(covered.get((bundle/name).resolve())==h for name,h in expected.items()),
                'Queue does not cover the initially admitted prepared image.')
    finally:db.close()


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release SD0 read resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'SD0 read entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='storage_read_result',
        source_files=source_files(),description='Supervised native SD0 read; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
