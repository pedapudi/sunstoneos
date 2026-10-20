#!/usr/bin/env python3
"""Pack two caller-selected texture inputs for the root-owned display session.

Shader/profile admission and GPU/HDMI physical evidence belong to the containing
root envelope. This module supplies no authority and never opens a device.
"""
import hashlib
import struct
MAGIC=0x4153373347444931
HEADER_BYTES=144
ENVELOPE_LIMIT=1048576


def inputs(frames, *, limit):
    """frames contains two existing texture.input_bytes() (shape, bytes) results.

    limit is the space remaining after the root envelope's authority fields.
    Both payloads follow the fixed144-byte application header without gaps.
    """
    if len(frames)!=2 or not HEADER_BYTES<=limit<=ENVELOPE_LIMIT:
        raise ValueError('Two frames and the containing envelope capacity are required')
    rows=[];payload=bytearray();next_offset=HEADER_BYTES
    for shape,data in frames:
        if len(shape)!=5:
            raise ValueError('Texture input shape differs')
        width,height,stride,vertices,indices=shape
        if not 1<=width<=1920 or not 1<=height<=1080 or stride!=((width+1)&~1):
            raise ValueError('Texture geometry differs')
        if not 1<=vertices<=65536 or not 3<=indices<=65535 or indices%3:
            raise ValueError('Indexed geometry differs')
        data=bytes(data)
        if len(data)!=vertices*24+indices*2+stride*height*4 or len(data)>limit-next_offset:
            raise ValueError('Canonical payload exceeds combined input capacity')
        rows.append(struct.pack('<8I32s',*shape,next_offset,len(data),0,hashlib.sha256(data).digest()))
        payload.extend(data);next_offset+=len(data)
    return struct.pack('<Q2I',MAGIC,1,next_offset)+b''.join(rows)+payload
