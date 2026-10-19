#!/usr/bin/env python3
"""Read acquired program output from an already running selected GPU session."""
import argparse
import struct
from pathlib import Path

def decode(raw,chunk,epoch,total):
    if len(raw)<40:raise ValueError('Short GPU reply')
    magic,version,result,first,count,words,sequence,lo,hi,acquired=struct.unpack_from('<10I',raw)
    if (magic,version,first,lo|(hi<<32),words)!=(0x50555047,1,chunk,epoch,total):raise ValueError('GPU session differs')
    if result==0:
        if count or sequence or acquired or len(raw)!=40:raise ValueError('Malformed pending reply')
        return None
    expected=min(32,total-chunk*32)
    if result!=1 or count!=expected or sequence!=1 or acquired!=total or len(raw)!=40+4*count:
        raise ValueError('Unacquired or malformed GPU output')
    return raw[40:]

if __name__=='__main__':
    import time
    import usb.core
    p=argparse.ArgumentParser(description=__doc__)
    for x in ('bus','address','epoch','words'):p.add_argument('--'+x,type=int,required=True)
    p.add_argument('--timeout',type=float,default=20);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if not 0<a.words<=992 or a.timeout<=0:raise ValueError('Invalid bounds')
    dev=usb.core.find(idVendor=0x1f3a,idProduct=0xefe8,bus=a.bus,address=a.address)
    if dev is None:raise ValueError('Selected GPU device absent')
    deadline=time.monotonic()+a.timeout;output=bytearray()
    for chunk in range((a.words+31)//32):
        while True:
            remaining=deadline-time.monotonic()
            if remaining<=0:raise TimeoutError('GPU copied output deadline')
            timeout=max(1,int(remaining*1000))
            dev.write(0x02,struct.pack('<I',chunk),timeout=timeout)
            raw=bytes(dev.read(0x81,168,timeout=timeout))
            data=decode(raw,chunk,a.epoch,a.words)
            if data is not None:output+=data;break
            time.sleep(.01)
    with a.output.open('xb') as f:f.write(output)
