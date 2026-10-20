#!/usr/bin/env python3
"""Decode supplied retained-RAM HDMI audio evidence; no live capture transport."""
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
    _,_,identity,metadata=preparation.image_metadata(image)
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
