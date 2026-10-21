#!/usr/bin/env python3
"""Capture one admitted supervised H.264 decode; retain resources until full power removal."""
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
supervisor=module('h264_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('h264_capture_preparation',ROOT/'apps/h264/prepare.py')
decoder=module('h264_capture_decoder',ROOT/'apps/h264/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','h264-supervised-software'}
PRODUCT='sunstoneos native H.264'
SERIAL='sunstoneos-a733-h264'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,reference=None):
    preparation.session_check(session)
    require(any(session[800:832])==(reference is not None) and
        (reference is None or (len(reference) in (2304,4608) and session[800:832]==hashlib.sha256(reference).digest())),
        'H.264 qualification oracle differs.')
    record.update(h264_retention='whole-board-power-removal',h264_result='unobserved');save()
    picture=0;copied=[]
    for sequence in range(1,2401):
        require(now()<deadline,'H.264 capture deadline expired.')
        frame=exchange(bytes(1)+struct.pack('<I',picture),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'h264-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('h264_frames',[]).append(entry);save()
        result=decoder.decode(frame,session,reference,picture);entry.update(result)
        require(now()<deadline,'H.264 response arrived after capture deadline.')
        if result['status']==0:
            pause(min(.025,max(0,deadline-now())));continue
        if result['status']==1:
            output=evidence/f'output-{picture:03}.yu12';supervisor.durable_frame(output,frame[160:])
            copied.append(frame[160:])
            require(now()<deadline,'H.264 output became durable after capture deadline.')
            record['copied_h264_output']={'path':output.name,'bytes':2304,'sha256':hashlib.sha256(frame[160:]).hexdigest()}
            record.setdefault('copied_h264_pictures',[]).append({**record['copied_h264_output'],'picture_index':picture})
            if picture+1<result['picture_count']:
                picture+=1;save();continue
            supervisor.durable_frame(evidence/'output.yu12',b''.join(copied))
        record['h264_result']='observed-success' if result['status']==1 else 'observed-failure'
        record['durable_result']=entry.copy()
        try:save()
        except BaseException:
            record['h264_result']='unobserved';record.pop('durable_result',None);record.pop('copied_h264_output',None);raise
        return result
    raise supervisor.CaptureWindowExpired('H.264 exhausted bounded pending-query budget.')


def capture_device(device,same,deadline,session,evidence,record,save,upload,reference=None,stream_frames=None,stream_manifest=None):
    if struct.unpack_from('<Q',session,600)[0] in (2,3,4):
        require(stream_frames is not None and stream_manifest is not None,'VGA frozen frame inputs required.')
        return preparation.stream_module().capture_device(device,same,deadline,session,evidence,record,save,stream_manifest,stream_frames,supervisor.durable_frame)
    # Bounded upload has no decoder access until the native commit publishes it.
    for offset in range(0,len(upload),512):
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'H.264 upload deadline expired.')
        chunk=upload[offset:offset+512]
        require(device.write(0x02,chunk,timeout=min(500,remaining))==len(chunk),
                'Incomplete H.264 upload fragment.')
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete H.264 request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'H.264 capture deadline expired after write.')
        return bytes(device.read(0x81,2464,timeout=min(timeout,remaining)))
    return capture(exchange,session,evidence,record,save,deadline,reference=reference)


def source_files():
    return [('h264-host-tool.py',Path(__file__)),('h264-preparation.py',Path(preparation.__file__)),
        ('h264-decoder.py',Path(decoder.__file__)),('h264-stream.py',ROOT/'apps/h264/stream.py'),('h264-fullhd.py',ROOT/'apps/h264/fullhd.py'),('h264-high.py',ROOT/'apps/h264/high.py'),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared H.264 metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid H.264 bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'H.264 input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'H.264')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-h264-supervised-software-v1',('admission_cases','launcher_cases','host_capture','host_lifecycle'),
        'H.264',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'H.264','h264-supervised-software',PREREQUISITES,
        reference_bytes,validate_software)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release H.264 resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    reference=prepared['_reference'];upload=prepared['_upload']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'H.264 entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,upload,reference,prepared.get('_stream_frames'),prepared.get('_stream_manifest')),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='h264_result',
        source_files=source_files(),description='Supervised native H.264; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
