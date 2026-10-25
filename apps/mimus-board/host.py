#!/usr/bin/env python3
"""Freeze exact recorded inputs and collect the fixed combined USB0 result."""
import argparse
import hashlib
import json
import importlib.util
from pathlib import Path
import struct
import time
ROOT=Path(__file__).resolve().parents[2]
ADDRESSES=(0x44000000,0x44800000,0x45000000)
def require(ok,message):
    if not ok:raise ValueError(message)
def declared():
    return json.loads((ROOT/'apps/bluetooth/recognition-models.json').read_text())['models']+[
        json.loads((ROOT/'apps/mimus-board/inputs.json').read_text())['recorded_wav']]
def freeze_assets(paths,output):
    require(len(paths)==3,'Three exact inputs required.')
    output=Path(output);output.mkdir(parents=True,exist_ok=False);items=[]
    for index,(path,expected,address) in enumerate(zip(paths,declared(),ADDRESSES)):
        data=Path(path).read_bytes();identity=hashlib.sha256(data).hexdigest()
        require(len(data)==expected['bytes'] and identity==expected['sha256'],'Input substitution refused.')
        name=f'asset-{index}.bin';(output/name).write_bytes(data)
        items.append({'name':name,'address':address,'bytes':len(data),'sha256':identity})
    manifest={'schema':'mimus-board-fixed-assets-v1','assets':items,'physical_access':False}
    (output/'assets.json').write_text(json.dumps(manifest,indent=2)+'\n');return validate_assets(output)
def validate_assets(output):
    output=Path(output);value=json.loads((output/'assets.json').read_text())
    require(value.get('schema')=='mimus-board-fixed-assets-v1' and value.get('physical_access') is False,'Asset schema differs.')
    require(len(value['assets'])==3,'Input count differs.')
    for index,(item,expected,address) in enumerate(zip(value['assets'],declared(),ADDRESSES)):
        require(item=={'name':f'asset-{index}.bin','address':address,'bytes':expected['bytes'],'sha256':expected['sha256']},'Asset declaration differs.')
        data=(output/item['name']).read_bytes()
        require(len(data)==item['bytes'] and hashlib.sha256(data).hexdigest()==item['sha256'],'Frozen input changed.')
    return value

def capture(exchange,epoch,generation,deadline,now=time.monotonic,pause=time.sleep,*,keyboard=False,recognition_generation=1,staged=False):
    """Exchange must preserve USB identity and never retry failed transport.

    It writes each complete request and assembles exactly one complete response.
    It retains the raw frames durably before returning them to this function.
    A final status query follows sector and transcript collection. The result is
    an observation within the session, not physical qualification of later time.
    """
    require(not staged or not keyboard and recognition_generation==1,
            'Staged capture permits one recorded recognition only.')
    frames=[]
    def query(op,offset=0,count=0):
        require(now()<deadline,'Capture expired.')
        packet=struct.pack('<BBHIQ',op,0,count,offset,recognition_generation if keyboard and op else 0)
        frame=exchange(packet);frames.append(frame)
        require(now()<deadline and len(frame)>=64,'Incomplete or expired response.')
        magic,version,state,size,total,position,storage,reply=struct.unpack_from('<8I',frame)
        require((magic,version,reply,position)==(0x424d494d,3 if keyboard else 1,op,offset),'Response identity differs.')
        require(struct.unpack_from('<QQ',frame,32)==(epoch,generation),'Session identity differs.')
        require(state in (0,1,2) and size<=1024 and len(frame)==64+size,'Invalid result extent.')
        require(storage == 3 if staged else storage in (0,1),'Storage mode failed or differs.')
        if keyboard:
            observed,limit,reserved=struct.unpack_from('<QII',frame,48)
            require(limit==0xffffffff and not reserved and 1<=recognition_generation<=limit,'Recognition bounds differ.')
            if op==0 and observed==recognition_generation-1:
                require(size==0 and state==0,'Previous generation payload refused.')
                return 0,0,b''
            require(observed==recognition_generation,'Recognition generation differs.')
        else:require(not any(frame[48:64]),'Reserved response bytes differ.')
        require(state!=2,'Native combined workload refused result.')
        require(state!=1 or storage==(3 if staged else 1),'Storage incomplete or mode differs.')
        require(state==1 or size==0,'Pending payload refused.')
        return state,total,frame[64:]
    budget=240
    def ready(op,offset=0,count=0):
        nonlocal budget
        while budget:
            budget-=1
            response=query(op,offset,count)
            if response[0]==1:return response
            pause(min(.25,max(0,deadline-now())))
        raise ValueError('Pending query budget exhausted.')
    state,total,_=ready(0)
    require(0<total<=16384,'Invalid transcript length.')
    text=bytearray()
    while len(text)<total:
        count=min(1024,total-len(text));state,current,data=ready(1,len(text),count)
        require(state==1 and current==total and len(data)==count,'Transcript changed or incomplete.')
        text.extend(data)
    sector=None
    if not staged:
        state,current,sector=ready(2,0,512)
        require(state==1 and current==total and len(sector)==512,'Copied sector incomplete.')
    trigger=None
    if keyboard:
        state,current,data=ready(3,0,128)
        require(state==1 and current==total and len(data)==128,'Trigger evidence incomplete.')
        trigger=decode_trigger(data)
        require(trigger['attachment_generation']==generation,'Trigger attachment generation differs.')
    state,current,data=ready(0)
    require(state==1 and current==total and not data,'Final live status refused.')
    if keyboard:
        state,current,data=ready(4)
        require(state==1 and current==total and not data,'Generation acknowledgment refused.')
    return {'transcript':bytes(text),'sector':sector,'trigger':trigger,'frames':frames,'recognition_generation':recognition_generation if keyboard else None}
