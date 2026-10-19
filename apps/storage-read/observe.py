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
def decode(raw,session,expected=None,session_magic=0x4153373352444131,session_bytes=880):
    if len(session)!=session_bytes or struct.unpack_from('<3Q',session,592)!=(session_magic,2,session_bytes):raise ValueError('Wrong SD0 session geometry')
    if len(raw)<HEADER:raise ValueError('Truncated SD0 record')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    profile=struct.unpack_from('<I',session,860)[0] if session_magic==0x4153373352444131 else 0
    if profile not in (0,1,2):raise ValueError('Unsupported SD0 bus profile')
    if (magic,version)!=(0x44524453,3 if profile else 2) or status not in range(5):raise ValueError('Wrong SD0 record')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[208:240]!=session[48:80]:raise ValueError('Different SD0 session/build')
    mode,card_hz=struct.unpack_from('<2I',raw,280)
    width,timing,divisor=(mode>>8)&255,(mode>>16)&255,mode>>24
    if profile:
        if mode&255!=profile or width not in (0,1,4) or timing>1 or divisor>32:raise ValueError('Invalid negotiated SD0 mode')
    elif mode or card_hz:raise ValueError('Nonzero reserved SD0 bytes')
    ticket,first,request_id,blocks,client,application_status,application_reads=struct.unpack_from('<3Q4I',raw,240)
    if client>1 or application_status>2 or application_reads>2:raise ValueError('Invalid native client summary')
    facts=struct.unpack_from('<19I',raw,112)
    if profile and facts[18]:
        if profile==1:
            speed=struct.unpack_from('<I',raw,80)[0]&255
            factors=(0,10,12,13,15,20,25,30,35,40,45,50,55,60,70,80)
            if speed&128 or speed&7>3 or not factors[(speed>>3)&15]:raise ValueError('Invalid default-speed CSD limit')
            cap=min(25000000,(100000,1000000,10000000,100000000)[speed&7]*factors[(speed>>3)&15]//10)
            if divisor!=(400000000+2*cap-1)//(2*cap):raise ValueError('Default speed exceeds CSD or differs from admitted plan')
        if (width!=4 or timing!=(profile==2) or not (8<=divisor<=32 if profile==1 else divisor==4) or card_hz!=400000000//(2*divisor)):
            raise ValueError('Incomplete negotiated SD0 mode')
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
        sd_profile=profile,bus_width=width if profile else 1,timing=timing,
        card_hz=card_hz if profile else 400000,module_hz=(400000000//divisor if divisor and card_hz else 0) if profile else 800000,
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
