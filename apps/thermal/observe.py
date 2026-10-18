#!/usr/bin/env python3
"""Decode copied calibration and two fresh five-channel observation sets."""
import hashlib
import importlib.util
from pathlib import Path
import struct
spec=importlib.util.spec_from_file_location('thermal_observe_prepare',Path(__file__).with_name('prepare.py'))
preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
def temperature(code):return (code-2822)*-62 if code>1769 else (code-2835)*-59

def decode(frame,session):
    preparation.session_check(session)
    if len(frame)<128:raise ValueError('Truncated thermal frame')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x4d524854,1) or status not in (0,1,2):raise ValueError('Wrong thermal frame')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest():raise ValueError('Thermal session differs')
    if status!=1:
        if count or len(frame)!=128 or frame[64:128]!=bytes(64):raise ValueError('Non-success carries thermal output')
        return dict(status=status,bytes=0)
    payload=frame[128:]
    if count!=224 or len(payload)!=224 or frame[64:96]!=hashlib.sha256(payload[:32]).digest() or frame[96:128]!=hashlib.sha256(payload).digest():raise ValueError('Thermal output hash/extent differs')
    factory,*calibration=struct.unpack_from('<11H',payload)
    if not 0<factory<4095 or payload[22:32]!=bytes(10):raise ValueError('Invalid factory calibration')
    for code,correction in zip(calibration[:5],calibration[5:]):
        if not 0<code<4095 or not 0<=correction<=4095 or correction!=2048-int((100*factory-temperature(code))/-62):raise ValueError('Thermal calibration differs')
    previous_sequence=0;previous_times=[0]*5;records=[]
    for i in range(2):
        raw=payload[32+96*i:128+96*i];first,last,valid,alarm,off,critical=struct.unpack_from('<2I4B',raw)
        times=struct.unpack_from('<5Q',raw,16);codes=struct.unpack_from('<5H',raw,56);temps=struct.unpack_from('<5i',raw,72)
        if not previous_sequence<first<=last or valid!=31 or (alarm|off|critical)&~31 or raw[12:16]!=bytes(4) or raw[66:72]!=bytes(6) or raw[92:96]!=bytes(4):raise ValueError('Thermal observation order/padding differs')
        if any(not t or t<before for t,before in zip(times,previous_times)) or any(not 0<c<=4095 or temperature(c)!=t for c,t in zip(codes,temps)):raise ValueError('Thermal channel sample differs')
        records.append(dict(first_sequence=first,last_sequence=last,observed_us=list(times),codes=list(codes),millicelsius=list(temps),alarm=alarm,alarm_off=off,critical=critical))
        previous_sequence=last;previous_times=times
    return dict(status=1,bytes=224,copied_verified=True,records=records)
