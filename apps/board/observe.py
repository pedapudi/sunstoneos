#!/usr/bin/env python3
"""Validate a captured board frame against the admitted session and radio profile."""
import argparse
import hashlib
from pathlib import Path
import struct

def decode(frame, session, profile=None):
    if len(session)!=592 or struct.unpack_from('<QQQ',session) not in ((0x41533733424f4131,1,592),(0x4153373342535332,2,592)):
        raise ValueError('invalid admitted session')
    if len(frame)<72:
        raise ValueError('short frame')
    magic,version,usb_state,usb_bytes,radio_state,radio_bytes,epoch,generation=struct.unpack_from('<IIIIIIQQ',frame)
    if magic!=0x52414f42 or version not in (1,2,3,4) or usb_state>3 or radio_state>3:
        raise ValueError('invalid frame header')
    if (epoch,generation)!=struct.unpack_from('<QQ',session,24):
        raise ValueError('session identity mismatch')
    if usb_bytes!=((544 if version==4 else 1088 if version==3 else 512) if usb_state==1 else 0) or len(frame)!=72+usb_bytes:
        raise ValueError('invalid copied sector extent')
    digest=session[368:400]
    workload=struct.unpack_from('<I',session,400)[0]
    if workload in (4,6,8) and struct.unpack_from('<Q',session)[0]!=0x4153373342535332:
        raise ValueError('Combined workload requires supervised session')
    if frame[40:72]!=digest:
        raise ValueError('radio profile identity mismatch')
    if usb_state==3:
        if workload not in (2,3) or struct.unpack_from('<Q',session)[0]!=0x4153373342535332:
            raise ValueError('disabled host is not admitted')
    elif workload in (2,3):
        raise ValueError('disabled host reported activity')
    if workload in (0,2,4,6,8):
        if profile is not None or radio_state or radio_bytes:
            raise ValueError('unexpected radio result')
    elif workload in (1,3):
        if profile is None or hashlib.sha256(profile).digest()!=digest:
            raise ValueError('admitted radio profile required')
        expected_version=4 if struct.unpack_from('<Q',session)[0]==0x4153373342535332 else 3
        if len(profile)!=1872 or struct.unpack_from('<QII',profile) not in ((0x3152464f52545353,expected_version,len(profile)),(0x3152464f52545353,6 if expected_version==4 else 5,len(profile))):
            raise ValueError('invalid radio profile')
        mode=struct.unpack_from('<H',profile,1338)[0];profile_version=struct.unpack_from('<I',profile,8)[0]
        if mode!=(1 if profile_version in (5,6) else 0) or (mode and not any(profile[1328:1332])):
            raise ValueError('invalid TCP client/listener mode or peer IPv4')
        policy,dns,reserved,address_timeout,lease_cap=struct.unpack_from('<B4s3sII',profile,1856)
        if policy not in (0,1) or reserved!=bytes(3) or (policy==0 and (address_timeout or lease_cap)) or (policy==1 and (not address_timeout or not lease_cap)):
            raise ValueError('invalid fixed/DHCP radio address policy')
        expected=struct.unpack_from('<H',profile,1336)[0]
        if not 0<expected<=256 or radio_state==0 or radio_bytes!=(expected if radio_state==2 else 0):
            raise ValueError('invalid checked radio result')
    else:
        raise ValueError('unsupported workload')
    result=dict(usb_state=usb_state,radio_state=radio_state,verified_radio_bytes=radio_bytes,
                kind='storage-sector' if usb_state==1 else 'pending-or-failed',
                sector=frame[72:] if usb_state==1 else None)
    if workload&1 and mode:result.update(radio_tcp_mode='listener',
                               radio_exchange_complete=radio_state==2,
                               radio_completion_scope='expected-response-and-socket-disposal',
                               tcp_wire_drain_established=False)
    if workload==4 and usb_state==1 and version!=3:raise ValueError('Combined workload requires combined result')
    if workload==8 and usb_state==1 and version!=2:raise ValueError('SS keyboard workload requires copied HID result')
    if workload==6 and usb_state==1 and version!=4:raise ValueError('SS-required workload needs SS result')
    if version==4:
        if workload!=6 or usb_state!=1:raise ValueError('SS result is not admitted')
        raw=frame[72:]
        if len(raw)!=544 or struct.unpack_from('<4I',raw)!=(0x43534d53,1,544,0):raise ValueError('Invalid copied SS sector header')
        generation=struct.unpack_from('<Q',raw,16)[0];address,speed,interface,epin,epout=raw[24:29]
        if not generation or not 1<=address<=127 or speed!=4 or interface or epin&~0x8f or not epin&128 or not epin&15 or epout&~15 or not epout or any(raw[29:32]):raise ValueError('Invalid SS device identity')
        result.update(kind='superspeed-sector',sector=raw[32:],device_address=address,device_speed='super-gen1',attachment_generation=generation,interface=interface,endpoint_in=epin,endpoint_out=epout,lba=0)
    if version==3:
        if workload!=4 or usb_state!=1:raise ValueError('Combined result is not admitted')
        result.update(decode_dual(frame[72:]))
    if version==2:
        if usb_state!=1:raise ValueError('HID version requires a completed copied batch')
        result.update(decode_hid(frame[72:],superspeed=workload==8))
        result['kind']='hid-input';result['sector']=None
    return result

