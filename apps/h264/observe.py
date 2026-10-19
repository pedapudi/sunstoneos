#!/usr/bin/env python3
"""Decode one indexed immutable H.264 output; never opens a device."""
import hashlib
import struct

def decode(raw,session,reference=None,picture=0):
    if len(session)!=856:raise ValueError('Wrong H.264 session geometry')
    oracle=any(session[800:832])
    if oracle!=(reference is not None):raise ValueError('Qualification oracle mode differs')
    if oracle and (len(reference) not in (2304,4608) or hashlib.sha256(reference).digest()!=session[800:832]):raise ValueError('Qualification oracle differs')
    if len(raw)<160:raise ValueError('Truncated H.264 result')
    magic,version,status,samples,epoch,generation=struct.unpack_from('<4I2Q',raw)
    index,count,frame_num,poc,width,height,slice_type,terminal=struct.unpack_from('<8I',raw,128)
    if (magic,version)!=(0x34363248,2) or status not in (0,1,2) or terminal not in (0,1,2):raise ValueError('Unsupported H.264 frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[32:96]!=session[768:832]:raise ValueError('H.264 session differs')
    if index!=picture or index>=2 or count>2 or (count and (index>=count or (width,height)!=(48,32))):raise ValueError('Picture selection differs')
    if count and (slice_type!=(2 if index==0 else 0) or frame_num!=index or (count==2 and poc!=2*index)):raise ValueError('Picture metadata differs')
    if not count and (status or frame_num or poc or width or height or slice_type or terminal):raise ValueError('Unpublished input carries metadata')
    if oracle and count and len(reference)!=count*2304:raise ValueError('Oracle picture count differs')
    if status==1:
        if not count or samples!=2304 or len(raw)!=2464 or hashlib.sha256(raw[160:]).digest()!=raw[96:128]:raise ValueError('Copied H.264 output differs')
        if oracle and raw[160:]!=reference[index*2304:(index+1)*2304]:raise ValueError('Pixel oracle differs')
    elif samples or len(raw)!=160 or raw[96:128]!=bytes(32):raise ValueError('Non-success carries H.264 bytes')
    return dict(status=status,bytes=samples,epoch=epoch,generation=generation,picture_index=index,picture_count=count,
                frame_num=frame_num,poc=poc,width=width,height=height,slice_type=slice_type,terminal=terminal,
                copied_verified=status==1,oracle_verified=status==1 and oracle)
