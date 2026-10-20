#!/usr/bin/env python3
"""Validate one copied encoder result; arithmetic qualification is independent."""
import hashlib,struct
HEADER=144
MAXIMUM=1048576

def decode(raw,session):
    if len(raw)<144 or len(session)!=856:raise ValueError('Truncated encoder result/session.')
    magic,version,status,n,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x4547504a,1) or status not in (0,1,2,3):raise ValueError('Encoder wire header differs.')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[32:64]!=session[768:800]:raise ValueError('Encoder result identity differs.')
    if struct.unpack_from('<I',raw,64)!=(1,) or any(raw[68:96]) or struct.unpack_from('<4I',raw,128)!=(640,480,640,75):raise ValueError('Encoder format differs.')
    if status!=1:
        if n or len(raw)!=144:raise ValueError('Non-success encoder result includes output.')
        return dict(status=status,bytes=0,arithmetic_qualified=False)
    if not 626<=n<=1048576 or len(raw)!=144+n:raise ValueError('Copied JPEG length differs.')
    jpeg=raw[144:]
    if jpeg[:2]!=b'\xff\xd8' or jpeg[-2:]!=b'\xff\xd9' or hashlib.sha256(jpeg).digest()!=raw[96:128]:raise ValueError('Copied JPEG content/digest differs.')
    return dict(status=1,bytes=n,sha256=hashlib.sha256(jpeg).hexdigest(),width=640,height=480,format='NV12',quality=75,arithmetic_qualified=False)

def ack(raw,session):
    result=decode(raw,session)
    if result['status']!=1:raise ValueError('Cannot acknowledge absent copied output.')
    return b'\x01'+session[32:40]+raw[96:128]
