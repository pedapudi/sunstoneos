#!/usr/bin/env python3
"""Capture one admitted supervised H264 encode; retain resources until full power removal."""
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
supervisor=module('h264_encode_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('h264_encode_capture_preparation',ROOT/'apps/h264-encode/prepare.py')
decoder=module('h264_encode_capture_decoder',ROOT/'apps/h264-encode/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','h264-encode-supervised-software'}
PRODUCT='sunstoneos native H264 encoder'
SERIAL='sunstoneos-a733-h264-encode'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,reference=None,upload=None,send_upload=None,send_ack=None):
    preparation.session_check(session)
    require(upload is not None and len(upload)==921632 and send_upload and send_ack,'Sealed two-frame upload and bounded senders required.')
    require(upload==preparation.upload_bytes(upload[32:],None,session),'Raw frame identity differs.')
    require(now()<deadline,'Encode deadline expired before upload.');send_upload(upload)
    require(now()<deadline,'Encode upload completed after deadline.')
    record.update(h264_encode_retention='whole-board-power-removal',h264_encode_result='unobserved',numerical_qualified=False);save()
    results=[]
    for ordinal in range(2):
        for sequence in range(2400):
            require(now()<deadline,'Encode copied-output deadline expired.')
            frame=exchange(decoder.query(ordinal),min(500,max(1,int((deadline-now())*1000))))
            path=evidence/f'picture-{ordinal}-{sequence:04}.bin';supervisor.durable_frame(path,frame)
            entry=dict(path=path.name,sha256=hashlib.sha256(frame).hexdigest())
            record.setdefault('h264_encode_frames',[]).append(entry);save()
            result=decoder.decode(frame,session,ordinal);entry.update(result)
            require(now()<deadline,'Encode result arrived after deadline.')
            if result['status']==0:
                pause(min(.025,max(0,deadline-now())));continue
            if result['status']!=1:
                record['h264_encode_result']='observed-failure';record['durable_result']=entry.copy();save();return result
            output=evidence/f'picture-{ordinal}.h264';supervisor.durable_frame(output,frame[decoder.HEADER:])
            require(now()<deadline,'Encoded output became durable after deadline.')
            record.setdefault('h264_encode_outputs',[]).append(dict(ordinal=ordinal,path=output.name,bytes=result['bytes'],sha256=result['sha256']))
            save() # The real supervisor fsyncs both the record and its directory.
            require(now()<deadline,'Encode record became durable after deadline.')
            send_ack(decoder.ack(frame,session,ordinal));record['h264_encode_outputs'][-1]['ack_sent']=True;save()
            results.append(result);break
        else:raise supervisor.CaptureWindowExpired('H264 encode exhausted bounded query budget.')
    record['h264_encode_result']='observed-success';record['durable_result']=results[-1];save()
    return dict(status=1,pictures=results,numerical_qualified=False)


def capture_device(device,same,deadline,session,evidence,record,save,*,reference=None,upload):
    def send_upload(data):
        for first in range(0,len(data),512):
            same();remaining=int((deadline-time.monotonic())*1000)
            require(remaining>0,'H264 encode upload deadline expired.')
            packet=data[first:first+512]
            require(device.write(0x02,packet,timeout=min(500,remaining))==len(packet),'Incomplete H264 encode upload.')
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete H264 encode request.')
        frame=bytearray();expected=None
        while expected is None or len(frame)<expected:
            same();remaining=int((deadline-time.monotonic())*1000)
            require(remaining>0,'H264 encode frame deadline expired.')
            maximum=(decoder.HEADER if expected is None else expected)-len(frame)
            maximum=min(maximum,16384)
            fragment=bytes(device.read(0x81,maximum,timeout=min(timeout,remaining)))
            path=evidence/f"fragment-{len(record.get('h264_encode_fragments',[]))+1:04}.bin"
            supervisor.durable_frame(path,fragment)
            record.setdefault('h264_encode_fragments',[]).append(dict(path=path.name,bytes=len(fragment),sha256=hashlib.sha256(fragment).hexdigest()));save()
            require(len(fragment)<=maximum,'Oversized H264 encode fragment.')
            if not fragment:continue
            frame.extend(fragment)
            if expected is None and len(frame)>=decoder.HEADER:
                magic,version,status,count=struct.unpack_from('<4I',frame)
                require((magic,version)==(0x45433248,1) and status in (0,1,2) and
                  (0<count<=decoder.MAXIMUM if status==1 else count==0),'Invalid H264 encode frame extent.')
                expected=decoder.HEADER+count;require(len(frame)<=expected,'H264 encode frame exceeds extent.')
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline,reference=reference,upload=upload,send_upload=send_upload,send_ack=send_upload)


def source_files():
    return [('h264-host-tool.py',Path(__file__)),('h264-preparation.py',Path(preparation.__file__)),
        ('h264-encode-observer.py',Path(decoder.__file__)),('h264-encode-producer.py',ROOT/'apps/h264-encode/producer.py'),
        ('h264-encode-sources.json',ROOT/'apps/h264-encode/sources.json'),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared H264 encode metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid H264 encode bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'H264 encode input changed before use: '+name)
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
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid H264 encode review reference.')
    path=Path(value['path']);require(path.is_absolute(),'H264 encode review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'H264 encode review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-h264-encode-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','host_lifecycle')),
            'H264 encode software evidence is incomplete.')
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
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'H264 encode queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'H264 encode prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted H264 encode entry.')
        _,data=reference_bytes(refs['h264-encode-supervised-software']);validate_software(json.loads(data),prepared)
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
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release H264 encode resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'H264 encode entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,reference=prepared['_reference'],upload=prepared['_upload']),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='h264_encode_result',
        source_files=source_files(),description='Supervised native H264 encode; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
