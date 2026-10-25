#!/usr/bin/env python3
"""Check truthful single-core producer packaging through the existing host path."""
import argparse, hashlib, importlib.util, json, struct, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
def run(image,manifest,fixture,native,out,clang,lld):
    out.mkdir(parents=True,exist_ok=False)
    host=module('single_core_storage_host',ROOT/'tools/check-a733-storage-read-supervised.py')
    prep=host.preparation;body=image.read_bytes();record=json.loads(manifest.read_text())
    prep.image_manifest_check(record,body)
    refused=[]
    for name,changes in [('old-profile',{'execution_profile':'storage-read-supervised-v2'}),
                          ('old-target',{'target':'sunstone-a733-storage-read','execution_profile':'storage-read-supervised-v2'}),
                          ('wrong-image',{'final_elf':'00'*32})]:
        try:prep.image_manifest_check({**record,**changes},body)
        except ValueError:refused.append(name)
        else:raise AssertionError(name)
    template=(fixture/'session-template.bin').read_bytes()
    assert int.from_bytes(template[532:536],'little')==2
    # Generic board packaging retains its native mode1 contract. Since
    # 5fd948d4 session_check admits modes 1 and 2 by default, and the board's
    # prepare() passes (1,) for every profile except the board's own
    # single-core one (apps/board/prepare.py, prepare); check that rule.
    try:prep.board.session_check(template[:592],usb_reference_modes=(1,))
    except ValueError:refused.append('generic-board-rom-mode')
    else:raise AssertionError('Generic board accepted ROM mode')
    admission=json.loads((fixture/'admission.json').read_text())
    # Snapshot the same authority producer, changing one concrete authorization.
    cache=prep.validate_admission(admission,hashlib.sha256(body).hexdigest(),hashlib.sha256(template).hexdigest(),template)
    platform=json.loads(cache[admission['platform']['path']])
    ref=platform['conditions']['usb-clock-power-reset-ownership']
    fact=json.loads(cache[ref['path']]);fact['authorize_one_disconnect_and_device_reset']=False
    import copy
    changed=copy.deepcopy(admission);modified=dict(cache)
    data=json.dumps(fact,sort_keys=True).encode();modified[ref['path']]=data
    platform['conditions']['usb-clock-power-reset-ownership']['sha256']=hashlib.sha256(data).hexdigest()
    data=json.dumps(platform,sort_keys=True).encode();modified[admission['platform']['path']]=data
    changed['platform']['sha256']=hashlib.sha256(data).hexdigest()
    try:prep.validate_admission(changed,hashlib.sha256(body).hexdigest(),hashlib.sha256(template).hexdigest(),template,modified,True)
    except ValueError:refused.append('rom-takeover-without-authorization')
    else:raise AssertionError('Missing ROM takeover authorization accepted')
    bundle=out/'MODEL-NOT-PHYSICAL-bundle'
    prep.prepare(image,manifest,fixture/'admission.json',fixture/'session-template.bin',bundle,clang,lld)
    frozen=host.freeze_bundle(bundle)
    assert frozen['_frozen']['image.elf']==body
    actual=json.loads(frozen['_frozen']['image-input-manifest.json'])
    assert actual['target']=='sunstone-a733-storage-read-single-core'
    assert actual['execution_profile']=='storage-read-single-core-supervised-v2'
    # Preserve actual native serializer outputs and native completion counts;
    # only rebind the package's freshly randomized epoch/generation.
    raw=frozen['_frozen']['session.bin'];frames=[]
    for name in ('idle-frame.bin','pending-frame.bin','native-frame.bin','pending-last-frame.bin','last-frame.bin'):
        frame=bytearray((native/name).read_bytes())
        struct.pack_into('<2Q',frame,16,*struct.unpack_from('<2Q',raw,24));frames.append(bytes(frame))
    packets=[b'\0',host.decoder.read_request(19,2,1),b'\0',host.decoder.read_request(33554424,8,2),b'\0']
    capture=out/'capture';capture.mkdir();calls=[];observed={}
    def exchange(packet,timeout):
        index=len(calls);assert packet==packets[index];calls.append(packet);return frames[index]
    result=host.capture(exchange,raw,capture,observed,lambda:None,time.monotonic()+10,ranges=((19,2),(33554424,8)))
    assert result['status']==1 and len(calls)==5 and observed['storage_read_application_reads']==2
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
      image_sha256=hashlib.sha256(body).hexdigest(),refused=refused,
      prepared_sha256=frozen['_prepared_sha256'],launcher_sha256=frozen['files']['launch.bin'],
      actual_native_frames=5,host_ranges=2,native_completed_reads=2,
      scope='Actual single-core manifest identity, full original authority/session preparation, launcher compilation, bundle validation and host freeze. Modeled authority never authorizes physical use; native read/USB completion behavior is covered separately.'),indent=2)+'\n')
    print('PASS single-core storage producer and host package')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','fixture','native','output','clang','lld'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.image,a.manifest,a.fixture,a.native,a.output,a.clang,a.lld)
