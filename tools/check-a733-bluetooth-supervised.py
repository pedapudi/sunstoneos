#!/usr/bin/env python3
"""Capture one admitted supervised Bluetooth controller identification; retain resources until full power removal."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
supervisor=module('bluetooth_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('bluetooth_capture_preparation',ROOT/'apps/bluetooth/prepare.py')
decoder=module('bluetooth_capture_decoder',ROOT/'apps/bluetooth/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','bluetooth-supervised-software'}
PRODUCT='sunstoneos Bluetooth HCI'
SERIAL='sunstoneos-a733-bluetooth'


def capture(exchange,session,profile,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,segments=2):
    preparation.profile_check(profile,session)
    if len(profile)==832 and struct.unpack_from('<I',profile,624)[0]==2:
        return capture_repeated(exchange,session,profile,evidence,record,save,deadline,segments,now,pause)
    if len(profile)>=752:
        result=capture_pcm(exchange,session,profile,evidence,record,save,deadline,now,pause)
        if len(profile)==832 and result['state']=='captured':
            return capture_recognition(exchange,session,profile,evidence,record,save,deadline,result,now,pause)
        return result
    record.update(bluetooth_retention='whole-board-power-removal',bluetooth_result='unobserved');save()
    for sequence in range(1,2401):
        require(now()<deadline,'Bluetooth capture deadline expired.')
        frame=exchange(bytes(1),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'bluetooth-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('bluetooth_frames',[]).append(entry);save()
        result=decoder.decode(frame,session,profile);entry.update(result)
        require(now()<deadline,'Bluetooth response arrived after capture deadline.')
        if result['state']=='pending':
            pause(min(.025,max(0,deadline-now())));continue
        record['bluetooth_result']='observed-success' if result['state']=='identified' else 'observed-failure'
        record['durable_result']=entry.copy()
        try:save()
        except BaseException:
            record['bluetooth_result']='unobserved';record.pop('durable_result',None);raise
        return result
    raise supervisor.CaptureWindowExpired('Bluetooth exhausted bounded pending-query budget.')


def capture_pcm(exchange,session,profile,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep):
    record.update(bluetooth_retention='whole-board-power-removal',bluetooth_result='unobserved');save()
    captured=[bytearray(),bytearray()];identity=None;sequence=0
    for kind in (0,1):
        total=struct.unpack_from('<I',profile,720)[0]*2 if kind==0 else identity['records']*24
        while len(captured[kind])<total:
            sequence+=1;require(sequence<=2600 and now()<deadline,'Headset capture budget/deadline expired.')
            offset=len(captured[kind]);count=min(1024,total-offset)
            request=struct.pack('<BBHI',kind+1,0,count,offset)
            frame=exchange(request,min(500,max(1,int((deadline-now())*1000))))
            path=evidence/f'bluetooth-pcm-{sequence:04}.bin';supervisor.durable_frame(path,frame)
            result,data=decoder.decode_pcm(frame,session,profile,kind,offset,count)
            entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),**result}
            record.setdefault('bluetooth_frames',[]).append(entry);save()
            require(now()<deadline,'Headset response arrived after capture deadline.')
            if result['state']=='pending':
                require(identity is None,'Published capture regressed to pending.')
                pause(min(.025,max(0,deadline-now())));continue
            if result['state']=='retained-failure':
                record['bluetooth_result']='observed-failure';record['durable_result']=entry.copy();save();return result
            if identity is None:identity=result
            require(identity==result,'Immutable headset capture changed between copied chunks.')
            captured[kind].extend(data)
    require(hashlib.sha256(captured[0]).hexdigest()==identity['sha256'],'Copied PCM digest differs.')
    cursor=0;acquired=0
    for serial,offset,source,copied,status,reserved in struct.iter_unpack('<QIHHII',captured[1]):
        require(serial>acquired and offset==cursor and 0<source<=255 and 0<=copied<=source and not status and not reserved,
                'Capture record ordering, segmentation or loss differs.')
        cursor+=copied;acquired=serial
    require(cursor==len(captured[0]),'Record extents do not cover copied PCM.')
    for name,data in [('bluetooth-8k-s16le.pcm',captured[0]),('bluetooth-pcm-records.bin',captured[1])]:
        supervisor.durable_frame(evidence/name,data)
    require(now()<deadline,'Headset capture deadline expired before durable publication.')
    record['bluetooth_result']='capture-complete' if len(profile)==832 else 'observed-success';record['durable_result']=identity.copy()
    try:save()
    except BaseException:
        record['bluetooth_result']='unobserved';record.pop('durable_result',None);raise
    return identity


def capture_recognition(exchange,session,profile,evidence,record,save,deadline,capture,now=time.monotonic,pause=time.sleep):
    text=bytearray();identity=None
    record['bluetooth_result']='unobserved';record.pop('durable_result',None);save()
    for sequence in range(1,2601):
        require(now()<deadline,'Headset recognition capture deadline expired.')
        count=min(1024,identity['bytes']-len(text)) if identity else 1
        frame=exchange(struct.pack('<BBHI',3,0,count,len(text)),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'bluetooth-transcription-{sequence:04}.bin';supervisor.durable_frame(path,frame)
        result,data=decoder.decode_recognition(frame,session,profile,len(text),count)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest(),**result}
        record.setdefault('bluetooth_frames',[]).append(entry);save()
        require(now()<deadline,'Recognition response arrived after capture deadline.')
        if result['state']=='pending':
            require(identity is None,'Completed transcription regressed to pending.')
            pause(min(.025,max(0,deadline-now())));continue
        if result['state']=='retained-failure':
            record['bluetooth_result']='observed-failure';record['durable_result']=entry.copy();save();return result
        for key in ('firmware_generation','uart_generation','acl_generation','audio_generation'):
            require(result[key]==capture[key],'Recognition belongs to another capture lifetime.')
        require(result['pcm_sha256']==capture['sha256'],'Recognition used different captured PCM.')
        if identity is None:identity=result
        require(identity==result,'Immutable transcription changed between chunks.')
        text.extend(data)
        if len(text)<identity['bytes']:continue
        require(hashlib.sha256(text).hexdigest()==identity['text_sha256'],'Copied transcript digest differs.')
        decoded=text.decode('utf-8')
        require('\nFORMATTED ' in decoded,'Completed recognition lacks formatted transcription.')
        supervisor.durable_frame(evidence/'bluetooth-transcription.log',text)
        require(now()<deadline,'Recognition expired before durable publication.')
        record['bluetooth_result']='observed-success';record['durable_result']=identity.copy()
        try:save()
        except BaseException:
            record['bluetooth_result']='unobserved';record.pop('durable_result',None);raise
        return identity
    raise supervisor.CaptureWindowExpired('Headset recognition exhausted bounded request budget.')


def capture_repeated(exchange,session,profile,evidence,record,save,deadline,segments=2,now=time.monotonic,pause=time.sleep):
    require(1<=segments<=32,'Repeated headset target must be1..32segments.')
    record.update(bluetooth_retention='whole-board-power-removal',bluetooth_result='unobserved');save()
    queries=0;last=0;cancelled=False;failure=False;previous=(0,0,0,0,0);identity=None
    def query(op,ticket=0,offset=0,count=0):
        nonlocal queries,previous,failure
        require(now()<deadline and queries<16384,'Repeated headset deadline/query budget expired.')
        frame=exchange(struct.pack('<BBHIQ',0x80,op,count,offset,ticket),min(500,max(1,int((deadline-now())*1000))))
        queries+=1;path=evidence/f'bluetooth-repeated-{queries:05}.bin';supervisor.durable_frame(path,frame)
        record.setdefault('bluetooth_frames',[]).append({'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()});save()
        value,data=decoder.decode_repeated(frame,session,profile,offset)
        require(value['published']>=previous[0] and value['consumed']>=previous[1] and
                value['flags']&previous[2]==previous[2] and value['disconnected']>=previous[3] and
                value['finished']>=previous[4],'Repeated Bluetooth observations regressed.')
        previous=(value['published'],value['consumed'],value['flags'],value['disconnected'],value['finished'])
        failure=failure or value['state']==3 or bool(value['flags']&~3)
        require(now()<deadline,'Repeated headset response arrived after deadline.')
        return value,data
    while queries<16384:
        value,_=query(0)
        if value['ticket']:
            require(value['ticket']==last+1,'Repeated headset skipped or repeated a transcript.')
            stamp={k:value[k] for k in ('firmware','uart','acl','audio','ticket','first_sample','bytes','input_samples','output_samples',
                   'pcm_sha256','wav_sha256','text_sha256','first_acquired','last_acquired')}
            lifetime=tuple(value[k] for k in ('firmware','uart','acl','audio'))
            require(identity is None or lifetime==identity,'Repeated headset audio identity changed.');identity=lifetime
            text=bytearray()
            while len(text)<stamp['bytes'] or not stamp['bytes']:
                current,data=query(1,stamp['ticket'],len(text),min(1024,stamp['bytes']-len(text)))
                require({k:current[k] for k in stamp}==stamp and len(data)==min(1024,stamp['bytes']-len(text)),
                        'Immutable repeated transcript changed during copy.')
                text.extend(data)
                if not stamp['bytes']:break
            require(hashlib.sha256(text).hexdigest()==stamp['text_sha256'],'Repeated transcript digest differs.')
            decoded=text.decode('utf-8');require('\nFORMATTED ' in decoded,'Repeated transcript has no formatted recognition.')
            target=evidence/f'transcript-{stamp["ticket"]:04}.txt';supervisor.durable_frame(target,text)
            record.setdefault('transcripts',[]).append({**stamp,'path':target.name});save()
            current,_=query(2,stamp['ticket']);require(current['ticket']==stamp['ticket'],'Repeated transcript ACK differs.')
            last=stamp['ticket'];value=current
        if not cancelled and (last>=segments or failure):
            # An unclassified retained failure supplies no further transport authority.
            require(not(value['state']==3 and not value['flags']),'Repeated headset retained failure; disconnect unestablished.')
            value,_=query(3);cancelled=True
        if value['disconnected'] and value['finished'] and not value['ticket']:
            require(value['published']==value['consumed']==last and (failure or last>=segments),'Repeated headset terminal work count differs.')
            result={k:value[k] for k in ('published','consumed','flags','disconnected','finished')}
            result.update(transcripts=last,wideband=False,physical_audio_qualified=False)
            record['bluetooth_result']='observed-failure' if failure else 'observed-success';record['repeated_recognition']=result
            record['durable_result']=record['bluetooth_frames'][-1].copy()
            try:save()
            except BaseException:
                record['bluetooth_result']='unobserved';record.pop('durable_result',None);record.pop('repeated_recognition',None);raise
            return result
        pause(min(.005,max(0,deadline-now())))
    raise supervisor.CaptureWindowExpired('Repeated headset exhausted query budget.')


def capture_device(device,same,deadline,session,profile,evidence,record,save,segments=2):
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete Bluetooth request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'Bluetooth capture deadline expired after write.')
        if len(profile)==832 and struct.unpack_from('<I',profile,624)[0]==2:
            output=bytearray();extent=None;empty=0
            while extent is None or len(output)<extent:
                same();remaining=int((deadline-time.monotonic())*1000);require(remaining>0,'Repeated Bluetooth response deadline expired.')
                part=bytes(device.read(0x81,1296,timeout=min(timeout,remaining)))
                if not part:empty+=1;require(empty<=16,'Repeated Bluetooth empty-transfer budget expired.');continue
                output.extend(part);require(len(output)<=1296,'Repeated Bluetooth response overflow.')
                if extent is None and len(output)>=272:
                    count=struct.unpack_from('<I',output,12)[0];require(count<=1024,'Repeated transcript response too large.');extent=272+count
                require(extent is None or len(output)<=extent,'Repeated Bluetooth response overflow.')
            return bytes(output)
        return bytes(device.read(0x81,1232 if len(profile)==832 else 1184 if len(profile)==752 else 177,timeout=min(timeout,remaining)))
    return capture(exchange,session,profile,evidence,record,save,deadline,segments=segments)


def source_files():
    return [('bluetooth-model-pin.json',ROOT/'apps/bluetooth/recognition-models.json'),('bluetooth-host-tool.py',Path(__file__)),('bluetooth-preparation.py',Path(preparation.__file__)),
        ('bluetooth-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared Bluetooth metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid Bluetooth bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'Bluetooth input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid Bluetooth review reference.')
    path=Path(value['path']);require(path.is_absolute(),'Bluetooth review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'Bluetooth review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-bluetooth-supervised-software-v1' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('admission_cases','launcher_cases','host_capture','host_lifecycle')),
            'Bluetooth software evidence is incomplete.')
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
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'Bluetooth queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'Bluetooth prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted Bluetooth entry.')
        _,data=reference_bytes(refs['bluetooth-supervised-software']);validate_software(json.loads(data),prepared)
        covered={}
        for value in submission['artifacts']:
            path,data=reference_bytes(value);covered[path]=hashlib.sha256(data).hexdigest()
        expected={'prepared.json':prepared['_prepared_sha256'],'image.elf':prepared['files']['image.elf'],
                  'launch.bin':prepared['files']['launch.bin']}
        require(all(covered.get((bundle/name).resolve())==h for name,h in expected.items()),
                'Queue does not cover the initially admitted prepared image.')
    finally:db.close()


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--segments',type=int,default=2,help='Mode2 transcript target before capture cancellation,1..32')
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release Bluetooth resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];profile=frozen['profile.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.board.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),evidence,True)
        require(admission['identity']==identity and admission['port']==port,'Bluetooth entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,profile,args.evidence,record,save,args.segments),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='bluetooth_result',
        source_files=source_files(),description='Supervised native Bluetooth; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
