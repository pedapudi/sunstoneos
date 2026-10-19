#!/usr/bin/env python3
"""Check actual full-HD producer/capture with native frames and explicit ZLP transport."""
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
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('image','inputs','native','launcher','output'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--bundles-only',action='store_true',help='Validate only changed producer metadata; preserve capture results.')
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    stream=module('stream_host',ROOT/'apps/h264/stream.py');prepare=module('prepare_host',ROOT/'apps/h264/prepare.py');capture=module('capture_host',ROOT/'tools/check-a733-h264-supervised.py')
    image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest();mf=a.output/'image-manifest.json';mf.write_text(json.dumps(dict(target='sunstone-a733-h264',execution_profile='h264-fullhd-supervised-v1',final_elf=image_hash)))
    admission=a.output/'modeled-admission.json';admission.write_text(json.dumps({'reservation':'modeled-no-physical-access'}))
    launcher=a.launcher;prepare.validate_admission=lambda *unused,**kwargs:{}
    prepare.build_launcher=lambda out,*unused,**kwargs:shutil.copy2(launcher,out/'launch.bin')
    reports=[]
    for mode in ('normal','qualification'):
        out=a.output/(mode+'-bundle');prepared=prepare.prepare(a.image,mf,admission,a.inputs/(mode+'-session.bin'),out,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',a.inputs/mode/'manifest.bin')
        assert prepared['image_sha256']==image_hash and len(prepared['_stream_frames'])==6
        modified=out/'frame-003.h264';original=modified.read_bytes();modified.write_bytes(original[:-1])
        try:prepare.validate_bundle(out)
        except (ValueError,RuntimeError):pass
        else:raise AssertionError('Changed sealed frame accepted')
        modified.write_bytes(original)
    if a.bundles_only:
        (a.output/'result.json').write_text(json.dumps(dict(passed=True,image_sha256=image_hash,physical_access=False,scope='Final version5 full-HD normal/qualification bundles; modeled physical admission; reused actual checked launcher; no capture replay.'),indent=2)+'\n')
        print('PASS version5 full-HD producer/bundle identities');return
    for mode in ('nominal','later-failure'):
        session=(a.inputs/'qualification-session.bin').read_bytes();manifest=(a.inputs/'qualification/manifest.bin').read_bytes();inputs=stream.validate_inputs(a.inputs/'qualification',manifest,3)
        frames=[(a.native/f'nominal-frame{i}.bin').read_bytes() for i in range(6)]
        if mode=='later-failure':frames[5]=(a.native/'later-failure-frame5.bin').read_bytes()
        folder=a.output/mode;folder.mkdir();record={};writes=[];requests=bytearray();queue=[];commands=[];zlp_reads=[0]
        class Device:
            def write(self,ep,part,timeout):
                assert ep==2 and 0<timeout<=500;part=bytes(part);writes.append(part);requests.extend(part)
                while len(requests)>=32:
                    magic,version,index,aux,epoch,generation=struct.unpack_from('<4I2Q',requests)
                    assert version==1 and (epoch,generation)==struct.unpack_from('<2Q',session,24)
                    needed=32+(aux if magic==stream.M else len(inputs[index][0])+(len(inputs[index][1]) if inputs[index][1] else 0) if magic==stream.F else 32 if magic==stream.A else 0)
                    if len(requests)<needed:break
                    packet=bytes(requests[:needed]);del requests[:needed];commands.append((magic,index))
                    if magic==stream.M:assert packet==stream.manifest_upload(session,manifest)
                    elif magic==stream.F:assert packet==stream.frame_upload(session,manifest,index,*inputs[index])
                    elif magic==stream.Q:
                        assert not aux;raw=frames[index]
                        # Model both the prior terminating ZLP and FIFO-drain ZLPs.
                        queue.append(b'');queue.append(raw[:192])
                        for offset in range(192,len(raw),2048):queue.extend([b'',raw[offset:offset+2048]])
                        queue.append(b'')
                    elif magic==stream.A:
                        assert packet[32:]==frames[index][96:128]
                        assert (folder/f'output-{index:03}.yu12').read_bytes()==inputs[index][1]
                        saved=json.loads((folder/'record.json').read_text())
                        assert any(item['path']==f'output-{index:03}.yu12' for item in saved['copied_h264_pictures'])
                    else:raise AssertionError('Unknown command')
                return len(part)
            def read(self,ep,n,timeout):
                assert ep==0x81 and 0<n<=4096 and 0<timeout<=500 and queue
                data=queue.pop(0)
                if not data:zlp_reads[0]+=1;return b''
                if len(data)>n:queue.insert(0,data[n:]);data=data[:n]
                return data
        def save():(folder/'record.json').write_text(json.dumps(record,indent=2)+'\n')
        result=capture.capture_device(Device(),lambda:None,time.monotonic()+30,session,folder,record,save,stream.manifest_upload(session,manifest),None,inputs,manifest)
        expected=6 if mode=='nominal' else 5
        assert len(record['copied_h264_pictures'])==expected and result['status']==(1 if mode=='nominal' else 2)
        assert [i for magic,i in commands if magic==stream.A]==list(range(expected)) and zlp_reads[0]>6
        for i in range(expected):assert (folder/f'output-{i:03}.yu12').read_bytes()==inputs[i][1]
        if mode=='later-failure':assert not (folder/'output-005.yu12').exists()
        reports.append(dict(case=mode,retained_outputs=expected,ZLP_reads=zlp_reads[0],ACKs=expected))
    normal_session=(a.inputs/'normal-session.bin').read_bytes();normal_manifest=(a.inputs/'normal/manifest.bin').read_bytes();normal=(a.native/'normal-frame0.bin').read_bytes()
    decoded=stream.decode(normal,normal_session,normal_manifest,0,None);assert decoded['status']==1 and decoded['oracle_verified'] is False
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=image_hash,cases=reports,normal_output=decoded,scope='Actual producer/bundle/capture and native-produced frames. Physical admission and USB transport modeled; actual separately checked launcher bytes reused; injected native pixels are not physical conformance.'),indent=2)+'\n')
    print('PASS full-HD sealed bundles, exact header/payload/ZLP reads, durable output-before-ACK and retained later failure')
if __name__=='__main__':main()
