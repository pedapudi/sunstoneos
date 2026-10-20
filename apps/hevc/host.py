#!/usr/bin/env python3
"""Retain copied HEVC pictures before ACK; hardware and numerical proof remain separate."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import time

ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
preparation=module('hevc_host_preparation',ROOT/'apps/hevc/prepare.py')
decoder=module('hevc_host_decoder',ROOT/'apps/hevc/observe.py')
supervisor=module('hevc_host_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
require=supervisor.require


def capture(exchange,session,evidence,record,save,deadline,*,upload,send_upload,send_ack,
            now=time.monotonic,pause=time.sleep):
    preparation.session_check(session)
    require(upload==preparation.upload_bytes(upload[32:],None,session),'HEVC upload identity differs.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    record.update(hevc_retention='whole-board-power-removal',hevc_result='unobserved',
        numerical_qualified=False,hevc_outputs=[],hevc_identity=dict(epoch=epoch,generation=generation,
        session_sha256=hashlib.sha256(session).hexdigest(),clip_sha256=session[768:800].hex()))
    save()
    try:
        require(now()<deadline,'HEVC deadline expired before upload.')
        send_upload(upload)
        require(now()<deadline,'HEVC upload completed after deadline.')
        for ordinal in range(2):
            for sequence in range(2400):
                require(now()<deadline,'HEVC copied-output deadline expired.')
                frame=exchange(decoder.query(ordinal),min(500,max(1,int((deadline-now())*1000))))
                path=evidence/f'picture-{ordinal}-{sequence:04}.bin'
                supervisor.durable_frame(path,frame)
                entry=dict(path=path.name,sha256=hashlib.sha256(frame).hexdigest())
                record.setdefault('hevc_frames',[]).append(entry);save()
                result=decoder.decode(frame,session,ordinal);entry.update(result)
                require(now()<deadline,'HEVC result arrived after deadline.')
                if result['status']==0:
                    pause(min(.025,max(0,deadline-now())));continue
                if result['status']==1:
                    output=evidence/f'picture-{ordinal}.nv12'
                    supervisor.durable_frame(output,frame[decoder.HEADER:])
                    record['hevc_outputs'].append(dict(ordinal=ordinal,poc=result['poc'],
                        path=output.name,bytes=result['bytes'],sha256=result['sha256'],ack_sent=False))
                    save()
                if result['status']!=1 or result['terminal']:
                    record['hevc_result']='observed-failure';record['durable_result']=entry.copy();save()
                    return dict(status=2,ordinal=ordinal,copied=bool(result['status']==1),numerical_qualified=False)
                require(now()<deadline,'HEVC output or record became durable after deadline.')
                output_record=record['hevc_outputs'][-1]
                output_record['ack_write_intent']=True;save()
                require(now()<deadline,'HEVC ACK journal exceeded deadline.')
                send_ack(decoder.ack(frame,session,ordinal))
                output_record['ack_sent']=True;save()
                require(now()<deadline,'HEVC ACK publication exceeded deadline.')
                break
            else:raise supervisor.CaptureWindowExpired('HEVC exhausted bounded query budget.')
        record['hevc_result']='observed-success';record['durable_result']=entry.copy()
        try:save()
        except BaseException:
            record['hevc_result']='unobserved';record.pop('durable_result',None);raise
        return dict(status=1,pictures=record['hevc_outputs'],numerical_qualified=False)
    except BaseException as error:
        record['hevc_result']='unobserved';record.pop('durable_result',None)
        record['hevc_capture_failure']=dict(type=type(error).__name__,message=str(error))
        save()
        raise


def capture_device(device,same,deadline,session,evidence,record,save,*,upload,
                   now=time.monotonic,pause=time.sleep):
    def send(data,label):
        for first in range(0,len(data),512):
            same();require(now()<deadline,'HEVC write deadline expired.')
            packet=data[first:first+512]
            entry=dict(kind=label,offset=first,bytes=len(packet),sha256=hashlib.sha256(packet).hexdigest())
            record.setdefault('hevc_writes',[]).append(entry);save()
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'HEVC write journal exceeded deadline.')
            count=device.write(0x02,packet,timeout=min(500,remaining))
            entry['accepted_bytes']=count
            require(count==len(packet),'Incomplete HEVC write.')
            # A completed ACK write remains recorded if later publication fails.
            if label=='ack':
                record['hevc_outputs'][-1]['ack_sent']=True
                save()
    def exchange(packet,timeout):
        send(packet,'query')
        frame=bytearray();expected=None
        while expected is None or len(frame)<expected:
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'HEVC response deadline expired.')
            maximum=min((decoder.HEADER if expected is None else expected)-len(frame),16384)
            fragment=bytes(device.read(0x81,maximum,timeout=min(timeout,remaining)))
            path=evidence/f"fragment-{len(record.get('hevc_fragments',[]))+1:04}.bin"
            supervisor.durable_frame(path,fragment)
            record.setdefault('hevc_fragments',[]).append(dict(path=path.name,bytes=len(fragment),sha256=hashlib.sha256(fragment).hexdigest()));save()
            require(0<len(fragment)<=maximum,'Empty or oversized HEVC response fragment.')
            frame.extend(fragment)
            if expected is None and len(frame)==decoder.HEADER:
                magic,version,status,count=struct.unpack_from('<4I',frame)
                require((magic,version)==(0x44363235,1) and status in (0,1,2) and
                    count==(decoder.MAXIMUM if status==1 else 0),'Invalid HEVC frame extent.')
                expected=decoder.HEADER+count
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline,upload=upload,
        send_upload=lambda data:send(data,'upload'),send_ack=lambda data:send(data,'ack'),
        now=now,pause=pause)
