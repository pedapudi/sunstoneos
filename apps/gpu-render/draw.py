#!/usr/bin/env python3
"""Submit copied triangles and read a framebuffer from an owned GPU session."""
import math
import struct
import time

READ,SUBMIT,CLOSE=1,2,3
def vertices(rows):
    if not 3<=len(rows)<=126 or len(rows)%3:
        raise ValueError('A triangle list requires 3..126 vertices in groups of three')
    result=bytearray()
    for row in rows:
        if len(row)!=8 or not all(math.isfinite(x) for x in row):
            raise ValueError('Each vertex requires eight finite binary32 values')
        if not all(-1<=x<=1 for x in row[:2]) or row[2]!=0 or row[3]!=1 or not all(0<=x<=1 for x in row[4:]):
            raise ValueError('Vertex outside the bounded position/color profile')
        result.extend(struct.pack('<8f',*row))
    return bytes(result)
def frame(operation,sequence,epoch,first=0,data=b''):
    if not 0<epoch<2**64 or sequence not in (0,1) or not 0<=first<4096 or len(data)>4032:
        raise ValueError('GPU draw request bounds')
    return struct.pack('<4IQ2I',0x49525047,3,operation,sequence,epoch,first,len(data))+data
def decode(raw,epoch,first):
    if len(raw)<40:raise ValueError('Short GPU draw reply')
    magic,version,state,index,count,total,sequence,lo,hi,checked=struct.unpack('<10I',raw[:40])
    if (magic,version,index,total,lo|hi<<32)!=(0x50555047,3,first//64,4096,epoch):
        raise ValueError('GPU draw reply identity differs')
    if state not in (0,1,2,3,4) or sequence not in (0,1) or count not in (0,64) or len(raw)!=40+count*4:
        raise ValueError('GPU draw reply extent differs')
    if (count,checked)!=((64,4096) if state==1 else (0,0)):
        raise ValueError('GPU draw output was not acquired')
    return state,sequence,raw[40:]
def sequence(exchange,epoch,data,timeout=20):
    """exchange(request,remaining_seconds) must complete one bounded transaction."""
    if not 0<timeout<=30 or len(data)%32 or not 96<=len(data)<=4032 or (len(data)//32)%3:
        raise ValueError('GPU draw input/deadline bounds')
    deadline=time.monotonic()+timeout
    def transaction(op,seq,first=0,payload=b''):
        remaining=deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('GPU draw deadline')
        raw=exchange(frame(op,seq,epoch,first,payload),remaining)
        if time.monotonic()>deadline:raise TimeoutError('GPU draw deadline after exchange')
        return decode(raw,epoch,first)
    state,actual,_=transaction(READ,0)
    if state!=0 or actual!=0:raise ValueError('GPU draw session is not unused')
    state,actual,_=transaction(SUBMIT,0,payload=data)
    if state!=0 or actual!=0:raise ValueError('GPU draw was not queued')
    output=bytearray()
    for first in range(0,4096,64):
        while True:
            state,actual,page=transaction(READ,1,first)
            if state==1 and actual==1:output.extend(page);break
            if state!=0:raise ValueError('GPU draw failed or was refused')
            time.sleep(.001)
    state,actual,_=transaction(CLOSE,1)
    if state!=0 or actual!=1:raise ValueError('GPU draw CLOSE was not queued')
    while True:
        state,actual,_=transaction(READ,1)
        if state==3 and actual==1:break
        if state!=0:raise ValueError('GPU draw shutdown incomplete')
        time.sleep(.001)
    return bytes(output)
def capture(device,epoch,data,timeout=20):
    """Use the coordinator's identity-validated, locked, already running handle.

    This function does not discover, open, launch or reset a device.
    """
    deadline=time.monotonic()+timeout
    def exchange(request,remaining):
        remaining=min(remaining,deadline-time.monotonic())
        if remaining<=0:raise TimeoutError('GPU draw transport deadline')
        if device.write(0x02,request,timeout=max(1,int(remaining*1000)))!=len(request):
            raise ValueError('Short GPU draw write')
        remaining=deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('GPU draw deadline after write')
        raw=bytes(device.read(0x81,296,timeout=max(1,int(remaining*1000))))
        if time.monotonic()>deadline:raise TimeoutError('GPU draw deadline after read')
        return raw
    return sequence(exchange,epoch,data,timeout)
