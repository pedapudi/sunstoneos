#!/usr/bin/env python3
"""Capture one supervised GPU session under an existing coordinator claim."""
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


supervisor = module('gpu_capture_supervisor', ROOT / 'tools/check-a733-smp-supervised.py')
preparation = module('gpu_capture_preparation', ROOT / 'apps/gpu-render/prepare.py')
decoder = module('gpu_capture_decoder', ROOT / 'apps/gpu-render/observe.py')
require, digest, artifact = supervisor.require, supervisor.digest, supervisor.artifact
PREREQUISITES = {'supervised-ram-recovery', 'gpu-supervised-software'}


def bundle_bytes(bundle, prepared, name):
    require(Path(name).name == name, 'Invalid bundle filename.')
    data = (bundle / name).read_bytes()
    require(hashlib.sha256(data).hexdigest() == prepared['files'].get(name),
            'Prepared input changed before use: ' + name)
    return data


def capture(exchange, session, evidence, record, save, deadline,
            now=time.monotonic, pause=time.sleep):
    """Copy the one verified render; only valid pending replies permit another query."""
    preparation.session_check(session)
    record['gpu_retention']='whole-board-power-removal';record.setdefault('gpu_result','unobserved');save()
    epoch=struct.unpack_from('<Q',session,24)[0]
    pixels=bytearray();chunk=0
    for sequence in range(1,305):
        require(now()<deadline,'GPU capture deadline expired.')
        timeout=min(500,max(1,int((deadline-now())*1000)))
        frame=exchange(struct.pack('<I',chunk),timeout)
        path=evidence/f'gpu-{sequence:03}.bin'
        supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),'chunk':chunk}
        record.setdefault('gpu_frames',[]).append(entry);save()
        require(len(frame)>=40,'Short GPU response.')
        state=decoder.decode(frame[:40],chunk,epoch)
        require(now()<deadline,'GPU response arrived after capture deadline.')
        if state==2:
            require(len(frame)==40,'Failed GPU response includes output.')
            record['gpu_result']='observed-failure';save();return
        if state==0:
            require(chunk==0,'GPU result regressed after copied output.')
            require(len(frame)==40,'Pending GPU response includes output.')
            pause(min(.01,max(0,deadline-now())));continue
        require(len(frame)==296 and frame[40:]==bytes.fromhex('2080c0ff')*64,
                'GPU copied pixels differ from fixed RGBA8 clear.')
        pixels.extend(frame[40:]);chunk+=1
        if chunk==64:
            output=evidence/'rgba8.bin';supervisor.durable_frame(output,pixels)
            require(now()<deadline,'GPU output became durable after capture deadline.')
            record['copied_gpu_output']={'path':output.name,'bytes':len(pixels),'sha256':digest(output)}
            record['gpu_result']='observed-success';record['durable_result']=entry.copy()
            try:save()
            except BaseException:
                record['gpu_result']='unobserved'
                record.pop('durable_result',None);record.pop('copied_gpu_output',None)
                raise
            return
    raise supervisor.CaptureWindowExpired('GPU exhausted its bounded pending-query budget.')


def capture_device(device,same,deadline,session,evidence,record,save):
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==4,'Incomplete GPU chunk request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'GPU capture deadline expired after write.')
        return bytes(device.read(0x81,296,timeout=min(timeout,remaining)))
    return capture(exchange,session,evidence,record,save,deadline)


def source_files():
    return [('gpu-host-tool.py', Path(__file__)),
            ('gpu-preparation.py', Path(preparation.__file__)),
            ('gpu-decoder.py', Path(decoder.__file__)),
            ('board-preparation.py',Path(preparation.board.__file__))]


def require_embedded_firmware(image):
    """Check only the initially frozen image bytes, never a later path value."""
    import io
    from elftools.elf.elffile import ELFFile
    elf=ELFFile(io.BytesIO(image))
    symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    require({'gpu_firmware','gpu_firmware_end'}<=set(symbols),'GPU firmware symbols missing.')
    first,last=symbols['gpu_firmware'],symbols['gpu_firmware_end']
    require(first<last and last-first<=1048576,'Invalid embedded GPU firmware extent.')
    for segment in elf.iter_segments():
        start=segment['p_vaddr'];data=segment.data()
        if start<=first<last<=start+len(data):
            require(hashlib.sha256(data[first-start:last-start]).hexdigest()==
                '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2',
                'Native image contains another GPU firmware.')
            return
    raise ValueError('Embedded GPU firmware has no file backing.')


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid GPU review reference.')
    path=Path(value['path']);require(path.is_absolute(),'GPU review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'GPU review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-gpu-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','host_lifecycle')),
            'GPU software evidence is incomplete.')
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
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'GPU queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'GPU prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted GPU entry.')
        _,data=reference_bytes(refs['gpu-supervised-software']);validate_software(json.loads(data),prepared)
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
    prepared_bytes=(bundle/'prepared.json').read_bytes()
    prepared=preparation.validate_bundle(bundle)
    require(prepared==json.loads(prepared_bytes),'Prepared GPU metadata changed during validation.')
    prepared['_prepared_sha256']=hashlib.sha256(prepared_bytes).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(bundle_bytes(bundle,prepared,name)) for name in prepared['files']}
    require_embedded_firmware(bundle_bytes(bundle,prepared,'image.elf'))
    admission = json.loads(bundle_bytes(bundle, prepared, 'admission.json'))
    session = bundle_bytes(bundle, prepared, 'session.bin')
    template_hash = hashlib.sha256(bundle_bytes(bundle, prepared, 'session-template.bin')).hexdigest()
    entry_evidence = {path: bundle_bytes(bundle, prepared, item['name'])
                      for path, item in prepared['entry_evidence'].items()}

    def validate_entry(identity, port):
        preparation.validate_admission(admission, prepared['image_sha256'], template_hash,
                                       entry_evidence, True)
        require(admission['identity'] == identity and admission['port'] == port,
                'GPU entry covers another USB instance.')

    require(prepared.get('retention')=='whole-board-power-removal','GPU lifetime retention plan required.')
    return supervisor.run_session(args, bundle, prepared,
        lambda: claimed(args.database, args.claim, args.owner, bundle,prepared), validate_entry,
        lambda device, same, deadline, record, save: capture_device(
            device, same, deadline, session, args.evidence, record, save),
        product='sunstoneos native GPU', serial='sunstoneos-a733-gpu',
        bcd_device=0x0100, capture_seconds=59, result_key='gpu_result',
        source_files=source_files(), description='Supervised native GPU; retain all resources until whole-board power removal')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, sqlite3.Error, struct.error) as error:
        sys.exit(f'Stopped: {error}')
