#!/usr/bin/env python3
"""Capture repeated admitted monitoring snapshots and a configured-disable acknowledgment."""
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
supervisor=module('monitor_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('monitor_capture_preparation',ROOT/'apps/monitor/prepare.py')
decoder=module('monitor_capture_decoder',ROOT/'apps/monitor/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','monitor-supervised-software'}
PRODUCT='sunstoneos native monitor'
SERIAL='sunstoneos-a733-monitor'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,sample_seconds=5):
    preparation.session_check(session)
    require(0<sample_seconds<=45,'Monitor sample interval must be within45seconds.')
    record.update(monitor_retention='whole-board-power-removal',monitor_result='unobserved');save()
    previous=None;first=None;stop_at=None;stopping=False
    for sequence in range(1,6001):
        require(now()<deadline,'Monitor capture deadline expired.')
        packet=bytes((1,)) if not stopping and stop_at is not None and now()>=stop_at else bytes((0,))+struct.pack('<Q',previous['generation'] if previous else 0)
        if packet==bytes((1,)):stopping=True
        frame=exchange(packet,min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'monitor-{sequence:04}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('monitor_frames',[]).append(entry);save()
        result=decoder.decode(frame,session,previous);entry.update(result)
        require(now()<deadline,'Monitor response arrived after deadline.')
        status=result['status']
        if status==2:
            require(not stopping,'Snapshot published after stop request.')
            previous=result
            if first is None:
                first=result;stop_at=now()+sample_seconds
                require(stop_at+1.5<deadline,'Insufficient remaining lifetime for sampling and configured disable.')
            output=evidence/f'output-{result["generation"]:016}.bin';supervisor.durable_frame(output,frame[128:])
            record['latest_copied_output']={'path':output.name,'bytes':192,'sha256':hashlib.sha256(frame[128:]).hexdigest()}
        elif status==4:
            require(stopping and first and previous,'Unexpected monitor stop acknowledgment.')
            require(all(previous[k]>=first[k]+2 for k in ('thermal_sets','adc_sets')),'Both devices must demonstrate repeated fresh sets.')
            record['monitor_result']='observed-success';record['durable_result']=entry.copy();record['configured_disabled']=True
            try:save()
            except BaseException:
                record['monitor_result']='unobserved';record.pop('durable_result',None);record.pop('configured_disabled',None);raise
            return result
        else:
            require(status in ((3,) if stopping else (0,1)),'Monitor failed or entered an unexpected state.')
        save();pause(min(.025,max(0,deadline-now())))
    raise supervisor.CaptureWindowExpired('Monitor exhausted finite query budget.')


def capture_device(device,same,deadline,session,evidence,record,save,sample_seconds=5):
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete monitor request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'Monitor deadline expired after write.')
        return bytes(device.read(0x81,320,timeout=min(timeout,remaining)))
    return capture(exchange,session,evidence,record,save,deadline,sample_seconds=sample_seconds)


def source_files():
    return [('monitor-host-tool.py',Path(__file__)),('monitor-preparation.py',Path(preparation.__file__)),
        ('monitor-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared monitor metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid monitor bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'monitor input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid monitor review reference.')
    path=Path(value['path']);require(path.is_absolute(),'monitor review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'monitor review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-monitor-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','native_composition')),
            'monitor software evidence is incomplete.')
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
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'monitor queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'monitor prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted monitor entry.')
        _,data=reference_bytes(refs['monitor-supervised-software']);validate_software(json.loads(data),prepared)
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
    parser.add_argument('--sample-seconds',type=float,default=5);parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release monitor resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'monitor entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,args.sample_seconds),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='monitor_result',
        source_files=source_files(),description='Supervised native monitor; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
