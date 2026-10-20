#!/usr/bin/env python3
"""Capture two interleaved GPU/NPU jobs and retain the combined hardware lifetime."""
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
preparation=module('accelerators_preparation',ROOT/'apps/accelerators/prepare.py')
host=module('accelerators_protocol',ROOT/'apps/accelerators/host.py')
supervisor=preparation.supervisor
require,digest,artifact=supervisor.require,supervisor.digest,supervisor.artifact
PRODUCT='sunstoneos GPU and NPU input'
SERIAL='sunstoneos-a733-accelerators'
PREREQUISITES={'supervised-ram-recovery','accelerators-supervised-software'}

def validate_inputs(prepared,gpu_input,npu_inputs,gpu_oracles=(),npu_oracles=()):
    package=prepared['_frozen']['session.bin'][prepared['session_layout'][5]:]
    input_bytes,output_bytes,count=struct.unpack_from('<3I',package,28)
    require(0<count<=992 and output_bytes==count*4 and len(gpu_input)==input_bytes and
        4<=input_bytes<=4096 and struct.unpack_from('<I',gpu_input)[0]==count,'GPU replacement shape differs from admitted package.')
    if struct.unpack_from('<I',package,12)[0]==3:
        require(len(gpu_input)>=12 and gpu_input[4:12]==package[172:180],'GPU grid replacement extent differs.')
    model=next(v for v in preparation.protocol.CATALOGUE if v['name']==prepared['model_name'])
    require(model.get('family')=='nn-pool' and len(npu_inputs)==2,'Two complete NN pool inputs required.')
    host.npu.validate_inputs(npu_inputs,list(npu_oracles),model)
    require(not gpu_oracles or (len(gpu_oracles)==2 and all(len(v)==output_bytes for v in gpu_oracles)),'Two same-shape GPU oracles required when qualifying copied results.')
    return count,model

def capture(exchange,prepared,evidence,record,save,deadline,gpu_input,npu_inputs,gpu_oracles=(),npu_oracles=()):
    words,model=validate_inputs(prepared,gpu_input,npu_inputs,gpu_oracles,npu_oracles)
    evidence=Path(evidence);counter=0
    record.update(accelerator_result='unobserved',manual_whole_board_power_removal_required=True,
        gpu_input_sha256=hashlib.sha256(gpu_input).hexdigest(),
        npu_input_sha256=[hashlib.sha256(v).hexdigest() for v in npu_inputs]);save()
    def observed(request,capacity,remaining):
        nonlocal counter
        require(counter<2048 and time.monotonic()<deadline,'Combined exchange budget expired.')
        counter+=1
        response=exchange(request,capacity,min(remaining,max(0,deadline-time.monotonic())))
        path=evidence/f'accelerators-{counter:04}.bin';supervisor.durable_frame(path,response)
        record.setdefault('accelerator_frames',[]).append({'path':path.name,'sha256':digest(path),
            'request_sha256':hashlib.sha256(request).hexdigest(),'response_capacity':capacity})
        save();require(time.monotonic()<deadline,'Combined response arrived after deadline.');return response
    result=host.sequence(observed,prepared,words,gpu_input,npu_inputs,model,deadline=deadline)
    require(result.get('gpu_closed') is True and result.get('npu_closed') is True and
        len(result['gpu_outputs'])==len(result['npu_outputs'])==2,'Both native job pairs and healthy CLOSE states required.')
    copied={}
    for kind in ('gpu','npu'):
        copied[kind]=[]
        for i,data in enumerate(result[kind+'_outputs']):
            require(len(data)==(words*4 if kind=='gpu' else model['reference']['bytes']),'Copied result extent differs.')
            path=evidence/f'{kind}-output-{i+1}.bin';supervisor.durable_frame(path,data)
            copied[kind].append({'path':path.name,'sha256':digest(path),'bytes':len(data)})
    record['copied_outputs']=copied
    # Store copied data before any optional numerical qualification can fail.
    save()
    for kind,oracles in (('gpu',gpu_oracles),('npu',npu_oracles)):
        if oracles:require(result[kind+'_outputs']==list(oracles),kind+' copied output differs from supplied oracle.')
    record.update(accelerator_result='observed-success',gpu_closed=True,npu_closed=True,
        gpu_jobs=2,npu_jobs=2,oracle_qualified={'gpu':bool(gpu_oracles),'npu':bool(npu_oracles)})
    try:save()
    except BaseException:
        record['accelerator_result']='unobserved';raise
    return result

