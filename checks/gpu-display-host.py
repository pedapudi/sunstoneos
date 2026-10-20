#!/usr/bin/env python3
"""Pack real caller inputs and verify the bounded GPU/display application ABI."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    host=module('display_host',ROOT/'apps/gpu-display/host.py')
    texture=module('display_texture',ROOT/'apps/gpu-render/texture.py')
    vertices=[(-1,-1,0,1,0,0),(1,-1,0,1,1,0),(1,1,0,1,1,1),(-1,1,0,1,0,1)]
    inputs=[]
    for frame in range(2):
        pixels=bytes(v for y in range(180) for x in range(320) for v in
                     ((x+frame*67)&255,(y*2+frame*31)&255,(x+y+frame*113)&255,255))
        inputs.append(texture.input_bytes(vertices,[0,1,2,0,2,3],320,180,pixels))
    raw=host.inputs(inputs,limit=host.ENVELOPE_LIMIT-4096)
    assert struct.unpack_from('<Q2I',raw)==(host.MAGIC,1,len(raw))
    at=144
    for i,(shape,data) in enumerate(inputs):
        record=struct.unpack_from('<8I32s',raw,16+64*i)
        assert record==(*shape,at,len(data),0,hashlib.sha256(data).digest())
        assert raw[at:at+len(data)]==data;at+=len(data)
    assert at==len(raw) and inputs[0][1]!=inputs[1][1]
    assert host.inputs(inputs,limit=len(raw))==raw
    try:host.inputs(inputs,limit=len(raw)-1)
    except ValueError:pass
    else:raise AssertionError('Containing-envelope capacity overflow accepted')
    try:host.inputs([(inputs[0][0],inputs[0][1][:-1]),inputs[1]],limit=host.ENVELOPE_LIMIT)
    except ValueError:pass
    else:raise AssertionError('Truncated canonical input accepted')
    (a.output/'inputs.bin').write_bytes(raw)
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
        bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest(),frames=2,texture_shape=[320,180],
        vertices=4,indices=6,exact_limit=True,short_limit_refused=True,truncated_input_refused=True,
        sources={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in
                 ['apps/gpu-display/host.py','apps/gpu-render/texture.py','checks/gpu-display-host.py']},
        scope='Actual existing texture input producer and new app-only host packer. No device, launcher, admission, native rendering or display execution.'),indent=2)+'\n')
    print('PASS GPU display caller input ABI and containing-envelope bounds')
if __name__=='__main__':main()
