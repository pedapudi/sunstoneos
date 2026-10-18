#!/usr/bin/env python3
"""Capture one admitted supervised SD0 read decode; retain resources until full power removal."""
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
supervisor=module('storage_read_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('storage_read_capture_preparation',ROOT/'apps/storage-read/prepare.py')
decoder=module('storage_read_capture_decoder',ROOT/'apps/storage-read/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','storage-read-supervised-software'}
PRODUCT='sunstoneos native SD0 read'
SERIAL='sunstoneos-a733-storage-read'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,ranges=((0,1),(1,1))):
    preparation.session_check(session)
    require(1<=len(ranges)<=4,'SD0 capture requires one to four bounded ranges.')
    packets=[decoder.read_request(first,blocks,i+1) for i,(first,blocks) in enumerate(ranges)]
    require(not record.get('storage_read_cancelled'),'Cancelled SD0 session output is invalid.')
    record.update(storage_read_retention='whole-board-power-removal',storage_read_result='unobserved');save()
    def query(packet):
        require(now()<deadline,'SD0 read capture deadline expired.')
        frame=exchange(packet,min(500,max(1,int((deadline-now())*1000))))
        sequence=len(record.get('storage_read_frames',[]))+1
        path=evidence/f'storage_read-{sequence:04}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('storage_read_frames',[]).append(entry);save()
        result=decoder.decode(frame,session);entry.update(result)
        if result['application_status']==2:
            record['storage_read_result']='observed-failure';record['durable_result']=entry.copy();save()
            raise ValueError('Native SD0 client failed; no further requests.')
        require(result['application_status']!=1 or result['application_reads']==2,
                'Native SD0 client completion has an incomplete read count.')
        require(now()<deadline and not record.get('storage_read_cancelled'),'Late or cancelled SD0 output.')
        save();return frame,result,entry
    for _ in range(2401):
        _,initial,entry=query(bytes(1))
        if initial['status']==3:break
        if initial['status']==2:
            record['storage_read_result']='observed-failure';record['durable_result']=entry.copy();save();return initial
        require(initial['status']==0,'SD0 service failed before range submission.')
        pause(min(.025,max(0,deadline-now())))
    else:raise supervisor.CaptureWindowExpired('SD0 identification query budget exhausted.')
    prior_ticket=0
    for sequence,((first,blocks),packet) in enumerate(zip(ranges,packets),1):
        require(first<initial['sectors'] and blocks<=initial['sectors']-first,'Requested SD0 range exceeds actual capacity.')
        current=packet;ticket=None
        for _ in range(2401):
            frame,result,entry=query(current);current=bytes(1)
            require(result['status']!=4,'SD0 request was rejected.')
            if result['status']==2:
                record['storage_read_result']='observed-failure';record['durable_result']=entry.copy();save();return result
            require(result['request_id']==sequence and result['first_sector']==first and result['blocks']==blocks and result['client']==0,
                    'SD0 response belongs to another client or request range.')
            if ticket is None:
                ticket=result['ticket'];require(ticket>prior_ticket,'SD0 request ticket is stale.')
            require(result['ticket']==ticket,'SD0 request ticket changed.')
            if result['status']==0:
                pause(min(.025,max(0,deadline-now())));continue
            require(result['status']==1,'SD0 service lost the accepted request.')
            output=evidence/f'output-{sequence:03}.bin';supervisor.durable_frame(output,frame[decoder.HEADER:])
            require(now()<deadline,'SD0 output became durable after capture deadline.')
            record.setdefault('storage_read_reads',[]).append(dict(path=output.name,bytes=blocks*512,
                sha256=hashlib.sha256(frame[decoder.HEADER:]).hexdigest(),ticket=ticket,request_id=sequence,first_sector=first,blocks=blocks))
            prior_ticket=ticket;record['durable_result']=entry.copy();save();break
        else:raise supervisor.CaptureWindowExpired('SD0 read exhausted bounded query budget.')
    # USB completion alone does not establish the second native client's run.
    for _ in range(2401):
        if result['application_status']==1 and result['application_reads']==2:break
        pause(min(.025,max(0,deadline-now())))
        _,result,entry=query(bytes(1))
        if result['status']==2:
            record['storage_read_result']='observed-failure';record['durable_result']=entry.copy();save();return result
        require(result['status'] in (0,1,3),'SD0 service rejected native-client completion query.')
    else:raise supervisor.CaptureWindowExpired('Native SD0 client completion query budget exhausted.')
    record['storage_read_application_reads']=result['application_reads']
    record['storage_read_application_result']=entry.copy()
    record['storage_read_result']='observed-success'
    try:save()
    except BaseException:record['storage_read_result']='unobserved';record.pop('durable_result',None);raise
    return result


