#!/usr/bin/env python3
"""Capture one immutable NV16 frame through an explicitly selected USB0 device."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import usb.core

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--bus',type=int,required=True)
p.add_argument('--address',type=int,required=True)
p.add_argument('--epoch',type=int,required=True)
p.add_argument('--generation',type=int,required=True)
p.add_argument('--session-sha256',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--cancel',action='store_true')
a=p.parse_args()
if a.output.exists():p.error('output already exists')
identity=bytes.fromhex(a.session_sha256)
if len(identity)!=32:p.error('session digest must contain 32 bytes')
d=usb.core.find(idVendor=0x1f3a,idProduct=0xefea,bus=a.bus,address=a.address)
if d is None:raise SystemExit('selected device is absent')
# One bounded exchange per request. Any transport failure terminates the process.
# Callers must hold the project board lock and establish launch/recovery authority.
d.set_configuration()
def exchange(op,offset=0,count=0):
    d.write(0x02,struct.pack('<III',op,offset,count),timeout=2000)
    data=bytearray()
    while len(data)<72:
        data.extend(d.read(0x81,4168,timeout=2000))
    magic,version,state,actual,size,total,epoch,generation=struct.unpack('<6I2Q',data[:40])
    if (magic,version,total,epoch,generation)!=(0x524d4143,1,614400,a.epoch,a.generation):
        raise RuntimeError('capture identity mismatch')
    if data[40:72]!=identity or actual!=offset or size>count:
        raise RuntimeError('capture session or response mismatch')
    while len(data)<72+size:data.extend(d.read(0x81,4168,timeout=2000))
    if len(data)!=72+size:raise RuntimeError('response extent mismatch')
    return state,data[72:]
if a.cancel:
    state,_=exchange(2)
    if state!=3:raise RuntimeError('cancellation was not acknowledged')
    print('capture cancelled; all device reservations remain retained')
else:
    state,_=exchange(0)
    if state!=2:raise SystemExit(f'frame unavailable: state {state}; no automatic retry')
    frame=bytearray()
    for offset in range(0,614400,4096):
        count=min(4096,614400-offset)
        state,chunk=exchange(1,offset,count)
        if state!=2 or len(chunk)!=count:raise RuntimeError('frame read refused')
        frame.extend(chunk)
    with a.output.open('xb') as output:output.write(frame)
    a.output.with_suffix(a.output.suffix+'.json').write_text(json.dumps(dict(
        format='NV16',width=640,height=480,epoch=a.epoch,generation=a.generation,
        session_sha256=a.session_sha256,sha256=hashlib.sha256(frame).hexdigest()),indent=2)+'\n')
    print(f'captured {len(frame)} bytes')
