#!/usr/bin/env python3
"""Decode a captured immutable JPEG result; never opens a device."""
import hashlib
import struct

def decode(raw,session,reference):
    if len(session)!=856 or len(reference)!=2304:raise ValueError('Wrong fixed JPEG input geometry')
    if len(raw)<128:raise ValueError('Truncated JPEG result')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x4745504a,1) or status not in (0,1,2):raise ValueError('Unsupported JPEG frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24):raise ValueError('JPEG session differs')
    if raw[32:96]!=session[768:832] or hashlib.sha256(reference).digest()!=session[800:832]:raise ValueError('JPEG fixture/reference differs')
    if status==1:
        if count!=2304 or len(raw)!=2432 or raw[128:]!=reference or hashlib.sha256(raw[128:]).digest()!=raw[96:128]:raise ValueError('Copied JPEG output differs')
    elif count or len(raw)!=128 or raw[96:128]!=bytes(32):raise ValueError('Non-success carries JPEG bytes')
    return {'status':status,'bytes':count,'epoch':epoch,'generation':generation,'copied_verified':status==1}