def capture_bundle(exchange,bundle,deadline,now=time.monotonic,pause=time.sleep,*,recognition_generation=1):
    """Select capture policy from the validated frozen session, before transport."""
    spec=importlib.util.spec_from_file_location('mimus_capture_prepare',ROOT/'apps/mimus-board/prepare.py')
    preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
    identity=preparation.validate_bundle(bundle,capture_identity=True)
    return capture(exchange,identity['epoch'],identity['generation'],deadline,now,pause,keyboard=identity['workload']==4,recognition_generation=recognition_generation)

def decode_trigger(data):
    require(len(data)==128,'Trigger extent differs.')
    require(struct.unpack_from('<4I',data)==(0x4752544b,1,128,2),'Trigger header differs.')
    attachment=struct.unpack_from('<Q',data,16)[0]
    address,speed,interface,protocol,endpoint,subclass,mode,reserved=struct.unpack_from('<8B',data,24)
    require(attachment and 1<=address<=127 and 1<=speed<=3 and not interface and protocol==1 and
        endpoint&0x80 and endpoint&15 and not endpoint&0x70 and subclass==1 and not mode and not reserved,'Keyboard authority differs.')
    baseline=struct.unpack_from('<Q',data,32)[0];pressed=struct.unpack_from('<Q',data,80)[0]
    require(baseline and pressed==baseline+1 and not any(data[40:80]) and
        data[88:96]==bytes([0,0,0x28,0,0,0,0,0]) and not any(data[96:]),'Neutral to Enter evidence differs.')
    return {'attachment_generation':attachment,'address':address,'speed':speed,'interface':interface,
        'protocol':protocol,'endpoint':endpoint,'baseline_sequence':baseline,'enter_sequence':pressed,'bytes':data}

def device_exchange(device,same,deadline,retain,now=time.monotonic):
    """Use an already claimed device; same() enforces the coordinator identity.

    retain(frame) must save each complete raw frame durably. Errors propagate;
    successful partial reads only assemble the current bounded response.
    """
    def exchange(packet):
        def timeout():
            remaining=deadline-now();require(remaining>0,'Capture expired.')
            return min(500,max(1,int(remaining*1000)))
        same();require(device.write(0x02,packet,timeout=timeout())==len(packet),'Incomplete request.')
        frame=bytearray();expected=None
        while expected is None or len(frame)<expected:
            same();part=bytes(device.read(0x81,1088-len(frame),timeout=timeout()))
            require(part,'Empty response fragment.');frame.extend(part)
            require(len(frame)<=1088,'Response too large.')
            if len(frame)>=64:
                count=struct.unpack_from('<I',frame,12)[0];require(count<=1024,'Response extent differs.')
                expected=64+count;require(len(frame)<=expected,'Coalesced response refused.')
        same();require(now()<deadline,'Response expired.');retain(bytes(frame));return bytes(frame)
    return exchange

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('base','overlay','wav','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();freeze_assets([a.base,a.overlay,a.wav],a.output)
