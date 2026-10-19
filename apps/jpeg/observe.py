#!/usr/bin/env python3
"""Decode copied runtime JPEG pixels and optional independent oracle results."""
import hashlib
import struct
HEADER=144
MAXIMUM=6144
def geometry(width,height,stride,rows):
    if not 1<=width<=128 or not 1<=height<=128 or stride!=((width+31)&~31) or rows!=((height+31)&~31):
        raise ValueError('Invalid parsed JPEG geometry')
    count=width*height+2*((width+1)//2)*((height+1)//2)
    if stride*rows>4096 or stride*rows//2>4096 or count>MAXIMUM:raise ValueError('JPEG planes exceed admitted capacity')
    return count

def decode(raw,session,reference=None):
    if len(session)!=856 or struct.unpack_from('<3Q',session,592)!=(0x415337334a504132,2,856):raise ValueError('Wrong runtime JPEG session')
    if len(raw)<HEADER:raise ValueError('Truncated JPEG result')
    reference=reference or None
    if (hashlib.sha256(reference).digest() if reference else bytes(32))!=session[800:832]:raise ValueError('JPEG oracle mode/digest differs')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x4745504a,2) or status not in (0,1,2):raise ValueError('Unsupported JPEG result')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[32:96]!=session[768:832]:raise ValueError('JPEG session/input differs')
    width,height,stride,rows=struct.unpack_from('<4I',raw,128)
    if status==1:
        if count!=geometry(width,height,stride,rows) or len(raw)!=HEADER+count or hashlib.sha256(raw[HEADER:]).digest()!=raw[96:128]:raise ValueError('Incomplete copied JPEG output')
        if reference is not None and (len(reference)!=count or raw[HEADER:]!=reference):raise ValueError('JPEG output differs from oracle')
    elif count or len(raw)!=HEADER or raw[96:128]!=bytes(32):raise ValueError('Non-success carries JPEG pixels')
    elif any((width,height,stride,rows)):geometry(width,height,stride,rows)
    return dict(status=status,bytes=count,epoch=epoch,generation=generation,width=width,height=height,
      stride=stride,rows=rows,copied_integrity_verified=status==1,oracle_verified=status==1 and reference is not None)
