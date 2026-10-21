"""Bounded High/CABAC producer and copied presentation-order capture."""
import hashlib,json,struct,subprocess,time
from pathlib import Path
M,F,Q,A=0x4d363248,0x46363248,0x51363248,0x41363248

def require(ok,message):
    if not ok:raise ValueError(message)
def word(raw,offset):return struct.unpack_from('<I',raw,offset)[0]
def header(magic,ordinal,session,aux=0):return struct.pack('<4I2Q',magic,2,ordinal,aux,*struct.unpack_from('<2Q',session,24))
def profile(manifest):
    require(len(manifest)>=128 and struct.unpack_from('<4I',manifest)==(0x34484d48,1,128,128),'Wrong High manifest.')
    values=struct.unpack_from('<18I',manifest);_,_,_,_,count,mode,w,h,vw,vh,left,top,right,bottom,refs,reorder,dpb,zero=values
    require(0<count<=256 and len(manifest)==128+128*count and mode in (0,1) and 0<w<=1920 and 0<h<=1088 and not (w|h)&15 and 0<vw<=1920 and 0<vh<=1080 and not (vw|vh)&1 and left+vw+right==w and top+vh+bottom==h and not (left|top|right|bottom)&1 and 0<refs<=4 and 0<=reorder<=2 and refs<=dpb<=4 and reorder<=dpb and not zero and any(manifest[72:104]) and not any(manifest[104:128]),'Invalid High geometry/bounds.')
    return dict(count=count,oracle=bool(mode),coded_width=w,coded_height=h,width=vw,height=vh,crop=(left,top,right,bottom),refs=refs,reorder=reorder,dpb=dpb,frame_bytes=vw*vh*3//2,header_bytes=256,response_version=5,input_limit=2097152,name='high-cabac-runtime')
def descriptors(manifest):
    p=profile(manifest);result=[];display=set()
    for i in range(p['count']):
        d=manifest[128+i*128:256+i*128];n,oracle,number,poc=struct.unpack_from('<4I',d);decode,present,gop,kind,ref,idr=struct.unpack_from('<6I',d,80)
        require(0<n<=2097152 and oracle==(p['frame_bytes'] if p['oracle'] else 0) and number<=65535 and any(d[16:48]) and bool(any(d[48:80]))==p['oracle'] and decode==i and present<p['count'] and present not in display and kind<=2 and ref<=3 and idr<=1 and not any(d[104:]),'Invalid High descriptor.')
        display.add(present);result.append(dict(ordinal=i,presentation=present,input_bytes=n,oracle_bytes=oracle,frame_num=number,poc=poc,gop=gop,kind=kind,reference=ref,idr=idr,input_hash=d[16:48],oracle_hash=d[48:80]))
    return result

def manifest_upload(session,manifest):
    p=profile(manifest);descriptors(manifest)
    require(word(session,600)==4 and not any(session[800:832]) and hashlib.sha256(manifest).digest()==session[768:800],'High manifest/session differs.')
    return header(M,p['count'],session,len(manifest))+manifest

def frame_upload(session,manifest,ordinal,data,oracle=None):
    rows=descriptors(manifest);require(0<=ordinal<len(rows),'High decode ordinal exceeds manifest.');d=rows[ordinal];oracle=b'' if oracle is None else oracle
    require(len(data)==d['input_bytes'] and hashlib.sha256(data).digest()==d['input_hash'] and len(oracle)==d['oracle_bytes'] and (not oracle or hashlib.sha256(oracle).digest()==d['oracle_hash']),'High upload differs from sealed descriptor.')
    return header(F,ordinal,session)+data+oracle

def validate_inputs(directory,manifest):
    root=Path(directory);dummy=bytearray(856);struct.pack_into('<Q',dummy,600,4);dummy[768:800]=hashlib.sha256(manifest).digest();frames=[]
    for d in descriptors(manifest):
        i=d['ordinal'];data=(root/f'frame-{i:03}.h264').read_bytes();oracle=(root/f'frame-{i:03}.yu12').read_bytes() if d['oracle_bytes'] else None
        frame_upload(dummy,manifest,i,data,oracle);frames.append((data,oracle))
    return frames

def prepare_inputs(clip,output,reference,helper):
    import re
    data=Path(clip).read_bytes();starts=list(re.finditer(b'\x00\x00(?:\x00)?\x01',data));require(starts and starts[0].start()==0,'AnnexB required.')
    config={};frames=[];out=Path(output);out.mkdir(exist_ok=False)
    for i,m in enumerate(starts):
        nal=data[m.end():starts[i+1].start() if i+1<len(starts) else len(data)].rstrip(b'\0');require(nal and not nal[0]&128,'Invalid High NAL.');kind=nal[0]&31
        if kind in (7,8):config[kind]=b'\0\0\0\1'+nal
        elif kind in (1,5):
            require(set(config)=={7,8} and len(frames)<256,'High configuration/count invalid.');au=config[7]+config[8]+b'\0\0\0\1'+nal;require(len(au)<=2097152,'High AU too large.')
            path=out/f'frame-{len(frames):03}.h264';path.write_bytes(au);frames.append((path,au))
        else:require(kind in (6,9),'Unsupported High NAL.')
    require(frames,'No High pictures.');command=[str(helper),*[str(p) for p,_ in frames]]
    proc=subprocess.run(command,capture_output=True,text=True);(out/'syntax-producer.json').write_text(json.dumps(dict(command=command,returncode=proc.returncode,stderr=proc.stderr),indent=2)+'\n');require(proc.returncode==0,'Bounded native syntax producer refused clip.')
    plans=json.loads(proc.stdout);require(len(plans)==len(frames),'High syntax count differs.');first=plans[0]
    ordered=sorted(range(len(plans)),key=lambda i:(plans[i]['gop'],plans[i]['poc']));positions={decode:present for present,decode in enumerate(ordered)}
    frame_bytes=first['visible_width']*first['visible_height']*3//2;pixels=Path(reference).read_bytes() if reference else None;require(pixels is None or len(pixels)==len(frames)*frame_bytes,'High visible oracle length differs.')
    cfghash=hashlib.sha256(frames[0][1][:first['config_bytes']]).digest()
    manifest=bytearray(struct.pack('<18I',0x34484d48,1,128,128,len(frames),int(pixels is not None),first['width'],first['height'],first['visible_width'],first['visible_height'],*first['crop'],first['refs'],first['reorder'],first['dpb'],0)+cfghash+bytes(24))
    for i,((path,au),plan) in enumerate(zip(frames,plans)):
        require(hashlib.sha256(au[:plan['config_bytes']]).digest()==cfghash and all(plan[k]==first[k] for k in ('width','height','visible_width','visible_height','crop','refs','reorder','dpb')),'High session configuration changes.')
        present=positions[i];oracle=pixels[present*frame_bytes:(present+1)*frame_bytes] if pixels is not None else b''
        if oracle:(out/f'frame-{i:03}.yu12').write_bytes(oracle)
        manifest+=struct.pack('<4I',len(au),len(oracle),plan['frame_num'],plan['poc']&0xffffffff)+hashlib.sha256(au).digest()+(hashlib.sha256(oracle).digest() if oracle else bytes(32))+struct.pack('<6I',i,present,plan['gop'],plan['kind'],plan['reference'],plan['idr'])+bytes(24)
    profile(manifest);descriptors(manifest);(out/'manifest.bin').write_bytes(manifest);(out/'syntax-plans.json').write_text(json.dumps(plans,indent=2)+'\n');return bytes(manifest)

def decode(raw,session,manifest,ordinal,oracle=None):
    p=profile(manifest);rows=descriptors(manifest);matches=[d for d in rows if d['presentation']==ordinal];require(len(matches)==1,'Unknown High presentation ordinal.');d=matches[0]
    require(len(raw)>=256 and struct.unpack_from('<2I',raw)==(0x34363248,5),'Wrong High response.')
    status,n=struct.unpack_from('<2I',raw,8);require(status in (0,1,2,3) and n==(p['frame_bytes'] if status==1 else 0) and len(raw)==256+n,'High response extent differs.')
    require(raw[16:32]==session[24:40] and raw[32:64]==session[768:800] and raw[64:96]==d['input_hash'],'High response identity differs.')
    require(struct.unpack_from('<7I',raw,128)==(ordinal,len(rows),d['frame_num'],d['poc'],p['width'],p['height'],d['kind']) and word(raw,156)<=2,'High visible metadata differs.')
    require(struct.unpack_from('<8I',raw,160)==(p['coded_width'],p['coded_height'],p['coded_width'],p['coded_width']//2,*p['crop']),'High coded/crop metadata differs.')
    require(word(raw,192)<=len(rows) and struct.unpack_from('<5I',raw,196)==(d['gop'],d['ordinal'],d['reference'],d['idr'],int(status==1 and p['oracle'])) and word(raw,216) in (0,1) and (not word(raw,216) or (status==1 and word(raw,156)==2)) and not any(raw[220:256]),'High ordinal/oracle metadata differs.')
    require((status==1 and hashlib.sha256(raw[256:]).digest()==raw[96:128]) or (status!=1 and not any(raw[96:128])),'High copied digest differs.')
    if status==1 and oracle is not None:require(raw[256:]==oracle,'High qualification pixels differ.')
    return dict(status=status,picture_index=ordinal,decode_ordinal=d['ordinal'],picture_count=len(rows),poc=struct.unpack('<i',struct.pack('<I',d['poc']))[0],width=p['width'],height=p['height'],terminal=word(raw,156),completed=word(raw,192),reorder_pending=bool(word(raw,216)),oracle_verified=status==1 and oracle is not None,sha256=raw[96:128].hex() if status==1 else None)

def capture_device(device,same,deadline,session,evidence,record,save,manifest,frames,durable):
    p=profile(manifest);rows=descriptors(manifest);require(len(frames)==len(rows),'High capture inputs missing.')
    def timeout():
        same();left=int((deadline-time.monotonic())*1000);require(left>0,'High capture deadline.');return min(500,left)
    def write(packet):
        for off in range(0,len(packet),512):part=packet[off:off+512];require(device.write(2,part,timeout=timeout())==len(part),'Short High write.')
    def response(present):
        write(header(Q,present,session));raw=bytearray();needed=256;reads=0
        while len(raw)<needed:
            reads+=1;require(reads<=2*(256+p['frame_bytes']),'High read budget exhausted.');part=bytes(device.read(0x81,min(4096,needed-len(raw)),timeout=timeout()));require(len(part)<=needed-len(raw),'High fragment exceeds extent.')
            if not part:continue
            raw+=part
            if len(raw)==256:needed=256+word(raw,12);require(needed in (256,256+p['frame_bytes']),'High response exceeds bound.')
        return bytes(raw)
    write(manifest_upload(session,manifest));record['h264_retention']='whole-board-power-removal';save();uploaded=0;present=0;completed=0
    while present<len(rows):
        if uploaded<len(rows) and completed==uploaded:write(frame_upload(session,manifest,uploaded,*frames[uploaded]));uploaded+=1
        d=next(d for d in rows if d['presentation']==present);oracle=frames[d['ordinal']][1]
        raw=response(present);result=decode(raw,session,manifest,present,oracle);require(completed<=result['completed']<=uploaded,'High decode progress differs.');completed=result['completed']
        if result['status']==0:time.sleep(min(.005,max(0,deadline-time.monotonic())));continue
        require(result['status'] in (1,2),'High retired output cannot complete capture.');durable(evidence/f'h264-{present:03}.bin',raw)
        if result['status']==2:record['h264_result']='observed-failure';record['durable_result']=result;save();return result
        name=f'output-{present:03}.yu12';durable(evidence/name,raw[256:]);timeout();record.setdefault('copied_h264_pictures',[]).append(dict(path=name,bytes=p['frame_bytes'],**result));save();write(header(A,present,session)+raw[96:128]);present+=1
    record['h264_result']='observed-success';record['durable_result']=result;save();return result
