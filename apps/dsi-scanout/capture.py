#!/usr/bin/env python3
"""Capture one admitted supervised dsi observation; copy the retained controller observation."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
supervisor=module('dsi_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('dsi_capture_preparation',ROOT/'apps/dsi-scanout/prepare.py')
decoder=module('dsi_capture_decoder',ROOT/'apps/dsi-scanout/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','dsi-supervised-software'}
PRODUCT='sunstoneos native DSI'
SERIAL='sunstoneos-a733-dsi'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,expected=None):
    preparation.session_check(session)
    record.update(dsi_retention='whole-board-power-removal',dsi_result='unobserved');save()
    for sequence in range(1,2401):
        require(now()<deadline,'DSI capture deadline expired.')
        frame=exchange(bytes(1),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'dsi-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('dsi_frames',[]).append(entry);save()
        result=decoder.decode(frame,session);entry.update(result)
        require(now()<deadline,'DSI response arrived after capture deadline.')
        if result['status']==0:
            pause(min(.025,max(0,deadline-now())));continue
        if result['status']==1:
            if expected is not None:require((result.get('sequence'),result.get('slot'))==expected,'Replay completion differs from outstanding presentation')
            output=evidence/'output.bin';supervisor.durable_frame(output,frame[128:])
            require(now()<deadline,'DSI output became durable after capture deadline.')
            record['copied_dsi_output']={'path':output.name,'bytes':256,'sha256':hashlib.sha256(frame[128:]).hexdigest()}
        record['dsi_result']='observed-success' if result['status']==1 else 'observed-failure'
        record['durable_result']=entry.copy()
        try:save()
        except BaseException:
            record['dsi_result']='unobserved';record.pop('durable_result',None);record.pop('copied_dsi_output',None);raise
        return result
    raise supervisor.CaptureWindowExpired('DSI exhausted bounded pending-query budget.')


def capture_device(device,same,deadline,session,evidence,record,save,frames=None,slots=(0,1,0)):
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==1,'Incomplete DSI request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'DSI capture deadline expired after write.')
        return bytes(device.read(0x81,384,timeout=min(timeout,remaining)))
    if struct.unpack_from('<Q',session,600)[0]==1:
        require(frames is None,'Ordinary scanout cannot accept replay frames')
        return capture(exchange,session,evidence,record,save,deadline)
    require(frames is not None and len(frames)==2 and 0<len(slots)<=256 and all(type(s)is int and s in (0,1) for s in slots),'Two frozen replay frames and bounded slot selections required')
    def send(data):
        for offset in range(0,len(data),512):
            same();remaining=int((deadline-time.monotonic())*1000);require(remaining>0,'Replay upload deadline expired')
            fragment=data[offset:offset+512];require(device.write(0x02,fragment,timeout=min(500,remaining))==len(fragment),'Incomplete replay upload')
    uploads=[preparation.profile.upload_bytes(session,slot,frames[slot]) for slot in range(2)]
    for data in uploads:send(data)
    for sequence,slot in enumerate(slots,1):
        send(preparation.profile.input_header(session,2,slot,sequence));child={};directory=evidence/f'present-{sequence:03}';directory.mkdir()
        record.setdefault('presentations',[]).append(child)
        result=capture(exchange,session,directory,child,save,deadline,expected=(sequence,slot))
        require(result['status']==1 and result['sequence']==sequence and result['slot']==slot,'Replay completion differs from requested presentation')
        save()
    record['dsi_result']='observed-success';return result


def source_files():
    return [('dsi-host-tool.py',Path(__file__)),('dsi-preparation.py',Path(preparation.__file__)),
        ('dsi-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('dsi-value-profile.py',ROOT/'apps/dsi-scanout/profile.py'),('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared DSI metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid DSI bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'DSI input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid DSI review reference.')
    path=Path(value['path']);require(path.is_absolute(),'DSI review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'DSI review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-dsi-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','native_composition','retained_scanout')),
            'DSI software evidence is incomplete.')
    for key,name in [('image','image.elf'),('launcher','launch.bin')]:
        _,data=reference_bytes(value[key])
        require(hashlib.sha256(data).hexdigest()==prepared['files'][name],
                'Software evidence covers different initially admitted executable bytes.')
    require(set(value.get('sources',{}))==set(prepared['_source_hashes']),'Software source identities are incomplete.')
    for name,expected in prepared['_source_hashes'].items():
        _,data=reference_bytes(value['sources'][name])
        require(hashlib.sha256(data).hexdigest()==expected,'Software evidence covers another implementation: '+name)
    if struct.unpack_from('<Q',prepared['_frozen']['session.bin'],600)[0]==2:
        require(all(value.get(k) is True for k in ('immutable_replay','upload_cases','rcq_cases')),'Replay software evidence incomplete')
    require(value.get('evidence'),'Software result evidence is missing.')
    for item in value['evidence']:reference_bytes(item)


def claimed(database,test_id,owner,bundle,prepared):
    db=sqlite3.connect(database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'DSI queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'DSI prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted DSI entry.')
        _,data=reference_bytes(refs['dsi-supervised-software']);validate_software(json.loads(data),prepared)
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
    parser.add_argument('--present-slot',type=int,choices=(0,1),action='append',help='Replay slot order; default0,1,0')
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board recovery plan for the retained board providers.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'DSI entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,
            frames=[frozen[f'frame{slot}.bin'] for slot in range(2)] if struct.unpack_from('<Q',session,600)[0]==2 else None,
            slots=args.present_slot or (0,1,0)),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='dsi_result',
        source_files=source_files(),description='Supervised native DSI controller observation; board providers require whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
