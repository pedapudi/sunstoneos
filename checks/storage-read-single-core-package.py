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
    # Build-provenance fields cannot override the linked startup.
    prep.image_manifest_check({'target':'unused','final_elf':'00'*32},body)
    damaged=bytearray(body);struct.pack_into('<Q',damaged,24,0)
    try:prep.image_manifest_check({},damaged)
    except ValueError:refused.append('wrong-native-entry')
    else:raise AssertionError('Invalid native startup accepted')
    template=(fixture/'session-template.bin').read_bytes()
    assert int.from_bytes(template[532:536],'little')==2
    # Generic board packaging retains its native mode1 contract: the board's own
    # prepare(), given a generic board manifest for these bytes, must stop at
    # the USB0 start mode before writing any bundle.
    generic=out/'generic-board-manifest.json';generic.write_text(json.dumps({'target':'sunstone-a733-board',
        'execution_profile':prep.board.PROFILES['sunstone-a733-board'],'final_elf':hashlib.sha256(body).hexdigest()}))
    board_template=out/'generic-board-template.bin';board_template.write_bytes(template[:592])
    try:prep.board.prepare(image,generic,fixture/'admission.json',board_template,out/'generic-board-bundle',clang,lld)
    except ValueError as error:
        assert 'Unsupported USB0 reference/takeover mode.' in str(error),error;refused.append('generic-board-rom-mode')
    else:raise AssertionError('Generic board accepted ROM mode')
    assert not (out/'generic-board-bundle').exists()
    admission=json.loads((fixture/'admission.json').read_text())
    direct={k:admission[k] for k in ('identity','port','reservation')}
    target=out/'direct-target.json';target.write_text(json.dumps(direct))
    prep.validate_admission(direct,'unused','unused',template)
    bundle=out/'MODEL-NOT-PHYSICAL-bundle'
    prep.prepare(image,None,target,fixture/'session-template.bin',bundle,clang,lld)
    frozen=host.freeze_bundle(bundle)
    assert frozen['_frozen']['image.elf']==body
    assert 'image-input-manifest.json' not in frozen['_frozen'] and not frozen['entry_evidence']
    # Preserve actual native serializer outputs and native completion counts;
    # only rebind the package's freshly randomized epoch/generation.
    raw=frozen['_frozen']['session.bin'];frames=[]
    names=('host-idle-frame.bin','host-frame.bin') if (native/'host-frame.bin').exists() else (
        'idle-frame.bin','pending-frame.bin','native-frame.bin','pending-last-frame.bin','last-frame.bin')
    for name in names:
        frame=bytearray((native/name).read_bytes())
        struct.pack_into('<2Q',frame,16,*struct.unpack_from('<2Q',raw,24));frames.append(bytes(frame))
    if len(frames)==2:
        value=host.decoder.decode(frames[-1],raw);ranges=((value['first_sector'],value['blocks']),)
        packets=[b'\0',host.decoder.read_request(*ranges[0],1,struct.unpack_from('<I',raw,860)[0])]
    else:
        ranges=((19,2),(33554424,8))
        packets=[b'\0',host.decoder.read_request(19,2,1),b'\0',host.decoder.read_request(33554424,8,2),b'\0']
    capture=out/'capture';capture.mkdir();calls=[];observed={}
    def exchange(packet,timeout):
        index=len(calls);assert packet==packets[index];calls.append(packet);return frames[index]
    result=host.capture(exchange,raw,capture,observed,lambda:None,time.monotonic()+10,ranges=ranges)
    assert result['status']==1 and len(calls)==len(frames) and observed['storage_read_application_reads']==2
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
      image_sha256=hashlib.sha256(body).hexdigest(),refused=refused,
      prepared_sha256=frozen['_prepared_sha256'],launcher_sha256=frozen['files']['launch.bin'],
      actual_native_frames=len(frames),host_ranges=len(ranges),native_completed_reads=2,
      scope='Actual single-core ELF startup, direct target/session preparation, launcher compilation and bundle validation. Modeled authority never authorizes physical use; native read/USB completion behavior is covered separately.'),indent=2)+'\n')
    print('PASS single-core storage producer and host package')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','fixture','native','output','clang','lld'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.image,a.manifest,a.fixture,a.native,a.output,a.clang,a.lld)
