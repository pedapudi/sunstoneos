#!/usr/bin/env python3
"""Decode a session-bound copied PCIe NVMe read result."""
import hashlib,importlib.util,struct
from pathlib import Path
spec=importlib.util.spec_from_file_location('pcie_nvme_observe_prepare',Path(__file__).with_name('prepare.py'))
preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
RECORD=192
OUTPUT=RECORD+4096
def decode(frame,session):
    preparation.session_check(session)
    if len(frame)<128:raise ValueError('Truncated PCIe NVMe frame')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x4d564e50,1) or status not in (0,1,2):raise ValueError('Wrong PCIe NVMe frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest():raise ValueError('PCIe NVMe session differs')
    if frame[96:128]!=bytes(32):raise ValueError('Reserved PCIe NVMe frame bytes differ')
    if status!=1:
        if count or len(frame)!=128 or frame[64:96]!=bytes(32):raise ValueError('Non-success carries PCIe NVMe output')
        return dict(status=status,bytes=0)
    payload=frame[128:]
    if count!=OUTPUT or len(payload)!=OUTPUT or frame[64:96]!=hashlib.sha256(payload).digest():raise ValueError('Copied NVMe output hash/extent differs')
    observed,size,flags=struct.unpack_from('<Q2I',payload)
    vendor,device,subsystem,link=struct.unpack_from('<4H',payload,16)
    nvme_version,namespaces,blocks,shift,mdts,lba,bar,bar_bytes=struct.unpack_from('<2IQ2IQ2I',payload,24)
    namespace,=struct.unpack_from('<I',payload,92);class_revision,function=struct.unpack_from('<2I',payload,136)
    if not observed or (size,flags)!=(4096,3):raise ValueError('NVMe read or orderly stop incomplete')
    session_vendor,session_device=struct.unpack_from('<2H',session,788)
    session_lba=struct.unpack_from('<Q',session,800)[0];session_function,session_namespace=struct.unpack_from('<2I',session,808)
    if (vendor,device,function,namespace,lba)!=(session_vendor,session_device,session_function,session_namespace,session_lba) or class_revision>>8!=0x010802 or (link&0x3ff)!=0x11:
        raise ValueError('NVMe endpoint identity or read target differs')
    if not 9<=shift<=12 or not 1<=namespace<=namespaces or blocks<(4096>>shift) or lba>blocks-(4096>>shift) or nvme_version<0x00010000:
        raise ValueError('NVMe namespace geometry differs')
    if bar_bytes<0x2000 or bar_bytes&(bar_bytes-1) or bar&(bar_bytes-1) or not 0x22000000<=bar<=0x28000000-bar_bytes:
        raise ValueError('NVMe BAR0 placement differs')
    if payload[144:176]!=session[816:848] or payload[176:RECORD]!=bytes(RECORD-176):raise ValueError('NVMe record authority differs')
    text=lambda at,n:payload[at:at+n].decode('ascii','replace').rstrip(' \x00')
    return dict(status=1,bytes=OUTPUT,observed_us=observed,vendor=vendor,device=device,subsystem_vendor=subsystem,
        nvme_version=nvme_version,namespaces=namespaces,blocks=blocks,lba_shift=shift,mdts=mdts,lba=lba,
        bar0=bar,bar0_bytes=bar_bytes,serial=text(64,20),firmware=text(84,8),model=text(96,40),
        data_sha256=hashlib.sha256(payload[RECORD:]).hexdigest())
