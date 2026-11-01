#!/usr/bin/env python3
"""Replay pinned public FTDI USB bulk-IN traffic through serial receive ownership."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT=Path(__file__).resolve().parents[1]

def captured_packets(data):
    """Read complete little-endian pcapng Enhanced Packet Blocks (DLT_USBPCAP)."""
    offset=0; links=[]; number=0
    while offset<len(data):
        assert len(data)-offset>=12, 'truncated pcapng block'
        kind,length=struct.unpack_from('<II',data,offset)
        assert length>=12 and length%4==0 and offset+length<=len(data)
        assert struct.unpack_from('<I',data,offset+length-4)[0]==length
        block=data[offset:offset+length]
        if kind==0x0a0d0d0a:
            assert not links and block[8:12]==b'\x4d\x3c\x2b\x1a', 'unsupported pcapng section'
        elif kind==1:
            links.append(struct.unpack_from('<H',block,8)[0])
        elif kind==6:
            number+=1
            interface,_,_,saved,original=struct.unpack_from('<IIIII',block,8)
            assert interface<len(links) and links[interface]==249
            assert saved==original and 28+saved<=length-4, 'truncated USBPcap packet'
            yield number,block[28:28+saved]
        offset+=length
    assert offset==len(data)

def decode(data):
    records=[]; frames=[]; config=None; identity=None; modes={}
    for frame,b in captured_packets(data):
        assert len(b)>=27
        header=struct.unpack_from('<H',b)[0]
        status=struct.unpack_from('<I',b,10)[0]
        count=struct.unpack_from('<I',b,23)[0]
        assert header>=27 and len(b)==header+count
        payload=b[header:]
        if b[22]==2 and b[16]==0 and len(payload)==8 and payload[:2]==b'\x40\x0b':
            value,channel=struct.unpack_from('<HH',payload,2); modes[channel]=value>>8
        if b[22]==2 and b[16]==1 and status==0:
            if len(payload)==18 and payload[:2]==b'\x12\x01': identity=payload
            if len(payload)==55 and payload[:2]==b'\x09\x02': config=payload
        if b[22]==3 and b[21]==0x81 and b[16]==1 and status==0 and count:
            assert count>=2 and modes.get(1)==0x40, 'selected stream must be channel A synchronous FIFO'
            records.append(payload); frames.append(frame)
    # These values come from the capture's own device/configuration responses.
    assert identity and struct.unpack_from('<HHH',identity,8)==(0x1d50,0x607c,0x0700)
    assert config and config[18:25]==bytes([7,5,0x81,2,0,2,0])
    packet=struct.unpack_from('<H',config,22)[0]
    # Independent wire-format decoder: discard each packet's modem/line status.
    expected=b''.join(raw[start+2:min(start+packet,len(raw))]
                      for raw in records for start in range(0,len(raw),packet))
    return records,frames,packet,expected

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,default=Path.home()/'.cache/sunstoneos/sources/usb-serial-traffic/FT2232H-OpenVizsla-FPGA-download-and-capture-start.pcapng.gz')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args(); out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    source=json.loads((ROOT/'docs/usb-serial-traffic-sources.json').read_text())
    compressed=a.capture.read_bytes(); digest=hashlib.sha256(compressed).hexdigest()
    assert digest==source['capture']['sha256'], 'capture hash differs from pinned public bytes'
    data=gzip.decompress(compressed)
    assert hashlib.sha256(data).hexdigest()==source['capture']['decompressed_sha256']
    records,frames,packet,expected=decode(data)
    actual_facts={'records':len(records),'raw_bytes':sum(map(len,records)),
                  'payload_bytes':len(expected),'payload_sha256':hashlib.sha256(expected).hexdigest(),
                  'status_only_records':sum(len(r)==2 for r in records),
                  'full_packet_records':sum(len(r)==packet for r in records),
                  'first_frame':frames[0],'last_frame':frames[-1]}
    assert actual_facts==source['expected'], (actual_facts,source['expected'])
    fixture=out/'captured-in.bin'
    fixture.write_bytes(b'SSUSBCP1'+struct.pack('<II',packet,len(records))+
                        b''.join(struct.pack('<I',len(raw))+raw for raw in records))
    commands=[]
    def run(command):
        command=list(map(str,command)); r=subprocess.run(command,capture_output=True,text=True,env=environment())
        commands.append({'command':command,'exit':r.returncode,'output':r.stdout+r.stderr})
        (out/'steps.json').write_text(json.dumps(commands,indent=2)+'\n')
        if r.returncode: raise SystemExit(r.stdout+r.stderr)
    run([CLANG,'-std=gnu11','-Wall','-Wextra','-Werror','-g','-O1',*SANITIZE,
         '-I'+str(ROOT/'include'),ROOT/'drivers/usb/host/serial.c',ROOT/'checks/usb-serial-capture.c','-o',out/'replay'])
    run([out/'replay',fixture,out/'delivered.bin'])
    delivered=(out/'delivered.bin').read_bytes()
    assert delivered==expected, 'native application views differ from independent capture decoder'
    result={'result':'PASS','hardware_access':False,'capture_sha256':digest,**actual_facts,
            'native_inputs':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in
                             [ROOT/'drivers/usb/host/serial.c',ROOT/'include/sunstone_usb_serial.h',Path(__file__),ROOT/'checks/usb-serial-capture.c']},
            'scope':'Captured FT2232H USB packet status stripping and receive ownership. The capture uses FPGA/FIFO modes; UART operation, baud, no-loss timing and physical A733 DMA remain unverified.'}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n'); print(json.dumps(result,indent=2))
if __name__=='__main__': main()
