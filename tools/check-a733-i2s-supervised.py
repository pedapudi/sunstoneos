#!/usr/bin/env python3
"""Capture one supervised I2S session under an existing coordinator claim."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


supervisor = module('i2s_capture_supervisor', ROOT / 'tools/check-a733-smp-supervised.py')
preparation = module('i2s_capture_preparation', ROOT / 'apps/i2s/prepare.py')
decoder = module('i2s_capture_decoder', ROOT / 'apps/i2s/observe.py')
require, digest, artifact = supervisor.require, supervisor.digest, supervisor.artifact
PREREQUISITES = {'supervised-ram-recovery', 'i2s-supervised-software'}


def bundle_bytes(bundle, prepared, name):
    require(Path(name).name == name, 'Invalid bundle filename.')
    data = (bundle / name).read_bytes()
    require(hashlib.sha256(data).hexdigest() == prepared['files'].get(name),
            'Prepared input changed before use: ' + name)
    return data


def capture(exchange,session,evidence,record,save,deadline,*,pcm,direction,capture_samples=None,now=time.monotonic,pause=time.sleep):
    """Persist native copied periods; TX consumption does not establish wire drain."""
    record['i2s_retention']='whole-board-power-removal';record['i2s_result']='unobserved';save()
    if capture_samples is not None:
        require(len(session)==640 and struct.unpack_from('<3Q',session)==(0x4153373341555331,2,640) and
                16000<=capture_samples<=80000 and struct.unpack_from('<I',session,552)[0]==capture_samples and direction==0,
                'I2S capture duration differs from validated transcription profile.')
    total=16384 if capture_samples is None else capture_samples*2
    output=bytearray();offset=0;ready=False
    for sequence in range(1,605):
        require(now()<deadline,'I2S capture deadline expired.')
        operation=1 if ready else 0;count=min(4096,total-offset) if ready else 0
        frame=exchange(struct.pack('<III',operation,offset,count),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'i2s-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),'operation':operation,'offset':offset}
        record.setdefault('i2s_frames',[]).append(entry);save()
        value=decoder.decode(frame,session,operation,offset,count,total)
        require(now()<deadline,'I2S response arrived after deadline.')
        if value['state']==3:
            record['i2s_result']='observed-failure';save();return
        if value['state']!=2:
            require(not ready,'I2S result regressed during copy.')
            pause(min(.01,max(0,deadline-now())));continue
        if not ready:ready=True;continue
        output.extend(value['payload']);offset+=count
        if offset==total:
            require(direction in (0,1,2) and (direction!=1 or bytes(output)==pcm),'I2S consumed TX memory differs from admitted PCM.')
            target=evidence/'pcm-s16le-16000-mono.bin';supervisor.durable_frame(target,output)
            info={'path':target.name,'bytes':len(output),'sha256':digest(target),'format':'S16_LE','rate_hz':16000,'channels':1,
                  'direction':('capture','playback','duplex')[direction],'observation':'dma-memory-consumed' if direction==1 else 'captured-periods',
                  'wire_drain_established':False,'acoustic_result_established':False}
            if direction==2:
                info['duplex_completion']={k:value[k] for k in ('capture_bytes','playback_bytes',
                    'capture_periods','playback_periods','drained_directions')}
            supervisor.save_json(evidence/'pcm.json',info)
            require(now()<deadline,'I2S durable output exceeded deadline.')
            record['copied_i2s_output']=info
            if capture_samples is not None:
                if not capture_transcript(exchange,session,evidence,record,save,deadline,info,now=now,pause=pause):return
            else:record['durable_result']=entry.copy()
            record['i2s_result']='observed-success'
            try:save()
            except BaseException:
                record['i2s_result']='unobserved';record.pop('durable_result',None);record.pop('copied_i2s_output',None);raise
            return
    raise supervisor.CaptureWindowExpired('I2S pending query budget exhausted.')


def capture_transcript(exchange,session,evidence,record,save,deadline,pcm,*,now=time.monotonic,pause=time.sleep):
    output=bytearray();identity=None;total=0
    # At10ms per pending reply this budget covers the entire59s host window
    # plus all transcript chunks. The absolute deadline remains authoritative.
    for sequence in range(1,6201):
        require(now()<deadline,'I2S transcription deadline expired.')
        count=min(4096,total-len(output)) if identity is not None else 0
        offset=len(output)
        frame=exchange(struct.pack('<III',3,offset,count),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'i2s-transcript-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),'operation':3,'offset':offset}
        record.setdefault('i2s_transcript_frames',[]).append(entry);save()
        value=decoder.decode_transcript(frame,session,offset,count)
        require(now()<deadline,'I2S transcription response arrived after deadline.')
        if value['state']==3:
            record['i2s_result']='observed-failure';save();return False
        if value['state']!=2:
            require(identity is None,'I2S transcription regressed during copy.')
            pause(min(.01,max(0,deadline-now())));continue
        require(value['pcm_sha256']==pcm['sha256'] and value['samples']*2==pcm['bytes'],
                'Transcription does not identify the durably copied PCM.')
        if identity is None:identity=frame[72:144];total=value['total'];continue
        require(frame[72:144]==identity,'I2S transcription identity changed during copy.')
        output.extend(value['payload'])
        require(len(output)<=total<=65536,'I2S transcription extent overflow.')
        if len(output)==total:
            require(hashlib.sha256(output).hexdigest()==value['text_sha256'],'I2S complete transcription hash differs.')
            output.decode('utf-8')
            target=evidence/'transcription.txt';supervisor.durable_frame(target,output)
            info={'path':target.name,'bytes':total,'sha256':digest(target),'samples':value['samples'],
                  'pcm_sha256':pcm['sha256'],'format':'UTF-8 recorded-engine output'}
            supervisor.save_json(evidence/'transcription.json',info)
            require(now()<deadline,'I2S durable transcription exceeded deadline.')
            record['copied_i2s_transcription']=info;record['durable_result']=entry.copy();return True
    raise supervisor.CaptureWindowExpired('I2S transcription query budget exhausted.')


def capture_repeated(exchange,session,evidence,record,save,deadline,periods=8,*,now=time.monotonic,pause=time.sleep):
    require(1<=periods<=256,'Repeated I2S host period limit must be1..256.')
    direction=struct.unpack_from('<I',session,208)[0];require(direction in (3,4,5),'Repeated I2S profile required.')
    mask=3 if direction==5 else 1
    record.update(i2s_retention='whole-board-power-removal',i2s_result='unobserved');save()
    copied=0;first=0;last_ticket=0;sequence=0;stopping=False;failure=False;terminal_seen=False;previous=(0,0,0,0);output=bytearray()
    def query(op,offset=0,count=0,ticket=0):
        nonlocal sequence,previous,failure,terminal_seen
        require(now()<deadline,'Repeated I2S deadline expired.')
        packet=struct.pack('<III',op,ticket&0xffffffff,ticket>>32) if op==5 else struct.pack('<III',op,offset,count)
        frame=exchange(packet,min(500,max(1,int((deadline-now())*1000))))
        sequence+=1;path=evidence/f'i2s-repeated-{sequence:04}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),'operation':op}
        record.setdefault('i2s_frames',[]).append(entry);save()
        value=decoder.decode_repeated(frame,session,op,offset,count,ticket)
        require(now()<deadline,'Repeated I2S response arrived after deadline.')
        require(value['rx_frames']>=previous[0] and value['tx_frames']>=previous[1] and
                value['drained']&previous[2]==previous[2] and value['flags']&previous[3]==previous[3],
                'Repeated I2S observations regressed.')
        previous=(value['rx_frames'],value['tx_frames'],value['drained'],value['flags'])
        failure=failure or value['state']==3 or bool(value['flags'])
        terminal_seen=terminal_seen or value['state'] in (2,3);return value,entry
    for _ in range(8192):
        # Only a query issued after terminal publication proves an empty backlog.
        stopped_before_query=terminal_seen
        value,entry=query(0 if direction==4 else 4,count=0 if direction==4 else 4096)
        had_period=bool(value['ticket'])
        if had_period:
            require(value['ticket']==last_ticket+1 and value['first_frame']==first and len(value['payload'])==4096,
                    'Repeated I2S copied stream skipped or repeated a period.')
            # A durable complete copy precedes the exact copied-buffer ACK.
            path=evidence/f'period-{value["ticket"]:04}.bin';supervisor.durable_frame(path,value['payload'])
            record.setdefault('copied_periods',[]).append({'path':path.name,'ticket':value['ticket'],
                'first_frame':first,'sha256':hashlib.sha256(value['payload']).hexdigest()});save()
            output.extend(value['payload']);first+=value['frames'];copied+=1;last_ticket=value['ticket']
            value,entry=query(5,ticket=last_ticket)
        enough=(direction==4 or copied>=periods) and (direction==3 or value['tx_frames']>=periods*2048)
        if not stopping and (enough or failure):
            value,entry=query(2);stopping=True
        if stopped_before_query and value['state'] in (2,3) and not had_period:
            require(stopping and value['drained']==mask,'Repeated I2S stopped without complete terminal drain.')
            require(failure or enough,'Repeated I2S stopped before requested work.')
            info={'rx_frames':value['rx_frames'],'tx_frames':value['tx_frames'],'drained':value['drained'],
                  'flags':value['flags'],'copied_periods':copied,'wire_drain_established':False}
            if direction!=4:
                target=evidence/'pcm-s16le-16000-mono.bin';supervisor.durable_frame(target,output)
                info.update(path=target.name,bytes=len(output),sha256=digest(target))
            require(now()<deadline,'Repeated I2S durable output exceeded deadline.')
            record['copied_i2s_output']=info;record['durable_result']=entry.copy()
            record['i2s_result']='observed-failure' if failure else 'observed-success'
            try:save()
            except BaseException:
                record['i2s_result']='unobserved';record.pop('durable_result',None);record.pop('copied_i2s_output',None);raise
            return
        pause(min(.001,max(0,deadline-now())))
    raise supervisor.CaptureWindowExpired('Repeated I2S query budget exhausted.')


def capture_runtime(exchange,session,evidence,record,save,deadline,pcm,periods=8,*,now=time.monotonic,pause=time.sleep):
    """Feed a caller PCM prefix plus four caller-provided guard periods, then cancel."""
    require(len(session)==552 and struct.unpack_from('<I',session,208)[0]==6,'Runtime PCM session required.')
    require(1<=periods<=256 and len(pcm)%4096==0 and (periods+4)*4096<=len(pcm)<=260*4096,
            'Runtime PCM needs requested periods plus four complete caller-provided guard periods.')
    # Selected native profile is mono S16:2memorybytes/frame, independently of32-bit wire slots.
    frames_per_period=4096//2;sequence=0;queries=0;previous=(0,0,0,0,0);stopping=False
    record.update(i2s_retention='whole-board-power-removal',i2s_result='unobserved',
                  runtime_pcm_sha256=hashlib.sha256(pcm).hexdigest());save()
    target=evidence/'runtime-input-s16le-16000-mono.bin';supervisor.durable_frame(target,pcm)
    def query(op,upload=0):
        nonlocal queries,previous
        require(now()<deadline,'Runtime PCM deadline expired.')
        packet=struct.pack('<III',op,upload&0xffffffff,upload>>32)
        if op==6:packet+=pcm[(upload-1)*4096:upload*4096]
        frame=exchange(packet,min(1000,max(1,int((deadline-now())*1000))))
        queries+=1;path=evidence/f'i2s-runtime-{queries:04}.bin';supervisor.durable_frame(path,frame)
        require(len(frame)==128 and struct.unpack_from('<II',frame)==(0x53493241,4),'Runtime PCM response version/extent differs.')
        state,operation,payload,total=struct.unpack_from('<4I',frame,8)
        require(state<=3 and operation==op and not payload and total==4096 and
                struct.unpack_from('<2Q',frame,24)==struct.unpack_from('<2Q',session,24) and
                frame[40:72]==hashlib.sha256(session).digest() and not any(frame[112:128]),'Runtime PCM response binding differs.')
        accepted,committed,consumed=struct.unpack_from('<3Q',frame,72)
        credits,flags,drained,result=struct.unpack_from('<4I',frame,96)
        require(credits<=4 and flags<=1 and drained<=1 and result in ((1,2) if op==6 else (0,)) and
                accepted<=len(pcm)//4096 and committed%frames_per_period==consumed%frames_per_period==0 and
                consumed<=committed<=accepted*frames_per_period and
                accepted>=previous[0] and committed>=previous[1] and consumed>=previous[2] and
                flags&previous[3]==previous[3] and drained&previous[4]==previous[4] and
                accepted==(upload if op==6 and result==1 else sequence),
                'Runtime PCM counters or ownership bounds differ.')
        previous=(accepted,committed,consumed,flags,drained)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),'operation':op};record.setdefault('i2s_frames',[]).append(entry);save()
        return dict(state=state,accepted=accepted,committed=committed,consumed=consumed,credits=credits,flags=flags,drained=drained,result=result,entry=entry)
    value=query(0)
    for _ in range(8192):
        failed=value['state'] in (2,3) or value['flags']
        if not stopping and (failed or value['consumed']>=periods*frames_per_period):
            value=query(2);stopping=True
        elif not stopping and value['credits'] and sequence<periods+4:
            value=query(6,sequence+1)
            if value['result']==1:
                require(value['accepted']==sequence+1,'Runtime PCM upload acknowledgment skipped a sequence.');sequence+=1
            else:require(value['accepted']==sequence,'Busy runtime PCM upload changed accepted sequence.')
        else:value=query(0)
        if value['state'] in (2,3) and stopping:
            require(stopping and value['drained']==1,'Runtime PCM terminal state lacks observed DMA stop.')
            require(value['state']==3 or value['consumed']>=periods*frames_per_period,'Runtime PCM stopped before requested memory consumption.')
            record['runtime_pcm_output']={k:value[k] for k in ('accepted','committed','consumed','drained','flags')}
            record['runtime_pcm_output'].update(wire_drain_established=False,input_sha256=digest(target),input_path=target.name)
            record['i2s_result']='observed-failure' if value['state']==3 or value['flags'] else 'observed-success'
            record['durable_result']=value['entry']
            try:save()
            except BaseException:
                record['i2s_result']='unobserved';record.pop('durable_result',None);record.pop('runtime_pcm_output',None);raise
            return
        pause(min(.001,max(0,deadline-now())))
    raise supervisor.CaptureWindowExpired('Runtime PCM query budget exhausted.')


def capture_runtime_file(exchange,session,evidence,record,save,deadline,pcm,*,now=time.monotonic,pause=time.sleep):
    """Observe exact file memory consumption and FIFO empty, retaining wire ownership."""
    require(len(session)==552 and struct.unpack_from('<I',session,208)[0]==6,'Runtime PCM session required.')
    require(pcm and len(pcm)%2==0 and len(pcm)<=260*4096,'File must contain bounded whole mono S16 frames.')
    total=len(pcm)//2;periods=(total+2047)//2048;sequence=0;queries=0;previous=(0,0,0);sealed=False
    target=evidence/'runtime-file-s16le-16000-mono.bin';supervisor.durable_frame(target,pcm)
    record.update(i2s_retention='whole-board-power-removal',i2s_result='unobserved');save()
    def query(op,value=0):
        nonlocal queries,previous
        require(now()<deadline,'Runtime file deadline expired.')
        packet=struct.pack('<III',op,value&0xffffffff,value>>32)
        if op==6:packet+=pcm[(value-1)*4096:value*4096]
        frame=exchange(packet,min(1000,max(1,int((deadline-now())*1000))))
        queries+=1;path=evidence/f'i2s-file-{queries:04}.bin';supervisor.durable_frame(path,frame)
        require(len(frame)==160 and struct.unpack_from('<II',frame)==(0x53493241,5),'File response extent/version differs.')
        state,operation,payload,capacity=struct.unpack_from('<4I',frame,8)
        accepted,committed,consumed=struct.unpack_from('<3Q',frame,72)
        credits,flags,idle,result=struct.unpack_from('<4I',frame,96)
        declared,accepted_frames=struct.unpack_from('<2Q',frame,112)
        eof,fifo,retained,wire,stopped,reserved=struct.unpack_from('<6I',frame,128)
        require(state<=3 and operation==op and not payload and capacity==4096 and
                struct.unpack_from('<2Q',frame,24)==struct.unpack_from('<2Q',session,24) and
                frame[40:72]==hashlib.sha256(session).digest() and declared==total and
                not reserved and not any(frame[152:160]) and not wire and not stopped and
                credits<=4 and flags<=1 and idle<=1 and eof<=1 and fifo<=1 and retained==fifo and
                result in ((1,2) if op==6 else (1,) if op in (7,8) else (0,)) and
                accepted<=periods and accepted_frames==min(total,accepted*2048) and
                consumed<=committed<=accepted_frames and
                all(a>=b for a,b in zip((accepted,committed,consumed),previous)) and
                accepted==(value if op==6 and result==1 else sequence), 'File response binding/accounting differs.')
        previous=(accepted,committed,consumed)
        record.setdefault('i2s_frames',[]).append({'path':path.name,'sha256':digest(path),'operation':op});save()
        return dict(state=state,accepted=accepted,committed=committed,consumed=consumed,credits=credits,
                    flags=flags,idle=idle,result=result,eof=eof,fifo=fifo,retained=retained,entry=record['i2s_frames'][-1])
    value=query(7,total)
    for _ in range(8192):
        require(value['state']!=3 and not value['flags'],'Runtime file failed; resources remain retained.')
        if sequence<periods and value['credits']:
            value=query(6,sequence+1)
            if value['result']==1:sequence+=1
        elif sequence==periods and not sealed:
            value=query(8,total);sealed=True
        else:value=query(0)
        if value['state']==2:
            require(sealed and value['accepted']==periods and value['committed']==value['consumed']==total and
                    value['eof']==value['fifo']==value['retained']==value['idle']==1,'File completion lacks exact retained accounting.')
            record['runtime_pcm_output']={k:value[k] for k in ('accepted','committed','consumed','eof','fifo','retained')}
            record['runtime_pcm_output'].update(scope='input-consumed-fifo-empty-wire-unknown',wire_drain_established=False,
                input_sha256=digest(target),input_path=target.name)
            record['i2s_result']='observed-success';record['durable_result']=value['entry']
            try:save()
            except BaseException:
                record['i2s_result']='unobserved';record.pop('durable_result',None);record.pop('runtime_pcm_output',None);raise
            return
        pause(min(.001,max(0,deadline-now())))
    raise supervisor.CaptureWindowExpired('Runtime file query budget exhausted.')


def capture_device(device,same,deadline,session,evidence,record,save,*,pcm,direction,capture_samples=None,repeated_periods=8,runtime_pcm=None,runtime_periods=8,runtime_file=False):
    serial=[0]
    def exchange(packet,timeout):
        same();require(time.monotonic()<deadline,'I2S deadline before write.')
        require(device.write(0x02,packet,timeout=timeout)==len(packet) and
                (12<len(packet)<=4108 and (len(packet)-12)%2==0 if runtime_file and struct.unpack_from('<I',packet)[0]==6 else len(packet)==4108 if direction==6 and struct.unpack_from('<I',packet)[0]==6 else len(packet)==12),
                'Incomplete I2S request.')
        serial[0]+=1;frame=bytearray();base=144 if struct.unpack_from('<I',packet)[0]==3 else 160 if direction==6 and runtime_file else 128 if direction>=3 else 96 if direction==2 else 72;extent=base+4096;part=0;header=False;empty=0
        while len(frame)<extent:
            same();remaining=int((deadline-time.monotonic())*1000);require(remaining>0,'I2S deadline during response.')
            raw=bytes(device.read(0x81,extent-len(frame),timeout=min(timeout,remaining)))
            part+=1;path=evidence/f'i2s-fragment-{serial[0]:03}-{part:03}.bin';supervisor.durable_frame(path,raw)
            record.setdefault('i2s_fragments',[]).append({'path':path.name,'sha256':hashlib.sha256(raw).hexdigest()});save()
            require(len(raw)<=extent-len(frame),'Invalid I2S fragment extent.')
            if not raw:
                empty+=1;require(empty<=16,'I2S empty-fragment budget exhausted.');continue
            frame.extend(raw)
            if not header and len(frame)>=72:
                header=True;size=struct.unpack_from('<I',frame,16)[0];require(size<=4096,'Oversized I2S response.');extent=base+size
                require(len(frame)<=extent,'I2S response exceeds header extent.')
        return bytes(frame)
    if direction==6 and runtime_file:return capture_runtime_file(exchange,session,evidence,record,save,deadline,runtime_pcm)
    if direction==6:return capture_runtime(exchange,session,evidence,record,save,deadline,runtime_pcm,runtime_periods)
    if direction>=3:return capture_repeated(exchange,session,evidence,record,save,deadline,repeated_periods)
    return capture(exchange,session,evidence,record,save,deadline,pcm=pcm,direction=direction,capture_samples=capture_samples,now=time.monotonic,pause=time.sleep)


def source_files():
    return [('i2s-host-tool.py',Path(__file__)),('i2s-preparation.py',Path(preparation.__file__)),
            ('i2s-decoder.py',Path(decoder.__file__)),
            ('dma-nonsecure-access.py',Path(preparation.security_authority.__file__))]


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid I2S review reference.')
    path=Path(value['path']);require(path.is_absolute(),'I2S review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'I2S review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-i2s-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','host_lifecycle')),
            'I2S software evidence is incomplete.')
    for key,name in [('image','image.elf'),('launcher','launch.bin')]:
        _,data=reference_bytes(value[key])
        require(hashlib.sha256(data).hexdigest()==prepared['files'][name],
                'Software evidence covers different initially admitted executable bytes.')
    require(set(value.get('sources',{}))==set(prepared['_source_hashes']),'Software source identities are incomplete.')
    for name,expected in prepared['_source_hashes'].items():
        _,data=reference_bytes(value['sources'][name])
        require(hashlib.sha256(data).hexdigest()==expected,'Software evidence covers another implementation: '+name)
    require(value.get('evidence'),'Software result evidence is missing.')
    for item in value['evidence']:reference_bytes(item)


def claimed(database,test_id,owner,bundle,prepared):
    db=sqlite3.connect(database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'I2S queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'I2S prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted I2S entry.')
        _,data=reference_bytes(refs['i2s-supervised-software']);validate_software(json.loads(data),prepared)
        covered={}
        for value in submission['artifacts']:
            path,data=reference_bytes(value);covered[path]=hashlib.sha256(data).hexdigest()
        expected={'prepared.json':prepared['_prepared_sha256'],'image.elf':prepared['files']['image.elf'],
                  'launch.bin':prepared['files']['launch.bin']}
        require(all(covered.get((bundle/name).resolve())==h for name,h in expected.items()),
                'Queue does not cover the initially admitted prepared image.')
    finally:db.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'fel', 'evidence', 'database'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--runtime-pcm',type=Path,help='Raw caller S16LE mono16kHz PCM, including four guard periods')
    parser.add_argument('--runtime-file',action='store_true',help='Declare a complete nonempty PCM file; observe DMA/FIFO accounting with wire ownership retained')
    parser.add_argument('--runtime-periods',type=int,default=8,help='Runtime PCM memory-consumed periods before cancellation,1..256')
    parser.add_argument('--repeated-periods',type=int,default=8,help='Completed periods before orderly cancellation in repeated mode,1..256')
    parser.add_argument('--claim', required=True)
    parser.add_argument('--owner', required=True)
    parser.add_argument('--supervised-manual-reconnect', action='store_true', required=True)
    args = parser.parse_args(argv)
    bundle = args.bundle.resolve()
    prepared=preparation.validate_bundle(bundle)
    require(hashlib.sha256((bundle/'prepared.json').read_bytes()).hexdigest()==prepared['_prepared_sha256'],'I2S metadata changed after validation.')
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    data=prepared['_frozen'];session=data['session.bin']
    runtime_pcm=None
    if args.runtime_pcm:
        with args.runtime_pcm.open('rb') as source:runtime_pcm=source.read(260*4096+1)
    require((prepared['direction']==6)==(runtime_pcm is not None),'Runtime PCM input is required only for direction6.')
    require(not args.runtime_file or runtime_pcm is not None,'--runtime-file requires --runtime-pcm.')
    if runtime_pcm is not None and args.runtime_file:
        require(bool(runtime_pcm) and len(runtime_pcm)%2==0 and len(runtime_pcm)<=260*4096,'Runtime PCM file extent invalid.')
    elif runtime_pcm is not None:
        require(1<=args.runtime_periods<=256 and len(runtime_pcm)%4096==0 and
                (args.runtime_periods+4)*4096<=len(runtime_pcm)<=260*4096,'Runtime PCM extent/guard periods invalid.')
    _,_,identity,layout,pcm=preparation.image_data(data['image.elf'])
    cache={path:data[item['name']] for path,item in prepared['entry_evidence'].items()}
    admission=json.loads(data['admission.json'])
    def validate_entry(target,port):
        preparation.validate_admission(data['admission.json'],session,layout,identity,pcm,prepared['image_sha256'],cache,True)
        require(admission['identity']==target and admission['port']==port,'I2S entry covers another instance.')

    require(prepared.get('retention')=='whole-board-power-removal','I2S lifetime retention plan required.')
    return supervisor.run_session(args, bundle, prepared,
        lambda: claimed(args.database, args.claim, args.owner, bundle,prepared), validate_entry,
        lambda device, same, deadline, record, save: capture_device(
            device, same, deadline, session, args.evidence, record, save,pcm=pcm,direction=prepared['direction'],capture_samples=prepared.get('capture_samples'),repeated_periods=args.repeated_periods,runtime_pcm=runtime_pcm,runtime_periods=args.runtime_periods,runtime_file=args.runtime_file),
        product='sunstoneos native I2S', serial='sunstoneos-a733-i2s',
        bcd_device=0x0100, capture_seconds=59, result_key='i2s_result',
        source_files=source_files(), description='Supervised native I2S; retain all resources until whole-board power removal')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, sqlite3.Error, struct.error) as error:
        sys.exit(f'Stopped: {error}')
