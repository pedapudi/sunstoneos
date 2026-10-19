#!/usr/bin/env python3
"""Prepare and capture the bounded VGA H264 profile with authenticated frame inputs."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import time
ROOT=Path(__file__).resolve().parents[2]
WIDTH,HEIGHT,FRAME_BYTES,INPUT_LIMIT,FRAME_LIMIT=640,480,460800,524288,256
M,F,Q,A=0x4d363248,0x46363248,0x51363248,0x41363248

def require(ok,message):
    if not ok:raise ValueError(message)

def header(magic,ordinal,session,aux=0):
    return struct.pack('<4I2Q',magic,1,ordinal,aux,*struct.unpack_from('<2Q',session,24))

def descriptors(manifest):
    require(0<len(manifest)<=256*96 and len(manifest)%96==0,'Invalid VGA manifest size.')
    result=[];previous=0
    for ordinal,offset in enumerate(range(0,len(manifest),96)):
        d=manifest[offset:offset+96];n,oracle,number,reserved=struct.unpack_from('<4I',d)
        require(0<n<=INPUT_LIMIT and oracle in (0,FRAME_BYTES) and number<=15 and
            (number==0 or (ordinal>0 and number==previous+1)) and not reserved and
            any(d[16:48]) and any(d[48:80])==bool(oracle) and not any(d[80:]),'Invalid VGA descriptor.')
        result.append(dict(ordinal=ordinal,input_bytes=n,oracle_bytes=oracle,frame_num=number,input_hash=d[16:48],oracle_hash=d[48:80]));previous=number
    return result

def manifest_upload(session,manifest):
    descriptors(manifest)
    require(struct.unpack_from('<Q',session,600)[0]==2 and not any(session[800:832]) and
        hashlib.sha256(manifest).digest()==session[768:800],'VGA manifest/session mismatch.')
    return header(M,len(manifest)//96,session,len(manifest))+manifest

def frame_upload(session,manifest,ordinal,data,oracle=None):
    rows=descriptors(manifest);require(0<=ordinal<len(rows),'VGA frame ordinal out of range.');d=rows[ordinal]
    require(len(data)==d['input_bytes'] and hashlib.sha256(data).digest()==d['input_hash'],'VGA input changed.')
    oracle=b'' if oracle is None else oracle
    require(len(oracle)==d['oracle_bytes'] and (not oracle or hashlib.sha256(oracle).digest()==d['oracle_hash']),'VGA oracle changed.')
    return header(F,ordinal,session)+data+oracle

def split_clip(data):
    import re
    starts=list(re.finditer(b'\x00\x00(?:\x00)?\x01',data));require(starts and starts[0].start()==0,'AnnexB input required.')
    config={};first=None;frames=[];number=0
    for i,m in enumerate(starts):
        nal=data[m.end():starts[i+1].start() if i+1<len(starts) else len(data)].rstrip(b'\0');require(nal,'Empty NAL.')
        kind=nal[0]&31
        if kind in (7,8):config[kind]=b'\0\0\0\1'+nal
        elif kind in (1,5):
            require(set(config)=={7,8},'SPS/PPS must precede slices.');prefix=config[7]+config[8]
            if first is None:first=prefix
            require(prefix==first,'VGA stream configuration changes are unsupported.')
            number=0 if kind==5 else number+1
            require((frames or kind==5) and number<=15,'IDR required at least every16pictures.')
            frames.append((number,prefix+b'\0\0\0\1'+nal))
        else:require(kind in (6,9),'Unsupported NAL in VGA input.')
    require(0<len(frames)<=FRAME_LIMIT,'VGA picture count exceeds bound.');return frames

def prepare_inputs(clip,output,reference=None):
    frames=split_clip(Path(clip).read_bytes());pixels=Path(reference).read_bytes() if reference else None
    require(pixels is None or len(pixels)==len(frames)*FRAME_BYTES,'VGA oracle length differs.')
    out=Path(output);out.mkdir(exist_ok=False);manifest=bytearray()
    for i,(number,data) in enumerate(frames):
        require(len(data)<=INPUT_LIMIT,'VGA access unit exceeds bound.');(out/f'frame-{i:03}.h264').write_bytes(data)
        oracle=pixels[i*FRAME_BYTES:(i+1)*FRAME_BYTES] if pixels else b''
        if oracle:(out/f'frame-{i:03}.yu12').write_bytes(oracle)
        manifest+=struct.pack('<4I',len(data),len(oracle),number,0)+hashlib.sha256(data).digest()+(hashlib.sha256(oracle).digest() if oracle else bytes(32))+bytes(16)
    (out/'manifest.bin').write_bytes(manifest)
    (out/'inputs.json').write_text(json.dumps(dict(profile='vga640x480-idr-p',pictures=len(frames),manifest_sha256=hashlib.sha256(manifest).hexdigest(),oracle=bool(pixels)),indent=2)+'\n')
    return bytes(manifest)

def validate_inputs(directory,manifest):
    root=Path(directory);frames=[]
    for d in descriptors(manifest):
        i=d['ordinal'];data=(root/f'frame-{i:03}.h264').read_bytes()
        oracle=(root/f'frame-{i:03}.yu12').read_bytes() if d['oracle_bytes'] else None
        # Epoch/generation are irrelevant to immutable file validation.
        dummy=bytearray(856);struct.pack_into('<Q',dummy,600,2);dummy[768:800]=hashlib.sha256(manifest).digest()
        frame_upload(dummy,manifest,i,data,oracle);frames.append((data,oracle))
    return frames

def decode(raw,session,manifest,ordinal,oracle=None):
    require(len(raw)>=160 and struct.unpack_from('<2I',raw)==(0x34363248,3),'Invalid VGA response.')
    status,n=struct.unpack_from('<2I',raw,8);require(status in (0,1,2,3) and n==(FRAME_BYTES if status==1 else 0) and len(raw)==160+n,'Invalid VGA payload length.')
    d=descriptors(manifest)[ordinal]
    require(raw[16:32]==session[24:40] and raw[32:64]==session[768:800] and raw[64:96]==d['input_hash'],'VGA response identity differs.')
    index,count,number,poc,width,height,kind,terminal=struct.unpack_from('<8I',raw,128)
    require((index,count,number,poc,width,height,kind)==(ordinal,len(manifest)//96,d['frame_num'],2*d['frame_num'],640,480,0 if d['frame_num'] else 2) and terminal<=2,'VGA picture metadata differs.')
    require((status==1 and hashlib.sha256(raw[160:]).digest()==raw[96:128]) or (status!=1 and not any(raw[96:128])),'VGA copied-output digest differs.')
    if status==1 and oracle is not None:require(raw[160:]==oracle,'VGA qualification pixel mismatch.')
    return dict(status=status,picture_index=index,picture_count=count,frame_num=number,poc=poc,width=width,height=height,terminal=terminal,oracle_verified=status==1 and oracle is not None,sha256=raw[96:128].hex() if status==1 else None)

def capture_device(device,same,deadline,session,evidence,record,save,manifest,frames,durable):
    require(len(frames)==len(descriptors(manifest)),'VGA capture requires every sealed frame input.')
    def timeout():
        same();remaining=int((deadline-time.monotonic())*1000);require(remaining>0,'VGA capture deadline expired.');return min(500,remaining)
    def write(packet):
        for offset in range(0,len(packet),512):
            part=packet[offset:offset+512];require(device.write(2,part,timeout=timeout())==len(part),'Short VGA request write.')
    def response(ordinal,oracle):
        write(header(Q,ordinal,session));raw=bytearray();needed=160;reads=0
        while len(raw)<needed:
            reads+=1;require(reads<=2*(160+FRAME_BYTES),'VGA response fragment budget exhausted.')
            part=bytes(device.read(0x81,min(4096,needed-len(raw)),timeout=timeout()));require(len(part)<=needed-len(raw),'Invalid VGA response fragment.')
            # TinyUSB can emit ZLPs after full endpoint packets, including the
            # previous response's final payload. Deadline/budget still apply.
            if not part:continue
            raw+=part
            if len(raw)==160:needed=160+struct.unpack_from('<I',raw,12)[0];require(needed in (160,160+FRAME_BYTES),'VGA response exceeds bound.')
        value=decode(bytes(raw),session,manifest,ordinal,oracle);return bytes(raw),value
    write(manifest_upload(session,manifest));record['h264_retention']='whole-board-power-removal';save()
    for ordinal,(data,oracle) in enumerate(frames):
        write(frame_upload(session,manifest,ordinal,data,oracle))
        for attempt in range(2401):
            raw,result=response(ordinal,oracle)
            if result['status']!=0:break
            time.sleep(min(.025,max(0,deadline-time.monotonic())))
        require(result['status'] in (1,2),'VGA pending or retired output cannot complete capture.')
        durable(evidence/f'h264-{ordinal:03}.bin',raw)
        if result['status']==1:
            name=f'output-{ordinal:03}.yu12';durable(evidence/name,raw[160:]);timeout()
            record.setdefault('copied_h264_pictures',[]).append(dict(path=name,bytes=FRAME_BYTES,**result));save()
            # The durable copied result is independent of the decoder's retained reference.
            write(header(A,ordinal,session)+raw[96:128])
        else:
            record['h264_result']='observed-failure';record['durable_result']=result;save();return result
    record['h264_result']='observed-success';record['durable_result']=result;save();return result

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--input',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--reference',type=Path)
    a=p.parse_args();prepare_inputs(a.input,a.output,a.reference)