def read_frame(device,same,deadline,evidence,record,save,now=time.monotonic):
    frame=bytearray();expected=None
    while expected is None or len(frame)<expected:
        same();remaining=int((deadline-now())*1000)
        require(remaining>0,'SD0 frame deadline expired.')
        maximum=(decoder.HEADER+decoder.MAXIMUM if expected is None else expected)-len(frame)
        fragment=bytes(device.read(0x81,maximum,timeout=min(500,remaining)))
        path=evidence/f"fragment-{len(record.get('storage_read_fragments',[]))+1:04}.bin"
        supervisor.durable_frame(path,fragment)
        record.setdefault('storage_read_fragments',[]).append(dict(path=path.name,sha256=hashlib.sha256(fragment).hexdigest(),bytes=len(fragment)));save()
        require(fragment and len(fragment)<=maximum and now()<deadline,'Incomplete/late SD0 fragment.')
        frame.extend(fragment)
        if expected is None and len(frame)>=decoder.HEADER:
            magic,version,status,count=struct.unpack_from('<4I',frame)
            require((magic,version)==(0x44524453,2) and status in range(5) and
                    ((0<count<=decoder.MAXIMUM and count%512==0) if status==1 else count==0),'Invalid SD0 frame extent.')
            expected=decoder.HEADER+count;require(len(frame)<=expected,'SD0 frame exceeds extent.')
    return bytes(frame)


def cancel_device(device,same,deadline,evidence,record,save,now=time.monotonic):
    # Mark cancellation before any I/O; this session can never publish output again.
    record['storage_read_cancelled']=True;record['storage_read_result']='cancelled';save()
    same();require(now()<deadline,'SD0 cancel deadline expired.')
    require(device.write(0x02,b'\1',timeout=min(500,max(1,int((deadline-now())*1000))))==1,'Incomplete SD0 cancellation.')


def capture_device(device,same,deadline,session,evidence,record,save,ranges=((0,1),(1,1))):
    def exchange(packet,timeout):
        require(not record.get('storage_read_cancelled'),'Cancelled SD0 session output is invalid.')
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete SD0 request.')
        return read_frame(device,same,deadline,evidence,record,save)
    return capture(exchange,session,evidence,record,save,deadline,ranges=ranges)


def source_files():
    return [('storage_read-host-tool.py',Path(__file__)),('storage_read-preparation.py',Path(preparation.__file__)),
        ('storage_read-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    prepared=preparation.validate_bundle(bundle)
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    return prepared


def reference_bytes(value):
    require(isinstance(value,dict) and set(value)=={'path','sha256'},'Invalid SD0 read review reference.')
    path=Path(value['path']);require(path.is_absolute(),'SD0 read review path must be absolute.')
    data=path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==value['sha256'],'SD0 read review reference changed.')
    return path.resolve(),data


def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths


def validate_software(value,prepared):
    require(value.get('schema')=='a733-storage-read-supervised-software-v2' and value.get('physical_access') is False and
            all(value.get(k) is True for k in ('session_binding','read_only_trace','revocation_retention','host_capture')),
            'SD0 read software evidence is incomplete.')
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
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'SD0 read queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner,'Matching coordinator claim is required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,'SD0 read prerequisites differ.')
        refs=review['prerequisites']
        path,data=reference_bytes(refs['supervised-ram-recovery'])
        require(path==(bundle/'admission.json').resolve() and hashlib.sha256(data).hexdigest()==prepared['files']['admission.json'],
                'Reviewed entry differs from the initially admitted SD0 read entry.')
        _,data=reference_bytes(refs['storage-read-supervised-software']);validate_software(json.loads(data),prepared)
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
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release SD0 read resources.')
    parser.add_argument('--read',action='append',metavar='FIRST:BLOCKS',help='One to four validated ranges; default 0:1 and 1:1.')
    args=parser.parse_args(argv)
    ranges=[tuple(map(int,x.split(':'))) for x in args.read] if args.read else [(0,1),(1,1)]
    require(1<=len(ranges)<=4 and all(len(x)==2 for x in ranges),'Invalid bounded read plan.')
    for i,(first,blocks) in enumerate(ranges):decoder.read_request(first,blocks,i+1)
    bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'SD0 read entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,ranges),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='storage_read_result',
        source_files=source_files(),description='Supervised native SD0 read; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
