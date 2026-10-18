#!/usr/bin/env python3
"""Capture one supervised I2S session under an existing coordinator claim."""
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


supervisor = module('i2s_capture_supervisor', ROOT / 'tools/check-a733-smp-supervised.py')
preparation = module('i2s_capture_preparation', ROOT / 'apps/i2s/prepare.py')
decoder = module('i2s_capture_decoder', ROOT / 'apps/i2s/observe.py')
require, digest, artifact = supervisor.require, supervisor.digest, supervisor.artifact
PREREQUISITES = {'supervised-ram-recovery', 'i2s-supervised-software'}


def bundle_bytes(bundle, prepared, name):
    require(Path(name).name == name, 'Invalid bundle filename.')
    data = (bundle / name).read_bytes()
    require(hashlib.sha256(data).hexdigest() == prepared['files'].get(name),
            'Prepared input changed before use: ' + name)
    return data


def capture(exchange,session,evidence,record,save,deadline,*,pcm,direction,now=time.monotonic,pause=time.sleep):
    """Persist native copied periods; TX consumption does not establish wire drain."""
    record['i2s_retention']='whole-board-power-removal';record['i2s_result']='unobserved';save()
    output=bytearray();offset=0;ready=False
    for sequence in range(1,605):
        require(now()<deadline,'I2S capture deadline expired.')
        operation=1 if ready else 0;count=4096 if ready else 0
        frame=exchange(struct.pack('<III',operation,offset,count),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'i2s-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),'operation':operation,'offset':offset}
        record.setdefault('i2s_frames',[]).append(entry);save()
        value=decoder.decode(frame,session,operation,offset,count)
        require(now()<deadline,'I2S response arrived after deadline.')
        if value['state']==3:
            record['i2s_result']='observed-failure';save();return
        if value['state']!=2:
            require(not ready,'I2S result regressed during copy.')
            pause(min(.01,max(0,deadline-now())));continue
        if not ready:ready=True;continue
        output.extend(value['payload']);offset+=count
        if offset==16384:
            require(direction in (0,1) and (direction==0 or bytes(output)==pcm),'I2S consumed TX memory differs from admitted PCM.')
            target=evidence/'pcm-s16le-16000-mono.bin';supervisor.durable_frame(target,output)
            info={'path':target.name,'bytes':len(output),'sha256':digest(target),'format':'S16_LE','rate_hz':16000,'channels':1,
                  'direction':'capture' if direction==0 else 'playback','observation':'captured-periods' if direction==0 else 'dma-memory-consumed',
                  'wire_drain_established':False,'acoustic_result_established':False}
            supervisor.save_json(evidence/'pcm.json',info)
            require(now()<deadline,'I2S durable output exceeded deadline.')
            record['copied_i2s_output']=info;record['durable_result']=entry.copy();record['i2s_result']='observed-success'
            try:save()
            except BaseException:
                record['i2s_result']='unobserved';record.pop('durable_result',None);record.pop('copied_i2s_output',None);raise
            return
    raise supervisor.CaptureWindowExpired('I2S pending query budget exhausted.')


def capture_device(device,same,deadline,session,evidence,record,save,*,pcm,direction):
    serial=[0]
    def exchange(packet,timeout):
        same();require(time.monotonic()<deadline,'I2S deadline before write.')
        require(device.write(0x02,packet,timeout=timeout)==12,'Incomplete I2S request.')
        serial[0]+=1;frame=bytearray();extent=4168;part=0;header=False
        while len(frame)<extent:
            same();remaining=int((deadline-time.monotonic())*1000);require(remaining>0,'I2S deadline during response.')
            raw=bytes(device.read(0x81,extent-len(frame),timeout=min(timeout,remaining)))
            part+=1;path=evidence/f'i2s-fragment-{serial[0]:03}-{part:03}.bin';supervisor.durable_frame(path,raw)
            record.setdefault('i2s_fragments',[]).append({'path':path.name,'sha256':hashlib.sha256(raw).hexdigest()});save()
            require(0<len(raw)<=extent-len(frame),'Invalid I2S fragment extent.')
            frame.extend(raw)
            if not header and len(frame)>=72:
                header=True;size=struct.unpack_from('<I',frame,16)[0];require(size<=4096,'Oversized I2S response.');extent=72+size
                require(len(frame)<=extent,'I2S response exceeds header extent.')
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline,pcm=pcm,direction=direction)


def source_files():
    return [('i2s-host-tool.py',Path(__file__)),('i2s-preparation.py',Path(preparation.__file__)),
            ('i2s-decoder.py',Path(decoder.__file__))]


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid I2S review reference.')
    path=Path(value['path']);require(path.is_absolute(),'I2S review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'I2S review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-i2s-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','host_lifecycle')),
            'I2S software evidence is incomplete.')
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
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'I2S queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'I2S prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted I2S entry.')
        _,data=reference_bytes(refs['i2s-supervised-software']);validate_software(json.loads(data),prepared)
        covered={}
        for value in submission['artifacts']:
            path,data=reference_bytes(value);covered[path]=hashlib.sha256(data).hexdigest()
        expected={'prepared.json':prepared['_prepared_sha256'],'image.elf':prepared['files']['image.elf'],
                  'launch.bin':prepared['files']['launch.bin']}
        require(all(covered.get((bundle/name).resolve())==h for name,h in expected.items()),
                'Queue does not cover the initially admitted prepared image.')
    finally:db.close()


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
    require(hashlib.sha256((bundle/'prepared.json').read_bytes()).hexdigest()==prepared['_prepared_sha256'],'I2S metadata changed after validation.')
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    data=prepared['_frozen'];session=data['session.bin']
    _,_,identity,layout,pcm=preparation.image_data(data['image.elf'])
    cache={path:data[item['name']] for path,item in prepared['entry_evidence'].items()}
    admission=json.loads(data['admission.json'])
    def validate_entry(target,port):
        preparation.validate_admission(data['admission.json'],session,layout,identity,pcm,prepared['image_sha256'],cache,True)
        require(admission['identity']==target and admission['port']==port,'I2S entry covers another instance.')

    require(prepared.get('retention')=='whole-board-power-removal','I2S lifetime retention plan required.')
    return supervisor.run_session(args, bundle, prepared,
        lambda: claimed(args.database, args.claim, args.owner, bundle,prepared), validate_entry,
        lambda device, same, deadline, record, save: capture_device(
            device, same, deadline, session, args.evidence, record, save,pcm=pcm,direction=prepared['direction']),
        product='sunstoneos native I2S', serial='sunstoneos-a733-i2s',
        bcd_device=0x0100, capture_seconds=59, result_key='i2s_result',
        source_files=source_files(), description='Supervised native I2S; retain all resources until whole-board power removal')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, sqlite3.Error, struct.error) as error:
        sys.exit(f'Stopped: {error}')
