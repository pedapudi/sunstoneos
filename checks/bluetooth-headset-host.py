#!/usr/bin/env python3
"""Exercise only new headset profile/copy/capture boundaries with modeled frames."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
prep=load('headset_preparation',ROOT/'apps/bluetooth/prepare.py')
decoder=load('headset_decoder',ROOT/'apps/bluetooth/observe.py')
host=load('headset_host',ROOT/'tools/check-a733-bluetooth-supervised.py')
def refused(fn):
    try:fn()
    except (ValueError,SystemExit):return
    raise AssertionError('Invalid capture accepted')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    baseline=Path('/home/sunil/.cache/sunstoneos/evidence/bluetooth-native-20261017/composition')
    profile=bytearray((baseline/'captured-profile.bin').read_bytes());assert len(profile)==624
    profile.extend(bytes(128));struct.pack_into('<II',profile,8,2,752)
    struct.pack_into('<2I',profile,624,1,1);profile[632:638]=bytes(range(1,7));profile[655]=1
    struct.pack_into('<2IQ',profile,656,2,7,17);profile[672:704]=hashlib.sha256(b'modeled scheduler').digest()
    struct.pack_into('<2QI4H2IBBH',profile,704,1000,2000,600,128,0x1fa0,0x800,0x12,1000000,1000000,1,0,0)
    session=bytearray(592);struct.pack_into('<3Q',session,0,0x4153373342535332,2,592);struct.pack_into('<2Q',session,24,31,37)
    struct.pack_into('<Q',session,312,struct.unpack_from('<Q',profile,24)[0]);struct.pack_into('<I',session,400,2)
    session[320:352]=hashlib.sha256(profile).digest();prep.profile_check(profile,session)
    pcm=bytes(range(200))*6;records=struct.pack('<QIHHII',10,0,255,255,0,0)+struct.pack('<QIHHII',11,255,255,255,0,0)
    # Source packet extents remain <=255; odd boundaries cross sample pairs.
    records=b''.join(struct.pack('<QIHHII',10+i,offset,min(255,len(pcm)-offset),min(255,len(pcm)-offset),0,0) for i,offset in enumerate(range(0,len(pcm),255)))
    identity=hashlib.sha256(pcm).digest()
    def frame(kind,offset,count,state=1):
        data=(records if kind else pcm)[offset:offset+count] if state==1 else b''
        raw=bytearray(160+len(data));struct.pack_into('<4I2Q',raw,0,0x4d435442,1,state,len(data),31,37);raw[32:64]=session[320:352]
        if state==1:
            struct.pack_into('<4Q5I',raw,64,4,9,1,1,len(pcm),len(records)//24,0,kind,offset);raw[116:148]=identity;raw[160:]=data
        return raw
    good=frame(0,0,1024);assert decoder.decode_pcm(good,session,profile,0,0,1024)[1]==pcm[:1024]
    for position in (16,32,64,96,104,108,112,116,148):
        bad=bytearray(good);bad[position]^=1
        if position==64:bad[64]=0
        if position==116:continue # Digest is checked only after full copied capture.
        refused(lambda:decoder.decode_pcm(bad,session,profile,0,0,1024))
    pending=frame(0,0,1024,0);assert decoder.decode_pcm(pending,session,profile,0,0,1024)[0]['state']=='pending'
    pending[64]=1;refused(lambda:decoder.decode_pcm(pending,session,profile,0,0,1024))
    bad_profile=bytearray(profile);bad_profile[740]=3;bad_session=bytearray(session);bad_session[320:352]=hashlib.sha256(bad_profile).digest()
    refused(lambda:prep.profile_check(bad_profile,bad_session))
    calls=0
    def exchange(packet,timeout):
        nonlocal calls
        kind,reserved,count,offset=struct.unpack('<BBHI',packet);assert not reserved and timeout>0;calls+=1
        return frame(kind-1,offset,count,0 if calls==1 else 1)
    record={};host.capture(exchange,session,profile,a.output,record,lambda:None,10,now=lambda:1,pause=lambda _:None)
    assert record['bluetooth_result']=='observed-success' and (a.output/'bluetooth-8k-s16le.pcm').read_bytes()==pcm and calls==4
    # A bad full hash must remain unobserved even when individual chunks parse.
    def wrong_hash(packet,timeout):
        kind,_,count,offset=struct.unpack('<BBHI',packet);raw=frame(kind-1,offset,count);raw[116]^=1;return raw
    bad_record={}
    failed=a.output/'bad-hash';failed.mkdir(exist_ok=True)
    refused(lambda:host.capture(wrong_hash,session,profile,failed,bad_record,lambda:None,10,now=lambda:1,pause=lambda _:None))
    assert bad_record['bluetooth_result']=='unobserved'
    def changing_identity(packet,timeout):
        kind,_,count,offset=struct.unpack('<BBHI',packet);raw=frame(kind-1,offset,count)
        if offset:raw[64]=5
        return raw
    changed={}
    changed_path=a.output/'changed-generation';changed_path.mkdir(exist_ok=True)
    refused(lambda:host.capture(changing_identity,session,profile,changed_path,changed,lambda:None,10,now=lambda:1,pause=lambda _:None))
    assert changed['bluetooth_result']=='unobserved'
    clock_calls=0
    def expires_after_copy():
        nonlocal clock_calls
        clock_calls+=1
        return 10 if clock_calls>=7 else 1
    late={}
    late_path=a.output/'late-finalization';late_path.mkdir(exist_ok=True)
    refused(lambda:host.capture(lambda packet,timeout:frame(struct.unpack('<BBHI',packet)[0]-1,struct.unpack('<BBHI',packet)[3],struct.unpack('<BBHI',packet)[2]),session,profile,late_path,late,lambda:None,10,now=expires_after_copy,pause=lambda _:None))
    assert late['bluetooth_result']=='unobserved'
    (a.output/'host-result.json').write_text(json.dumps({'physical_access':False,'cases':['profile-discovery-metadata','copied-frame-identities','pending-private','bounded-copy-capture','full-pcm-hash'],'pcm_sha256':identity.hex()},indent=2)+'\n')
    print('PASS new host headset profile, copied chunk identity, pending privacy, capture and digest cases')
if __name__=='__main__':main()
