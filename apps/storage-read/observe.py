#!/usr/bin/env python3
"""Decode copied bounded SD0 reads without making filesystem-validity claims."""
import hashlib
import struct
HEADER=288
MAXIMUM=4096
def read_request(first,blocks,request_id):
    if not 0<=first<=0xffffffff or not 1<=blocks<=8 or blocks-1>0xffffffff-first or not 0<request_id<2**64:
        raise ValueError('Invalid bounded SD0 read request')
    return struct.pack('<BBHIQQQ',2,2,32,blocks,request_id,first,0)
def cancel_request(ticket):
    if not 0<ticket<2**64:raise ValueError('Invalid SD0 cancellation ticket')
    return struct.pack('<BBHIQQQ',3,2,32,0,ticket,0,0)
def decode(raw,session,expected=None):
    if len(session)!=880 or struct.unpack_from('<3Q',session,592)!=(0x4153373352444131,2,880):raise ValueError('Wrong SD0 session geometry')
    if len(raw)<HEADER:raise ValueError('Truncated SD0 record')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x44524453,2) or status not in range(5):raise ValueError('Wrong SD0 record')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[208:240]!=session[48:80]:raise ValueError('Different SD0 session/build')
    if any(raw[280:HEADER]):raise ValueError('Nonzero reserved SD0 bytes')
    ticket,first,request_id,blocks,client,application_status,application_reads=struct.unpack_from('<3Q4I',raw,240)
    if client>1 or application_status>2 or application_reads>2:raise ValueError('Invalid native client summary')
    facts=struct.unpack_from('<19I',raw,112)
    if status==1:
        sectors=struct.unpack_from('<Q',raw,104)[0]
        if not ticket or not request_id or not 1<=blocks<=8 or first>=sectors or blocks>sectors-first:raise ValueError('Invalid completed read range')
        if first+blocks-1>(0xffffffff if raw[102] else 0xffffffff//512):raise ValueError('Read command argument overflows')
        if count!=blocks*512 or len(raw)!=HEADER+count or raw[32:64]!=hashlib.sha256(raw[HEADER:]).digest():raise ValueError('Incomplete copied sectors')
        if facts[0] or facts[5]!=count or facts[8] or facts[18]!=1:raise ValueError('Read integrity/card validity failed')
    elif count or len(raw)!=HEADER or any(raw[32:64]):raise ValueError('Non-success carries data')
    result=dict(status=status,bytes=count,epoch=epoch,generation=generation,ticket=ticket,
        first_sector=first,blocks=blocks,request_id=request_id,client=client,
        application_status=application_status,application_reads=application_reads,
        cid=list(struct.unpack_from('<4I',raw,64)),csd=list(struct.unpack_from('<4I',raw,80)),
        ocr=struct.unpack_from('<I',raw,96)[0],rca=struct.unpack_from('<H',raw,100)[0],
        sector_addressed=raw[102],ext_revision=raw[103],sectors=struct.unpack_from('<Q',raw,104)[0],
        error=facts[0],interrupts=facts[1],controller_status=facts[2],response=facts[3],stop_response=facts[4],
        received=facts[5],submitted=facts[6],confirmed_blocks=facts[7],write_started=facts[8],
        card_state=facts[9],platform_stage=facts[10],command=facts[11],argument=facts[12],seen=facts[13],
        setup=facts[14],protocol=facts[15],command_step=facts[16],response_seen=facts[17],card_valid=facts[18],
        raw_response=list(struct.unpack_from('<4I',raw,188)),reason=struct.unpack_from('<I',raw,204)[0],
        sector_sha256=raw[32:64].hex() if status==1 else None,filesystem_validity='not assessed')
    if expected and any(result[k]!=v for k,v in expected.items()):raise ValueError('Different SD0 request/client ticket or range')
    return result
