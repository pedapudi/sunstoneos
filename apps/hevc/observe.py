#!/usr/bin/env python3
"""Validate copied NV12 frames; decoding and physical output qualification are separate."""
import hashlib
import struct

HEADER=160
MAXIMUM=460800


def decode(raw,session,ordinal):
    if len(raw)<HEADER or len(session)!=856 or ordinal not in (0,1):
        raise ValueError('HEVC frame/session extent differs.')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x44363235,1) or status not in (0,1,2):
        raise ValueError('HEVC response header differs.')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[32:64]!=session[768:800]:
        raise ValueError('HEVC response session/input identity differs.')
    picture,pictures,poc,width,height,ystride,uvstride,uvoffset,format_,slice_,terminal=struct.unpack_from('<11I',raw,96)
    if (picture,pictures,width,height,ystride,uvstride,uvoffset,format_,slice_)!=(ordinal,2,640,480,640,640,307200,6,1 if ordinal else 2):
        raise ValueError('HEVC picture format or ordinal differs.')
    if (poc!=0 if ordinal==0 else not (0<poc<=32767 if status==1 else 0<=poc<=32767)) or terminal not in (0,1) or any(raw[140:160]):
        raise ValueError('HEVC picture order or reserved fields differ.')
    if status==0 and terminal or status==2 and not terminal:
        raise ValueError('HEVC terminal state differs.')
    result=dict(status=status,ordinal=ordinal,poc=poc,bytes=0,terminal=bool(terminal),numerical_qualified=False)
    if status!=1:
        if count or len(raw)!=HEADER or any(raw[64:96]):
            raise ValueError('HEVC non-success response contains copied pixels.')
        return result
    if count!=MAXIMUM or len(raw)!=HEADER+count:
        raise ValueError('HEVC copied NV12 extent differs.')
    identity=hashlib.sha256(raw[HEADER:]).digest()
    if identity!=raw[64:96]:
        raise ValueError('HEVC copied NV12 digest differs.')
    result.update(bytes=count,sha256=identity.hex(),width=640,height=480,format='NV12')
    return result


def query(ordinal):
    if ordinal not in (0,1):raise ValueError('Invalid HEVC picture ordinal.')
    return bytes((0,ordinal))


def ack(raw,session,ordinal):
    result=decode(raw,session,ordinal)
    if result['status']!=1 or result['terminal']:
        raise ValueError('Cannot acknowledge absent or terminal HEVC output.')
    return b'\1'+struct.pack('<I',ordinal)+session[32:40]+raw[64:96]
