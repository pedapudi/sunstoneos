#!/usr/bin/env python3
"""Capture one retained deinterlace operation with durable copied pixels before acknowledgment."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import time

ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
preparation=module('deinterlace_host_preparation',ROOT/'apps/deinterlace/prepare.py')
supervisor=module('deinterlace_host_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
require=supervisor.require
HEADER=192
MAXIMUM=460800


def decode(raw,session):
    preparation.session_check(session)
    require(len(raw)>=HEADER,'Short deinterlace response.')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    require((magic,version)==(0x52494453,1) and status in (0,1,2,3),
        'Deinterlace response header differs.')
    require((epoch,generation)==struct.unpack_from('<2Q',session,24) and
        raw[32:64]==session[768:800] and raw[64:96]==hashlib.sha256(session).digest(),
        'Deinterlace session or input identity differs.')
    geometry=struct.unpack_from('<8I',raw,128)
    require(geometry==struct.unpack_from('<8I',session,856),
        'Deinterlace geometry differs.')
    finish,hardware_status,scanned,guards=struct.unpack_from('<4I',raw,160)
    require(not any(raw[176:192]),'Deinterlace reserved result bytes differ.')
    require(count==(MAXIMUM if status==1 else 0) and len(raw)==HEADER+count,
        'Deinterlace copied extent differs.')
    result=dict(status=status,bytes=count,width=640,height=480,y_stride=640,uv_stride=640,
        format='NV12',mode='WEAVE' if geometry[5]==1 else 'MOTION-OUT_1FRAME',top_field_first=True,matched_finish=finish,
        hardware_status=hardware_status,scanned_masters=scanned,guards=guards,
        physical_qualified=False)
    if status in (1,3):
        require(finish==1 and hardware_status&1 and not hardware_status&0x100 and scanned==0x200 and guards==1,
            'Deinterlace completion or guard proof differs.')
        if status==1:
            require(hashlib.sha256(raw[HEADER:]).digest()==raw[96:128],
                'Deinterlace copied pixel digest differs.')
        result['sha256']=raw[96:128].hex()
    else:
        require(not any(raw[96:128]),'Unpublished deinterlace output carries a digest.')
    return result


def expected_output(source,session):
    """Return the WEAVE identity oracle; motion has no numerical pixel oracle."""
    preparation.session_check(session)
    require(len(source)==preparation.geometry_check(struct.unpack_from('<8I',session,856)) and
        hashlib.sha256(source).digest()==session[768:800],
        'Deinterlace source differs from sealed input.')
    return bytes(source) if struct.unpack_from('<I',session,876)[0]==1 else None

def ack(frame,session):
    require(decode(frame,session)['status']==1,'Cannot acknowledge absent deinterlace output.')
    return b'\1'+session[32:40]+frame[96:128]


def receipt_check(raw,session,copied_header):
    result=decode(raw,session)
    require(result['status']==3 and raw[16:HEADER]==copied_header[16:HEADER],
        'deinterlace ACK receipt changed copied identity or proofs.')
    return result


def capture(exchange,session,evidence,record,save,deadline,*,upload,send_upload,send_ack,
            now=time.monotonic,pause=time.sleep):
    preparation.session_check(session)
    require(upload==preparation.upload_bytes(upload[32:],session),'deinterlace upload bytes differ.')
    expected=expected_output(upload[32:],session)
    epoch,generation,lease=struct.unpack_from('<3Q',session,24)
    record.update(deinterlace_result='unobserved',deinterlace_retention='whole-board-power-removal',
        deinterlace_identity=dict(epoch=epoch,generation=generation,lease=lease,build_id=session[48:80].hex(),
            session_sha256=hashlib.sha256(session).hexdigest(),input_sha256=session[768:800].hex()),
        physical_qualified=False)
    save()
    try:
        require(now()<deadline,'deinterlace deadline before upload.')
        send_upload(upload)
        require(now()<deadline,'deinterlace upload exceeded deadline.')
        for sequence in range(1,801):
            require(now()<deadline,'deinterlace capture deadline expired.')
            frame=exchange(b'\0',min(500,max(1,int((deadline-now())*1000))))
            path=evidence/f'deinterlace-{sequence:04}.bin';supervisor.durable_frame(path,frame)
            entry=dict(path=path.name,sha256=hashlib.sha256(frame).hexdigest())
            record.setdefault('deinterlace_frames',[]).append(entry);save()
            result=decode(frame,session);entry.update(result)
            require(now()<deadline,'deinterlace response arrived after deadline.')
            if result['status']==0:
                pause(min(.025,max(0,deadline-now())));continue
            if result['status']==2:
                record['deinterlace_result']='observed-failure';record['durable_result']=entry.copy();save();return result
            require(result['status']==1,'Unexpected already-acknowledged deinterlace output.')
            output=evidence/'output.nv12';supervisor.durable_frame(output,frame[HEADER:])
            copied=dict(path=output.name,bytes=result['bytes'],sha256=result['sha256'],ack_sent=False,
                matches_cpu_reference=frame[HEADER:]==expected if expected is not None else None,
                pixel_reference_checked=expected is not None,wire_header_hex=frame[:HEADER].hex())
            record['deinterlace_output']=copied;save()
            if expected is not None and not copied['matches_cpu_reference']:
                result['matches_cpu_reference']=False
                record['deinterlace_result']='observed-failure';record['durable_result']=entry.copy();save();return result
            copied['ack_write_intent']=True;save()
            require(now()<deadline,'deinterlace copied output or ACK journal exceeded deadline.')
            send_ack(ack(frame,session));copied['ack_sent']=True;save()
            require(now()<deadline,'deinterlace ACK publication exceeded deadline.')
            receipt=exchange(b'\0',min(500,max(1,int((deadline-now())*1000))))
            terminal=receipt_check(receipt,session,frame[:HEADER])
            copied['ack_observed']=True
            record.setdefault('deinterlace_ack_receipt',{}).update(received_hex=receipt.hex(),sha256=hashlib.sha256(receipt).hexdigest())
            path=evidence/'deinterlace-ack-receipt.bin';supervisor.durable_frame(path,receipt)
            receipt_entry=dict(path=path.name,sha256=hashlib.sha256(receipt).hexdigest())
            record.setdefault('deinterlace_frames',[]).append(receipt_entry);save()
            receipt_entry.update(terminal)
            record['deinterlace_ack_receipt']['path']=path.name
            require(now()<deadline,'deinterlace ACK receipt exceeded deadline.')
            copied['ack_observed']=True
            record['deinterlace_result']='observed-success';record['durable_result']=receipt_entry.copy()
            try:save()
            except BaseException:
                record['deinterlace_result']='unobserved';record.pop('durable_result',None);raise
            result.update(matches_cpu_reference=True if expected is not None else None,
                pixel_reference_checked=expected is not None,ack_observed=True)
            return result
        raise supervisor.CaptureWindowExpired('deinterlace exhausted its bounded pending-query budget.')
    except BaseException as error:
        record['deinterlace_result']='unobserved';record.pop('durable_result',None)
        record['deinterlace_capture_failure']=dict(type=type(error).__name__,message=str(error))
        save()
        raise


def capture_device(device,same,deadline,session,evidence,record,save,*,upload,
                   now=time.monotonic,pause=time.sleep):
    same();require(now()<deadline,'deinterlace descriptor deadline expired.')
    endpoints=[ep for ep in device.get_active_configuration()[(0,0)] if ep.bEndpointAddress==0x81]
    require(len(endpoints)==1 and endpoints[0].bmAttributes&3==2 and
        endpoints[0].wMaxPacketSize in (64,512),'deinterlace bulk-IN packet geometry differs.')
    packet_size=endpoints[0].wMaxPacketSize
    record['deinterlace_bulk_in_packet_size']=packet_size
    def send(data,label):
        # The immutable upload already exists in the frozen bundle. One durable
        # intent binds its complete bytes; per-write progress stays in memory
        # and is saved on completion or by capture's failure path. This avoids
        # thousands of journal fsyncs inside the fixed native session lifetime.
        entry=dict(kind=label,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),accepted_bytes=0)
        record.setdefault('deinterlace_writes',[]).append(entry);save()
        for first in range(0,len(data),512):
            packet=data[first:first+512]
            entry.update(attempt_offset=first,attempt_bytes=len(packet),last_write_completed=False)
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'deinterlace request journal or write exceeded deadline.')
            count=device.write(0x02,packet,timeout=min(500,remaining))
            entry.update(last_write_completed=True,last_accepted_bytes=count)
            entry['accepted_bytes']+=count
            require(count==len(packet),'Incomplete deinterlace write.')
        if label=='ack':record['deinterlace_output']['ack_sent']=True
        save()
    def exchange(packet,timeout):
        send(packet,'query')
        frame=bytearray();expected=None
        receipt_fragments=[]
        def observe_receipt(received):
            copied=record.get('deinterlace_output',{})
            if not copied.get('ack_sent') or len(received)!=HEADER:return
            try:receipt_check(received,session,bytes.fromhex(copied['wire_header_hex']))
            except ValueError:return
            copied['ack_observed']=True
            record['deinterlace_ack_receipt']=dict(received_hex=bytes(received).hex(),
                sha256=hashlib.sha256(received).hexdigest(),fragments=receipt_fragments.copy())
        terminated=False
        def retain(fragment):
            path=evidence/f"fragment-{len(record.get('deinterlace_fragments',[]))+1:04}.bin"
            received=frame+fragment
            observe_receipt(received)
            supervisor.durable_frame(path,fragment)
            retained=dict(path=path.name,bytes=len(fragment),sha256=hashlib.sha256(fragment).hexdigest())
            record.setdefault('deinterlace_fragments',[]).append(retained)
            receipt_fragments.append(retained);observe_receipt(received);save()
        while expected is None or len(frame)<expected:
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'deinterlace response deadline expired.')
            maximum=16384 if expected is None else min(16384,
                ((expected-len(frame)+packet_size-1)//packet_size)*packet_size)
            fragment=bytes(device.read(0x81,maximum,timeout=min(timeout,remaining)))
            retain(fragment)
            require(0<len(fragment)<=maximum,'Empty or oversized deinterlace data fragment.')
            frame.extend(fragment)
            if expected is None and len(frame)>=HEADER:
                magic,version,status,count=struct.unpack_from('<4I',frame)
                require((magic,version)==(0x52494453,1) and status in (0,1,2,3) and
                    (0<count<=MAXIMUM if status==1 else count==0),'Invalid deinterlace response extent.')
                expected=HEADER+count
            require(expected is None or len(frame)<=expected,'deinterlace stream exceeds declared extent.')
            terminated=len(fragment)<maximum
            require(not terminated or expected is not None and len(frame)==expected,
                'deinterlace short packet or ZLP interrupted the response.')
        if not terminated:
            require(expected%packet_size==0,'deinterlace stream lacks its terminal short packet.')
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'deinterlace terminal ZLP deadline expired.')
            terminal=bytes(device.read(0x81,packet_size,timeout=min(timeout,remaining)))
            retain(terminal)
            require(not terminal,'deinterlace expected one terminal ZLP after its complete response.')
        record.setdefault('deinterlace_stream_completions',[]).append(dict(bytes=expected,
            packet_size=packet_size,terminal_zlp_separate=not terminated))
        save()
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline,upload=upload,
        send_upload=lambda data:send(data,'upload'),send_ack=lambda data:send(data,'ack'),now=now,pause=pause)
