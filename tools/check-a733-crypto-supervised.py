#!/usr/bin/env python3
"""Capture one admitted supervised CRYPTO decode; retain resources until full power removal."""
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
supervisor=module('crypto_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('crypto_capture_preparation',ROOT/'apps/crypto/prepare.py')
decoder=module('crypto_capture_decoder',ROOT/'apps/crypto/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','crypto-supervised-software'}
REFERENCE=preparation.vectors.REFERENCE
PRODUCT='sunstoneos native CRYPTO'
SERIAL='sunstoneos-a733-crypto'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,reference=REFERENCE,request=None):
    preparation.session_check(session)
    runtime=struct.unpack_from('<Q',session,600)[0]==2
    if runtime:preparation.operation.decode(request,session)
    else:require(len(reference)==160 and session[792:824]==hashlib.sha256(reference).digest(),
        'CRYPTO diagnostic fixture/reference differs.')
    record.update(crypto_retention='whole-board-power-removal',crypto_result='unobserved');save()
    for sequence in range(1,2401):
        require(now()<deadline,'CRYPTO capture deadline expired.')
        packet=b"\1"+struct.pack("<I",len(request))+request if runtime and sequence==1 else bytes(1)
        frame=exchange(packet,min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'crypto-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('crypto_frames',[]).append(entry);save()
        result=decoder.decode(frame,session,reference,request);entry.update(result)
        require(now()<deadline,'CRYPTO response arrived after capture deadline.')
        if result['status']==0:
            pause(min(.025,max(0,deadline-now())));continue
        if result['status']==1:
            output=evidence/'output.bin';supervisor.durable_frame(output,frame[144 if runtime else 128:])
            require(now()<deadline,'CRYPTO output became durable after capture deadline.')
            record['copied_crypto_output']={'path':output.name,'bytes':result['bytes'],'sha256':hashlib.sha256(frame[144 if runtime else 128:]).hexdigest()}
            if runtime:record['copied_crypto_output'].update({k:result[k] for k in ('request_id','operation','input_bytes','oracle_checked')})
        record['crypto_result']='observed-success' if result['status']==1 else 'observed-failure'
        record['durable_result']=entry.copy()
        try:save()
        except BaseException:
            record['crypto_result']='unobserved';record.pop('durable_result',None);record.pop('copied_crypto_output',None);raise
        return result
    raise supervisor.CaptureWindowExpired('CRYPTO exhausted bounded pending-query budget.')


def capture_device(device,same,deadline,session,evidence,record,save,*,reference=REFERENCE,request=None):
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete CRYPTO request.')
        base=144 if request is not None else 128;extent=base+(2048 if request is not None else 160)
        frame=bytearray();empty_reads=0
        while len(frame)<extent:
            same();remaining=int((deadline-time.monotonic())*1000)
            require(remaining>0,'CRYPTO capture deadline expired after write.')
            part=bytes(device.read(0x81,min(512,extent-len(frame)),timeout=min(timeout,remaining)))
            target=evidence/f'crypto-fragment-{len(record.get("crypto_fragments",[]))+1:05}.bin'
            supervisor.durable_frame(target,part)
            record.setdefault('crypto_fragments',[]).append({'path':target.name,'sha256':hashlib.sha256(part).hexdigest()});save()
            require(len(part)<=extent-len(frame),'Invalid CRYPTO fragment extent.')
            if not part:
                empty_reads+=1;require(empty_reads<=8,'CRYPTO empty-fragment budget exhausted.');continue
            frame.extend(part)
            if len(frame)>=16:
                count=struct.unpack_from('<I',frame,12)[0]
                require(count<=(2048 if request is not None else 160),'Oversized CRYPTO output.')
                extent=base+count;require(len(frame)<=extent,'CRYPTO output exceeds header extent.')
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline,reference=reference,request=request)


def source_files():
    return [('crypto-host-tool.py',Path(__file__)),('crypto-preparation.py',Path(preparation.__file__)),
        ('crypto-decoder.py',Path(decoder.__file__)),('crypto-vectors.py',Path(preparation.vectors.__file__)),('crypto-operation.py',Path(preparation.operation.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared CRYPTO metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid CRYPTO bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'CRYPTO input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    prepared['_reference']=preparation.embedded_reference(frozen['image.elf'],frozen['session.bin'])
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid CRYPTO review reference.')
    path=Path(value['path']);require(path.is_absolute(),'CRYPTO review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'CRYPTO review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-crypto-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','host_lifecycle')),
            'CRYPTO software evidence is incomplete.')
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
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'CRYPTO queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'CRYPTO prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted CRYPTO entry.')
        _,data=reference_bytes(refs['crypto-supervised-software']);validate_software(json.loads(data),prepared)
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
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release CRYPTO resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'CRYPTO entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,reference=prepared['_reference'],request=prepared.get('_operation')),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='crypto_result',
        source_files=source_files(),description='Supervised native CRYPTO; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
