#!/usr/bin/env python3
"""Capture one supervised camera session under an existing coordinator claim."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


supervisor = module('camera_capture_supervisor', ROOT / 'tools/check-a733-smp-supervised.py')
preparation = module('camera_capture_preparation', ROOT / 'apps/camera/prepare.py')
decoder = module('camera_capture_decoder', ROOT / 'apps/camera/observe.py')
require, digest, artifact = supervisor.require, supervisor.digest, supervisor.artifact
PREREQUISITES = {'supervised-ram-recovery', 'camera-supervised-software'}


def bundle_bytes(bundle, prepared, name):
    require(Path(name).name == name, 'Invalid bundle filename.')
    data = (bundle / name).read_bytes()
    require(hashlib.sha256(data).hexdigest() == prepared['files'].get(name),
            'Prepared input changed before use: ' + name)
    return data


def capture(exchange,session,evidence,record,save,deadline,*,now=time.monotonic,pause=time.sleep):
    """Persist the complete copied NV16 payload; guards never become image bytes."""
    record['camera_retention']='whole-board-power-removal';record['camera_result']='unobserved';save()
    output=bytearray();offset=0;ready=False
    for sequence in range(1,605):
        require(now()<deadline,'camera capture deadline expired.')
        operation=1 if ready else 0;count=4096 if ready else 0
        frame=exchange(struct.pack('<III',operation,offset,count),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'camera-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),'operation':operation,'offset':offset}
        record.setdefault('camera_frames',[]).append(entry);save()
        value=decoder.decode(frame,session,operation,offset,count)
        require(now()<deadline,'camera response arrived after deadline.')
        if value['state']==3:
            record['camera_result']='observed-failure';save();return
        if value['state']!=2:
            require(not ready,'camera result regressed during copy.')
            pause(min(.01,max(0,deadline-now())));continue
        if not ready:ready=True;continue
        output.extend(value['payload']);offset+=count
        if offset==614400:
            target=evidence/'frame-640x480-nv16.bin';supervisor.durable_frame(target,output)
            info={'path':target.name,'bytes':len(output),'sha256':digest(target),'format':'NV16',
                  'width':640,'height':480,'planes':[307200,307200],'physical_qualification_established':False}
            supervisor.save_json(evidence/'frame.json',info)
            require(now()<deadline,'camera durable output exceeded deadline.')
            record['copied_camera_output']=info;record['durable_result']=entry.copy();record['camera_result']='observed-success'
            try:save()
            except BaseException:
                record['camera_result']='unobserved';record.pop('durable_result',None);record.pop('copied_camera_output',None);raise
            return
    raise supervisor.CaptureWindowExpired('camera pending query budget exhausted.')


def capture_device(device,same,deadline,session,evidence,record,save):
    serial=[0]
    def exchange(packet,timeout):
        same();require(time.monotonic()<deadline,'camera deadline before write.')
        require(device.write(0x02,packet,timeout=timeout)==12,'Incomplete camera request.')
        serial[0]+=1;frame=bytearray();extent=4168;part=0;header=False
        while len(frame)<extent:
            same();remaining=int((deadline-time.monotonic())*1000);require(remaining>0,'camera deadline during response.')
            raw=bytes(device.read(0x81,extent-len(frame),timeout=min(timeout,remaining)))
            part+=1;path=evidence/f'camera-fragment-{serial[0]:03}-{part:03}.bin';supervisor.durable_frame(path,raw)
            record.setdefault('camera_fragments',[]).append({'path':path.name,'sha256':hashlib.sha256(raw).hexdigest()});save()
            require(0<len(raw)<=extent-len(frame),'Invalid camera fragment extent.')
            frame.extend(raw)
            if not header and len(frame)>=72:
                header=True;size=struct.unpack_from('<I',frame,16)[0];require(size<=4096,'Oversized camera response.');extent=72+size
                require(len(frame)<=extent,'camera response exceeds header extent.')
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline)


def source_files():
    return [('camera-host-tool.py',Path(__file__)),('camera-preparation.py',Path(preparation.__file__)),
            ('camera-decoder.py',Path(decoder.__file__))]


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'camera')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-camera-supervised-software-v1',('admission_cases','launcher_cases','host_capture','host_lifecycle'),
        'camera',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'camera','camera-supervised-software',PREREQUISITES,
        reference_bytes,validate_software)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'fel', 'evidence', 'database'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--claim', required=True)
    parser.add_argument('--owner', required=True)
    parser.add_argument('--supervised-manual-reconnect', action='store_true', required=True)
    args = parser.parse_args(argv)
    bundle = args.bundle.resolve()
    prepared=preparation.validate_bundle(bundle)
    require(hashlib.sha256((bundle/'prepared.json').read_bytes()).hexdigest()==prepared['_prepared_sha256'],'camera metadata changed after validation.')
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    data=prepared['_frozen'];session=data['session.bin']
    _,_,identity,layout=preparation.image_data(data['image.elf'])
    cache={path:data[item['name']] for path,item in prepared['entry_evidence'].items()}
    admission=json.loads(data['admission.json'])
    def validate_entry(target,port):
        preparation.validate_admission(data['admission.json'],session,layout,identity,prepared['image_sha256'],cache,True)
        require(admission['identity']==target and admission['port']==port,'camera entry covers another instance.')

    require(prepared.get('retention')=='whole-board-power-removal','camera lifetime retention plan required.')
    return supervisor.run_session(args, bundle, prepared,
        lambda: claimed(args.database, args.claim, args.owner, bundle,prepared), validate_entry,
        lambda device, same, deadline, record, save: capture_device(
            device, same, deadline, session, args.evidence, record, save),
        product='sunstoneos native camera', serial='sunstoneos-a733-camera',
        bcd_device=0x0100, capture_seconds=59, result_key='camera_result',
        source_files=source_files(), description='Supervised native camera; retain all resources until whole-board power removal')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, sqlite3.Error, struct.error) as error:
        sys.exit(f'Stopped: {error}')
