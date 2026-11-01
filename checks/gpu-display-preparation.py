#!/usr/bin/env python3
"""Check direct GPU/display inputs, native ABI and preserved upload integrity."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from harness import artifact_refused
import struct
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
p=module('display_preparation_check',ROOT/'apps/gpu-display/prepare.py')
def refused(call):
    try:call()
    except (ValueError,RuntimeError,KeyError,struct.error):return
    raise AssertionError('Invalid preparation admitted')
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('image','root-session','output'):ap.add_argument('--'+name,type=Path,required=True)
    ap.add_argument('--clang',type=Path,default=Path('/usr/lib/llvm-21/bin/clang'))
    ap.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    ap.add_argument('--objcopy',type=Path,default=Path('/usr/lib/llvm-21/bin/llvm-objcopy'))
    a=ap.parse_args();out=p.output_directory(a.output)
    image=a.image.read_bytes();parts,address,build=p.image_data(image)
    original=a.root_session.read_bytes();hdmi=bytearray(original[24:568]);hdmi[48:80]=build
    struct.pack_into('<I',hdmi,300,24000000)
    data=original[p.INPUT_OFFSET:];p.validate_inputs(data);p.session_profile(hdmi)
    hpath=out/'hdmi.bin';hpath.write_bytes(hdmi);inputs=out/'inputs.bin';inputs.write_bytes(data)
    prepared=p.prepare(a.image,None,hpath,inputs,None,out/'bundle',a.clang,a.lld,a.objcopy)
    assert prepared['entry_evidence']=={}
    raw=prepared['_frozen']['session.bin'];record,cache,digests,usb=p.validate_admission(b'{}',bytes(hdmi),data,build,p.sha(image))
    assert raw==p.assemble(bytes(hdmi),data,record,digests,usb) and cache=={}
    request,contract=p.request_bytes(address,build,raw,*struct.unpack_from('<3Q',raw,48))
    assert contract[224:256]==hashlib.sha256(raw).digest() and len(request)==128
    cases=['prepare-without-process-records','native-session-and-contract']
    for offset in (328,336+11*4,392):
        bad=bytearray(hdmi);bad[offset]=0;refused(lambda:p.session_profile(bad))
    cases.append('invalid-pin-handoff')
    bad=bytearray(data);bad[48:80]=bytes(32);refused(lambda:p.validate_inputs(bytes(bad)))
    cases.append('changed-input-digest')
    refused(lambda:p.validate_admission(b'{}',hdmi,data,bytes(32),p.sha(image)))
    refused(lambda:p.validate_admission(json.dumps({'usb_supply_uv':[3300000,3300000,1200000,800000]}).encode(),hdmi,data,build,p.sha(image)))
    refused(lambda:p.validate_admission(json.dumps({'usb_owner':record['owner']}).encode(),hdmi,data,build,p.sha(image)))
    cases+=['wrong-build','wrong-usb-voltage','owner-collision']
    bundle=out/'bundle';metadata=bundle/'prepared.json';before=metadata.read_bytes()
    changed=json.loads(before);changed['payloads'][0]['address']+=64;p.save_json(metadata,changed)
    refused(lambda:p.validate_bundle(bundle));metadata.write_bytes(before);cases.append('upload-address-tamper')
    for path in bundle.iterdir():
        if path.is_file() and path.name not in prepared['files'] and path.name!='prepared.json':
            original=path.read_bytes()
            try:
                path.write_bytes(b'Changed archived build notes or source')
                assert p.validate_bundle(bundle)['_frozen']==prepared['_frozen']
            finally:path.write_bytes(original)
    cases.append('archived-build-files-independent')
    launch=bundle/'launch.bin';before=launch.read_bytes();launch.write_bytes(before+b'x')
    refused(lambda:p.validate_bundle(bundle));launch.write_bytes(before);cases.append('launcher-tamper')
    marker=bundle/'preserved.txt';marker.write_bytes(b'previous evidence')
    artifact_refused(lambda:p.prepare(a.image,None,hpath,inputs,None,bundle,a.clang,a.lld,a.objcopy),bundle)
    cases.append('existing-output-refused-without-changes')
    p.save_json(out/'result.json',dict(passed=True,physical_access=False,cases=cases,image_sha256=p.sha(image)))
    print('PASS GPU/display preparation:',len(cases),'case groups')
if __name__=='__main__':main()
