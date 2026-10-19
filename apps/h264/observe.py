#!/usr/bin/env python3
"""Decode a captured immutable H.264 result; never opens a device."""
import hashlib
import struct

def decode(raw,session,reference=None):
    if len(session)!=856:raise ValueError('Wrong H.264 session geometry')
    oracle=any(session[800:832])
    if oracle!=(reference is not None):raise ValueError('Qualification oracle mode differs')
    if oracle and (len(reference)!=2304 or hashlib.sha256(reference).digest()!=session[800:832]):raise ValueError('Qualification oracle differs')
    if len(raw)<128:raise ValueError('Truncated H.264 result')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x34363248,1) or status not in (0,1,2):raise ValueError('Unsupported H.264 frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24):raise ValueError('H.264 session differs')
    if raw[32:96]!=session[768:832]:raise ValueError('H.264 fixture/reference differs')
    if status==1:
        if count!=2304 or len(raw)!=2432 or (oracle and raw[128:]!=reference) or hashlib.sha256(raw[128:]).digest()!=raw[96:128]:raise ValueError('Copied H.264 output differs')
    elif count or len(raw)!=128 or raw[96:128]!=bytes(32):raise ValueError('Non-success carries H.264 bytes')
    return {'status':status,'bytes':count,'epoch':epoch,'generation':generation,'copied_verified':status==1,'oracle_verified':status==1 and oracle}
