#!/usr/bin/env python3
"""Check indexed public upload/capture and sealed bundles with real native frames.

Physical admission and unchanged launcher construction are explicit modeled
boundaries. Pixel bytes come from injected-DMA native evidence, not hardware.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import time
ROOT=Path(__file__).resolve().parents[1]

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def refused(call):
    try:call()
    except (ValueError,RuntimeError):return
    raise AssertionError('Invalid bytes accepted')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','inputs','native','launcher','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    prepare=module('prepare',ROOT/'apps/h264/prepare.py');host=module('host',ROOT/'tools/check-a733-h264-supervised.py')
    manifest=a.output/'image-manifest.json';image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest()
    manifest.write_text(json.dumps(dict(target='sunstone-a733-h264',execution_profile='h264-supervised-v1',final_elf=image_hash)))
    admission=a.output/'modeled-admission.json';admission.write_text(json.dumps(dict(reservation='modeled-no-physical-access')))
    admission_check=prepare.validate_admission;launcher_build=prepare.build_launcher
    prepare.validate_admission=lambda *args,**kwargs:{}
    prepare.build_launcher=lambda out,*args:shutil.copy2(a.launcher,out/'launch.bin')
    results=[]
    try:
        for mode,variant in [('normal','normal'),('qualification','nominal'),('P-error','P-error')]:
            oracle=None if mode=='normal' else (a.inputs/'reference.yuv').read_bytes()
            session=(a.inputs/('normal-session.bin' if mode=='normal' else 'qualification-session.bin')).read_bytes()
            stream=(a.inputs/'input.h264').read_bytes();wire=prepare.upload_bytes(session,stream,oracle)
            frames=[(a.native/f'{variant}-frame{i}.bin').read_bytes() for i in range(2)]
            folder=a.output/mode;folder.mkdir();record={};writes=[];offset=[0];selected=[None]
            def save():(folder/'record.json').write_text(json.dumps(record,indent=2)+'\n')
            class Device:
                def write(self,endpoint,part,timeout):
                    assert endpoint==2 and 0<timeout<=500;part=bytes(part);writes.append(part)
                    if offset[0]<len(wire):
                        assert part==wire[offset[0]:offset[0]+len(part)];offset[0]+=len(part)
                    else:
                        assert len(part)==5 and part[0]==0;selected[0]=struct.unpack_from('<I',part,1)[0]
                    return len(part)
                def read(self,endpoint,count,timeout):assert endpoint==0x81 and count==2464;return frames[selected[0]]
            result=host.capture_device(Device(),lambda:None,time.monotonic()+10,session,folder,record,save,wire,oracle)
            assert result['picture_index']==1 and result['picture_count']==2
            assert result['status']==(2 if mode=='P-error' else 1)
            assert (folder/'output-000.yu12').read_bytes()==frames[0][160:]
            if mode=='P-error':assert not (folder/'output-001.yu12').exists() and not (folder/'output.yu12').exists()
            else:assert (folder/'output.yu12').read_bytes()==frames[0][160:]+frames[1][160:]
            altered=bytearray(frames[1]);struct.pack_into('<I',altered,128,0)
            refused(lambda:host.decoder.decode(altered,session,oracle,1))
            if mode!='P-error':
                bundle=a.output/(mode+'-bundle')
                prepared=prepare.prepare(a.image,manifest,admission,a.inputs/(mode+'-session.bin'),bundle,
                    '/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',a.inputs/'input.h264',a.inputs/'reference.yuv' if oracle else None)
                assert prepared['image_sha256']==image_hash and prepared['_upload'][4:8]==struct.pack('<I',2)
                changed=(bundle/'input.h264').read_bytes();(bundle/'input.h264').write_bytes(changed[:-1]);refused(lambda:prepare.validate_bundle(bundle));(bundle/'input.h264').write_bytes(changed)
            results.append(dict(mode=mode,queries=[struct.unpack_from('<I',x,1)[0] for x in writes if len(x)==5 and x[0]==0],status=result['status'],durable_pictures=len(record['copied_h264_pictures'])))
    finally:prepare.validate_admission=admission_check;prepare.build_launcher=launcher_build
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=image_hash,cases=results),indent=2)+'\n')
    print('PASS indexed two-picture normal/oracle capture, sealed runtime bundles and durable IDR on P failure')

if __name__=='__main__':main()
