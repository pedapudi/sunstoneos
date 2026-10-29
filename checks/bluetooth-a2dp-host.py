#!/usr/bin/env python3
"""Validate actual native-produced PCM result frames through durable host capture."""
import argparse
import hashlib
import importlib.util
import pathlib
import struct
from harness import refused
root=pathlib.Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
p=argparse.ArgumentParser();p.add_argument('--capture',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
prep=load('a2dp_prep',root/'apps/bluetooth/prepare.py');decoder=load('a2dp_decoder',root/'apps/bluetooth/observe.py');host=load('a2dp_host',root/'tools/check-a733-bluetooth-supervised.py')
profile=(a.capture/'profile.bin').read_bytes();session=(a.capture/'session.bin').read_bytes();prep.profile_check(profile,session)
a.output.mkdir(parents=True,exist_ok=False)
def exchange(request,timeout):
    kind,zero,count,offset=struct.unpack('<BBHI',request);assert zero==0 and timeout>0
    raw=(a.capture/f'frame-{kind-1}-{offset}.bin').read_bytes();assert len(raw)==160+count;return raw
record={};result=host.capture_pcm(exchange,session,profile,a.output,record,lambda:None,10,now=lambda:1)
assert result['state']=='captured' and record['bluetooth_result']=='observed-success'
name=f'bluetooth-{result["sample_rate"]}hz-{result["channels"]}ch-s16le.pcm'
assert hashlib.sha256((a.output/name).read_bytes()).hexdigest()==result['sha256']
raw=bytearray((a.capture/'frame-0-0.bin').read_bytes());struct.pack_into('<I',raw,148,8000)
refused(lambda:decoder.decode_pcm(raw,session,profile,0,0,len(raw)-160),'Invalid A2DP PCM geometry')
# Pending responses cannot leak the extra geometry fields.
pending=bytearray(160);pending[:64]=raw[:64];struct.pack_into('<2I',pending,8,0,0);struct.pack_into('<I',pending,148,48000)
refused(lambda:decoder.decode_pcm(pending,session,profile,0,0,1),'Incomplete capture published producer metadata')
# Preserve the actual PCM frames but corrupt one native record's geometry.
def corrupt_record(request,timeout):
    raw=bytearray(exchange(request,timeout))
    if request[0]==2 and struct.unpack_from('<I',request,4)[0]==0:struct.pack_into('<I',raw,180,0)
    return raw
bad=a.output/'bad-record';bad.mkdir()
refused(lambda:host.capture_pcm(corrupt_record,session,profile,bad,{},lambda:None,10,now=lambda:1),'Capture record ordering')
print('PASS actual native USB frames, mode6 profile, PCM geometry, durable digest/output and damaged metadata refusal')
