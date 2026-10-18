#!/usr/bin/env python3
"""Decode a copied native SD0 record; never infer filesystem validity."""
import hashlib
import struct
def decode(raw,session):
    if len(session)!=864 or struct.unpack_from('<3Q',session,592)!=(0x4153373352444131,1,864):raise ValueError('Wrong SD0 session geometry')
    if len(raw)<256:raise ValueError('Truncated SD0 record')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x44524453,1) or status not in (0,1,2):raise ValueError('Wrong SD0 record')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[208:240]!=session[48:80]:raise ValueError('Different SD0 session/build')
    if any(raw[240:256]):raise ValueError('Nonzero reserved SD0 bytes')
    facts=struct.unpack_from('<19I',raw,112)
    if status==1:
        if count!=512 or len(raw)!=768 or raw[32:64]!=hashlib.sha256(raw[256:]).digest():raise ValueError('Incomplete copied sector')
        if facts[0] or facts[5]!=512 or facts[8] or facts[18]!=1:raise ValueError('Read integrity/card validity failed')
    elif count or len(raw)!=256 or any(raw[32:64]):raise ValueError('Non-success carries data')
    return dict(status=status,bytes=count,epoch=epoch,generation=generation,
        cid=list(struct.unpack_from('<4I',raw,64)),csd=list(struct.unpack_from('<4I',raw,80)),
        ocr=struct.unpack_from('<I',raw,96)[0],rca=struct.unpack_from('<H',raw,100)[0],
        sector_addressed=raw[102],ext_revision=raw[103],sectors=struct.unpack_from('<Q',raw,104)[0],
        error=facts[0],interrupts=facts[1],controller_status=facts[2],response=facts[3],stop_response=facts[4],
        received=facts[5],submitted=facts[6],confirmed_blocks=facts[7],write_started=facts[8],
        card_state=facts[9],platform_stage=facts[10],command=facts[11],argument=facts[12],seen=facts[13],
        setup=facts[14],protocol=facts[15],command_step=facts[16],response_seen=facts[17],card_valid=facts[18],
        raw_response=list(struct.unpack_from('<4I',raw,188)),reason=struct.unpack_from('<I',raw,204)[0],
        sector_sha256=raw[32:64].hex() if status==1 else None,filesystem_validity='not assessed')
