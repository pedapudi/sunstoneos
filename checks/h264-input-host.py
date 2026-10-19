#!/usr/bin/env python3
"""Check real upload/bundle/capture producers with explicit transport authority.

Initial physical admission and unchanged launcher construction are substituted.
The actual bundle constructor/validator, upload producer and capture run.
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
    raise AssertionError('Invalid input accepted')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','inputs','native','launcher','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    prepare=module('h264_prepare',ROOT/'apps/h264/prepare.py');capture=module('h264_capture',ROOT/'tools/check-a733-h264-supervised.py')
    image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest()
    manifest=a.output/'image-manifest.json';manifest.write_text(json.dumps(dict(target='sunstone-a733-h264',execution_profile='h264-supervised-v1',final_elf=image_hash)))
    admission=a.output/'modeled-admission.json';admission.write_text(json.dumps({'reservation':'modeled-no-physical-access'}))
    stream=(a.inputs/'normal-upload.bin').read_bytes()[32:]
    oracle=(a.inputs/'qualification-upload.bin').read_bytes()[-2304:]
    stream_path=a.output/'input.h264';stream_path.write_bytes(stream)
    oracle_path=a.output/'oracle.yuv';oracle_path.write_bytes(oracle)
    original_admission=prepare.validate_admission;original_launcher=prepare.build_launcher
    prepare.validate_admission=lambda *args,**kwargs:{}
    prepare.build_launcher=lambda out,*args:shutil.copy2(a.launcher,out/'launch.bin')
    results=[]
    try:
        for mode in ('normal','qualification'):
            session=(a.inputs/(mode+'-session.bin')).read_bytes()
            bundle=a.output/(mode+'-bundle')
            value=prepare.prepare(a.image,manifest,admission,a.inputs/(mode+'-session.bin'),bundle,
                '/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',stream_path,
                oracle_path if mode=='qualification' else None)
            raw=(bundle/'session.bin').read_bytes()
            assert value['image_sha256']==image_hash
            assert value['_upload']==prepare.upload_bytes(raw,stream,oracle if mode=='qualification' else None)
            assert ('oracle.yuv' in value['files'])==(mode=='qualification')
            assert all(item['name'] not in ('input.h264','oracle.yuv') for item in value['segments'])
            # Sealed runtime bytes are separate from ROM image upload segments.
            saved=(bundle/'input.h264').read_bytes();(bundle/'input.h264').write_bytes(saved[:-1])
            refused(lambda:prepare.validate_bundle(bundle));(bundle/'input.h264').write_bytes(saved)
            prepare.validate_bundle(bundle)
            refused(lambda:prepare.upload_bytes(session,stream[:-1],oracle if mode=='qualification' else None))
            refused(lambda:prepare.upload_bytes(session,stream,None if mode=='qualification' else oracle))
            frame=(a.native/(mode+'-frame.bin')).read_bytes();writes=[];record={};destination=a.output/(mode+'-capture');destination.mkdir()
            class Device:
                def write(self,endpoint,data,timeout):assert endpoint==2 and 0<timeout<=500;writes.append(bytes(data));return len(data)
                def read(self,endpoint,count,timeout):assert endpoint==0x81 and count==2432;return frame
            def save():(destination/'record.json').write_text(json.dumps(record,indent=2)+'\n')
            upload=prepare.upload_bytes(session,stream,oracle if mode=='qualification' else None)
            result=capture.capture_device(Device(),lambda:None,time.monotonic()+5,session,destination,record,save,
                upload,oracle if mode=='qualification' else None)
            assert b''.join(writes)==upload+b'\0' and all(len(part)<=512 for part in writes)
            assert result['copied_verified'] and result['oracle_verified']==(mode=='qualification')
            assert (destination/'output.yu12').read_bytes()==frame[128:]
            # A changed output byte with the original digest cannot publish.
            bad=bytearray(frame);bad[-1]^=1
            refused(lambda:capture.decoder.decode(bad,session,oracle if mode=='qualification' else None))
            results.append(dict(mode=mode,image_sha256=image_hash,input_sha256=hashlib.sha256(stream).hexdigest(),
                                upload_bytes=len(upload),oracle_verified=result['oracle_verified']))
    finally:
        prepare.validate_admission=original_admission;prepare.build_launcher=original_launcher
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results,
        boundary='Physical authority and unchanged launcher reused; transport modeled; actual public bundle/upload/capture producers'),indent=2)+'\n')
    print('PASS same image with runtime normal/oracle inputs, sealed bundle refusal and durable host capture')

if __name__=='__main__':main()
