#!/usr/bin/env python3
"""Capture one admitted supervised JPEG encode; retain resources until full power removal."""
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
supervisor=module('jpeg_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('jpeg_capture_preparation',ROOT/'apps/jpeg-encode/prepare.py')
decoder=module('jpeg_capture_decoder',ROOT/'apps/jpeg-encode/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','jpeg-encode-supervised-software'}
PRODUCT='sunstoneos native JPEG encoder'
SERIAL='sunstoneos-a733-jpeg-encode'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,reference=None,upload=None,send_upload=None,send_ack=None):
    preparation.session_check(session)
    require(upload is not None and len(upload)>=32 and send_upload is not None,'Frozen JPEG upload and sender required.')
    magic,version,jpeg_bytes,oracle_bytes,epoch,generation=struct.unpack_from('<4I2Q',upload)
    require((magic,version)==(0x55454a53,1) and len(upload)==32+jpeg_bytes+oracle_bytes and
       (epoch,generation)==struct.unpack_from('<2Q',session,24),'JPEG upload identity/extent differs.')
    require(upload==preparation.upload_bytes(upload[32:32+jpeg_bytes],reference,session),'JPEG upload digest or oracle differs.')
    require(now()<deadline,'JPEG deadline expired before upload.')
    send_upload(upload)
    require(now()<deadline,'JPEG upload completed after deadline.')
    record.update(jpeg_encode_retention='whole-board-power-removal',jpeg_encode_result='unobserved');save()
    for sequence in range(1,2401):
        require(now()<deadline,'JPEG capture deadline expired.')
        frame=exchange(bytes(1),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'jpeg-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('jpeg_frames',[]).append(entry);save()
        result=decoder.decode(frame,session);entry.update(result)
        require(now()<deadline,'JPEG response arrived after capture deadline.')
        if result['status']==0:
            pause(min(.025,max(0,deadline-now())));continue
        if result['status']==1:
            output=evidence/'output.jpg';supervisor.durable_frame(output,frame[decoder.HEADER:])
            require(now()<deadline,'JPEG output became durable after capture deadline.')
            record['copied_encoded_jpeg_output']={'path':output.name,'bytes':result['bytes'],'sha256':hashlib.sha256(frame[decoder.HEADER:]).hexdigest()}
        record['jpeg_encode_result']='observed-success' if result['status']==1 else 'observed-failure'
        record['durable_result']=entry.copy();record['arithmetic_qualified']=False
        try:save()
        except BaseException:
            record['jpeg_encode_result']='unobserved';record.pop('durable_result',None);record.pop('copied_encoded_jpeg_output',None);raise
        if result['status']==1:
            require(send_ack is not None,'Encoder copied-output ACK sender required.')
            require(now()<deadline,'Encoder output became durable after deadline.')
            send_ack(decoder.ack(frame,session));record['copied_output_ack_sent']=True;save()
        return result
    raise supervisor.CaptureWindowExpired('JPEG exhausted bounded pending-query budget.')


def capture_device(device,same,deadline,session,evidence,record,save,*,reference=None,upload):
    def send_upload(data):
        for first in range(0,len(data),512):
            same();remaining=int((deadline-time.monotonic())*1000)
            require(remaining>0,'JPEG upload deadline expired.')
            packet=data[first:first+512]
            require(device.write(0x02,packet,timeout=min(500,remaining))==len(packet),'Incomplete JPEG upload.')
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete JPEG request.')
        frame=bytearray();expected=None
        while expected is None or len(frame)<expected:
            same();remaining=int((deadline-time.monotonic())*1000)
            require(remaining>0,'JPEG frame deadline expired.')
            maximum=(decoder.HEADER if expected is None else expected)-len(frame)
            maximum=min(maximum,16384)
            fragment=bytes(device.read(0x81,maximum,timeout=min(timeout,remaining)))
            path=evidence/f"fragment-{len(record.get('jpeg_fragments',[]))+1:04}.bin"
            supervisor.durable_frame(path,fragment)
            record.setdefault('jpeg_fragments',[]).append(dict(path=path.name,bytes=len(fragment),sha256=hashlib.sha256(fragment).hexdigest()));save()
            require(len(fragment)<=maximum,'Oversized JPEG fragment.')
            if not fragment:continue
            frame.extend(fragment)
            if expected is None and len(frame)>=decoder.HEADER:
                magic,version,status,count=struct.unpack_from('<4I',frame)
                require((magic,version)==(0x4547504a,1) and status in (0,1,2) and
                  (0<count<=decoder.MAXIMUM if status==1 else count==0),'Invalid JPEG frame extent.')
                expected=decoder.HEADER+count;require(len(frame)<=expected,'JPEG frame exceeds extent.')
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline,reference=reference,upload=upload,send_upload=send_upload,send_ack=send_upload)


def source_files():
    return [('jpeg-host-tool.py',Path(__file__)),('jpeg-preparation.py',Path(preparation.__file__)),
        ('jpeg-encode-observer.py',Path(decoder.__file__)),('jpeg-encode-producer.py',ROOT/'apps/jpeg-encode/producer.py'),
        ('jpeg-encode-sources.json',ROOT/'apps/jpeg-encode/sources.json'),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared JPEG metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid JPEG bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'JPEG input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    preparation.embedded_reference(frozen['image.elf'],frozen['session.bin'])
    prepared['_reference']=frozen['reference.yu12'] or None
    prepared['_upload']=frozen['upload.bin']
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'JPEG')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-jpeg-encode-supervised-software-v1',('admission_cases','launcher_cases','host_capture','host_lifecycle'),
        'JPEG',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'JPEG','jpeg-encode-supervised-software',PREREQUISITES,
        reference_bytes,validate_software)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release JPEG resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'JPEG entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,reference=prepared['_reference'],upload=prepared['_upload']),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='jpeg_encode_result',
        source_files=source_files(),description='Supervised native JPEG; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
