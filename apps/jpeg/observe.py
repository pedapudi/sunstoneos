#!/usr/bin/env python3
"""Decode copied runtime JPEG pixels and optional independent oracle results."""
import hashlib
import struct
HEADER=144
# Native capacities from apps/jpeg/input.h; prepare.py checks them in the image.
INPUT_CAPACITY=524288
PLANE_CAPACITY=307200
MAXIMUM=460800
def geometry(width,height,stride,rows):
    if not 1<=width<=1920 or not 1<=height<=1080 or stride!=((width+31)&~31) or rows!=((height+31)&~31):
        raise ValueError('Invalid parsed JPEG geometry')
    count=width*height+2*((width+1)//2)*((height+1)//2)
    if stride*rows>PLANE_CAPACITY or count>MAXIMUM:raise ValueError('JPEG planes exceed admitted capacity')
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

SEQUENCE_HEADER=160
def decode_sequence(raw,session,manifest,index,reference=None):
    """Decode the version 3 result of image index in a sequence session."""
    if len(session)!=856 or struct.unpack_from('<3Q',session,592)!=(0x415337334a504132,3,856):raise ValueError('Wrong JPEG sequence session')
    if hashlib.sha256(manifest).digest()!=session[768:800] or any(session[800:832]):raise ValueError('JPEG sequence manifest differs')
    count=struct.unpack_from('<I',manifest,8)[0]
    if not 0<=index<count:raise ValueError('JPEG sequence index exceeds manifest')
    jpeg_bytes,oracle_bytes,width,height=struct.unpack_from('<4I',manifest,16+80*index);entry=manifest[16+80*index:96+80*index]
    reference=reference or None
    if (hashlib.sha256(reference).digest() if reference else bytes(32))!=entry[48:80]:raise ValueError('JPEG oracle mode/digest differs')
    if len(raw)<SEQUENCE_HEADER:raise ValueError('Truncated JPEG result')
    magic,version,status,length,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x4745504a,3) or status not in (0,1,2):raise ValueError('Unsupported JPEG sequence result')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[32:64]!=entry[16:48] or raw[64:96]!=entry[48:80]:raise ValueError('JPEG session/input differs')
    if struct.unpack_from('<4I',raw,144)!=(index,count,index+1,0):raise ValueError('JPEG sequence position differs')
    if struct.unpack_from('<4I',raw,128)!=(width,height,(width+31)&~31,(height+31)&~31):raise ValueError('JPEG sequence geometry differs')
    expected=geometry(width,height,(width+31)&~31,(height+31)&~31)
    if status==1:
        if length!=expected or len(raw)!=SEQUENCE_HEADER+length or hashlib.sha256(raw[SEQUENCE_HEADER:]).digest()!=raw[96:128]:raise ValueError('Incomplete copied JPEG output')
        if reference is not None and raw[SEQUENCE_HEADER:]!=reference:raise ValueError('JPEG output differs from oracle')
    elif length or len(raw)!=SEQUENCE_HEADER or raw[96:128]!=bytes(32):raise ValueError('Non-success carries JPEG pixels')
    return dict(status=status,index=index,count=count,bytes=length,width=width,height=height,
      copied_integrity_verified=status==1,oracle_verified=status==1 and reference is not None)
