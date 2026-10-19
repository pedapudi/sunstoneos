#!/usr/bin/env python3
"""Capture the admitted supervised NPU convolution; retain hardware ownership."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import sqlite3
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
preparation=module('npu_supervised_preparation',ROOT/'apps/npu-convolution/prepare-supervised.py')
supervisor,protocol=preparation.supervisor,preparation.protocol
require,digest,artifact=supervisor.require,supervisor.digest,supervisor.artifact
PREREQUISITES={'supervised-ram-recovery','npu-supervised-software'}


def capture(exchange,prepared,challenge,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep):
    timing=None
    for sequence in range(1,769):
        require(now()<deadline,'NPU capture deadline expired.')
        packet=protocol.request(sequence,prepared,challenge)
        frame=exchange(packet,min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'npu-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('npu_frames',[]).append(entry);save()
        result=protocol.response(frame,sequence,prepared,challenge)
        current=(result['armed_at'],result['diagnostic_deadline'])
        require(current[1]-current[0]==20*24000000 and (timing is None or timing==current),'NPU boot timing changed.')
        timing=current;entry.update(result)
        require(now()<deadline,'NPU frame arrived after capture deadline.')
        if result['status']!='pending':
            if result['output_hex'] is not None:
                output=evidence/'output.bin';supervisor.durable_frame(output,bytes.fromhex(result['output_hex']))
                record['copied_output']={'path':output.name,'sha256':digest(output)}
            record['npu_result']='observed-'+result['status'];record['durable_result']=entry.copy()
            try:save()
            except BaseException:
                record['npu_result']='unobserved';record.pop('durable_result',None);raise
            return result
        save();pause(min(.025,max(0,deadline-now())))
    raise supervisor.CaptureWindowExpired('NPU exhausted bounded pending-query budget.')


def capture_device(device,same,deadline,prepared,evidence,record,save):
    challenge=secrets.token_bytes(32);record.update(challenge=challenge.hex(),manual_whole_board_power_removal_required=True);save()
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete NPU request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'NPU deadline expired after write.')
        return bytes(device.read(0x81,1040,timeout=min(timeout,remaining)))
    return capture(exchange,prepared,challenge,evidence,record,save,deadline)


def source_files(runtime=False,large=False):
    return ([('npu-runtime-host.py',ROOT/('apps/npu-runtime-large/host.py' if large else 'apps/npu-runtime/host.py'))] if runtime else [])+[('npu-host-tool.py',Path(__file__)),('npu-preparation.py',Path(preparation.__file__)),
        ('npu-protocol.py',Path(protocol.__file__)),('npu-artifacts.py',ROOT/'apps/npu-convolution/artifacts.py'),
        ('npu-catalogue.json',ROOT/'tools/npu-models.json'),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def validate_software(value,bundle,prepared):
    require(value.get('schema')=='a733-npu-supervised-software-v1' and value.get('physical_access') is False and
        all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','host_lifecycle')),'NPU software evidence incomplete.')
    for key,name in [('image','image.elf'),('launcher','launch.bin')]:
        artifact(value[key])
        require(value[key]['sha256']==hashlib.sha256(prepared['_frozen'][name]).hexdigest(),
            'NPU software covers different frozen executable bytes.')
    sources=dict(source_files(prepared.get('execution_profile') in ('npu-runtime-single-core-supervised-v1','npu-runtime-large-single-core-supervised-v1'),prepared.get('execution_profile')=='npu-runtime-large-single-core-supervised-v1'));sources.update({'supervised-host-tool.py':Path(supervisor.__file__),
        'base-host-tool.py':Path(supervisor.base.__file__),'observer-host-tool.py':Path(supervisor.observer.__file__)})
    require(set(value.get('sources',{}))==set(sources),'Incomplete software source identities.')
    for name,path in sources.items():require(digest(artifact(value['sources'][name]))==digest(path),'Software source differs: '+name)
    require(value.get('evidence'),'Missing NPU software results.')
    for item in value['evidence']:artifact(item)


def claimed(database,test_id,owner,bundle,prepared):
    expected={**prepared['files'],'prepared.json':prepared['_prepared_sha256']}
    for name,identity in expected.items():require(digest(bundle/name)==identity,'Reviewed NPU bundle changed: '+name)
    db=sqlite3.connect(database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'Board queue halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Coordinator claim required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'NPU queue prerequisites differ.')
        refs={k:artifact(v) for k,v in review['prerequisites'].items()}
        require(refs['supervised-ram-recovery']==(bundle/'admission.json').resolve() and
            review['prerequisites']['supervised-ram-recovery']['sha256']==prepared['files']['admission.json'],
            'Reviewed NPU entry differs from frozen admission.')
        validate_software(json.loads(refs['npu-supervised-software'].read_text()),bundle,prepared)
        require({((bundle/n).resolve(),expected[n]) for n in ('prepared.json','image.elf','launch.bin')}<=
            {(artifact(v),v['sha256']) for v in submission['artifacts']},'Queue does not cover exact frozen NPU bundle.')
    finally:db.close()


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--input',type=Path,action='append',default=[])
    parser.add_argument('--oracle',type=Path,action='append',default=[])
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect alone does not release NPU resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=preparation.validate_bundle(bundle)
    frozen=prepared['_frozen'];admission=frozen['admission.json'];raw=frozen['session.bin']
    cache={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,raw,bytes.fromhex(prepared['build_id']),prepared['session_layout'],prepared['image_sha256'],cache,True)
        record=json.loads(admission);require(record['identity']==identity and record['port']==port,'NPU admission covers another live instance.')
    large=prepared.get('execution_profile')=='npu-runtime-large-single-core-supervised-v1'
    runtime=large or prepared.get('execution_profile')=='npu-runtime-single-core-supervised-v1'
    if runtime:
        host=module('npu_runtime_host',ROOT/('apps/npu-runtime-large/host.py' if large else 'apps/npu-runtime/host.py'))
        require(args.input,'Runtime profile requires same-shape input files.')
        inputs=[p.read_bytes() for p in args.input];oracles=[p.read_bytes() for p in args.oracle]
        model=next(v for v in protocol.CATALOGUE if v['name']==prepared['model_name'])
        host.validate_inputs(inputs,oracles,model)
    else:require(not args.input and not args.oracle,'Runtime inputs require runtime image.')
    return supervisor.run_session(args,bundle,prepared,lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:(host.capture_device(device,same,deadline,prepared,args.evidence,record,save,inputs,oracles,model,durable=supervisor.durable_frame) if runtime else capture_device(device,same,deadline,prepared,args.evidence,record,save)),
        product=('sunstoneos NPU large graph input' if large else 'sunstoneos NPU runtime input' if runtime else protocol.PRODUCT),serial=('sunstoneos-a733-npu-large' if large else 'sunstoneos-a733-npu-runtime' if runtime else protocol.SERIAL),bcd_device=(0x0105 if large else 0x0104 if runtime else 0x0103),capture_seconds=19,result_key='npu_result',
        source_files=source_files(runtime,large),description='Supervised NPU convolution; retain all resources until manual whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error) as error:sys.exit(f'Stopped: {error}')
