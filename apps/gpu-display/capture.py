#!/usr/bin/env python3
"""Read copied GPU/HDMI observations from an already admitted USB0 device.

The caller supplies the selected device and original absolute host deadline.
Hashes and fixed samples identify captured bytes; they do not prove complete
frame correctness or that any pixel became visible on a monitor.
"""
import math
import struct
import time
MAGIC=0x4f444747
VERSION=1
VID,PID,BCD_DEVICE=0x1f3a,0xefe8,0x0110
SERIAL='sunstoneos-a733-gpu-display'
PIXEL_BYTES=1280*720*4
FRAME_BYTES=1847296
SAMPLE_X=(0,426,853,1279)
SAMPLE_Y=(0,239,479,719)


def require(condition,message):
    if not condition:
        raise ValueError(message)


def request(request_id,epoch,generation):
    require(1<=request_id<=200 and 0<epoch<2**64 and 0<generation<2**64,
            'Invalid observation request identity')
    return struct.pack('<4I2Q',MAGIC,VERSION,request_id,32,epoch,generation)


def identity_bytes(value):
    value=bytes.fromhex(value) if isinstance(value,str) else bytes(value)
    require(len(value)==32,'Identity digest must contain 32 bytes')
    return value


def decode(raw,*,request_id,epoch,generation,lease,build_id,session_sha256):
    raw=bytes(raw)
    expected_build=identity_bytes(build_id);expected_session=identity_bytes(session_sha256)
    require(len(raw)==1024,'Observation must contain exactly 1024 bytes')
    magic,version,size,ident,ep,gen,state,flags,frames,presentations,now,remaining=struct.unpack_from('<4I2Q4I2Q',raw)
    require((magic,version,size,ident,ep,gen)==(MAGIC,VERSION,1024,request_id,epoch,generation),
            'Observation identity differs from the request')
    require(state<=3 and not flags&~15 and frames in (0,2) and presentations<=3,
            'Unknown observation state')
    require(bool(flags&1)==bool(frames),'Frame publication flag differs')
    result=dict(request_id=ident,epoch=ep,generation=gen,state=state,flags=flags,
                frame_count=frames,presentation_count=presentations,now_us=now,remaining_us=remaining,
                frames=[],presentations=[])
    for index in range(2):
        at=64+256*index
        if not frames:
            require(not any(raw[at:at+256]),'Unpublished frame evidence is not zero')
            continue
        sequence,count,valid,reserved=struct.unpack_from('<4I',raw,at)
        require((sequence,count,valid,reserved)==(index+1,PIXEL_BYTES,1,0) and
                not any(raw[at+240:at+256]),'Invalid immutable frame evidence')
        samples=[]
        for i in range(16):
            rgba,rgb565,zero=struct.unpack_from('<IHH',raw,at+112+8*i)
            red,green,blue=(rgba&255,(rgba>>8)&255,(rgba>>16)&255)
            require(not zero and rgb565==(((red>>3)<<11)|((green>>2)<<5)|(blue>>3)),
                    'Sample conversion or reserved field differs')
            samples.append(dict(x=SAMPLE_X[i%4],y=SAMPLE_Y[i//4],rgba=rgba,rgb565=rgb565))
        result['frames'].append(dict(sequence=sequence,rgba_bytes=count,flags=valid,
            input_sha256=raw[at+16:at+48].hex(),rgba_sha256=raw[at+48:at+80].hex(),
            rgb565_sha256=raw[at+80:at+112].hex(),samples=samples))
    previous=0
    for index in range(3):
        at=576+64*index
        if index>=presentations:
            require(not any(raw[at:at+64]),'Unpublished presentation is not zero')
            continue
        slot,sequence,rcq,end,address,status,valid,observed,zero0,zero1=struct.unpack_from('<2I3Q2I3Q',raw,at)
        require(frames==2 and (slot,sequence)==((0,1,0)[index],(1,2,1)[index]) and
                valid==1 and not zero0 and not zero1 and
                address==0x10001000+slot*FRAME_BYTES and 0<rcq<=end<=observed<=now and
                rcq>=previous,'Presentation sequence, completion or mapping differs')
        previous=end
        result['presentations'].append(dict(slot=slot,sequence=sequence,rcq_done_us=rcq,
            frame_end_us=end,verified_source_address=address,raw_status=status,flags=valid,observed_us=observed))
    values=struct.unpack_from('<8I2Q',raw,768)
    keys=('gpu_closed','hdmi_state','de_state','pmic_parked','fatal','usb_ready','display_stage','flags','stop_at','deadline')
    terminal=dict(zip(keys,values))
    terminal['lease']=struct.unpack_from('<Q',raw,816)[0]
    terminal['build_id']=raw[824:856].hex();terminal['session_sha256']=raw[856:888].hex()
    require(0<lease<2**64 and terminal['lease']==lease and raw[824:856]==expected_build and
            raw[856:888]==expected_session,'Observation build, lease or full session identity differs')
    require(not any(raw[888:]) and not terminal['flags'] and terminal['gpu_closed']<=1 and
            terminal['pmic_parked']<=1 and terminal['fatal']<=1 and terminal['usb_ready']==1 and
            terminal['hdmi_state']<=14 and terminal['de_state']<=4 and terminal['display_stage']<=4,
            'Invalid terminal record')
    require(bool(flags&2)==bool(terminal['gpu_closed']) and bool(flags&4)==(state==2) and
            bool(flags&8)==bool(terminal['fatal']),'Terminal publication flags differ')
    require(terminal['stop_at']+2000000==terminal['deadline']<2**64 and
            0<remaining and now+remaining==terminal['deadline'],'Original device deadline differs')
    if state==2:
        require(frames==2 and presentations==3 and flags==7 and
                (terminal['gpu_closed'],terminal['hdmi_state'],terminal['de_state'],terminal['pmic_parked'],
                 terminal['fatal'],terminal['display_stage'])==(1,13,3,1,0,4),
                'Healthy stop has incomplete ownership or presentation evidence')
    result['terminal']=terminal
    return result


def capture(device,*,epoch,generation,lease,build_id,session_sha256,deadline,observer=None,clock=time.monotonic,sleep=time.sleep):
    """Capture until healthy stop; device is already selected and claimed.

    A 1025-byte read consumes the automatic terminal ZLP following the fixed
    1024-byte response. Each raw reply reaches observer before validation.
    """
    previous_time=0
    device_deadline=None
    for ident in range(1,201):
        left=deadline-clock()
        if left<=0:
            raise TimeoutError('Original host capture deadline expired')
        timeout=max(1,min(1000,math.ceil(left*1000)))
        packet=request(ident,epoch,generation)
        require(device.write(0x02,packet,timeout=timeout)==len(packet),'Short observation request write')
        left=deadline-clock()
        if left<=0:
            raise TimeoutError('Original host capture deadline expired before read')
        raw=bytes(device.read(0x81,1025,timeout=max(1,min(1000,math.ceil(left*1000)))))
        if observer is not None:
            observer(raw)
        decoded=decode(raw,request_id=ident,epoch=epoch,generation=generation,lease=lease,
                       build_id=build_id,session_sha256=session_sha256)
        require(decoded['now_us']>=previous_time,'Device observation clock moved backward')
        previous_time=decoded['now_us']
        current=decoded['terminal']['deadline']
        require(device_deadline in (None,current),'Device session deadline changed')
        device_deadline=current
        if decoded['state']==3 or decoded['terminal']['fatal']:
            raise ValueError('GPU/display reported retained failure')
        if decoded['state']==2:
            return decoded
        left=deadline-clock()
        if left<=0:
            raise TimeoutError('Original host capture deadline expired')
        sleep(min(0.1,left))
    raise TimeoutError('Observation poll bound exhausted')
