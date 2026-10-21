#!/usr/bin/env python3
"""Decode retained RAM or durably capture admitted copied USB HDMI audio evidence."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
spec=importlib.util.spec_from_file_location('hdmi_audio_capture_prepare',Path(__file__).with_name('prepare.py'))
preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
require=preparation.require


def decode(raw,ready,session,pcm):
    fields=preparation.session_check(session,pcm)
    require(ready==struct.pack('<I',1),'HDMI audio terminal readiness was not published.')
    require(len(raw)==160,'HDMI audio result must be160 bytes.')
    magic,version,size,stage,frames,source_bytes,consumed,dma_result,fifo_empty,tmds,rate,wire=struct.unpack_from('<12Q',raw)
    require((magic,version,size)==(0x4153373348415231,1,160) and stage in (2,3),
        'HDMI audio terminal result ABI differs.')
    require(frames==fields['frames'] and source_bytes==len(pcm) and consumed<=source_bytes and
        not consumed%4 and dma_result in (0,1,2,3) and fifo_empty in (0,1) and wire==0 and
        rate==48000 and raw[96:128]==session[440:472] and
        raw[128:160]==hashlib.sha256(session).digest(),
        'HDMI audio result source identity or observations differ.')
    raw_hz=struct.unpack_from('<I',session,300)[0]
    require(raw_hz in (24000000,26000000),'Unsupported admitted HDMI raw clock.')
    expected_tmds=raw_hz*(99 if raw_hz==24000000 else 91)//32
    require(tmds in (0,expected_tmds),'HDMI audio TMDS rate differs from admitted clock profile.')
    if stage==2:
        require(consumed==source_bytes and dma_result==1 and fifo_empty==1 and tmds==expected_tmds,
            'HDMI audio input-ended result lacks source/FIFO completion.')
    require(consumed==0 or dma_result==1 and consumed==source_bytes,
        'HDMI audio source consumption was not established by complete DMA service.')
    return dict(stage='input-ended-retained' if stage==2 else 'fault-retained',frames=frames,
        source_bytes=source_bytes,source_consumed_bytes=consumed,dma_result=dma_result,
        fifo_empty=bool(fifo_empty),tmds_hz=tmds,rate_hz=rate,wire_drained=False,
        pcm_sha256=raw[96:128].hex(),session_sha256=raw[128:160].hex(),physical_qualified=False,
        retention='whole-board-power-removal',live_capture=False)


def durable(path,data):
    with path.open('xb') as stream:stream.write(data);stream.flush();os.fsync(stream.fileno())
    descriptor=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY)
    try:os.fsync(descriptor)
    finally:os.close(descriptor)


def capture(image,session,pcm,result,ready,output,*,save=durable):
    """Archive supplied observations before publishing an offline decode result.

    Caller-provided files do not establish a coherent read from a running board.
    The full admitted-session digest binds image, epoch, generation and proofs.
    """
    _,_,identity,metadata=preparation.image_metadata(image,session_size=len(session))
    require(session[48:80]==identity,
        'HDMI audio session names another linked image.')
    decoded=decode(result,ready,session,pcm)
    # Requiring an existing parent keeps directory-entry durability bounded.
    output.mkdir(exist_ok=False)
    descriptor=os.open(output.parent,os.O_RDONLY|os.O_DIRECTORY)
    try:os.fsync(descriptor)
    finally:os.close(descriptor)
    files={'result.bin':result,'ready.bin':ready,'session.bin':session,'input.pcm':pcm,'image.elf':image}
    for name,data in files.items():save(output/name,data)
    record=dict(schema='a733-hdmi-audio-offline-observation-v1',physical_access=False,
        executable_live_capture=False,**metadata,result=decoded,
        files={name:hashlib.sha256(data).hexdigest() for name,data in files.items()},
        session_sha256=hashlib.sha256(session).hexdigest())
    save(output/'observation.json',(json.dumps(record,indent=2)+'\n').encode())
    return record


# Optional copied USB observation. The 656-byte format has no live caller.
def request_bytes(request_id,session):
    require(len(session)==816 and struct.unpack_from('<3Q',session)==
        (preparation.base.SESSION_MAGIC,2,816),'Copied USB requires the version2 session.')
    require(type(request_id) is int and 1<=request_id<=200,'Copied USB request budget exceeded.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    require(epoch and generation,'Copied USB requires live session identities.')
    return struct.pack('<4I2Q',0x4f414748,1,request_id,32,epoch,generation)


def decode_observation(frame,request_id,session,pcm):
    preparation.session_check(session,pcm);request_bytes(request_id,session)
    require(len(frame)==512,'HDMI audio copied response must be512 bytes.')
    magic,version,size,received,epoch,generation,lease,state,flags,now_us,remaining_us=struct.unpack_from('<4I3Q2I2Q',frame)
    require((magic,version,size,received)==(0x52414748,1,512,request_id) and state in (0,1,2) and
        not flags&~15 and (state==2 or not flags&8),'HDMI audio copied response header differs.')
    require((epoch,generation,lease)==struct.unpack_from('<3Q',session,24) and
        frame[64:96]==session[48:80] and frame[96:128]==hashlib.sha256(session).digest(),
        'HDMI audio copied response belongs to another request/session/image.')
    require(not any(frame[288:]),'HDMI audio copied reserved bytes differ.')
    require(now_us>0 and 0<remaining_us<=20000000 and now_us<(1<<64)-remaining_us,
        'HDMI audio copied response lifetime differs.')
    audio=None
    if flags&1:audio=decode(frame[128:288],struct.pack('<I',1),session,pcm)
    else:require(not any(frame[128:288]),'Unpublished HDMI audio result carries bytes.')
    if state==0 and audio is not None:
        require(audio['stage']=='input-ended-retained','Pending copied response carries failed audio.')
    if state==1:
        require(flags==7 and audio is not None and audio['stage']=='input-ended-retained',
            'HDMI audio success requires completed audio, stopped HDMI and ready USB.')
    return dict(state=state,flags=flags,now_us=now_us,remaining_us=remaining_us,
        native_deadline_us=now_us+remaining_us,audio=audio,physical_qualified=False,
        wire_drained=False,retention='whole-board-power-removal')


def capture_copied(exchange,session,pcm,evidence,record,save,deadline,*,now=None,pause=None):
    import time
    now=time.monotonic if now is None else now;pause=time.sleep if pause is None else pause
    preparation.session_check(session,pcm);request_bytes(1,session)
    record.update(hdmi_audio_result='unobserved',hdmi_audio_retention='whole-board-power-removal',
        physical_qualified=False,hdmi_audio_session_sha256=hashlib.sha256(session).hexdigest())
    save();native_deadline=None;last_now=0
    try:
        for sequence in range(1,201):
            require(now()<deadline,'HDMI audio capture deadline expired.')
            packet=request_bytes(sequence,session)
            entry=dict(request_id=sequence,request_sha256=hashlib.sha256(packet).hexdigest())
            record.setdefault('hdmi_audio_requests',[]).append(entry);save()
            exchange_deadline=min(deadline,now()+1)
            frame=exchange(packet,min(1000,max(1,int((exchange_deadline-now())*1000))))
            path=evidence/f'hdmi-audio-{sequence:03}.bin';durable(path,frame)
            entry.update(path=path.name,sha256=hashlib.sha256(frame).hexdigest(),bytes=len(frame));save()
            require(now()<exchange_deadline,'HDMI audio response exceeded exchange deadline.')
            result=decode_observation(frame,sequence,session,pcm);entry.update(result)
            require(now()<deadline,'HDMI audio copied response persisted after deadline.')
            require((native_deadline is None or result['native_deadline_us']==native_deadline) and
                result['now_us']>=last_now,'HDMI audio native deadline changed or counter regressed.')
            native_deadline=result['native_deadline_us'];last_now=result['now_us']
            if result['state']==0:save();pause(min(.025,max(0,deadline-now())));continue
            if result['audio'] is not None:
                output=evidence/'copied-audio-result.bin';durable(output,frame[128:288])
                record['hdmi_audio_output']=dict(path=output.name,sha256=hashlib.sha256(frame[128:288]).hexdigest())
            require(now()<deadline,'HDMI audio copied result persisted after deadline.')
            record['hdmi_audio_result']='observed-success' if result['state']==1 else 'observed-failure'
            record['durable_result']=entry.copy();save()
            return result
        raise ValueError('HDMI audio copied pending-query budget exhausted.')
    except BaseException as error:
        record['hdmi_audio_result']='unobserved';record.pop('durable_result',None)
        record['hdmi_audio_capture_failure']=dict(type=type(error).__name__,message=str(error))
        save();raise


def capture_device(device,same,deadline,session,pcm,evidence,record,save,*,now=None,pause=None):
    import time
    now=time.monotonic if now is None else now
    def exchange(packet,timeout):
        exchange_deadline=min(deadline,now()+min(1000,timeout)/1000)
        def remaining():
            same();left=int((exchange_deadline-now())*1000)
            require(left>0,'HDMI audio copied exchange exceeded its deadline.');return min(1000,left)
        require(device.write(0x02,packet,timeout=remaining())==len(packet),'Incomplete HDMI audio copied request.')
        # A read larger than512 cannot finish at an exact packet boundary until
        # the device supplies its terminal ZLP. A missing ZLP is a transport fault.
        raw=bytes(device.read(0x81,513,timeout=remaining()))
        return raw
    return capture_copied(exchange,session,pcm,evidence,record,save,deadline,now=now,pause=pause)
