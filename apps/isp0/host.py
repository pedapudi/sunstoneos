#!/usr/bin/env python3
"""Capture one retained ISP0 operation with durable copied pixels before acknowledgment."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import time

ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
preparation=module('isp0_host_preparation',ROOT/'apps/isp0/prepare.py')
supervisor=module('isp0_host_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
require=supervisor.require
HEADER=192
MAXIMUM=614400


def decode(raw,session):
    preparation.session_check(session)
    require(len(raw)>=HEADER,'Short ISP0 response.')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    require((magic,version)==(0x30495053,1) and status in (0,1,2,3),'ISP0 response header differs.')
    require((epoch,generation)==struct.unpack_from('<2Q',session,24) and
        raw[32:64]==session[768:800] and raw[64:96]==hashlib.sha256(session).digest(),
        'ISP0 session, build, lease or input identity differs.')
    geometry=struct.unpack_from('<8I',raw,128)
    require(geometry==struct.unpack_from('<8I',session,848),'ISP0 sealed geometry differs.')
    complete,isp,vipp,dma,top,scanned,guards,copied=struct.unpack_from('<8I',raw,160)
    require(count==(MAXIMUM if status==1 else 0) and len(raw)==HEADER+count,'ISP0 copied extent differs.')
    result=dict(status=status,bytes=count,width=640,height=480,stride=640,
        format='NV16-Y-then-interleaved-UV',complete=complete,isp_status=isp,vipp_status=vipp,
        dma_status=dma,top_pattern_control=top,scanned_masters=scanned,guards=guards,
        copied_bytes=copied,physical_qualified=False,numerical_qualified=False)
    if status in (1,3):
        completion_check(complete,isp,vipp,dma,top,scanned,guards,copied)
        require(any(raw[96:128]),'ISP0 completed output has no digest.')
        if status==1:
            require(hashlib.sha256(raw[HEADER:]).digest()==raw[96:128],'ISP0 copied pixel digest differs.')
        result['sha256']=raw[96:128].hex()
    else:
        require(raw[96:128]==bytes(32),'ISP0 pending/failed result published an output digest.')
    return result


def completion_check(complete,isp,vipp,dma,top,scanned,guards,copied):
    require(complete==1 and scanned==6 and guards==1 and copied==MAXIMUM,
        'ISP0 completion, IOMMU, guard or copy proof differs.')
    require(isp&0xc==0xc and not isp&~0x1f and vipp==0x100 and dma&1 and not dma&~0xa3,
        'ISP0 ISP, VIPP or DMA completion status differs.')
    require(top&1 and not top&(0x10|0xff0000),'ISP0 TOP generator, START or cycle state differs.')


def ack(frame,session):
    require(decode(frame,session)['status']==1,'Cannot acknowledge absent ISP0 output.')
    return b'\1'+session[32:40]+frame[96:128]


def receipt_check(raw,session,copied_header):
    result=decode(raw,session)
    require(result['status']==3 and raw[16:HEADER]==copied_header[16:HEADER],
        'ISP0 ACK receipt changed copied identity or proofs.')
    return result


def capture(exchange,session,evidence,record,save,deadline,*,upload,send_upload,send_ack,
            now=time.monotonic,pause=time.sleep):
    preparation.session_check(session)
    require(upload==preparation.upload_bytes(upload[32:],session),'ISP0 upload bytes differ.')
    epoch,generation,lease=struct.unpack_from('<3Q',session,24)
    record.update(isp0_result='unobserved',isp0_retention='whole-board-power-removal',
        isp0_identity=dict(epoch=epoch,generation=generation,lease=lease,build_id=session[48:80].hex(),
            session_sha256=hashlib.sha256(session).hexdigest(),input_sha256=session[768:800].hex()),
        physical_qualified=False,numerical_qualified=False)
    save()
    try:
        require(now()<deadline,'ISP0 deadline before upload.')
        send_upload(upload)
        require(now()<deadline,'ISP0 upload exceeded deadline.')
        for sequence in range(1,801):
            require(now()<deadline,'ISP0 capture deadline expired.')
            frame=exchange(b'\0',min(500,max(1,int((deadline-now())*1000))))
            path=evidence/f'isp0-{sequence:04}.bin';supervisor.durable_frame(path,frame)
            entry=dict(path=path.name,sha256=hashlib.sha256(frame).hexdigest())
            record.setdefault('isp0_frames',[]).append(entry);save()
            result=decode(frame,session);entry.update(result)
            require(now()<deadline,'ISP0 response arrived after deadline.')
            if result['status']==0:
                pause(min(.025,max(0,deadline-now())));continue
            if result['status']==2:
                record['isp0_result']='observed-failure';record['durable_result']=entry.copy();save();return result
            require(result['status']==1,'Unexpected already-acknowledged ISP0 output.')
            output=evidence/'output.nv16';supervisor.durable_frame(output,frame[HEADER:])
            copied=dict(path=output.name,bytes=result['bytes'],sha256=result['sha256'],ack_sent=False,
                numerical_qualified=False,wire_header_hex=frame[:HEADER].hex())
            record['isp0_output']=copied;save()
            copied['ack_write_intent']=True;save()
            require(now()<deadline,'ISP0 copied output or ACK journal exceeded deadline.')
            send_ack(ack(frame,session));copied['ack_sent']=True;save()
            require(now()<deadline,'ISP0 ACK publication exceeded deadline.')
            receipt=exchange(b'\0',min(500,max(1,int((deadline-now())*1000))))
            terminal=receipt_check(receipt,session,frame[:HEADER])
            copied['ack_observed']=True
            record.setdefault('isp0_ack_receipt',{}).update(received_hex=receipt.hex(),sha256=hashlib.sha256(receipt).hexdigest())
            path=evidence/'isp0-ack-receipt.bin';supervisor.durable_frame(path,receipt)
            receipt_entry=dict(path=path.name,sha256=hashlib.sha256(receipt).hexdigest())
            record.setdefault('isp0_frames',[]).append(receipt_entry);save()
            receipt_entry.update(terminal)
            record['isp0_ack_receipt']['path']=path.name
            require(now()<deadline,'ISP0 ACK receipt exceeded deadline.')
            copied['ack_observed']=True
            record['isp0_result']='observed-success';record['durable_result']=receipt_entry.copy()
            try:save()
            except BaseException:
                record['isp0_result']='unobserved';record.pop('durable_result',None);raise
            result.update(ack_observed=True)
            return result
        raise supervisor.CaptureWindowExpired('ISP0 exhausted its bounded pending-query budget.')
    except BaseException as error:
        record['isp0_result']='unobserved';record.pop('durable_result',None)
        record['isp0_capture_failure']=dict(type=type(error).__name__,message=str(error))
        save()
        raise


def capture_device(device,same,deadline,session,evidence,record,save,*,upload,
                   now=time.monotonic,pause=time.sleep):
    same();require(now()<deadline,'ISP0 descriptor deadline expired.')
    endpoints=[ep for ep in device.get_active_configuration()[(0,0)] if ep.bEndpointAddress==0x81]
    require(len(endpoints)==1 and endpoints[0].bmAttributes&3==2 and
        endpoints[0].wMaxPacketSize in (64,512),'ISP0 bulk-IN packet geometry differs.')
    packet_size=endpoints[0].wMaxPacketSize
    record['isp0_bulk_in_packet_size']=packet_size
    def send(data,label):
        # The immutable upload already exists in the frozen bundle. One durable
        # intent binds its complete bytes; per-write progress stays in memory
        # and is saved on completion or by capture's failure path. This avoids
        # thousands of journal fsyncs inside the fixed native session lifetime.
        entry=dict(kind=label,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),accepted_bytes=0)
        record.setdefault('isp0_writes',[]).append(entry);save()
        for first in range(0,len(data),512):
            packet=data[first:first+512]
            entry.update(attempt_offset=first,attempt_bytes=len(packet),last_write_completed=False)
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'ISP0 request journal or write exceeded deadline.')
            count=device.write(0x02,packet,timeout=min(500,remaining))
            entry.update(last_write_completed=True,last_accepted_bytes=count)
            entry['accepted_bytes']+=count
            require(count==len(packet),'Incomplete ISP0 write.')
        if label=='ack':record['isp0_output']['ack_sent']=True
        save()
    def exchange(packet,timeout):
        send(packet,'query')
        frame=bytearray();expected=None
        receipt_fragments=[]
        def observe_receipt(received):
            copied=record.get('isp0_output',{})
            if not copied.get('ack_sent') or len(received)!=HEADER:return
            try:receipt_check(received,session,bytes.fromhex(copied['wire_header_hex']))
            except ValueError:return
            copied['ack_observed']=True
            record['isp0_ack_receipt']=dict(received_hex=bytes(received).hex(),
                sha256=hashlib.sha256(received).hexdigest(),fragments=receipt_fragments.copy())
        terminated=False
        def retain(fragment):
            path=evidence/f"fragment-{len(record.get('isp0_fragments',[]))+1:04}.bin"
            received=frame+fragment
            observe_receipt(received)
            supervisor.durable_frame(path,fragment)
            retained=dict(path=path.name,bytes=len(fragment),sha256=hashlib.sha256(fragment).hexdigest())
            record.setdefault('isp0_fragments',[]).append(retained)
            receipt_fragments.append(retained);observe_receipt(received);save()
        while expected is None or len(frame)<expected:
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'ISP0 response deadline expired.')
            maximum=16384 if expected is None else min(16384,
                ((expected-len(frame)+packet_size-1)//packet_size)*packet_size)
            fragment=bytes(device.read(0x81,maximum,timeout=min(timeout,remaining)))
            retain(fragment)
            require(0<len(fragment)<=maximum,'Empty or oversized ISP0 data fragment.')
            frame.extend(fragment)
            if expected is None and len(frame)>=HEADER:
                magic,version,status,count=struct.unpack_from('<4I',frame)
                require((magic,version)==(0x30495053,1) and status in (0,1,2,3) and
                    (0<count<=MAXIMUM if status==1 else count==0),'Invalid ISP0 response extent.')
                expected=HEADER+count
            require(expected is None or len(frame)<=expected,'ISP0 stream exceeds declared extent.')
            terminated=len(fragment)<maximum
            require(not terminated or expected is not None and len(frame)==expected,
                'ISP0 short packet or ZLP interrupted the response.')
        if not terminated:
            require(expected%packet_size==0,'ISP0 stream lacks its terminal short packet.')
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'ISP0 terminal ZLP deadline expired.')
            terminal=bytes(device.read(0x81,packet_size,timeout=min(timeout,remaining)))
            retain(terminal)
            require(not terminal,'ISP0 expected one terminal ZLP after its complete response.')
        record.setdefault('isp0_stream_completions',[]).append(dict(bytes=expected,
            packet_size=packet_size,terminal_zlp_separate=not terminated))
        save()
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline,upload=upload,
        send_upload=lambda data:send(data,'upload'),send_ack=lambda data:send(data,'ack'),now=now,pause=pause)
