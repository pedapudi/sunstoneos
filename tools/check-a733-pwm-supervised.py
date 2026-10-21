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
        path=evidence/f"pwm-{len(record.get('pwm_frames',[]))+1:03}.bin";supervisor.durable_frame(path,frame)
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
        maximum=512-len(frame)
        fragment=bytes(device.read(0x81,maximum,timeout=min(500,remaining)))
        path=evidence/f"fragment-{len(record.get('pwm_fragments',[]))+1:04}.bin"
        supervisor.durable_frame(path,fragment)
        record.setdefault('pwm_fragments',[]).append(dict(path=path.name,sha256=hashlib.sha256(fragment).hexdigest(),bytes=len(fragment)));save()
        require(fragment and len(fragment)<=maximum and now()<deadline,'Incomplete/late PWM fragment.')
        frame.extend(fragment)
        if expected is None and len(frame)>=256:
            magic,version,status,count=struct.unpack_from('<4I',frame)
            require((magic,version)==(0x314d5750,2) and status in (0,1,2) and count==512,'Invalid PWM frame extent.')
            expected=512;require(len(frame)<=expected,'PWM frame exceeds extent.')
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


def command_device(device,same,deadline,session,evidence,record,save,sequence,kind,argument=0,budget_us=100000):
    require(not record.get('pwm_cancelled'),'Cancelled PWM output is invalid.')
    packet=decoder.command(session,sequence,kind,argument,budget_us)
    record['pwm_result']='unobserved';record.pop('durable_result',None)
    record.setdefault('pwm_commands',[]).append(dict(sequence=sequence,kind=kind,argument=argument,budget_us=budget_us,packet_sha256=hashlib.sha256(packet).hexdigest(),state='submitted'));save()
    same();require(time.monotonic()<deadline,'Command deadline expired.')
    require(device.write(0x02,packet,timeout=min(500,max(1,int((deadline-time.monotonic())*1000))))==len(packet),'Incomplete PWM command.')
    for _ in range(2400):
        result=capture_device(device,same,deadline,session,evidence,record,save)
        if result['status']==2:
            record['pwm_command_failure']=record.get('durable_result');save()
            raise ValueError('PWM session retained; command cannot complete.')
        if result['rejected_sequence']==sequence and result['rejection']:raise ValueError('PWM command refused: '+str(result['rejection']))
        if result['command']['sequence']==sequence and result['command']['ack_state']==2 and result['status']==1:
            require((result['command']['kind'],result['command']['argument'])==(kind,argument),'Different command acknowledged.')
            record['pwm_commands'][-1]['state']='completed';save();return result
        record['pwm_result']='unobserved';record.pop('durable_result',None);save()
    raise ValueError('PWM command exhausted status budget.')

def run_program(device,same,deadline,session,evidence,record,save,program):
    result=capture_device(device,same,deadline,session,evidence,record,save)
    try:
        if result['status']==2:
            record['pwm_command_failure']=record.get('durable_result');save()
            raise ValueError('PWM initialization retained; commands are prohibited.')
        for sequence,(kind,argument) in enumerate(program,1):result=command_device(device,same,deadline,session,evidence,record,save,sequence,kind,argument)
        return result
    except BaseException:
        record['pwm_result']='unobserved';record.pop('durable_result',None);save();raise

def source_files():
    return [('pwm-host-tool.py',Path(__file__)),('pwm-preparation.py',Path(preparation.__file__)),
        ('pwm-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    prepared=preparation.validate_bundle(bundle)
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    return prepared


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'PWM')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-pwm-supervised-software-v1',('session_binding','waveform_irq_stop','reusable_commands','revocation_retention','host_capture'),
        'PWM',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'PWM','pwm-supervised-software',PREREQUISITES,
        reference_bytes,validate_software)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release PWM resources.')
    parser.add_argument('--command',action='append',default=[],help='Ordered start:TICKS, duty:TICKS, stop or led:0/1; no automatic retries.')
    args=parser.parse_args(argv);program=[]
    for text in args.command:
        name,sep,value=text.partition(':');require(name in ('start','duty','stop','led'),'Unknown PWM operation.')
        kind={'start':1,'duty':2,'stop':3,'led':4}[name];argument=int(value) if sep else 0
        require((kind in (1,2) and 1<=argument<600) or (kind==3 and not sep) or (kind==4 and argument in (0,1) and sep),'Invalid PWM operation.')
        program.append((kind,argument))
    bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    running=False
    for sequence,(kind,argument) in enumerate(program,1):
        decoder.command(session,sequence,kind,argument)
        require(kind!=1 or not running,'START requires stopped output.')
        require(kind!=2 or running,'SET_DUTY requires running output.')
        if kind==1:running=True
        elif kind==3:running=False
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True,prepared.get("execution_profile","pwm-service-supervised-v1"))
        require(admission['identity']==identity and admission['port']==port,'PWM entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:run_program(device,same,deadline,session,args.evidence,record,save,program),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='pwm_result',
        source_files=source_files(),description='Supervised native PWM; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
