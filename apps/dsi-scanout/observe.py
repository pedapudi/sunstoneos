"""Decode copied controller observations; never infer visible pixels."""
import hashlib,importlib.util,struct
from pathlib import Path
spec=importlib.util.spec_from_file_location('dsi_value_profile',Path(__file__).with_name('profile.py'));profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)
def decode(frame,session):
    if len(session)!=4096 or struct.unpack_from('<3Q',session,592)!=(profile.MAGIC,1,4096):raise ValueError('Wrong DSI session')
    if len(frame)<128:raise ValueError('Truncated DSI response')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x31525344,1) or status not in (0,1,2):raise ValueError('Wrong DSI response')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest() or any(frame[96:128]):raise ValueError('DSI session differs')
    if status!=1:
        if count or len(frame)!=128 or any(frame[64:96]):raise ValueError('Pending/failure contains observation')
        return dict(status=status,bytes=0,physical_qualification=False)
    p=frame[128:]
    if count!=256 or len(p)!=256 or frame[64:96]!=hashlib.sha256(p).digest():raise ValueError('DSI observation extent/hash differs')
    if struct.unpack_from('<4I',p)!=(0x52495344,1,256,1) or struct.unpack_from('<2Q',p,16)!=(epoch,generation):raise ValueError('Wrong observation identity')
    first,last=struct.unpack_from('<2Q',p,32)
    if not 0<first<=last or p[48:80]!=hashlib.sha256(session).digest() or p[80:112]!=profile.frame_digest(session):raise ValueError('Observation time/frame differs')
    if p[112:176]!=session[832:896] or not all(struct.unpack_from('<3Q',p,176)):raise ValueError('Observation geometry/ownership differs')
    if struct.unpack_from('<6I',p,216)!=(3,12,2,2,2,0x100):raise ValueError('Incomplete controller observation')
    if struct.unpack_from('<2Q',p,240)!=(profile.frame_bytes(session),struct.unpack_from('<Q',session,632)[0]):raise ValueError('Frame extent/panel differs')
    return dict(status=1,bytes=256,kind='dsi-controller-observation',epoch=epoch,generation=generation,first_us=first,last_us=last,width=struct.unpack_from('<I',p,124)[0],height=struct.unpack_from('<I',p,128)[0],raw_status=list(struct.unpack_from('<4I',p,200)),resources_retained=True,physical_qualification=False)
