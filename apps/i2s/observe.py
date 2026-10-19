#!/usr/bin/env python3
"""Decode saved I2S frames offline. Physical capture uses check-a733-i2s-supervised.py."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def decode(frame,session,operation,offset=0,count=0,pcm_bytes=16384):
    if len(session)<48 or struct.unpack_from('<Q',session,16)[0]!=len(session):
        raise ValueError('Invalid admitted I2S session size')
    if operation not in (0,1,2) or not 0<=offset<=pcm_bytes or not 0<=count<=min(4096,pcm_bytes-offset) or (operation!=1 and (offset or count)):
        raise ValueError('Invalid I2S request shape')
    duplex=len(session)>=216 and struct.unpack_from('<I',session,208)[0]==2
    base=96 if duplex else 72
    if len(frame)<base:raise ValueError('Short I2S response')
    magic,version,state,actual,size,total,epoch,generation=struct.unpack_from('<6I2Q',frame)
    if (magic,version,total,epoch,generation)!=(0x53493241,2 if duplex else 1,pcm_bytes,*struct.unpack_from('<2Q',session,24)):
        raise ValueError('I2S response identity mismatch')
    if frame[40:72]!=hashlib.sha256(session).digest() or actual!=offset or state not in (0,1,2,3):
        raise ValueError('I2S response session, offset or state mismatch')
    expected=count if operation==1 and state==2 else 0
    if size!=expected or len(frame)!=base+size or (operation==2 and state!=3):
        raise ValueError('I2S response extent or cancellation mismatch')
    completed={}
    if duplex:
        values=struct.unpack_from('<6I',frame,72)
        if values!=((16384,16384,4,4,3,0) if state==2 else (0,0,0,0,0,0)):
            raise ValueError('Incomplete I2S duplex transfer counts or drain status')
        if state==2:completed={'capture_bytes':values[0],'playback_bytes':values[1],
            'capture_periods':values[2],'playback_periods':values[3],'drained_directions':values[4]}
    return {'state':state,'offset':actual,'bytes':size,'payload':frame[base:],**completed}

def decode_transcript(frame,session,offset=0,count=0,samples=None):
    if len(session)!=640 or struct.unpack_from('<3Q',session)!=(0x4153373341555331,2,640) or not 0<=count<=4096:
        raise ValueError('Invalid admitted transcription session/request')
    admitted,reserved=struct.unpack_from('<2I',session,552)
    if not 16000<=admitted<=80000 or reserved or (samples is not None and samples!=admitted):
        raise ValueError('Invalid admitted transcription duration')
    if len(frame)<144:raise ValueError('Short I2S transcription frame')
    magic,version,state,actual,size,total,epoch,generation=struct.unpack_from('<6I2Q',frame)
    if (magic,version,epoch,generation)!=(0x54533249,1,*struct.unpack_from('<2Q',session,24)) or state not in (0,2,3):
        raise ValueError('I2S transcription identity/state mismatch')
    if frame[40:72]!=hashlib.sha256(session).digest() or actual!=offset or len(frame)!=144+size:
        raise ValueError('I2S transcription binding/extent mismatch')
    text_bytes,captured=struct.unpack_from('<2I',frame,72)
    if state==2:
        if not 0<total<=65536 or text_bytes!=total or not 0<=offset<=total or count>total-offset or size!=count or not 16000<=captured<=80000 or captured!=admitted:
            raise ValueError('I2S transcription completion extent mismatch')
    elif size or total or frame[72:144]!=bytes(72):raise ValueError('Partial I2S transcription exposed')
    return {'state':state,'samples':captured,'total':total,'pcm_sha256':frame[80:112].hex(),
            'text_sha256':frame[112:144].hex(),'payload':frame[144:]}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frame',type=Path,required=True)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--operation',type=int,choices=(0,1,2,3,4,5),required=True)
    parser.add_argument('--offset',type=int,default=0);parser.add_argument('--count',type=int,default=0)
    parser.add_argument('--ticket',type=int,default=0)
    parser.add_argument('--pcm-bytes',type=int,default=16384);parser.add_argument('--samples',type=int)
    a=parser.parse_args()
    session=a.session.read_bytes()
    result=decode_repeated(a.frame.read_bytes(),session,a.operation,a.offset,a.count,a.ticket) if len(session)>=216 and struct.unpack_from('<I',session,208)[0]>=3 else decode_transcript(a.frame.read_bytes(),session,a.offset,a.count,a.samples) if a.operation==3 else decode(a.frame.read_bytes(),session,a.operation,a.offset,a.count,a.pcm_bytes)
    payload=result.pop('payload');result['payload_sha256']=hashlib.sha256(payload).hexdigest()
    print(json.dumps(result,indent=2))


def decode_repeated(frame,session,operation,offset=0,count=0,ticket=0):
    if len(session)!=552 or struct.unpack_from('<3Q',session)!=(0x4153373341555331,1,552):
        raise ValueError('Invalid admitted repeated I2S session')
    direction=struct.unpack_from('<I',session,208)[0]
    if direction not in (3,4,5) or operation not in (0,2,4,5):raise ValueError('Invalid repeated I2S operation')
    if operation==4:
        if direction==4 or not 0<=offset<=4096 or not 0<=count<=4096-offset:raise ValueError('Invalid repeated period read')
    elif offset or count or (operation==5 and not 0<ticket<2**64):raise ValueError('Invalid repeated I2S control request')
    if len(frame)<128:raise ValueError('Short repeated I2S frame')
    magic,version,state,actual,size,total,epoch,generation=struct.unpack_from('<6I2Q',frame)
    if (magic,version,total,epoch,generation)!=(0x53493241,3,4096,*struct.unpack_from('<2Q',session,24)) or state not in (0,1,2,3):
        raise ValueError('Repeated I2S identity/state mismatch')
    if actual!=offset or frame[40:72]!=hashlib.sha256(session).digest():raise ValueError('Repeated I2S session/offset mismatch')
    period,first,frames,counter,rx,tx,drained,flags,reserved=struct.unpack_from('<QQIIQQIIQ',frame,72)
    mask=3 if direction==5 else 1
    if reserved or drained&~mask or flags&~3 or (state==2 and (drained!=mask or flags)):
        raise ValueError('Repeated I2S terminal drain mismatch')
    if (direction==3 and tx) or (direction==4 and rx) or rx%2048 or tx%2048:
        raise ValueError('Repeated I2S completed frame mismatch')
    if operation==4 and period:
        if frames!=2048 or first%2048 or first+frames>rx or size!=count:raise ValueError('Repeated copied-period extent mismatch')
    elif size or first or frames or counter or period!=(ticket if operation==5 else 0):
        raise ValueError('Unexpected repeated I2S period metadata')
    if len(frame)!=128+size:raise ValueError('Repeated I2S response extent mismatch')
    return dict(state=state,ticket=period,first_frame=first,frames=frames,counter=counter,
                rx_frames=rx,tx_frames=tx,drained=drained,flags=flags,payload=frame[128:])

if __name__=='__main__':main()
