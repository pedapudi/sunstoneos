#!/usr/bin/env python3
"""Decode a session-bound copied endpoint-DMA COPY result."""
import hashlib,importlib.util,struct
from pathlib import Path
spec=importlib.util.spec_from_file_location('pcie_test_observe_prepare',Path(__file__).with_name('prepare.py'))
preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
def decode(frame,session):
    preparation.session_check(session)
    if len(frame)<128:raise ValueError('Truncated PCIe test frame')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x54504350,1) or status not in (0,1,2):raise ValueError('Wrong PCIe test frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest():raise ValueError('PCIe test session differs')
    if frame[96:128]!=bytes(32):raise ValueError('Reserved PCIe frame bytes differ')
    if status!=1:
        if count or len(frame)!=128 or frame[64:96]!=bytes(32):raise ValueError('Non-success carries PCIe output')
        return dict(status=status,bytes=0)
    payload=frame[128:]
    if count!=4224 or len(payload)!=4224 or frame[64:96]!=hashlib.sha256(payload).digest():raise ValueError('Copied DMA output hash/extent differs')
    observed,command,flags=struct.unpack_from('<Q2I',payload)
    vendor,device,pci_command,link=struct.unpack_from('<4H',payload,16)
    bar,events=struct.unpack_from('<2I',payload,24)
    bar_bytes,src,dst,msi=struct.unpack_from('<4Q',payload,32)
    ep_status,size,window=struct.unpack_from('<2IQ',payload,64)
    if not observed or (command,flags,events,ep_status,size,window)!=(32,1,1,0x50,4096,0x22000000):raise ValueError('DMA completion contract differs')
    if (vendor,device)!=struct.unpack_from('<2H',session,788) or bar!=struct.unpack_from('<Q',session,800)[0] or pci_command&0x407!=0x406 or link!=0x11:raise ValueError('Endpoint identity/configuration differs')
    if not 48<=bar_bytes<=0x100000 or bar_bytes&(bar_bytes-1) or payload[80:112]!=session[808:840] or payload[112:128]!=bytes(16):raise ValueError('BAR/ordering proof differs')
    ranges=[(src-4096,src+8192),(dst-4096,dst+8192),(msi,msi+4096)]
    if any(not 0x41c00000<=a<b<=0x41e00000 or a&4095 for a,b in ranges) or any(a<d and c<b for i,(a,b) in enumerate(ranges) for c,d in ranges[i+1:]):raise ValueError('DMA extent geometry differs')
    expected=bytes(((n*37+(n>>7)*19+(generation&0xffffffff)*13)^0x5b)&255 for n in range(4096))
    if payload[128:]!=expected:raise ValueError('Copied DMA bytes differ from original expected pattern')
    return dict(status=1,bytes=4224,copied_verified=True,observed_us=observed,vendor=vendor,device=device,bar=bar,bar_bytes=bar_bytes,dma_bytes=4096,endpoint_status=ep_status,events=events)
