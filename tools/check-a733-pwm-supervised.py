#!/usr/bin/env python3
"""Capture one admitted supervised PWM decode; retain resources until full power removal."""
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
supervisor=module('pwm_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('pwm_capture_preparation',ROOT/'apps/pwm/prepare.py')
decoder=module('pwm_capture_decoder',ROOT/'apps/pwm/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','pwm-supervised-software'}
PRODUCT='sunstoneos native PWM'
SERIAL='sunstoneos-a733-pwm'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep):
    preparation.session_check(session)
    require(not record.get('pwm_cancelled'),'Cancelled PWM session output is invalid.')
    record.update(pwm_retention='whole-board-power-removal',pwm_result='unobserved');save()
    for sequence in range(1,2401):
        require(now()<deadline,'PWM capture deadline expired.')
        frame=exchange(bytes(1),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'pwm-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('pwm_frames',[]).append(entry);save()
        result=decoder.decode(frame,session);entry.update(result)
        require(now()<deadline,'PWM response arrived after capture deadline.')
        if result['status']==0:
            pause(min(.025,max(0,deadline-now())));continue
        require(not record.get('pwm_cancelled'),'Cancelled PWM session output is invalid.')
        record['pwm_result']='observed-success' if result['status']==1 else 'observed-failure'
        record['durable_result']=entry.copy()
        try:save()
        except BaseException:
            record['pwm_result']='unobserved';record.pop('durable_result',None);record.pop('copied_pwm_record',None);raise
        return result
    raise supervisor.CaptureWindowExpired('PWM exhausted bounded pending-query budget.')


def read_frame(device,same,deadline,evidence,record,save,now=time.monotonic):
    frame=bytearray();expected=None
    while expected is None or len(frame)<expected:
        same();remaining=int((deadline-now())*1000)
        require(remaining>0,'PWM frame deadline expired.')
        maximum=256-len(frame)
        fragment=bytes(device.read(0x81,maximum,timeout=min(500,remaining)))
        path=evidence/f"fragment-{len(record.get('pwm_fragments',[]))+1:04}.bin"
        supervisor.durable_frame(path,fragment)
        record.setdefault('pwm_fragments',[]).append(dict(path=path.name,sha256=hashlib.sha256(fragment).hexdigest(),bytes=len(fragment)));save()
        require(fragment and len(fragment)<=maximum and now()<deadline,'Incomplete/late PWM fragment.')
        frame.extend(fragment)
        if expected is None and len(frame)>=256:
            magic,version,status,count=struct.unpack_from('<4I',frame)
            require((magic,version)==(0x314d5750,1) and status in (0,1,2) and count==256,'Invalid PWM frame extent.')
            expected=256;require(len(frame)<=expected,'PWM frame exceeds extent.')
    return bytes(frame)


def cancel_device(device,same,deadline,evidence,record,save,now=time.monotonic):
    # Mark cancellation before any I/O; this session can never publish output again.
    record['pwm_cancelled']=True;record['pwm_result']='cancelled';save()
    same();require(now()<deadline,'PWM cancel deadline expired.')
    require(device.write(0x02,b'\1',timeout=min(500,max(1,int((deadline-now())*1000))))==1,'Incomplete PWM cancellation.')


def capture_device(device,same,deadline,session,evidence,record,save):
    def exchange(packet,timeout):
        require(not record.get('pwm_cancelled'),'Cancelled PWM session output is invalid.')
        same();require(device.write(0x02,packet,timeout=timeout)==1,'Incomplete PWM request.')
        return read_frame(device,same,deadline,evidence,record,save)
    return capture(exchange,session,evidence,record,save,deadline)


def source_files():
    return [('pwm-host-tool.py',Path(__file__)),('pwm-preparation.py',Path(preparation.__file__)),
        ('pwm-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    prepared=preparation.validate_bundle(bundle)
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    return prepared


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid PWM review reference.')
    path=Path(value['path']);require(path.is_absolute(),'PWM review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'PWM review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-pwm-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('session_binding','waveform_irq_stop','revocation_retention','host_capture')),
            'PWM software evidence is incomplete.')
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
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'PWM queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'PWM prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted PWM entry.')
        _,data=reference_bytes(refs['pwm-supervised-software']);validate_software(json.loads(data),prepared)
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
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release PWM resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'PWM entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='pwm_result',
        source_files=source_files(),description='Supervised native PWM; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