def capture_device(device,same,deadline,prepared,evidence,record,save,gpu_input,npu_inputs,gpu_oracles=(),npu_oracles=()):
    def exchange(request,capacity,remaining):
        same();left=min(remaining,deadline-time.monotonic());require(left>0,'Combined write deadline expired.')
        require(device.write(0x02,request,timeout=max(1,int(left*1000)))==len(request),'Short combined request.')
        same();left=deadline-time.monotonic();require(left>0,'Combined read deadline expired.')
        # Capacity exceeds every GPU response, consuming its FS terminal ZLP.
        if capacity!=1040:return bytes(device.read(0x81,capacity,timeout=max(1,int(left*1000))))
        parts=[];remaining=capacity
        while remaining:
            same();left=deadline-time.monotonic();require(left>0,'Combined fragmented read deadline expired.')
            part=bytes(device.read(0x81,remaining,timeout=max(1,int(left*1000))))
            require(part and len(part)<=remaining,'Incomplete combined NPU reply.')
            parts.append(part);remaining-=len(part)
        return b''.join(parts)
    return capture(exchange,prepared,evidence,record,save,deadline,gpu_input,npu_inputs,gpu_oracles,npu_oracles)

def source_files():
    return [('accelerators-host-tool.py',Path(__file__)),('accelerators-preparation.py',Path(preparation.__file__)),
        ('accelerators-protocol.py',Path(host.__file__)),('gpu-runtime.py',ROOT/'apps/gpu-program/runtime.py'),
        ('npu-runtime-large.py',ROOT/'apps/npu-runtime-large/host.py'),
        ('npu-preparation.py',Path(preparation.npu.__file__)),('npu-protocol.py',Path(preparation.protocol.__file__)),
        ('npu-artifacts.py',ROOT/'apps/npu-convolution/artifacts.py'),('npu-catalogue.json',ROOT/'tools/npu-models.json'),
        ('sources.lock.json',ROOT/'sources.lock.json'),('usb-access.py',ROOT/'tools/usb-access.py')]

def validate_software(value,bundle,prepared):
    require(value.get('schema')=='a733-accelerators-supervised-software-v1' and value.get('physical_access') is False and
        all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','host_lifecycle')),'Combined software evidence incomplete.')
    for key,name in [('image','image.elf'),('launcher','launch.bin')]:
        artifact(value[key]);require(value[key]['sha256']==hashlib.sha256(prepared['_frozen'][name]).hexdigest(),'Software covers different combined executable bytes.')
    sources=dict(source_files());sources.update({'supervised-host-tool.py':Path(supervisor.__file__),
        'base-host-tool.py':Path(supervisor.base.__file__),'observer-host-tool.py':Path(supervisor.observer.__file__)})
    require(set(value.get('sources',{}))==set(sources),'Incomplete combined software source identities.')
    for name,path in sources.items():require(digest(artifact(value['sources'][name]))==digest(path),'Software source changed: '+name)
    require(value.get('evidence'),'Missing combined software results.')
    for ref in value['evidence']:artifact(ref)

def claimed(database,test_id,owner,bundle,prepared):
    expected={**prepared['files'],'prepared.json':prepared['_prepared_sha256']}
    for name,identity in expected.items():require(digest(bundle/name)==identity,'Reviewed combined bundle changed: '+name)
    db=sqlite3.connect(database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'Board queue halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Coordinator claim required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'Combined queue prerequisites differ.')
        refs={k:artifact(v) for k,v in review['prerequisites'].items()}
        require(refs['supervised-ram-recovery']==(bundle/'admission.json').resolve() and
            review['prerequisites']['supervised-ram-recovery']['sha256']==prepared['files']['admission.json'],'Reviewed combined entry differs.')
        validate_software(json.loads(refs['accelerators-supervised-software'].read_text()),bundle,prepared)
        require({((bundle/n).resolve(),expected[n]) for n in ('prepared.json','image.elf','launch.bin')}<=
            {(artifact(v),v['sha256']) for v in submission['artifacts']},'Queue omits exact combined bundle.')
    finally:db.close()

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database','gpu-input'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('npu-input','gpu-oracle','npu-oracle'):parser.add_argument('--'+name,type=Path,action='append',default=[])
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True)
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=preparation.validate_bundle(bundle)
    gpu_input=args.gpu_input.read_bytes();inputs=[p.read_bytes() for p in args.npu_input]
    gpu_oracles=[p.read_bytes() for p in args.gpu_oracle];npu_oracles=[p.read_bytes() for p in args.npu_oracle]
    validate_inputs(prepared,gpu_input,inputs,gpu_oracles,npu_oracles)
    frozen=prepared['_frozen'];cache={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(frozen['admission.json'],frozen['session.bin'],bytes.fromhex(prepared['build_id']),prepared['session_layout'],prepared['image_sha256'],cache,True)
        record=json.loads(frozen['admission.json']);require(record['identity']==identity and record['port']==port,'Combined admission covers another instance.')
    return supervisor.run_session(args,bundle,prepared,lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,prepared,args.evidence,record,save,gpu_input,inputs,gpu_oracles,npu_oracles),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0105,capture_seconds=19,result_key='accelerator_result',source_files=source_files(),
        description='Combined GPU/NPU jobs; retain resources until manual whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error) as error:sys.exit(f'Stopped: {error}')
