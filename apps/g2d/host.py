#!/usr/bin/env python3
"""Capture one retained G2D operation with durable copied pixels before acknowledgment."""
import hashlib
import importlib.util
from pathlib import Path
from fractions import Fraction
import struct
import time

ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
preparation=module('g2d_host_preparation',ROOT/'apps/g2d/prepare.py')
supervisor=module('g2d_host_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
require=supervisor.require
HEADER=192
MAXIMUM=3686400
FORMATS=('ARGB8888','ABGR8888','RGBA8888','BGRA8888','XRGB8888','XBGR8888','RGBX8888','BGRX8888')
OPERATIONS={1:'fill',2:'copy',3:'source-over-solid',4:'scale2x',5:'scale',6:'source-over-image'}
# Bit positions of A,R,G,B in a little-endian word for ARGB, ABGR, RGBA and BGRA orders.
CHANNELS=((24,16,8,0),(24,0,8,16),(0,24,16,8),(0,8,16,24))


def decode(raw,session):
    preparation.session_check(session)
    require(len(raw)>=HEADER,'Short G2D response.')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    require((magic,version)==(0x44324753,1) and status in (0,1,2,3),'G2D response header differs.')
    require((epoch,generation)==struct.unpack_from('<2Q',session,24) and
        raw[32:64]==session[768:800] and raw[64:96]==hashlib.sha256(session).digest(),
        'G2D session, build, lease or input identity differs.')
    geometry=struct.unpack_from('<12I',raw,128)
    require(geometry==struct.unpack_from('<12I',session,856),'G2D operation geometry differs.')
    complete,rcq,scanned,guards=struct.unpack_from('<4I',raw,176)
    size=geometry[1]*geometry[2]
    require(count==(size if status==1 else 0) and len(raw)==HEADER+count,'G2D copied extent differs.')
    output_format=struct.unpack_from('<I',session,820)[0]
    result=dict(status=status,bytes=count,width=geometry[0],height=geometry[1],stride=geometry[2],
        format=FORMATS[geometry[3]]+'-little-endian-word',output_format=FORMATS[output_format]+'-little-endian-word',
        operation=OPERATIONS[geometry[4]],
        complete=complete,rcq_status=rcq,scanned_masters=scanned,guards=guards,physical_qualified=False)
    if status in (1,3):
        require(complete==1 and rcq&1 and scanned==0x400 and guards==1,'G2D completion or guard proof differs.')
        if status==1:
            require(hashlib.sha256(raw[HEADER:]).digest()==raw[96:128],'G2D copied pixel digest differs.')
        result['sha256']=raw[96:128].hex()
    if geometry[4] in (3,6):result['arithmetic_scope']='Ideal straight-alpha /255 nearest reference; hardware rounding unqualified.'
    if geometry[4] in (4,5):result['arithmetic_scope']='Scaling interpolation unchecked; no qualified numerical oracle.'
    if diagnostic(session) and geometry[4]==2:result['arithmetic_scope']='Ideal channel reordering; X-channel handling and conversion unqualified.'
    return result


def diagnostic(session):
    """Whether a CPU reference is an unqualified diagnostic that does not gate ACK."""
    geometry,extension=preparation.session_geometry(session)
    return geometry[4] in (3,6) or (geometry[4]==2 and bool(geometry[3] or extension[5]))


def unpack_pixel(word,format_):
    """Return A,R,G,B from one little-endian 32-bit word; X formats read as opaque."""
    a,r,g,b=[(word>>shift)&255 for shift in CHANNELS[format_&3]]
    return (255 if format_>=4 else a),r,g,b


def pack_pixel(argb,format_):
    """Pack A,R,G,B; the ideal reference writes X channels as 255."""
    a,r,g,b=argb;shifts=CHANNELS[format_&3]
    return struct.pack('<I',((255 if format_>=4 else a)<<shifts[0])|(r<<shifts[1])|(g<<shifts[2])|(b<<shifts[3]))


def over(foreground,background,plane=255):
    """Ideal straight-alpha SRCOVER of two A,R,G,B pixels, rounded to nearest."""
    fa=Fraction(foreground[0]*plane,255*255);ba=Fraction(background[0],255)
    out=fa+ba*(1-fa)
    if not out:return (0,0,0,0)
    color=[(foreground[c]*fa+background[c]*ba*(1-fa))/out for c in (1,2,3)]
    return tuple(int(value+Fraction(1,2)) for value in (out*255,*color))


def expected_output(source,session):
    """Fill/copy bytes, ideal conversion or source-over bytes, or None for scaling.

    Blending rounds the ideal /255 result to nearest. Hardware quantization is
    unqualified; modeled pixels compared here do not qualify device arithmetic.
    Scaling has no numerical oracle and must not imply a pixel comparison."""
    preparation.session_check(session)
    (width,height,stride,format_,operation,dx,dy,w,h,sx,sy,color),extension=preparation.session_geometry(session)
    bx,by,alpha,output_format=extension[2:6]
    require(len(source)==height*stride and hashlib.sha256(source).digest()==session[768:800],
        'G2D reference source differs from sealed input.')
    if operation in (4,5):return None
    out=bytearray(len(source))
    row=struct.pack('<I',color)*w if operation==1 else None
    def pixel(at,format_=0):return unpack_pixel(struct.unpack_from('<I',source,at)[0],format_)
    for y in range(h):
        target=(dy+y)*stride+dx*4
        start=(sy+y)*stride+sx*4
        if operation==3:
            background=struct.pack('<I',color)
            for x in range(w):
                pixel_=source[start+x*4:start+x*4+4];alpha_=pixel_[3]
                out[target+x*4:target+x*4+4]=bytes(
                    [(pixel_[c]*alpha_+background[c]*(255-alpha_)+127)//255 for c in range(3)]+[255])
        elif operation==6:
            under=(by+y)*stride+bx*4
            for x in range(w):
                out[target+x*4:target+x*4+4]=pack_pixel(over(pixel(start+x*4),pixel(under+x*4),alpha),0)
        elif operation==2 and (format_ or output_format):
            for x in range(w):
                out[target+x*4:target+x*4+4]=pack_pixel(pixel(start+x*4,format_),output_format)
        else:out[target:target+w*4]=row if operation==1 else source[start:start+w*4]
    return bytes(out)


def untouched_zero(pixels,session):
    """Check exact zero bytes outside the written rectangle, including padding."""
    preparation.session_check(session)
    _,height,stride,_,_,dx,dy,w,h,_,_,_=struct.unpack_from('<12I',session,856)
    require(len(pixels)==height*stride,'G2D untouched-byte extent differs.')
    for y in range(height):
        start=y*stride
        if dy<=y<dy+h:
            if any(pixels[start:start+dx*4]) or any(pixels[start+(dx+w)*4:start+stride]):return False
        elif any(pixels[start:start+stride]):return False
    return True


def ack(frame,session):
    require(decode(frame,session)['status']==1,'Cannot acknowledge absent G2D output.')
    return b'\1'+session[32:40]+frame[96:128]


def receipt_check(raw,session,copied_header):
    result=decode(raw,session)
    require(result['status']==3 and raw[16:HEADER]==copied_header[16:HEADER],
        'G2D ACK receipt changed copied identity or proofs.')
    return result


def capture(exchange,session,evidence,record,save,deadline,*,upload,send_upload,send_ack,
            now=time.monotonic,pause=time.sleep):
    preparation.session_check(session)
    require(upload==preparation.upload_bytes(upload[32:],session),'G2D upload bytes differ.')
    expected=expected_output(upload[32:],session)
    epoch,generation,lease=struct.unpack_from('<3Q',session,24)
    record.update(g2d_result='unobserved',g2d_retention='whole-board-power-removal',
        g2d_identity=dict(epoch=epoch,generation=generation,lease=lease,build_id=session[48:80].hex(),
            session_sha256=hashlib.sha256(session).hexdigest(),input_sha256=session[768:800].hex()),
        physical_qualified=False)
    save()
    try:
        require(now()<deadline,'G2D deadline before upload.')
        send_upload(upload)
        require(now()<deadline,'G2D upload exceeded deadline.')
        for sequence in range(1,801):
            require(now()<deadline,'G2D capture deadline expired.')
            frame=exchange(b'\0',min(500,max(1,int((deadline-now())*1000))))
            path=evidence/f'g2d-{sequence:04}.bin';supervisor.durable_frame(path,frame)
            entry=dict(path=path.name,sha256=hashlib.sha256(frame).hexdigest())
            record.setdefault('g2d_frames',[]).append(entry);save()
            result=decode(frame,session);entry.update(result)
            require(now()<deadline,'G2D response arrived after deadline.')
            if result['status']==0:
                pause(min(.025,max(0,deadline-now())));continue
            if result['status']==2:
                record['g2d_result']='observed-failure';record['durable_result']=entry.copy();save();return result
            require(result['status']==1,'Unexpected already-acknowledged G2D output.')
            output=evidence/'output.argb';supervisor.durable_frame(output,frame[HEADER:])
            copied=dict(path=output.name,bytes=result['bytes'],sha256=result['sha256'],ack_sent=False,
                matches_cpu_reference=None if expected is None else frame[HEADER:]==expected,wire_header_hex=frame[:HEADER].hex())
            record['g2d_output']=copied;save()
            if expected is None:
                copied['reference_qualified']=False
                copied['reference_comparison']='unchecked-no-interpolation-oracle'
                copied['untouched_bytes_zero']=untouched_zero(frame[HEADER:],session)
                result.update(matches_cpu_reference=None,reference_qualified=False,
                    reference_comparison=copied['reference_comparison'],untouched_bytes_zero=copied['untouched_bytes_zero'])
                if not copied['untouched_bytes_zero']:
                    record['g2d_result']='observed-failure';record['durable_result']=entry.copy();save();return result
                save()
            elif diagnostic(session):
                copied['reference_qualified']=False
                copied['reference_comparison']='matches-ideal' if copied['matches_cpu_reference'] else 'differs-from-ideal'
                save()
            elif not copied['matches_cpu_reference']:
                result['matches_cpu_reference']=False
                record['g2d_result']='observed-failure';record['durable_result']=entry.copy();save();return result
            copied['ack_write_intent']=True;save()
            require(now()<deadline,'G2D copied output or ACK journal exceeded deadline.')
            send_ack(ack(frame,session));copied['ack_sent']=True;save()
            require(now()<deadline,'G2D ACK publication exceeded deadline.')
            receipt=exchange(b'\0',min(500,max(1,int((deadline-now())*1000))))
            terminal=receipt_check(receipt,session,frame[:HEADER])
            copied['ack_observed']=True
            record.setdefault('g2d_ack_receipt',{}).update(received_hex=receipt.hex(),sha256=hashlib.sha256(receipt).hexdigest())
            path=evidence/'g2d-ack-receipt.bin';supervisor.durable_frame(path,receipt)
            receipt_entry=dict(path=path.name,sha256=hashlib.sha256(receipt).hexdigest())
            record.setdefault('g2d_frames',[]).append(receipt_entry);save()
            receipt_entry.update(terminal)
            record['g2d_ack_receipt']['path']=path.name
            require(now()<deadline,'G2D ACK receipt exceeded deadline.')
            copied['ack_observed']=True
            record['g2d_result']='observed-success';record['durable_result']=receipt_entry.copy()
            try:save()
            except BaseException:
                record['g2d_result']='unobserved';record.pop('durable_result',None);raise
            result.update(matches_cpu_reference=copied['matches_cpu_reference'],ack_observed=True)
            if 'reference_qualified' in copied:result['reference_qualified']=False
            if expected is None:result['reference_comparison']=copied['reference_comparison']
            return result
        raise supervisor.CaptureWindowExpired('G2D exhausted its bounded pending-query budget.')
    except BaseException as error:
        record['g2d_result']='unobserved';record.pop('durable_result',None)
        record['g2d_capture_failure']=dict(type=type(error).__name__,message=str(error))
        save()
        raise


def capture_device(device,same,deadline,session,evidence,record,save,*,upload,
                   now=time.monotonic,pause=time.sleep):
    same();require(now()<deadline,'G2D descriptor deadline expired.')
    endpoints=[ep for ep in device.get_active_configuration()[(0,0)] if ep.bEndpointAddress==0x81]
    require(len(endpoints)==1 and endpoints[0].bmAttributes&3==2 and
        endpoints[0].wMaxPacketSize in (64,512),'G2D bulk-IN packet geometry differs.')
    packet_size=endpoints[0].wMaxPacketSize
    record['g2d_bulk_in_packet_size']=packet_size
    def send(data,label):
        # The immutable upload already exists in the frozen bundle. One durable
        # intent binds its complete bytes; per-write progress stays in memory
        # and is saved on completion or by capture's failure path. This avoids
        # thousands of journal fsyncs inside the fixed native session lifetime.
        entry=dict(kind=label,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),accepted_bytes=0)
        record.setdefault('g2d_writes',[]).append(entry);save()
        for first in range(0,len(data),512):
            packet=data[first:first+512]
            entry.update(attempt_offset=first,attempt_bytes=len(packet),last_write_completed=False)
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'G2D request journal or write exceeded deadline.')
            count=device.write(0x02,packet,timeout=min(500,remaining))
            entry.update(last_write_completed=True,last_accepted_bytes=count)
            entry['accepted_bytes']+=count
            require(count==len(packet),'Incomplete G2D write.')
        if label=='ack':record['g2d_output']['ack_sent']=True
        save()
    def exchange(packet,timeout):
        send(packet,'query')
        frame=bytearray();expected=None
        receipt_fragments=[]
        def observe_receipt(received):
            copied=record.get('g2d_output',{})
            if not copied.get('ack_sent') or len(received)!=HEADER:return
            try:receipt_check(received,session,bytes.fromhex(copied['wire_header_hex']))
            except ValueError:return
            copied['ack_observed']=True
            record['g2d_ack_receipt']=dict(received_hex=bytes(received).hex(),
                sha256=hashlib.sha256(received).hexdigest(),fragments=receipt_fragments.copy())
        terminated=False
        def retain(fragment):
            path=evidence/f"fragment-{len(record.get('g2d_fragments',[]))+1:04}.bin"
            received=frame+fragment
            observe_receipt(received)
            supervisor.durable_frame(path,fragment)
            retained=dict(path=path.name,bytes=len(fragment),sha256=hashlib.sha256(fragment).hexdigest())
            record.setdefault('g2d_fragments',[]).append(retained)
            receipt_fragments.append(retained);observe_receipt(received);save()
        while expected is None or len(frame)<expected:
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'G2D response deadline expired.')
            maximum=16384 if expected is None else min(16384,
                ((expected-len(frame)+packet_size-1)//packet_size)*packet_size)
            fragment=bytes(device.read(0x81,maximum,timeout=min(timeout,remaining)))
            retain(fragment)
            require(0<len(fragment)<=maximum,'Empty or oversized G2D data fragment.')
            frame.extend(fragment)
            if expected is None and len(frame)>=HEADER:
                magic,version,status,count=struct.unpack_from('<4I',frame)
                require((magic,version)==(0x44324753,1) and status in (0,1,2,3) and
                    (0<count<=MAXIMUM if status==1 else count==0),'Invalid G2D response extent.')
                expected=HEADER+count
            require(expected is None or len(frame)<=expected,'G2D stream exceeds declared extent.')
            terminated=len(fragment)<maximum
            require(not terminated or expected is not None and len(frame)==expected,
                'G2D short packet or ZLP interrupted the response.')
        if not terminated:
            require(expected%packet_size==0,'G2D stream lacks its terminal short packet.')
            same();remaining=int((deadline-now())*1000)
            require(remaining>0,'G2D terminal ZLP deadline expired.')
            terminal=bytes(device.read(0x81,packet_size,timeout=min(timeout,remaining)))
            retain(terminal)
            require(not terminal,'G2D expected one terminal ZLP after its complete response.')
        record.setdefault('g2d_stream_completions',[]).append(dict(bytes=expected,
            packet_size=packet_size,terminal_zlp_separate=not terminated))
        save()
        return bytes(frame)
    return capture(exchange,session,evidence,record,save,deadline,upload=upload,
        send_upload=lambda data:send(data,'upload'),send_ack=lambda data:send(data,'ack'),now=now,pause=pause)
