#!/usr/bin/env python3
"""Capture one admitted supervised RTC decode; retain resources until full power removal."""
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
supervisor=module('rtc_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('rtc_capture_preparation',ROOT/'apps/rtc/prepare.py')
decoder=module('rtc_capture_decoder',ROOT/'apps/rtc/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','rtc-supervised-software'}
PRODUCT='sunstoneos native RTC'
SERIAL='sunstoneos-a733-rtc'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,cancel=False):
    preparation.session_check(session)
    require(not cancel or struct.unpack_from("<Q",session,600)[0]!=4,"Service cancellation requires the matching native client request token.")
    record.update(rtc_retention='whole-board-power-removal',rtc_result='unobserved');save()
    for sequence in range(1,2401):
        require(now()<deadline,'RTC capture deadline expired.')
        frame=exchange(bytes([int(cancel and sequence==1)]),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'rtc-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('rtc_frames',[]).append(entry);save()
        result=decoder.decode(frame,session);entry.update(result)
        require(now()<deadline,'RTC response arrived after capture deadline.')
        if result['status']==0:
            pause(min(.025,max(0,deadline-now())));continue
        if result['status']==1:
            output=evidence/'rtc-snapshot.bin';supervisor.durable_frame(output,frame[64:])
            require(now()<deadline,'RTC output became durable after capture deadline.')
            record['copied_rtc_output']={'path':output.name,'bytes':len(frame)-64,'sha256':hashlib.sha256(frame[64:]).hexdigest()}
        require(now()<deadline,'RTC finalization exceeded capture deadline.')
        record['rtc_result']=({1:'observed-success',2:'observed-cancelled',3:'observed-applied-after-cancel',4:'observed-retained-failure'}[result['runtime_outcome']]
            if result.get('runtime_outcome') else 'observed-retained-failure' if result.get('service_retained') else 'observed-success' if result['status']==1 else 'observed-failure')
        record['durable_result']=entry.copy()
        try:
            save()
            require(now()<deadline,'RTC durable terminal publication exceeded capture deadline.')
        except BaseException:
            record['rtc_result']='unobserved';record.pop('durable_result',None);record.pop('copied_rtc_output',None);raise
        return result
    raise supervisor.CaptureWindowExpired('RTC exhausted bounded pending-query budget.')


def capture_device(device,same,deadline,session,evidence,record,save,cancel=False):
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==1,'Incomplete RTC request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'RTC capture deadline expired after write.')
        return bytes(device.read(0x81,2480 if struct.unpack_from("<Q",session,600)[0]==4 else 200,timeout=min(timeout,remaining)))
    return capture(exchange,session,evidence,record,save,deadline,cancel=cancel)


def source_files():
    return [('rtc-host-tool.py',Path(__file__)),('rtc-preparation.py',Path(preparation.__file__)),
        ('rtc-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared RTC metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid RTC bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'RTC input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'RTC')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-rtc-supervised-software-v1',('admission_cases','launcher_cases','host_capture'),
        'RTC',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'RTC','rtc-supervised-software',PREREQUISITES,
        reference_bytes,validate_software)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--cancel',action='store_true',help='Request cancellation instead of waiting for the owned alarm.')
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release RTC resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True,prepared.get("execution_profile","rtc-supervised-v1"))
        require(admission['identity']==identity and admission['port']==port,'RTC entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,cancel=args.cancel),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='rtc_result',
        source_files=source_files(),description='Supervised native RTC; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
