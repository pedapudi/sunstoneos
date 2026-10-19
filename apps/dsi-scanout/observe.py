"""Decode copied controller observations; never infer visible pixels."""
import hashlib,importlib.util,struct
from pathlib import Path
spec=importlib.util.spec_from_file_location('dsi_value_profile',Path(__file__).with_name('profile.py'));profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)
def decode(frame,session):
    if len(session)!=4096 or (struct.unpack_from('<Q',session,592)[0]!=profile.MAGIC or struct.unpack_from('<Q',session,600)[0] not in (1,2) or struct.unpack_from('<Q',session,608)[0]!=4096):raise ValueError('Wrong DSI session')
    if len(frame)<128:raise ValueError('Truncated DSI response')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x31525344,2 if struct.unpack_from('<Q',session,600)[0]==2 else 1) or status not in (0,1,2):raise ValueError('Wrong DSI response')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest() or any(frame[96:128]):raise ValueError('DSI session differs')
    if status!=1:
        if count or len(frame)!=128 or any(frame[64:96]):raise ValueError('Pending/failure contains observation')
        return dict(status=status,bytes=0,physical_qualification=False)
    p=frame[128:]
    if count!=256 or len(p)!=256 or frame[64:96]!=hashlib.sha256(p).digest():raise ValueError('DSI observation extent/hash differs')
    if version==2:return replay(p,session,epoch,generation)
    if struct.unpack_from('<4I',p)!=(0x52495344,1,256,1) or struct.unpack_from('<2Q',p,16)!=(epoch,generation):raise ValueError('Wrong observation identity')
    first,last=struct.unpack_from('<2Q',p,32)
    if not 0<first<=last or p[48:80]!=hashlib.sha256(session).digest() or p[80:112]!=profile.frame_digest(session):raise ValueError('Observation time/frame differs')
    if p[112:176]!=session[832:896] or not all(struct.unpack_from('<3Q',p,176)):raise ValueError('Observation geometry/ownership differs')
    if struct.unpack_from('<6I',p,216)!=(3,12,2,2,2,0x100):raise ValueError('Incomplete controller observation')
    if struct.unpack_from('<2Q',p,240)!=(profile.frame_bytes(session),struct.unpack_from('<Q',session,632)[0]):raise ValueError('Frame extent/panel differs')
    return dict(status=1,bytes=256,kind='dsi-controller-observation',epoch=epoch,generation=generation,first_us=first,last_us=last,width=struct.unpack_from('<I',p,124)[0],height=struct.unpack_from('<I',p,128)[0],raw_status=list(struct.unpack_from('<4I',p,200)),resources_retained=True,physical_qualification=False)

def replay(p,session,epoch,generation):
    if struct.unpack_from('<4I',p)!=(0x52505344,1,256,1):raise ValueError('Wrong replay observation')
    sequence,slot,observed_epoch,observed_generation,authority,rcq_done,frame_end=struct.unpack_from('<7Q',p,16)
    if not sequence or slot>1 or (observed_epoch,observed_generation)!=(epoch,generation) or authority!=struct.unpack_from('<Q',session,640)[0] or not 0<rcq_done<frame_end:raise ValueError('Replay identity/completion differs')
    if p[72:104]!=session[3616+slot*32:3648+slot*32] or p[104:136]!=hashlib.sha256(session).digest():raise ValueError('Replay frame/session differs')
    bank=2*profile.frame_bytes(session)+8192
    if not all(struct.unpack_from('<2Q',p,136)) or struct.unpack_from('<3Q',p,152)!=(bank,profile.frame_bytes(session),struct.unpack_from('<Q',session,632)[0]):raise ValueError('Replay ownership/geometry differs')
    raw,scanned=struct.unpack_from('<2I',p,176)
    if not raw&1 or scanned!=0x100 or any(p[184:]):raise ValueError('Incomplete replay observation')
    return dict(status=1,bytes=256,kind='dsi-immutable-frame-replay',sequence=sequence,slot=slot,epoch=epoch,generation=generation,rcq_done_us=rcq_done,frame_end_us=frame_end,frame_sha256=p[72:104].hex(),resources_retained=True,scanout_buffer_cpu_reusable=False,physical_qualification=False)
