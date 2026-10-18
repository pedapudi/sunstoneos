#!/usr/bin/env python3
"""New finite recognition admission and copied-output boundaries; no hardware access."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
CACHE=Path('/home/sunil/.cache/sunstoneos')
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def refused(fn):
    try:fn()
    except (ValueError,SystemExit):return
    raise AssertionError('Invalid recognition boundary accepted')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--composition',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    prep=load('finite_headset_prepare',ROOT/'apps/bluetooth/prepare.py');decoder=load('finite_headset_decode',ROOT/'apps/bluetooth/observe.py');host=load('finite_headset_host',ROOT/'tools/check-a733-bluetooth-supervised.py')
    profile=bytearray((CACHE/'evidence/bluetooth-native-20261017/composition/captured-profile.bin').read_bytes())+bytearray(208)
    struct.pack_into('<II',profile,8,3,832);struct.pack_into('<2I',profile,624,1,1);profile[632:638]=bytes(range(1,7));profile[655]=1
    struct.pack_into('<2IQ',profile,656,2,7,17)
    struct.pack_into('<2QI4H2IBBH',profile,704,1000,2000,24000,128,0x1fa0,0x800,0x12,1000000,5000000,1,0,0)
    for i,pin in enumerate(prep.MODEL_PIN['models']):struct.pack_into('<Q',profile,752+8*i,pin['bytes']);profile[768+32*i:800+32*i]=bytes.fromhex(pin['sha256'])
    cache={};record={'identity':'software-fixture','port':'not-a-device','payloads':[{}]*4,'radio_ddr':{}}
    def evidence(label,data):
        path=str(a.output.resolve()/label);cache[path]=data
        return {'path':path,'sha256':hashlib.sha256(data).hexdigest()}
    for key,offset in [('bluetooth_wiring',56),('bluetooth_sid',88),('bluetooth_transport',120),('bluetooth_scheduling',672)]:
        ref=evidence(key,b'explicit software-only fixture '+key.encode());record[key]=ref;profile[offset:offset+32]=bytes.fromhex(ref['sha256'])
    proof=evidence('modeled-ddr-source',b'Only schema behavior is modeled; no physical reservation or board is claimed.')
    ddr={'schema':'a733-ddr-reservation-v1','identity':record['identity'],'port':record['port'],'image_sha256':'fixture-image',
         'exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,
         'ranges':[[0x44000000,0x45000000],[0x46000000,0x47000000]],'evidence':[proof]}
    record['bluetooth_mimus_ddr']=evidence('modeled-ddr',json.dumps(ddr).encode())
    record['bluetooth_models']=[]
    for i,pin in enumerate(prep.MODEL_PIN['models']):
        path=CACHE/'inputs/mimus-bench'/pin['name'];ref={'path':str(path),'sha256':pin['sha256'],'address':0x44000000+i*0x800000};cache[str(path)]=path.read_bytes();record['bluetooth_models'].append(ref)
    session=bytearray(592);struct.pack_into('<3Q',session,0,0x4153373342535332,2,592);struct.pack_into('<2Q',session,24,31,37)
    struct.pack_into('<Q',session,312,struct.unpack_from('<Q',profile,24)[0]);struct.pack_into('<I',session,400,2);session[320:352]=hashlib.sha256(profile).digest()
    manifest={'target':'sunstone-a733-bluetooth','execution_profile':'bluetooth-supervised-v1','final_elf':'fixture-image','headset_provider':{'commit':'64e98eb5278d790f3b3e5279ca8b39b45097bbc0'},'recognition_provider':prep.MODEL_PIN}
    prep.bluetooth_admission(record,profile,session,manifest,'fixture-image',cache,True)
    refused(lambda:prep.bluetooth_admission({**record,'bluetooth_mimus_ddr':None},profile,session,manifest,'fixture-image',cache,True))
    bad_models=[{**record['bluetooth_models'][0],'address':0x43000000},record['bluetooth_models'][1]]
    refused(lambda:prep.bluetooth_admission({**record,'bluetooth_models':bad_models},profile,session,manifest,'fixture-image',cache,True))
    refused(lambda:prep.bluetooth_admission(record,profile,session,{**manifest,'recognition_provider':None},'fixture-image',cache,True))
    changed=dict(cache);path=record['bluetooth_models'][0]['path'];changed[path]=bytes([cache[path][0]^1])+cache[path][1:]
    refused(lambda:prep.bluetooth_admission(record,profile,session,manifest,'fixture-image',changed,True))
    text=(a.composition/'success-transcription.log').read_bytes();snapshot=(a.composition/'success-transcription.log.snapshot.bin').read_bytes();assert len(snapshot)==144
    capture=dict(zip(('firmware_generation','uart_generation','acl_generation','audio_generation'),struct.unpack_from('<4Q',snapshot)))
    capture['sha256']=snapshot[48:80].hex()
    def frame(offset,count,pending=False):
        raw=bytearray(208+(0 if pending else count));struct.pack_into('<4I2Q',raw,0,0x52544242,1,0 if pending else 1,0 if pending else count,31,37);raw[32:64]=session[320:352]
        if not pending:raw[64:208]=snapshot;raw[208:]=text[offset:offset+count]
        return raw
    pending=frame(0,1,True);assert decoder.decode_recognition(pending,session,profile,0,1)[0]['state']=='pending'
    pending[64]=1;refused(lambda:decoder.decode_recognition(pending,session,profile,0,1))
    raw=frame(0,1);assert decoder.decode_recognition(raw,session,profile,0,1)[1]==text[:1]
    for position in (16,32,64,96,100,104,108):
        bad=bytearray(raw);bad[position]^=1
        if position==64:bad[64]=0
        if position==96:bad[96:100]=bytes(4)
        refused(lambda:decoder.decode_recognition(bad,session,profile,0,1))
    calls=0
    def exchange(packet,timeout):
        nonlocal calls
        kind,reserved,count,offset=struct.unpack('<BBHI',packet);assert kind==3 and not reserved and timeout>0;calls+=1
        return frame(offset,count,calls==1)
    record={};good=a.output/'copied-transcript';good.mkdir(exist_ok=True)
    result=host.capture_recognition(exchange,session,profile,good,record,lambda:None,10,capture,now=lambda:1,pause=lambda _:None)
    assert result['state']=='recognized' and record['bluetooth_result']=='observed-success' and (good/'bluetooth-transcription.log').read_bytes()==text
    def bad_digest(packet,timeout):
        _,_,count,offset=struct.unpack('<BBHI',packet);raw=frame(offset,count);raw[176]^=1;return raw
    bad_record={};failed=a.output/'bad-digest';failed.mkdir(exist_ok=True)
    refused(lambda:host.capture_recognition(bad_digest,session,profile,failed,bad_record,lambda:None,10,capture,now=lambda:1,pause=lambda _:None));assert bad_record['bluetooth_result']=='unobserved'
    (a.output/'result.json').write_text(json.dumps({'physical_access':False,'passed':True,'profile_sha256':hashlib.sha256(profile).hexdigest(),'transcript_sha256':hashlib.sha256(text).hexdigest(),'scope':'Model/range/schema refusal and copied result publication; native USB framing is modeled.'},indent=2)+'\n')
    print('PASS pinned model/range/hash admission, copied transcript identity/privacy/digest and durable publication boundaries')
if __name__=='__main__':main()
