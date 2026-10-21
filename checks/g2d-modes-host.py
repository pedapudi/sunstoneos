#!/usr/bin/env python3
"""Check host sealing, references and ACK policy for general scaling, image blending and conversion.

An accepted earlier session supplies the board prefix and identities. New
sessions are sealed by the actual producer and checked by actual preparation.
Copied responses are synthetic host-side frames: no native task, USB transfer or
device pixel is involved, and the ideal CPU references qualify no hardware rounding.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def frame(session,payload,status=1):
    """Assemble one native-format response; an ACK receipt keeps the digest but no payload."""
    epoch,generation=struct.unpack_from('<2Q',session,24)
    header=struct.pack('<4I2Q',0x44324753,1,status,len(payload) if status==1 else 0,epoch,generation)
    header+=session[768:800]+hashlib.sha256(session).digest()+hashlib.sha256(payload).digest()
    header+=session[856:904]+struct.pack('<4I',1,1,0x400,1)
    assert len(header)==192
    return header+(payload if status==1 else b'')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True,help='An accepted earlier G2D session; read only')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    host=module('g2d_modes_host',ROOT/'apps/g2d/host.py');prep=host.preparation
    producer=module('g2d_modes_producer',ROOT/'apps/g2d/producer.py')
    base=args.session.read_bytes();prep.session_check(base)
    assert not any(base[800:832]),'Earlier sessions keep every extension word zero.'
    owner,authority,client=struct.unpack_from('<3Q',base,616)
    facts={name:('model-'+name).encode() for name in ('initial','memory','electrical','supervision')}
    width,height,stride=60,64,256
    source=bytes((x*7+y*13+c*61)&255 if c<3 else (x*29+y*3)&255
        for y in range(height) for x in range(stride//4) for c in range(4))
    def seal(**geometry):
        return producer.extend(base[:592],source,owner=owner,authority=authority,client=client,**facts,
            width=width,height=height,stride=stride,**geometry)
    sessions=dict(
        scale_reduce=seal(operation=5,dx=3,dy=4,w=13,h=9,sx=2,sy=1,sw=55,sh=60),
        scale_enlarge=seal(operation=5,dx=1,dy=2,w=57,h=61,sx=40,sy=50,sw=7,sh=5,source_format=3,output_format=4),
        convert=seal(operation=2,dx=9,dy=17,w=31,h=23,sx=5,sy=7,source_format=2,output_format=1),
        convert_x=seal(operation=2,dx=0,dy=0,w=60,h=64,source_format=4,output_format=6),
        blend=seal(operation=6,dx=9,dy=17,w=31,h=23,sx=5,sy=7,bx=20,by=30,alpha=200),
        copy=seal(operation=2,dx=9,dy=17,w=31,h=23,sx=5,sy=7))
    for name,session in sessions.items():
        prep.session_check(session);assert prep.upload_bytes(source,session)[32:]==source,name
    # The producer refuses words an operation does not use.
    refusals=[]
    for name,geometry in (('scale-identity',dict(operation=5,dx=0,dy=0,w=9,h=9,sw=9,sh=9)),
            ('copy-background',dict(operation=2,dx=0,dy=0,w=9,h=9,bx=1)),
            ('blend-convert',dict(operation=6,dx=0,dy=0,w=9,h=9,alpha=9,source_format=1)),
            ('blend-zero-alpha',dict(operation=6,dx=0,dy=0,w=9,h=9)),
            ('fill-convert',dict(operation=1,dx=0,dy=0,w=9,h=9,output_format=2)),
            ('format-range',dict(operation=2,dx=0,dy=0,w=9,h=9,output_format=8))):
        try:seal(**geometry)
        except ValueError:refusals.append(name)
        else:raise AssertionError('Invalid G2D session sealed: '+name)
    # Reserved words are not reachable through the producer; corrupt a sealed copy.
    corrupt=bytearray(sessions['copy']);corrupt[824]=1
    try:prep.session_check(bytes(corrupt))
    except ValueError:refusals.append('reserved-word')
    else:raise AssertionError('Reserved G2D extension word accepted.')
    # Ideal references: identities, the earlier opaque formula and channel orders.
    assert host.over((0,1,2,3),(255,40,50,60))==(255,40,50,60)
    assert host.over((255,1,2,3),(0,40,50,60))==(255,1,2,3)
    assert host.over((0,1,2,3),(0,40,50,60))==(0,0,0,0)
    for a in range(256):
        for value,under in ((0,255),(17,90),(255,0),(128,128)):
            assert host.over((a,value,value,value),(255,under,under,under))[1]==(value*a+under*(255-a)+127)//255
    word=0x11223344
    assert host.unpack_pixel(word,0)==(0x11,0x22,0x33,0x44) and host.unpack_pixel(word,1)==(0x11,0x44,0x33,0x22)
    assert host.unpack_pixel(word,2)==(0x44,0x11,0x22,0x33) and host.unpack_pixel(word,3)==(0x44,0x33,0x22,0x11)
    assert host.unpack_pixel(word,4)==(255,0x22,0x33,0x44)
    for f in range(4):assert host.pack_pixel(host.unpack_pixel(word,f),f)==struct.pack('<I',word)
    expected={name:host.expected_output(source,session) for name,session in sessions.items()}
    assert expected['scale_reduce'] is None and expected['scale_enlarge'] is None
    assert [host.diagnostic(sessions[name]) for name in ('convert','convert_x','blend','copy','scale_reduce')]==[True,True,True,False,False]
    first=(17*stride+9*4);pixel=struct.unpack_from('<I',source,7*stride+5*4)[0]
    assert expected['convert'][first:first+4]==host.pack_pixel(host.unpack_pixel(pixel,2),1)
    assert expected['convert_x'][0]==255 # RGBX keeps X in the low byte.
    under=struct.unpack_from('<I',source,30*stride+20*4)[0]
    assert expected['blend'][first:first+4]==host.pack_pixel(host.over(host.unpack_pixel(pixel,0),host.unpack_pixel(under,0),200),0)
    # Capture policy: diagnostics record differences and still acknowledge;
    # an exact copy and the scaling bounded-write invariant stay strict.
    cases=[]
    for case,name,payload,acknowledged in (
            ('blend-differs-from-ideal','blend',bytes(len(source)),True),
            ('convert-matches-ideal','convert',expected['convert'],True),
            ('convert-differs-from-ideal','convert_x',bytes(len(source)),True),
            ('copy-differs','copy',bytes(len(source)),False),
            ('scale-confined','scale_reduce',None,True),
            ('scale-outside-rectangle','scale_reduce',None,False)):
        session=sessions[name];upload=prep.upload_bytes(source,session)
        if payload is None:
            out=bytearray(len(source));(_,_,_,_,_,dx,dy,w,h,_,_,_),_=prep.session_geometry(session)
            for y in range(h):out[(dy+y)*stride+dx*4:(dy+y)*stride+(dx+w)*4]=b'\x5a'*(w*4)
            if case=='scale-outside-rectangle':out[0]=1
            payload=bytes(out)
        response=frame(session,payload);receipt=frame(session,payload,3)
        folder=args.output/case;folder.mkdir();record={};queries=[];acks=[]
        def save():host.supervisor.save_json(folder/'session.json',record)
        def exchange(packet,timeout):
            queries.append(packet);assert len(queries)<=2 and (len(queries)==1 or acks)
            return response if len(queries)==1 else receipt
        result=host.capture(exchange,session,folder,record,save,100,upload=upload,send_upload=lambda data:None,
            send_ack=acks.append,now=lambda:1,pause=lambda delay:None)
        assert bool(acks)==acknowledged,case
        assert record['g2d_result']==('observed-success' if acknowledged else 'observed-failure'),case
        if host.diagnostic(session):
            comparison=record['g2d_output']['reference_comparison']
            assert comparison==('matches-ideal' if case.endswith('matches-ideal') else 'differs-from-ideal'),case
            assert record['g2d_output']['reference_qualified'] is False
        decoded=host.decode(response,session)
        cases.append(dict(case=case,operation=decoded['operation'],format=decoded['format'],
            output_format=decoded['output_format'],result=record['g2d_result'],ack_sent=bool(acks),
            arithmetic_scope=decoded.get('arithmetic_scope')))
    summary=dict(passed=True,physical_access=False,physical_qualified=False,pixels_modeled=True,
        base_session_sha256=hashlib.sha256(base).hexdigest(),refused=refusals,cases=cases,
        sessions={name:hashlib.sha256(raw).hexdigest() for name,raw in sessions.items()},
        sources={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in
            (Path(__file__),Path(host.__file__),Path(prep.__file__),Path(producer.__file__))})
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
if __name__=='__main__':main()
