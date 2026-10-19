#!/usr/bin/env python3
"""Decode one authenticated copied UART2 passive-loopback result."""
import hashlib,importlib.util,struct
from pathlib import Path
spec=importlib.util.spec_from_file_location('uart_observe_prepare',Path(__file__).with_name('prepare.py'))
preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
def decode(frame,session):
    preparation.session_check(session)
    if len(frame)<128:raise ValueError('Truncated UART frame')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x31545255,1) or status not in (0,1,2):raise ValueError('Wrong UART frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest():raise ValueError('UART session differs')
    if frame[96:128]!=bytes(32):raise ValueError('Reserved UART frame bytes differ')
    if status!=1:
        if count or len(frame)!=128 or frame[64:96]!=bytes(32):raise ValueError('Non-success carries UART output')
        return dict(status=status,bytes=0)
    payload=frame[128:]
    if count!=128 or len(payload)!=128 or frame[64:96]!=hashlib.sha256(payload).digest():raise ValueError('UART output hash/extent differs')
    observed,baud,tx,rx,errors,port,closed=struct.unpack_from('<Q6I',payload)
    if not observed or baud!=115384 or (tx,rx)!=struct.unpack_from('<2I',session,792) or (errors,port,closed)!=(0,2,3):raise ValueError('UART completion differs')
    if payload[32:64]!=session[800:832] or payload[64:96]!=payload[32:64] or payload[96:128]!=bytes(32):raise ValueError('UART echo or byte status differs')
    return dict(status=1,bytes=128,copied_verified=True,observed_us=observed,actual_baud=baud,request_hex=payload[32:32+tx].hex(),response_hex=payload[64:64+rx].hex(),resources_released=True)
