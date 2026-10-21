#!/usr/bin/env python3
"""Capture one supervised mimus session under an existing coordinator claim."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import struct
import sys
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


supervisor = module('mimus_capture_supervisor', ROOT / 'tools/check-a733-smp-supervised.py')
preparation = module('mimus_capture_preparation', ROOT / 'apps/mimus/prepare.py')

require, digest, artifact = supervisor.require, supervisor.digest, supervisor.artifact
PREREQUISITES = {'supervised-ram-recovery', 'mimus-supervised-software'}


def bundle_bytes(bundle, prepared, name):
    require(Path(name).name == name, 'Invalid bundle filename.')
    data = (bundle / name).read_bytes()
    require(hashlib.sha256(data).hexdigest() == prepared['files'].get(name),
            'Prepared input changed before use: ' + name)
    return data


def capture(exchange,session,evidence,record,save,deadline,*,now=time.monotonic,pause=time.sleep,verify=None):
    """Keep the existing fsync-before-ack protocol and bind every copied response."""
    record['mimus_retention']='whole-board-power-removal';record['mimus_result']='unobserved';save()
    epoch,generation=struct.unpack_from('<2Q',session,24);identity=hashlib.sha256(session).digest()
    output=bytearray();offset=0;previous_total=0;terminal=False
    for sequence in range(1,20001):
        require(now()<deadline,'Mimus capture deadline expired.')
        frame=exchange(struct.pack('<II',offset,offset),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'mimus-{sequence:05}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),'offset':offset}
        record.setdefault('mimus_frames',[]).append(entry);save()
        require(len(frame)>=64,'Short mimus response.')
        total,finished,returned,size=struct.unpack_from('<4I',frame)
        require(struct.unpack_from('<2Q',frame,16)==(epoch,generation) and frame[32:64]==identity,'Mimus response session differs.')
        require(returned==offset and size<=1024 and len(frame)==64+size and offset+size<=total<=262144 and finished in (0,1,2),'Invalid mimus response extent/state.')
        require(total>=previous_total and (not terminal or finished==1 and total==previous_total),'Mimus output regressed after publication.')
        require(now()<deadline,'Mimus response arrived after deadline.')
        previous_total=total;terminal=finished==1
        if finished==2:record['mimus_result']='observed-failure';save();return
        if size:
            output.extend(frame[64:]);offset+=size
            target=evidence/'recognition.log'
            with target.open('ab') as stream:
                stream.write(frame[64:]);stream.flush();os.fsync(stream.fileno())
            fd=os.open(evidence,os.O_RDONLY|os.O_DIRECTORY)
            try:os.fsync(fd)
            finally:os.close(fd)
        # A zero-byte terminal reply completes acknowledgement of all previously
        # durable bytes. No further device operation is issued after this point.
        if terminal and not size and offset==total:
            target=evidence/'recognition.log'
            if not target.exists():supervisor.durable_frame(target,b'')
            info={'path':target.name,'bytes':offset,'sha256':digest(target),'physical_qualification_established':False}
            supervisor.save_json(evidence/'recognition.json',info)
            require(now()<deadline,'Mimus durable output exceeded deadline.')
            if verify is not None:verify(target)
            require(now()<deadline,'Mimus parity verification exceeded deadline.')
            record['copied_mimus_output']=info;record['durable_result']=entry.copy();record['mimus_result']='observed-success'
            try:save()
            except BaseException:
                record['mimus_result']='unobserved';record.pop('durable_result',None);record.pop('copied_mimus_output',None);raise
            return
        if not size:pause(min(.01,max(0,deadline-now())))
    raise supervisor.CaptureWindowExpired('Mimus pending query budget exhausted.')


def capture_device(device,same,deadline,session,evidence,record,save,verify=None):
    serial=[0]
    def exchange(packet,timeout):
        same();require(time.monotonic()<deadline,'Mimus deadline before write.')
        require(device.write(0x02,packet,timeout=timeout)==8,'Incomplete mimus request.')
        serial[0]+=1;frame=bytearray();extent=1088;part=0;header=False
        while len(frame)<extent:
            same();remaining=int((deadline-time.monotonic())*1000);require(remaining>0,'Mimus deadline during response.')
            raw=bytes(device.read(0x81,extent-len(frame),timeout=min(timeout,remaining)))
            part+=1;path=evidence/f'mimus-fragment-{serial[0]:05}-{part:03}.bin';supervisor.durable_frame(path,raw)
            record.setdefault('mimus_fragments',[]).append({'path':path.name,'sha256':hashlib.sha256(raw).hexdigest()});save()
            require(0<len(raw)<=extent-len(frame),'Invalid mimus fragment extent.')
            frame.extend(raw)
            if not header and len(frame)>=64:
                header=True;size=struct.unpack_from('<I',frame,12)[0];require(size<=1024,'Oversized mimus response.');extent=64+size
                require(len(frame)<=extent,'Mimus response exceeds header extent.')
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline,verify=verify)


def source_files():
    return [('mimus-host-tool.py',Path(__file__)),('mimus-preparation.py',Path(preparation.__file__)),('mimus-parity.py',ROOT/'checks/mimus-parity.py')]


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'mimus')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-mimus-supervised-software-v1',('admission_cases','launcher_cases','host_capture','host_lifecycle'),
        'mimus',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'mimus','mimus-supervised-software',PREREQUISITES,
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
    require(hashlib.sha256((bundle/'prepared.json').read_bytes()).hexdigest()==prepared['_prepared_sha256'],'mimus metadata changed after validation.')
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    data=prepared['_frozen'];session=data['session.bin']
    _,_,identity,layout=preparation.image_data(data['image.elf'])
    cache={path:data[item['name']] for path,item in prepared['entry_evidence'].items()}
    admission=json.loads(data['admission.json'])
    def validate_entry(target,port):
        preparation.validate_admission(data['admission.json'],session,layout,identity,prepared['image_sha256'],cache,True)
        require(admission['identity']==target and admission['port']==port,'mimus entry covers another instance.')

    def verify_log(path):
        ref=admission['recognition_reference'];reference=cache[ref['path']]
        reference_path=args.evidence/'recognition-reference.json';supervisor.durable_frame(reference_path,reference)
        candidate=args.evidence/'recognition-candidate.json'
        supervisor.save_json(candidate,{'exit':0,'stdout':path.read_text(),'stderr':'',
            'image_sha256':prepared['image_sha256'],'bundle_sha256':data['session.bin'][216:248].hex(),
            'paced':bool(struct.unpack_from('<Q',session,168)[0])})
        checker=args.evidence/'mimus-parity.py'
        require(digest(checker)==prepared['_source_hashes']['mimus-parity.py'],'Frozen parity checker differs.')
        command=[sys.executable,'-B',str(checker),'--reference',str(reference_path),
            '--candidate',str(candidate),'--output',str(args.evidence/'recognition-parity.json')]
        result=subprocess.run(command,capture_output=True,text=True,timeout=10)
        supervisor.durable_frame(args.evidence/'recognition-parity.log',(result.stdout+result.stderr).encode())
        require(result.returncode==0,'Copied mimus output differs from pinned reference.')

    require(prepared.get('retention')=='whole-board-power-removal','mimus lifetime retention plan required.')
    return supervisor.run_session(args, bundle, prepared,
        lambda: claimed(args.database, args.claim, args.owner, bundle,prepared), validate_entry,
        lambda device, same, deadline, record, save: capture_device(
            device, same, deadline, session, args.evidence, record, save, verify_log),
        product='sunstoneos native mimus', serial='sunstoneos-a733-mimus',
        bcd_device=0x0100, capture_seconds=179, result_key='mimus_result',
        source_files=source_files(), description='Supervised native mimus; retain all resources until whole-board power removal')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, sqlite3.Error, struct.error) as error:
        sys.exit(f'Stopped: {error}')
