#!/usr/bin/env python3
"""Capture bounded copied GPU/HDMI evidence; retain the whole board lifetime."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
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





def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence'):parser.add_argument('--'+name,type=Path,required=True)

    parser.add_argument('--supervised-manual-reconnect',action='store_true')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=preparation.validate_bundle(bundle)
    frozen=prepared['_frozen'];cache={path:frozen[ref['name']] for path,ref in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(preparation.native_configuration(frozen['session.bin']),frozen['session.bin'][24:568],frozen['session.bin'][preparation.INPUT_OFFSET:],bytes.fromhex(prepared['build_id']),prepared['image_sha256'],cache,True,image_raw=frozen['image.elf'])

    return supervisor.run_session(args,bundle,prepared,lambda:None,validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,prepared,args.evidence,record,save),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0110,capture_seconds=19,result_key='gpu_display_result',source_files=source_files(),
        description='Copied GPU/HDMI observation; retain resources until manual whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError) as error:sys.exit(f'Stopped: {error}')
