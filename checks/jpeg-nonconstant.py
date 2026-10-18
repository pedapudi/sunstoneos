#!/usr/bin/env python3
"""Verify the exact nonconstant JPEG oracle without decoding on hardware."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def oracle(out):
    out.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('jpeg_fixture',ROOT/'apps/jpeg/fixture.py')
    fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
    shutil.copyfile(ROOT/'apps/jpeg/fixture.py',out/'fixture-source.py')
    shutil.copyfile(Path(__file__),out/'check-source.py')
    jpeg=fixture.nonconstant();reference=fixture.nonconstant_reference()
    assert 155<len(jpeg)<=4096 and len(reference)==2304
    assert len(set(reference[:1536]))>32 and len(set(reference[1536:1920]))>16 and len(set(reference[1920:]))>16
    assert reference[1536:1920]!=reference[1920:]
    assert all(len(set(reference[base:base+width*height]))>1 for base,width,height in ((0,48,32),(1536,24,16),(1920,24,16)))
    (out/'fixture.jpg').write_bytes(jpeg);(out/'analytic.yuv').write_bytes(reference)
    decoder=Path('/usr/bin/ffmpeg');libraries={}
    ldd=subprocess.check_output(['ldd',str(decoder)],text=True)
    for line in ldd.splitlines():
        found=re.search(r'=> (/\S+)',line)
        if found:
            path=Path(found[1]).resolve();libraries[str(path)]=sha(path)
    metadata={'executable':str(decoder),'sha256':sha(decoder.resolve()),
        'version':subprocess.check_output([str(decoder),'-version'],text=True),
        'packages':subprocess.check_output(['dpkg-query','-W','-f=${Package} ${Version}\n','ffmpeg','libavcodec62','libavutil60'],text=True),
        'shared_libraries':libraries,'source':'FFmpeg 8.0.1, Ubuntu source package ffmpeg 8.0.1-3ubuntu2; CPU-only MJPEG decode, no device/runtime dependency.'}
    assert metadata['version'].startswith('ffmpeg version 8.0.1-3ubuntu2 ')
    (out/'decoder.json').write_text(json.dumps(metadata,indent=2)+'\n')
    command=[str(decoder),'-nostdin','-v','error','-threads','1','-bitexact','-cpuflags','0','-hwaccel','none','-idct','simple','-i',str(out/'fixture.jpg'),'-pix_fmt','yuvj420p','-f','rawvideo',str(out/'reference.yuv')]
    result=subprocess.run(command,capture_output=True,text=True);(out/'decode.log').write_text(result.stdout+result.stderr)
    (out/'commands.json').write_text(json.dumps([command],indent=2)+'\n')
    assert result.returncode==0,'CPU decoder refused generated JPEG; inspect decode.log'
    decoded=(out/'reference.yuv').read_bytes()
    differences=[i for i,(a,b) in enumerate(zip(reference,decoded)) if a!=b]
    report={'passed':len(decoded)==2304 and not differences,'fixture_bytes':len(jpeg),'fixture_sha256':sha(out/'fixture.jpg'),
        'reference_sha256':sha(out/'analytic.yuv'),'decoded_bytes':len(decoded),'different_samples':len(differences),
        'first_differences':differences[:16],'comparison':'Exact active planar Y/Cb/Cr samples; zero tolerance. DC/frequency 4 dequantized multiples of 8 have integer inverse values, with no clipping or color conversion.',
        'hardware_access':False}
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    assert all(sha(Path(path))==identity for path,identity in libraries.items()),'Decoder library changed during execution'
    assert sha(decoder.resolve())==metadata['sha256'],'Decoder executable changed during execution'
    assert report['passed'],'No exact numerical oracle established; stop before native changes'
    print(json.dumps(report,indent=2))
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--oracle-output',type=Path,required=True)
    a=parser.parse_args();oracle(a.oracle_output.resolve())
