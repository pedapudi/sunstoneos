#!/usr/bin/env python3
"""Exercise host streaming with an explicit transport model and optional native reply."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('hdmi_stream_host',ROOT/'apps/hdmi-audio/capture.py')
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
prep=host.preparation


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('session','pcm','output'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--response',type=Path)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    session=args.session.read_bytes();pcm=args.pcm.read_bytes();prep.session_check(session,pcm)
    assert len(session)==848
    frames=len(pcm)//4;count=(frames+1023)//1024;checks=[]
    def refuses(name,call):
        try:call()
        except (ValueError,OSError):checks.append(name)
        else:raise AssertionError(name)
    manifest=prep.manifest_bytes(pcm)
    assert struct.unpack_from('<QIIQII',manifest)==(0x4153373348534d31,1,len(manifest),frames,4,1024)
    assert manifest[32:64]==hashlib.sha256(pcm).digest()
    for i in range(count):assert manifest[64+i*32:96+i*32]==hashlib.sha256(pcm[i*4096:(i+1)*4096]).digest()
    checks.append('manifest-full-and-short-period-hashes')
    for offset in (440,816,847):
        changed=bytearray(session);changed[offset]^=1
        refuses('session-binding-'+str(offset),lambda:prep.session_check(changed,pcm))
    for n in (0,576000*4+4,3):refuses('manifest-extent-'+str(n),lambda:prep.manifest_bytes(bytes(n)))
    for label,op,seq,payload in [('early-end',2,0,b''),('short-write',1,0,b'x'),
            ('over-end',1,count,b''),('status-payload',0,0,b'x'),('unknown-op',4,0,b'')]:
        refuses(label,lambda:host.request_bytes(1,session,op,seq,payload))
    refuses('request-limit',lambda:host.request_bytes(2*count+65,session))
    def response(request_id,seq,accepted,*,state=0,stage=None,credits=4,consumed=None):
        consumed=accepted if consumed is None else consumed
        raw=bytearray(512);flags=2 if stage is None else (3 if state==0 else 7)
        struct.pack_into('<4I3Q2I2Q',raw,0,0x52414748,2,512,request_id,
            *struct.unpack_from('<3Q',session,24),state,flags,1000,19999000)
        raw[64:96]=session[48:80];raw[96:128]=hashlib.sha256(session).digest()
        if stage is not None:
            hz=struct.unpack_from('<I',session,300)[0];tmds=hz*(99 if hz==24000000 else 91)//32
            struct.pack_into('<12Q',raw,128,0x4153373348415231,2,160,stage,frames,len(pcm),consumed*4,
                1 if stage==2 else 2,0 if stage==4 else 1,tmds,48000,0)
            raw[224:256]=hashlib.sha256(pcm).digest();raw[256:288]=hashlib.sha256(session).digest()
        struct.pack_into('<II4Q',raw,288,credits,0,accepted,accepted,consumed,seq)
        return bytes(raw)
    nominal=response(1,count,frames,state=1,stage=2)
    host.decode_observation(nominal,1,session,pcm,count)
    for label,offset,value,fmt in [('credits',288,5,'I'),('command',292,1,'I'),
            ('accepted',296,frames+1,'Q'),('committed',304,frames+1,'Q'),('consumed',312,frames+1,'Q'),
            ('sequence',320,count+1,'Q'),('reserved',328,1,'I'),('version',4,1,'I')]:
        altered=bytearray(nominal);struct.pack_into('<'+fmt,altered,offset,value)
        refuses('reply-'+label,lambda:host.decode_observation(altered,1,session,pcm,count))
    for state in (0,3):host.decode_observation(response(1,count,frames,state=state,stage=4,consumed=0),1,session,pcm,count)
    host.decode_observation(response(1,count,frames,state=2,stage=3,consumed=0),1,session,pcm,count)
    checks.append('fault-and-cancellation-partial-consumption')
    if args.response:
        actual=args.response.read_bytes();rid=struct.unpack_from('<I',actual,12)[0];seq=struct.unpack_from('<Q',actual,320)[0]
        decoded=host.decode_observation(actual,rid,session,pcm,seq)
        assert decoded['state']==1;checks.append('actual-native-terminal-response')
    def capture(name,mode='normal'):
        folder=args.output/name;folder.mkdir();record={};clock=[0.0];head=[0];ended=[False];packets=[]
        def exchange(packet,timeout):
            magic,version,rid,size,epoch,generation,op,n,seq=struct.unpack_from('<4I2Q2IQ',packet)
            assert (magic,version,size)==(0x4f414748,2,48) and (epoch,generation)==struct.unpack_from('<2Q',session,24)
            assert seq==head[0] and len(packet)==48+n and not ended[0]
            packets.append(packet)
            if op==1:
                assert packet[48:]==pcm[seq*4096:(seq+1)*4096];head[0]+=1
            elif op==2:assert head[0]==count;ended[0]=True
            else:assert op==0
            accepted=min(head[0]*1024,frames)
            raw=response(rid,seq,accepted,state=1 if ended[0] else 0,stage=2 if ended[0] else None)
            if mode=='stale':raw=bytearray(raw);raw[96]^=1;raw=bytes(raw)
            if mode=='late':clock[0]=1.1
            if mode=='early-eof':raw=response(rid,seq,frames,state=1,stage=2)
            return raw
        def save():(folder/'record.json').write_text(json.dumps(record,indent=2)+'\n')
        def run():return host.capture_copied(exchange,session,pcm,folder,record,save,19,
            now=lambda:clock[0],pause=lambda delay:None)
        if mode=='normal':
            assert run()['state']==1 and ended[0] and len(packets)==count+1
            assert record['hdmi_audio_result']=='observed-success'
        elif mode=='archive-failure':
            with patch.object(host,'durable',side_effect=OSError('injected archive failure')):refuses(name,run)
        else:
            refuses(name,run);assert (folder/'hdmi-audio-001.bin').is_file()
        if mode!='normal':assert record['hdmi_audio_result']=='unobserved' and 'durable_result' not in record
        checks.append(name)
    for mode in ('normal','stale','late','early-eof','archive-failure'):capture(mode,mode)
    # Device adapter must fragment the request and request a terminal ZLP.
    folder=args.output/'fragmented-device';folder.mkdir();record={};buffer=bytearray();head=[0];writes=[]
    class Device:
        def write(self,endpoint,data,timeout):
            assert endpoint==2 and 0<len(data)<=512 and timeout<=1000
            writes.append(len(data));buffer.extend(data);return len(data)
        def read(self,endpoint,size,timeout):
            assert endpoint==0x81 and size==513 and timeout<=1000
            _,_,rid,_,_,_,op,n,seq=struct.unpack_from('<4I2Q2IQ',buffer)
            assert len(buffer)==48+n and seq==head[0]
            if op==1:assert buffer[48:]==pcm[seq*4096:(seq+1)*4096];head[0]+=1
            else:assert op==2 and head[0]==count
            buffer.clear()
            return response(rid,seq,min(head[0]*1024,frames),state=1 if op==2 else 0,stage=2 if op==2 else None)
    host.capture_device(Device(),lambda:None,19,session,pcm,folder,record,lambda:None,now=lambda:0,pause=lambda _:None)
    assert max(writes)==512 and record['hdmi_audio_result']=='observed-success';checks.append('fragmented-device-and-terminal-read')
    summary=dict(passed=True,modeled_transport=True,physical_access=False,checks=checks,
        session_sha256=prep.sha(session),pcm_sha256=prep.sha(pcm),
        sources={str(p.relative_to(ROOT)):prep.sha(p.read_bytes()) for p in
            (Path(__file__),ROOT/'apps/hdmi-audio/prepare.py',ROOT/'apps/hdmi-audio/capture.py')})
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))


if __name__=='__main__':main()
