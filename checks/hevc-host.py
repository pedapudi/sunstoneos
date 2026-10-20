#!/usr/bin/env python3
"""Check HEVC preparation and durable capture using a frozen actual native image.

All pixel payloads and device/scheduling effects below are explicit software
models. Admission files supplied to this offline check must identify themselves
as fixtures, not actual physical authority. No board, USB, queue, native image
build, proprietary reference library or numerical decoder executes. Preparation
compiles its SRAM launcher once; later cases reuse that exact artifact.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','session','input','admission','output','clang','lld'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--case',action='append',choices=('prepare','nominal','pending','fatal','terminal-copy',
        'raw-failure','output-failure','record-failure','post-ack-failure','fragment-failure','deadline','identity'))
    parser.add_argument('--bundle',type=Path,help='Reuse an already prepared fixture bundle; do not compile another launcher.')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    caller=module('hevc_supervised_host_check',ROOT/'tools/check-a733-hevc-supervised.py')
    host,prep=caller.host,caller.preparation
    fixture=json.loads(args.admission.read_text())
    assert fixture.get('physical_access') is False and fixture.get('fixture_only'), 'Explicit nonphysical fixture authority required.'
    results=[]
    if args.bundle:
        bundle=args.bundle
    else:
        bundle=args.output/'MODEL-NOT-PHYSICAL-bundle'
        prep.prepare(args.image,args.manifest,args.admission,args.session,bundle,str(args.clang),str(args.lld),args.input)
    frozen=caller.freeze_bundle(bundle)
    session=frozen['_frozen']['session.bin'];upload=frozen['_upload']
    assert frozen['image_sha256']==hashlib.sha256(args.image.read_bytes()).hexdigest()
    epoch,generation=struct.unpack_from('<2Q',session,24)
    images=[bytes((i*3+ordinal)&255 for i in range(460800)) for ordinal in range(2)]
    frames=[]
    for ordinal,data in enumerate(images):
        header=bytearray(160)
        struct.pack_into('<4I2Q',header,0,0x44363235,1,1,len(data),epoch,generation)
        header[32:64]=session[768:800];header[64:96]=hashlib.sha256(data).digest()
        struct.pack_into('<11I',header,96,ordinal,2,7 if ordinal else 0,640,480,640,640,307200,6,1 if ordinal else 2,0)
        frames.append(bytes(header)+data)
    selected=set(args.case or ('prepare','nominal','pending','fatal','terminal-copy','raw-failure','output-failure',
        'record-failure','post-ack-failure','fragment-failure','deadline','identity'))
    if 'prepare' in selected:
        for name,change in [('gap',lambda b:struct.pack_into('<Q',b,816,struct.unpack_from('<Q',b,808)[0]+1)),
                            ('wrap',lambda b:struct.pack_into('<Q',b,824,2**64-1)),
                            ('empty',lambda b:struct.pack_into('<Q',b,808,0))]:
            bad=bytearray(session);change(bad)
            try:prep.session_check(bad)
            except ValueError:pass
            else:raise AssertionError(name)
        for name,data,reference in [('short',upload[32:-1],None),('extra',upload[32:]+b'\0',None),
                                    ('oracle',upload[32:],b'pixels')]:
            try:prep.upload_bytes(data,reference,session)
            except ValueError:pass
            else:raise AssertionError(name)
        # Use the actual build-ID address from the actual image when checking bytes.
        _,address,identity=prep.image_parts(args.image)
        request,contract=prep.request_bytes(address,identity,session,epoch,generation,frozen['lease'])
        assert request==frozen['_frozen']['request.bin'] and contract==frozen['_frozen']['contract.bin']
        results.append(dict(case='prepare',actual_image_geometry=True,strict_admission=True,range_upload_refusals=True))

    for case in ('nominal','pending','fatal','terminal-copy','raw-failure','output-failure','record-failure',
                 'post-ack-failure','fragment-failure','deadline','identity'):
        if case not in selected:continue
        evidence=args.output/case;evidence.mkdir();record={};failed=False
        class Device:
            def __init__(self):
                self.upload=bytearray();self.reply=bytearray();self.acks=[];self.queries=[];self.reads=0;self.clock=1;self.ord=0
            def write(self,endpoint,packet,timeout):
                assert endpoint==2 and timeout>0
                if len(self.upload)<len(upload):
                    self.upload.extend(packet);assert upload.startswith(self.upload);return len(packet)
                assert bytes(self.upload)==upload
                if len(packet)==45:
                    assert packet==host.decoder.ack(frames[self.ord],session,self.ord)
                    saved=json.loads((evidence/'session.json').read_text())
                    value=saved['hevc_outputs'][-1]
                    assert value['ordinal']==self.ord and value['ack_write_intent'] and not value['ack_sent']
                    assert (evidence/value['path']).read_bytes()==images[self.ord]
                    assert (evidence/saved['hevc_frames'][-1]['path']).read_bytes()==frames[self.ord]
                    self.acks.append(self.ord)
                    if case=='deadline':self.clock=101
                    return len(packet)
                assert len(packet)==2 and packet[0]==0
                self.ord=packet[1];assert self.ord==0 or self.acks==[0]
                self.queries.append(self.ord)
                raw=frames[self.ord]
                if case=='pending' and self.queries.count(self.ord)==1:
                    raw=bytearray(raw[:160]);struct.pack_into('<II',raw,8,0,0);raw[64:96]=bytes(32)
                    if self.ord:struct.pack_into('<I',raw,104,0)
                if case=='fatal' and self.ord:
                    raw=bytearray(raw[:160]);struct.pack_into('<II',raw,8,2,0);raw[64:96]=bytes(32);struct.pack_into('<I',raw,136,1)
                if case=='terminal-copy':
                    raw=bytearray(raw);struct.pack_into('<I',raw,136,1)
                if case=='identity':
                    raw=bytearray(raw);raw[32]^=1
                self.reply=bytearray(raw);return len(packet)
            def read(self,endpoint,size,timeout):
                assert endpoint==0x81 and timeout>0;self.reads+=1
                if case=='fragment-failure' and self.reads==2:raise OSError('modeled fragment transport failure')
                count=min(size,97 if size<=160 else 16384,len(self.reply))
                data=bytes(self.reply[:count]);del self.reply[:count];return data
        device=Device()
        def save():
            nonlocal failed
            values=record.get('hevc_outputs',[])
            fail=(case=='record-failure' and len(values)==2 or
                  case=='post-ack-failure' and values and values[-1].get('ack_sent'))
            if fail and not failed:
                failed=True;raise OSError('modeled record durability failure')
            caller.supervisor.save_json(evidence/'session.json',record)
        original=host.supervisor.durable_frame
        def durable(path,data):
            nonlocal failed
            fail=(case=='raw-failure' and path.name.startswith('picture-0-') or
                  case=='output-failure' and path.name=='picture-1.nv12')
            if fail and not failed:
                failed=True;raise OSError('modeled output durability failure')
            original(path,data)
        with patch.object(host.supervisor,'durable_frame',durable):
            try:
                result=host.capture_device(device,lambda:None,100,session,evidence,record,save,upload=upload,
                    now=lambda:device.clock,pause=lambda _:None)
            except (OSError,ValueError):
                assert case in ('raw-failure','output-failure','record-failure','post-ack-failure','fragment-failure','deadline','identity')
                assert record['hevc_result']=='unobserved' and 'durable_result' not in record
            else:
                assert result['status']==(2 if case in ('fatal','terminal-copy') else 1)
                assert record['hevc_result']==('observed-failure' if result['status']==2 else 'observed-success')
        expected=[0,1] if case in ('nominal','pending') else [0] if case in ('fatal','output-failure','record-failure','post-ack-failure','deadline') else []
        assert device.acks==expected
        if case in ('post-ack-failure','deadline'):
            assert device.queries==[0] and record['hevc_outputs'][0]['ack_sent']
        if case in ('fatal','output-failure','record-failure'):
            assert (evidence/'picture-0.nv12').read_bytes()==images[0] and record['hevc_outputs'][0]['ack_sent']
        if case=='terminal-copy':assert (evidence/'picture-0.nv12').read_bytes()==images[0] and device.queries==[0]
        if case=='fragment-failure':assert device.queries==[0] and len(record['hevc_fragments'])==1
        assert not record.get('numerical_qualified')
        results.append(dict(case=case,acked=device.acks,queries=device.queries,physical_access=False))
    output=dict(passed=True,physical_access=False,native_image_sha256=frozen['image_sha256'],
        launcher_sha256=caller.digest(bundle/'launch.bin'),bundle=str(bundle),cases=results,scope=__doc__)
    (args.output/'result.json').write_text(json.dumps(output,indent=2)+'\n');print(json.dumps(output))


if __name__=='__main__':main()