def decode_dual(raw):
    if len(raw)!=1088 or struct.unpack_from('<4I',raw)!=(0x4c415544,1,1088,2):
        raise ValueError('Invalid combined result header')
    generation=struct.unpack_from('<Q',raw,16)[0]
    address,speed,interface,epin,epout=raw[24:29]
    lba,sector_offset,sector_bytes,hid_offset,hid_bytes=struct.unpack_from('<5I',raw,32)
    # Check the complete extent before creating either caller-facing slice.
    if not generation or not 1<=address<=127 or speed!=3 or interface or epin&~0x8f or not epin&128 or not epin&15 or epout&~15 or not epout or any(raw[29:32]) or any(raw[52:64]) or (lba,sector_offset,sector_bytes,hid_offset,hid_bytes)!=(0,64,512,576,512) or sector_offset+sector_bytes!=hid_offset or hid_offset+hid_bytes!=len(raw):
        raise ValueError('Invalid combined storage identity or extents')
    hid=decode_hid(raw[hid_offset:hid_offset+hid_bytes])
    if hid['hid_generation']!=generation or hid['device_address']==address:
        raise ValueError('Combined leaf identity mismatch')
    return dict(kind='storage-and-hid',sector=raw[sector_offset:sector_offset+sector_bytes],
                msc_address=address,msc_speed='high',msc_interface=interface,msc_in=epin,msc_out=epout,lba=lba,**hid)

def decode_hid(raw,superspeed=False):
    if len(raw)!=512 or struct.unpack_from('<3I',raw)!=(0x52444948,1,4):raise ValueError('Invalid copied HID batch')
    address=struct.unpack_from('<I',raw,12)[0];generation=struct.unpack_from('<Q',raw,16)[0]
    speed,interface,protocol,endpoint=raw[24:28]
    if not 1<=address<=127 or not generation or speed not in ((4,) if superspeed else (1,2,3)) or interface or protocol>2 or endpoint&~0x8f or not endpoint&0x80 or not endpoint&15 or any(raw[28:32]) or any(raw[352:]):
        raise ValueError('Invalid HID device/interface identity')
    if superspeed and protocol!=1:raise ValueError('SS keyboard protocol differs')
    reports=[];previous=0
    for i in range(4):
        at=32+i*80;sequence,count=struct.unpack_from('<QI',raw,at)
        if not sequence or sequence<=previous or count>(8 if speed==2 else 64) or any(raw[at+12:at+16]) or any(raw[at+16+count:at+80]):raise ValueError('Invalid HID report extent or sequence')
        reports.append(dict(sequence=sequence,bytes=count,data_hex=raw[at+16:at+16+count].hex()));previous=sequence
    return dict(hid_generation=generation,device_address=address,device_speed={1:'full',2:'low',3:'high',4:'super-gen1'}[speed],
      interface=interface,protocol=protocol,endpoint=endpoint,reports=reports)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frame',type=Path,required=True)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--profile',type=Path)
    parser.add_argument('--sector-output',type=Path)
    parser.add_argument('--hid-output',type=Path)
    a=parser.parse_args()
    result=decode(a.frame.read_bytes(),a.session.read_bytes(),a.profile.read_bytes() if a.profile else None)
    sector=result.pop('sector')
    if a.sector_output:
        if sector is None:raise ValueError('sector is not complete')
        with a.sector_output.open('xb') as output:output.write(sector)
    if a.hid_output:
        if result['kind'] not in ('hid-input','storage-and-hid'):raise ValueError('HID input is not complete')
        import json
        with a.hid_output.open('x') as output:json.dump(result,output,indent=2);output.write('\n')
    print(result)
if __name__=='__main__':main()
