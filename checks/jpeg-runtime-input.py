#!/usr/bin/env python3
"""Produce real uploads and run focused bounded JPEG ingestion checks."""
import argparse,hashlib,importlib.util,json,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def constant(width,height):
    original=(Path.home()/'.cache/sunstoneos/evidence/jpeg-application-20261017/batch1/fixture.jpg').read_bytes()
    pos=2;parts=[]
    while pos<len(original):
        marker=original[pos+1];length=int.from_bytes(original[pos+2:pos+4],'big')
        body=bytearray(original[pos:pos+length+2]);pos+=length+2
        if marker==0xc0:body[5:9]=struct.pack('>HH',height,width)
        parts.append(bytes(body))
        if marker==0xda:break
    # Six zero-coefficient blocks per 16x16 MCU, each DC0/EOB uses two bits.
    bits=((width+15)//16)*((height+15)//16)*12
    entropy=bytes(bits//8)+(bytes([(1<<(8-bits%8))-1]) if bits%8 else b'')
    return b'\xff\xd8'+b''.join(parts)+entropy+b'\xff\xd9'
def produce(image,out):
    out.mkdir(parents=True,exist_ok=False);prep=module('jpeg_runtime_prepare',ROOT/'apps/jpeg/prepare.py')
    baseline=Path.home()/'.cache/sunstoneos/evidence/jpeg-sealing-20261018/session.bin'
    raw=bytearray(baseline.read_bytes());assert len(raw)==856
    _,_,identity=prep.base.image_parts(image);raw[48:80]=identity;struct.pack_into('<Q',raw,600,2)
    cases=[]
    nonconstant=Path.home()/'.cache/sunstoneos/evidence/jpeg-nonconstant-20261017/oracle'
    for name,w,h,oracle in [('partial-mcu',33,17,False),('partial-mcu-oracle',63,63,True),('maximum-width',128,32,False),('registered-optional-oracle',48,32,True),
                              ('vga',640,480,False),('vga-oracle',640,480,True),('wide-maximum-plane',1920,160,False)]:
        case=out/name;case.mkdir();jpeg=constant(w,h);ref=bytes([128])*(w*h+2*((w+1)//2)*((h+1)//2)) if oracle else b''
        if name.startswith('registered'):jpeg=(nonconstant/'fixture.jpg').read_bytes();ref=(nonconstant/'reference.yuv').read_bytes()
        session=bytearray(raw);session[768:800]=hashlib.sha256(jpeg).digest();session[800:832]=hashlib.sha256(ref).digest() if ref else bytes(32)
        prep.session_check(session);prep.embedded_reference(image.read_bytes(),session)
        for filename,data in [('session.bin',session),('input.jpg',jpeg),('reference.yu12',ref),('upload.bin',prep.upload_bytes(jpeg,ref,session))]:(case/filename).write_bytes(data)
        cases.append(dict(name=name,width=w,height=h,path=str(case),oracle=oracle))
    (out/'cases.json').write_text(json.dumps(dict(physical_access=False,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),baseline=str(baseline),baseline_sha256=hashlib.sha256(baseline.read_bytes()).hexdigest(),cases=cases),indent=2)+'\n')
    return cases
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--executable',type=Path,required=True);a=p.parse_args()
    cases=produce(a.image,a.output);commands=[]
    for case in cases:
        path=Path(case['path']);command=list(map(str,[a.executable,path/'session.bin',path/'upload.bin',case['width'],case['height']]))
        r=subprocess.run(command,capture_output=True,text=True);(path/'input-check.log').write_text(r.stdout+r.stderr);assert r.returncode==0,(case['name'],r.stdout,r.stderr);commands.append(command)
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=cases,commands=commands,scope='Actual Python upload producer, actual parser/input/digest under address and undefined-behavior sanitizers. Fragment sizes1/17/512, oracle/normal mode, partial-MCU dimensions, 640x480 and 1920x160 maximum planes with and without an oracle, refused larger planes, mutated hashes/identity/authority, backward time, expiry during final validation and immutable publication.'),indent=2)+'\n')
