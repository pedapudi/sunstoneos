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
SOFTWARE_PREREQUISITE='storage-read-supervised-software'
SOFTWARE_SCHEMA='a733-storage-read-supervised-software-v2'
PREREQUISITES={'supervised-ram-recovery',SOFTWARE_PREREQUISITE}
PRODUCT='sunstoneos native SD0 read'
SERIAL='sunstoneos-a733-storage-read'


THROUGHPUT=((128,8,'sequential'),(96,8,'random'),(32,1,'single'))


def throughput_plan(first,span):
    """128 sequential and 96 random eight-sector reads and 32 single-sector reads.

    Random reads start at eight-sector boundaries of [first, first+span), from a
    fixed linear congruential sequence, so that every run reads the same sectors."""
    require(span>=8*THROUGHPUT[1][0] and span%8==0 and first+span<=0xffffffff+1,'Invalid SD0 throughput span.')
    plan=[(first+8*i,8) for i in range(THROUGHPUT[0][0])];state=first^span
    for _ in range(THROUGHPUT[1][0]):
        state=(state*1103515245+12345)&0x7fffffff;plan.append((first+8*(state%(span//8)),8))
    return plan+[(first+i,1) for i in range(THROUGHPUT[2][0])]


def throughput_summary(reads):
    """MB/s (10^6 bytes per second) from each read's native first-poll-to-completion time."""
    require(len(reads)==sum(n for n,_,_ in THROUGHPUT) and all(r.get('elapsed_us') for r in reads),'Incomplete SD0 throughput reads.')
    out,index={},0
    for count,blocks,name in THROUGHPUT:
        part=reads[index:index+count];index+=count
        require(all(r['blocks']==blocks for r in part),'SD0 throughput plan mismatch.')
        elapsed=sum(r['elapsed_us'] for r in part);total=sum(r['bytes'] for r in part)
        out[name]=dict(requests=count,bytes=total,elapsed_us=elapsed,mb_per_s=round(total/elapsed,3),mean_us=round(elapsed/count,1))
    block=(out['sequential']['mean_us']-out['single']['mean_us'])/7
    out['per_block_us']=round(block,2);out['per_request_us']=round(out['single']['mean_us']-block,2)
    return out


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,ranges=((0,1),(1,1)),maximum=4):
    preparation.session_check(session)
    require(1<=len(ranges)<=maximum,'SD0 capture requires a bounded number of ranges.')
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
                sha256=hashlib.sha256(frame[decoder.HEADER:]).hexdigest(),ticket=ticket,request_id=sequence,first_sector=first,blocks=blocks,
                elapsed_us=result.get('elapsed_us')))
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
    if maximum>4:record['storage_read_throughput']=throughput_summary(record['storage_read_reads'])
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
            require(magic==0x44524453 and version in (2,3,4) and status in range(5) and
                    ((0<count<=decoder.MAXIMUM and count%512==0) if status==1 else count==0),'Invalid SD0 frame extent.')
            expected=decoder.HEADER+count;require(len(frame)<=expected,'SD0 frame exceeds extent.')
    return bytes(frame)


def cancel_device(device,same,deadline,evidence,record,save,now=time.monotonic):
    # Mark cancellation before any I/O; this session can never publish output again.
    record['storage_read_cancelled']=True;record['storage_read_result']='cancelled';save()
    same();require(now()<deadline,'SD0 cancel deadline expired.')
    require(device.write(0x02,b'\1',timeout=min(500,max(1,int((deadline-now())*1000))))==1,'Incomplete SD0 cancellation.')


def capture_device(device,same,deadline,session,evidence,record,save,ranges=((0,1),(1,1)),maximum=4):
    def exchange(packet,timeout):
        require(not record.get('storage_read_cancelled'),'Cancelled SD0 session output is invalid.')
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete SD0 request.')
        return read_frame(device,same,deadline,evidence,record,save)
    return capture(exchange,session,evidence,record,save,deadline,ranges=ranges,maximum=maximum)


def source_files():
    return [('storage_read-host-tool.py',Path(__file__)),('storage_read-preparation.py',Path(preparation.__file__)),
        ('storage_read-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    prepared=preparation.validate_bundle(bundle)
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    return prepared


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'SD0 read')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,SOFTWARE_SCHEMA,('session_binding','read_only_trace','revocation_retention','host_capture'),
        'SD0 read',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'SD0 read',SOFTWARE_PREREQUISITE,PREREQUISITES,
        reference_bytes,validate_software)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release SD0 read resources.')
    parser.add_argument('--read',action='append',metavar='FIRST:BLOCKS',help='One to four validated ranges; default 0:1 and 1:1.')
    parser.add_argument('--throughput',metavar='FIRST:SPAN',help='Read-only throughput plan over [FIRST, FIRST+SPAN); needs record version 4.')
    args=parser.parse_args(argv)
    require(not (args.read and args.throughput),'Choose --read or --throughput.')
    maximum=4
    if args.throughput:
        first,span=map(int,args.throughput.split(':'));ranges=throughput_plan(first,span);maximum=len(ranges)
        require(struct.unpack_from('<I',freeze_bundle(args.bundle.resolve())['_frozen']['session.bin'],860)[0] in (2,3),
                'Throughput timing needs the high-speed or UHS-I profile.')
    else:ranges=[tuple(map(int,x.split(':'))) for x in args.read] if args.read else [(0,1),(1,1)]
    require(1<=len(ranges)<=maximum and all(len(x)==2 for x in ranges),'Invalid bounded read plan.')
    for i,(first,blocks) in enumerate(ranges):decoder.read_request(first,blocks,i+1)
    bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'SD0 read entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,ranges,maximum),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='storage_read_result',
        source_files=source_files(),description='Supervised native SD0 read; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
