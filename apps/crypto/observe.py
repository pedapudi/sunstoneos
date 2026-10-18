#!/usr/bin/env python3
"""Decode one copied crypto diagnostic frame without opening a device."""
import hashlib
import importlib.util
from pathlib import Path
import struct
_spec=importlib.util.spec_from_file_location('crypto_output_vectors',Path(__file__).with_name('vectors.py'))
vectors=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(vectors)
def decode(raw,session,reference=vectors.REFERENCE):
    if len(session)!=824 or reference!=vectors.REFERENCE:raise ValueError('Wrong crypto geometry/reference')
    if struct.unpack_from('<3Q',session,592)!=(0x4153373343524131,1,824):raise ValueError('Wrong crypto session')
    if session[760:792]!=hashlib.sha256(vectors.INPUTS).digest() or session[792:824]!=hashlib.sha256(reference).digest():raise ValueError('Wrong crypto vectors')
    if len(raw)<128:raise ValueError('Truncated crypto frame')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x54505943,1) or status not in (0,1,2):raise ValueError('Wrong crypto frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[32:96]!=session[760:824]:raise ValueError('Crypto session differs')
    if status==1:
        if count!=160 or len(raw)!=288 or raw[128:]!=reference or raw[96:128]!=hashlib.sha256(raw[128:]).digest():raise ValueError('Crypto output differs')
    elif count or len(raw)!=128 or raw[96:128]!=bytes(32):raise ValueError('Non-success carries crypto output')
    return dict(status=status,bytes=count,epoch=epoch,generation=generation,copied_verified=status==1)
