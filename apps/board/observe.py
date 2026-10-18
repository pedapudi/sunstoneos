#!/usr/bin/env python3
"""Validate a captured board frame against the admitted session and radio profile."""
import argparse
import hashlib
from pathlib import Path
import struct

def decode(frame, session, profile=None):
    if len(session)!=592 or struct.unpack_from('<QQQ',session)!=(0x41533733424f4131,1,592):
        raise ValueError('invalid admitted session')
    if len(frame)<72:
        raise ValueError('short frame')
    magic,version,usb_state,usb_bytes,radio_state,radio_bytes,epoch,generation=struct.unpack_from('<IIIIIIQQ',frame)
    if magic!=0x52414f42 or version!=1 or usb_state>2 or radio_state>3:
        raise ValueError('invalid frame header')
    if (epoch,generation)!=struct.unpack_from('<QQ',session,24):
        raise ValueError('session identity mismatch')
    if usb_bytes!=(512 if usb_state==1 else 0) or len(frame)!=72+usb_bytes:
        raise ValueError('invalid copied sector extent')
    digest=session[368:400]
    workload=struct.unpack_from('<I',session,400)[0]
    if frame[40:72]!=digest:
        raise ValueError('radio profile identity mismatch')
    if workload==0:
        if profile is not None or radio_state or radio_bytes:
            raise ValueError('unexpected radio result')
    elif workload==1:
        if profile is None or hashlib.sha256(profile).digest()!=digest:
            raise ValueError('admitted radio profile required')
        if len(profile)<528 or struct.unpack_from('<QII',profile)!=(0x3152464f52545353,1,len(profile)):
            raise ValueError('invalid radio profile')
        expected=struct.unpack_from('<H',profile,len(profile)-520)[0]
        if not 0<expected<=256 or radio_state==0 or radio_bytes!=(expected if radio_state==2 else 0):
            raise ValueError('invalid checked radio result')
    else:
        raise ValueError('unsupported workload')
    return dict(usb_state=usb_state,radio_state=radio_state,verified_radio_bytes=radio_bytes,
                sector=frame[72:] if usb_state==1 else None)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frame',type=Path,required=True)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--profile',type=Path)
    parser.add_argument('--sector-output',type=Path)
    a=parser.parse_args()
    result=decode(a.frame.read_bytes(),a.session.read_bytes(),a.profile.read_bytes() if a.profile else None)
    sector=result.pop('sector')
    if a.sector_output:
        if sector is None:raise ValueError('sector is not complete')
        with a.sector_output.open('xb') as output:output.write(sector)
    print(result)
if __name__=='__main__':main()
