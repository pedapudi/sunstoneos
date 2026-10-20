#!/usr/bin/env python3
"""Capture bounded copied GPU/HDMI evidence; retain the whole board lifetime."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
preparation=module('gpu_display_preparation',ROOT/'apps/gpu-display/prepare.py')
supervisor=preparation.gpu.board.supervisor
require,digest,artifact=supervisor.require,supervisor.digest,supervisor.artifact
PRODUCT='sunstoneos GPU HDMI capture'
SERIAL='sunstoneos-a733-gpu-display'
PREREQUISITES={'supervised-ram-recovery','gpu-display-supervised-software'}

def capture_device(device,same,deadline,prepared,evidence,record,save,*,clock=time.monotonic,sleep=time.sleep):
    protocol=module('gpu_display_capture_protocol',ROOT/'apps/gpu-display/capture.py')
    record.update(gpu_display_result='unobserved',manual_whole_board_power_removal_required=True);save()
    class RetainedDevice:
        def write(self,endpoint,data,timeout):
            same();left=int((deadline-clock())*1000);require(left>0,'GPU/display write deadline expired.')
            return device.write(endpoint,data,timeout=min(timeout,left))
        def read(self,endpoint,capacity,timeout):
            same();left=int((deadline-clock())*1000);require(left>0,'GPU/display read deadline expired.')
            return device.read(endpoint,capacity,timeout=min(timeout,left))
    def observe(raw):
        frames=record.setdefault('gpu_display_frames',[]);require(len(frames)<200,'GPU/display response budget exceeded.')
        path=Path(evidence)/f'gpu-display-{len(frames)+1:03}.bin';supervisor.durable_frame(path,raw)
        frames.append({'path':path.name,'sha256':digest(path),'bytes':len(raw)});save()
        require(clock()<deadline,'GPU/display reply arrived after capture deadline.')
    expected_session=hashlib.sha256(prepared['_frozen']['session.bin']).hexdigest()
    result=protocol.capture(RetainedDevice(),epoch=prepared['epoch'],generation=prepared['generation'],
        lease=prepared['lease'],build_id=prepared['build_id'],session_sha256=expected_session,deadline=deadline,clock=clock,sleep=sleep,observer=observe)
    expected=preparation.validate_inputs(prepared['_frozen']['session.bin'][preparation.INPUT_OFFSET:])
    require(result.get('state')==2 and len(result.get('frames',[]))==2,'Healthy copied two-frame terminal result required.')
    require((result['terminal']['lease'],result['terminal']['build_id'],result['terminal']['session_sha256'])==
        (prepared['lease'],prepared['build_id'],expected_session),'GPU/display observation belongs to another sealed bundle.')
    require([v['input_sha256'] for v in result['frames']]==[hashlib.sha256(data).hexdigest() for _,data in expected],'GPU/display result belongs to different caller inputs.')
    path=Path(evidence)/'copied-observation.json';supervisor.durable_frame(path,(json.dumps(result,indent=2)+'\n').encode())
    record['copied_observation']={'path':path.name,'sha256':digest(path)};save()
    require(clock()<deadline,'GPU/display terminal evidence persisted after deadline.')
    record.update(gpu_display_result='observed-success',gpu_display_observation=result)
    try:save()
    except BaseException:
        record['gpu_display_result']='unobserved';raise
    return result

def source_files():
    return [('gpu-display-host-tool.py',Path(__file__)),('gpu-display-preparation.py',Path(preparation.__file__)),
        ('gpu-display-capture.py',ROOT/'apps/gpu-display/capture.py'),('gpu-display-inputs.py',ROOT/'apps/gpu-display/host.py'),
        ('gpu-preparation.py',ROOT/'apps/gpu-render/prepare.py'),('board-preparation.py',ROOT/'apps/board/prepare.py'),
        ('sources.lock.json',ROOT/'sources.lock.json'),('usb-access.py',ROOT/'tools/usb-access.py')]

def validate_software(value,bundle,prepared):
    require(value.get('schema')=='a733-gpu-display-supervised-software-v2' and value.get('physical_access') is False and
        all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','host_lifecycle')),'GPU/display software evidence incomplete.')
    for key,name in [('image','image.elf'),('launcher','launch.bin')]:
        artifact(value[key]);require(value[key]['sha256']==hashlib.sha256(prepared['_frozen'][name]).hexdigest(),'GPU/display software covers different executable bytes.')
    sources=dict(source_files());sources.update({'supervised-host-tool.py':Path(supervisor.__file__),
        'base-host-tool.py':Path(supervisor.base.__file__),'observer-host-tool.py':Path(supervisor.observer.__file__)})
    require(set(value.get('sources',{}))==set(sources),'Incomplete GPU/display software source identities.')
    for name,path in sources.items():require(digest(artifact(value['sources'][name]))==digest(path),'GPU/display source changed: '+name)
    require(value.get('evidence'),'Missing GPU/display software results.')
    for ref in value['evidence']:artifact(ref)

def claimed(database,test_id,owner,bundle,prepared):
    expected={**prepared['files'],'prepared.json':prepared['_prepared_sha256']}
    for name,identity in expected.items():require(digest(bundle/name)==identity,'Reviewed GPU/display bundle changed: '+name)
    db=sqlite3.connect(database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'Board queue halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Coordinator claim required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'GPU/display prerequisites differ.')
        refs={k:artifact(v) for k,v in review['prerequisites'].items()}
        require(refs['supervised-ram-recovery']==(bundle/'admission.json').resolve() and review['prerequisites']['supervised-ram-recovery']['sha256']==prepared['files']['admission.json'],'Reviewed GPU/display physical admission differs.')
        validate_software(json.loads(refs['gpu-display-supervised-software'].read_text()),bundle,prepared)
        require({((bundle/n).resolve(),expected[n]) for n in ('prepared.json','image.elf','launch.bin')}<=
            {(artifact(v),v['sha256']) for v in submission['artifacts']},'Queue omits exact GPU/display bundle.')
    finally:db.close()

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True)
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=preparation.validate_bundle(bundle)
    frozen=prepared['_frozen'];cache={path:frozen[ref['name']] for path,ref in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(frozen['admission.json'],frozen['session.bin'][24:568],frozen['session.bin'][preparation.INPUT_OFFSET:],bytes.fromhex(prepared['build_id']),prepared['image_sha256'],cache,True,image_raw=frozen['image.elf'])
        record=json.loads(frozen['admission.json']);require(record['identity']==identity and record['port']==port,'GPU/display admission covers another live instance.')
    return supervisor.run_session(args,bundle,prepared,lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,prepared,args.evidence,record,save),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0110,capture_seconds=19,result_key='gpu_display_result',source_files=source_files(),
        description='Copied GPU/HDMI observation; retain resources until manual whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error) as error:sys.exit(f'Stopped: {error}')
