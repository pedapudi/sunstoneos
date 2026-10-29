#!/usr/bin/env python3
"""Decode one authenticated copied SPI3 write-read exchange."""
import hashlib,importlib.util,struct
from pathlib import Path
spec=importlib.util.spec_from_file_location('spi_observe_prepare',Path(__file__).with_name('prepare.py'))
preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
def decode(frame,session):
    preparation.session_check(session)
    if len(frame)<128:raise ValueError('Truncated SPI frame')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x31495053,2 if preparation.dma_mode(session) else 1) or status not in (0,1,2):raise ValueError('Wrong SPI frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest():raise ValueError('SPI session differs')
    if frame[96:128]!=bytes(32):raise ValueError('Reserved SPI frame bytes differ')
    if status!=1:
        if count or len(frame)!=128 or frame[64:96]!=bytes(32):raise ValueError('Non-success carries SPI output')
        return dict(status=status,bytes=0)
    dma=preparation.dma_mode(session)
    payload=frame[128:]
    if count!=(4192 if dma else 96) or len(payload)!=count or frame[64:96]!=hashlib.sha256(payload).digest():raise ValueError('SPI output hash/extent differs')
    observed,hz,tx,rx,kind,port,notifications=struct.unpack_from('<Q6I',payload)
    if not observed or hz!=500000 or (tx,rx)!=struct.unpack_from('<2I',session,792) or (kind,port)!=(3,3):raise ValueError('SPI exchange selection differs')
    irq=struct.unpack_from('<Q',session,600)[0]>=2
    if (irq and not 1<=notifications<=32) or (not irq and notifications):raise ValueError('SPI interrupt observation differs')
    if payload[32:64]!=session[800:832]:raise ValueError('SPI request differs')
    offset=96 if dma else 64
    if dma:
        if struct.unpack_from('<8I',payload,64)!=(3,26,0,3,3,3,4096,tx):raise ValueError('SPI DMA completion/drain facts differ')
    elif payload[64+rx:96]!=bytes(32-rx):raise ValueError('SPI response padding differs')
    return dict(status=1,bytes=count,copied_verified=True,interrupt_notifications=notifications,observed_us=observed,actual_hz=hz,request_hex=payload[32:32+tx].hex(),response_hex=payload[offset:offset+rx].hex())
