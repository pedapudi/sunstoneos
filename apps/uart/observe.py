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
    dma=preparation.dma_mode(session)
    if (magic,version)!=(0x31545255,3 if dma else 2) or status not in (0,1,2):raise ValueError('Wrong UART frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest():raise ValueError('UART session differs')
    if frame[96:128]!=bytes(32):raise ValueError('Reserved UART frame bytes differ')
    if status!=1:
        if count or len(frame)!=128 or frame[64:96]!=bytes(32):raise ValueError('Non-success carries UART output')
        return dict(status=status,bytes=0)
    payload=frame[128:]
    if dma:
        n=struct.unpack_from('<I',session,792)[0]
        if count!=96+2*n or len(payload)!=count or frame[64:96]!=hashlib.sha256(payload).digest():raise ValueError('DMA UART output hash/extent differs')
        observed,baud,port,tx,rx,errors,closed=struct.unpack_from('<Q6I',payload)
        rt,tt=struct.unpack_from('<2Q',payload,32)
        ro,to,released,disabled,drained,rp,tp,kind=struct.unpack_from('<8I',payload,48)
        if not observed or (baud,port,tx,rx,errors,closed)!=(115384,2,n,n,0,3):raise ValueError('DMA UART completion differs')
        if not rt or tt<=rt or (ro,to,released,disabled,drained,kind)!=(1,1,3,1,1,1) or not rp or not tp:raise ValueError('DMA UART ownership/drain differs')
        if payload[80:96]!=bytes(16) or payload[96:96+n]!=session[800:800+n] or payload[96+n:]!=payload[96:96+n]:raise ValueError('DMA UART copied echo differs')
        return dict(status=1,bytes=count,mode='dma',copied_verified=True,observed_us=observed,actual_baud=baud,
            request_hex=payload[96:96+n].hex(),response_hex=payload[96+n:].hex(),rx_job_ticket=rt,tx_job_ticket=tt,
            aggregate_lsr_errors=errors,uart_resources_released=True,dma_channels_closed=True,dma_parent_retained=True)
    if count!=144 or len(payload)!=144 or frame[64:96]!=hashlib.sha256(payload).digest():raise ValueError('UART output hash/extent differs')
    observed,baud,tx,rx,errors,port,closed=struct.unpack_from('<Q6I',payload)
    if not observed or baud!=115384 or (tx,rx)!=struct.unpack_from('<2I',session,792) or (errors,port,closed)!=(0,2,3):raise ValueError('UART completion differs')
    if payload[32:64]!=session[800:832] or payload[64:96]!=payload[32:64] or payload[96:128]!=bytes(32):raise ValueError('UART echo or byte status differs')
    notifications,tx_irqs,rx_irqs,other_irqs=struct.unpack_from('<4I',payload,128)
    if not 1<=notifications<=128 or not tx_irqs or not rx_irqs or notifications!=tx_irqs+rx_irqs+other_irqs:raise ValueError('UART IRQ evidence differs')
    return dict(status=1,bytes=144,notifications=notifications,tx_irqs=tx_irqs,rx_irqs=rx_irqs,copied_verified=True,observed_us=observed,actual_baud=baud,request_hex=payload[32:32+tx].hex(),response_hex=payload[64:64+rx].hex(),resources_released=True)
