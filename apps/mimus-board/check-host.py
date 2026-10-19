#!/usr/bin/env python3
"""Exercise only the application's fixed host framing and input preservation."""
import argparse
import importlib.util
import hashlib
import json
from pathlib import Path
import struct
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
spec=importlib.util.spec_from_file_location('mimus_board_host',Path(__file__).with_name('host.py'));host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
text=b'fixed transcript\n'*80;sector=bytes(range(256))*2;counts={};calls=0
class Device:
    def __init__(self):self.response=bytearray();self.failed=False
    def write(self,ep,data,timeout):
        global calls
        assert ep==2 and timeout>0;calls+=1
        op,zero,count,offset,reserved=struct.unpack('<BBHIQ',data);assert not zero and not reserved
        key=(op,offset,count);counts[key]=counts.get(key,0)+1
        state=0 if counts[key]==1 else 1
        body=(text[offset:offset+count] if op==1 else sector if op==2 else b'') if state==1 else b''
        self.response=bytearray(struct.pack('<8I2Q16x',0x424d494d,1,state,len(body),len(text),offset,1,op,19,23)+body)
        return len(data)
    def read(self,ep,size,timeout):
        assert ep==0x81 and timeout>0
        count=min(29,size,len(self.response));part=self.response[:count];del self.response[:count];return part
d=Device();frames=[];exchange=host.device_exchange(d,lambda:None,100,frames.append,now=lambda:1)
result=host.capture(exchange,19,23,100,now=lambda:1,pause=lambda n:None)
assert result['transcript']==text and result['sector']==sector and result['frames']==frames
assert all(n>=2 for n in counts.values())
failed_calls=0
def failed(packet):
    global failed_calls
    failed_calls+=1;raise OSError('Modeled transport loss')
try:host.capture(failed,19,23,100,now=lambda:1)
except OSError:pass
else:raise AssertionError('Transport error retried/hidden')
assert failed_calls==1
for name,frame in [('fault',struct.pack('<8I2Q16x',0x424d494d,1,2,0,0,0,2,0,19,23)),('identity',struct.pack('<8I2Q16x',0x424d494d,1,1,0,len(text),0,1,0,20,23))]:
    try:host.capture(lambda packet:frame,19,23,100,now=lambda:1)
    except ValueError:pass
    else:raise AssertionError(name)
paths=[Path('/home/sunil/.cache/sunstoneos/inputs/mimus-bench')/n for n in ('base.mimus','overlay.mimus')]+[Path('/home/sunil/data/asr/device/replay_devother/4515-11057-0027.wav')]
fixed=host.freeze_assets(paths,a.output/'assets');first=a.output/'assets/asset-0.bin';data=first.read_bytes();first.write_bytes(bytes([data[0]^1])+data[1:])
try:host.validate_assets(a.output/'assets')
except ValueError:pass
else:raise AssertionError('Frozen substitution admitted')
first.write_bytes(data);host.validate_assets(a.output/'assets')
record={'fragmented_replies':True,'pending_each_request_kind':True,'transport_no_retry':True,'fault_refused':True,'identity_refused':True,'fixed_asset_substitution_refused':True,'frames':len(frames),'physical_access':False}
(a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
