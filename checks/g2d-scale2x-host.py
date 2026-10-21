#!/usr/bin/env python3
"""Check new scaling geometry and one exact native copied result; no pixel oracle."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('entry','response','receipt','output'):parser.add_argument('--'+key,type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    host=module('g2d_scale2x_host_check',ROOT/'apps/g2d/host.py');prep=host.preparation
    producer=module('g2d_scale2x_producer_check',ROOT/'apps/g2d/producer.py')
    session=(args.entry/'session.bin').read_bytes();source=(args.entry/'input.argb').read_bytes()
    image=(args.entry/'image.elf').read_bytes();upload=(args.entry/'upload.bin').read_bytes()
    response=args.response.read_bytes();receipt=args.receipt.read_bytes()
    geometry=list(struct.unpack_from('<12I',session,856))
    assert geometry==[60,64,256,0,4,9,17,32,24,40,42,0]
    assert upload==prep.upload_bytes(source,session)
    prep.embedded_reference(image,session)
    owner,authority,client=struct.unpack_from('<3Q',session,616)
    facts={name:(args.entry/(name+'.json')).read_bytes() for name in ('initial','memory','electrical','supervision')}
    reconstructed=producer.extend(session[:592],source,owner=owner,authority=authority,client=client,**facts,
        width=60,height=64,stride=256,operation=4,dx=9,dy=17,w=32,h=24,sx=40,sy=42,color=0)
    assert reconstructed==session and host.expected_output(source,session) is None
    valid=(geometry,[2,2,8,0,4,0,0,2,2,1,1,0],
        [1280,720,5120,0,4,0,0,1280,720,640,360,0],
        [60,64,256,0,4,9,17,32,24,44,52,0],[3,3,16,0,4,1,1,2,2,2,2,0])
    for value in valid:assert prep.geometry_check(value)==value[1]*value[2]
    refusals=[]
    changes=(('zero-destination-width',7,0),('zero-destination-height',8,0),
        ('odd-destination-width',7,31),('odd-destination-height',8,23),
        ('source-x-at-edge',9,60),('source-y-at-edge',10,64),
        ('source-half-width-overflow',9,45),('source-half-height-overflow',10,53),
        ('destination-width-overflow',5,29),('destination-height-overflow',6,41),
        ('nonzero-color',11,1),('source-origin-overflow',9,0xffffffff))
    for name,index,value in changes:
        trial=geometry.copy();trial[index]=value
        try:prep.geometry_check(trial)
        except ValueError:refusals.append(name)
        else:raise AssertionError('Invalid scale2x geometry accepted: '+name)
    for name,pixels in (('altered-source',bytes([source[0]^1])+source[1:]),('short-source',source[:-4])):
        try:host.expected_output(pixels,session)
        except ValueError:refusals.append(name)
        else:raise AssertionError('Unchecked interpolation skipped source authentication: '+name)
    decoded=host.decode(response,session);assert decoded['status']==1 and decoded['operation']=='scale2x'
    host.receipt_check(receipt,session,response[:192])
    assert host.untouched_zero(response[192:],session)
    cases=[]
    for case in ('actual-native-copy','outside-rectangle'):
        folder=args.output/case;folder.mkdir();record={};queries=[];uploads=[];acks=[]
        frame=response
        if case=='outside-rectangle':
            # Rehash the altered payload so the independent bounded-write
            # invariant, rather than the transport digest, must refuse it.
            altered=bytearray(response);offset=192+geometry[6]*geometry[2]+geometry[0]*4
            assert altered[offset]==0;altered[offset]=1
            altered[96:128]=hashlib.sha256(altered[192:]).digest();frame=bytes(altered)
        def save():host.supervisor.save_json(folder/'session.json',record)
        def send_upload(data):assert data==upload;uploads.append(hashlib.sha256(data).hexdigest())
        def send_ack(data):
            assert case=='actual-native-copy' and data==host.ack(response,session)
            saved=json.loads((folder/'session.json').read_bytes());copied=saved['g2d_output']
            assert copied['ack_write_intent'] and copied['matches_cpu_reference'] is None
            assert copied['reference_qualified'] is False and copied['untouched_bytes_zero'] is True
            assert (folder/copied['path']).read_bytes()==response[192:]
            acks.append(data.hex())
        def exchange(packet,timeout):
            assert packet==b'\0' and 0<timeout<=500;queries.append(packet.hex())
            assert len(queries)<=2 and (len(queries)==1 or acks)
            return frame if len(queries)==1 else receipt
        result=host.capture(exchange,session,folder,record,save,100,upload=upload,
            send_upload=send_upload,send_ack=send_ack,now=lambda:1,pause=lambda delay:None)
        assert len(uploads)==1 and result['matches_cpu_reference'] is None
        assert result['reference_qualified'] is False and result['reference_comparison']=='unchecked-no-interpolation-oracle'
        assert (folder/'output.argb').read_bytes()==frame[192:]
        if case=='actual-native-copy':
            assert record['g2d_result']=='observed-success' and result['ack_observed'] is True
            assert len(acks)==1 and len(queries)==2 and (folder/'g2d-ack-receipt.bin').read_bytes()==receipt
        else:
            assert record['g2d_result']=='observed-failure' and not acks and len(queries)==1
            assert result['untouched_bytes_zero'] is False and not record['g2d_output'].get('ack_write_intent')
        cases.append(dict(case=case,result=record['g2d_result'],ack_sent=bool(acks),untouched_bytes_zero=result['untouched_bytes_zero']))
    summary=dict(passed=True,physical_access=False,physical_qualified=False,pixels_modeled=True,
        interpolation_checked=False,geometry=geometry,geometry_accepts=len(valid),refused=refusals,cases=cases,
        image_sha256=hashlib.sha256(image).hexdigest(),session_sha256=hashlib.sha256(session).hexdigest(),
        source_sha256=hashlib.sha256(source).hexdigest(),native_response_sha256=hashlib.sha256(response).hexdigest(),
        native_receipt_sha256=hashlib.sha256(receipt).hexdigest(),
        sources={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in
            (Path(__file__),Path(host.__file__),Path(prep.__file__),Path(producer.__file__))})
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
if __name__=='__main__':main()
