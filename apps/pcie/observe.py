#!/usr/bin/env python3
"""Decode one authenticated copied direct-endpoint configuration observation."""
import hashlib,importlib.util,struct
from pathlib import Path
spec=importlib.util.spec_from_file_location('pcie_observe_prepare',Path(__file__).with_name('prepare.py'))
preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
def decode(frame,session):
    preparation.session_check(session)
    if len(frame)<128:raise ValueError('Truncated PCIe frame')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x45494350,1) or status not in (0,1,2):raise ValueError('Wrong PCIe frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest():raise ValueError('PCIe session differs')
    if frame[96:128]!=bytes(32):raise ValueError('Reserved PCIe frame bytes differ')
    if status!=1:
        if count or len(frame)!=128 or frame[64:96]!=bytes(32):raise ValueError('Non-success carries PCIe output')
        return dict(status=status,bytes=0)
    payload=frame[128:]
    if count!=64 or len(payload)!=64 or frame[64:96]!=hashlib.sha256(payload).digest():raise ValueError('PCIe output hash/extent differs')
    observed,reads,function=struct.unpack_from('<Q2I',payload);vendor,device,command,link,revision,header,pcie,msi,reserved=struct.unpack_from('<4HI4B',payload,16)
    if not observed or reads!=4 or function or (vendor,device)!=struct.unpack_from('<2H',session,788) or command&4 or link&0x3ff!=0x11 or header&127 or reserved:raise ValueError('PCIe identity/link/command differs')
    if not 0x40<=pcie<=0xe4 or pcie&3 or msi and (not 0x40<=msi<=0xe8 or msi&3):raise ValueError('PCIe capability pointers differ')
    pairs=[struct.unpack_from('<2I',payload,32+8*i) for i in range(4)]
    if [a for a,v in pairs]!=[0,4,8,12]:raise ValueError('PCIe copied offset selection differs')
    values=[v for a,v in pairs]
    if values[0]!=(device<<16|vendor) or values[1]&65535!=command or not values[1]&(1<<20) or values[2]!=revision or values[3]>>16&255!=header:raise ValueError('PCIe copied header differs')
    return dict(status=1,bytes=64,copied_verified=True,observed_us=observed,vendor=vendor,device=device,command=command,link_status=link,class_revision=revision,configuration=dict(pairs))
