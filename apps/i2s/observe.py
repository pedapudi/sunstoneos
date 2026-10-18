#!/usr/bin/env python3
"""Decode saved I2S frames offline. Physical capture uses check-a733-i2s-supervised.py."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def decode(frame,session,operation,offset=0,count=0):
    if len(session)<48 or struct.unpack_from('<Q',session,16)[0]!=len(session):
        raise ValueError('Invalid admitted I2S session size')
    if operation not in (0,1,2) or not 0<=offset<=16384 or not 0<=count<=min(4096,16384-offset) or (operation!=1 and (offset or count)):
        raise ValueError('Invalid I2S request shape')
    if len(frame)<72:raise ValueError('Short I2S response')
    magic,version,state,actual,size,total,epoch,generation=struct.unpack_from('<6I2Q',frame)
    if (magic,version,total,epoch,generation)!=(0x53493241,1,16384,*struct.unpack_from('<2Q',session,24)):
        raise ValueError('I2S response identity mismatch')
    if frame[40:72]!=hashlib.sha256(session).digest() or actual!=offset or state not in (0,1,2,3):
        raise ValueError('I2S response session, offset or state mismatch')
    expected=count if operation==1 and state==2 else 0
    if size!=expected or len(frame)!=72+size or (operation==2 and state!=3):
        raise ValueError('I2S response extent or cancellation mismatch')
    return {'state':state,'offset':actual,'bytes':size,'payload':frame[72:]}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frame',type=Path,required=True)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--operation',type=int,choices=(0,1,2),required=True)
    parser.add_argument('--offset',type=int,default=0);parser.add_argument('--count',type=int,default=0)
    a=parser.parse_args();result=decode(a.frame.read_bytes(),a.session.read_bytes(),a.operation,a.offset,a.count)
    payload=result.pop('payload');result['payload_sha256']=hashlib.sha256(payload).hexdigest()
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
