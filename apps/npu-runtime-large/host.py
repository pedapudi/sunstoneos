#!/usr/bin/env python3
"""Bounded chunked FP32 input and copied output for the fixed SH/NT graph."""
import hashlib
import struct
import time
MAGIC=0x53534f53
FRAME=0x415337334e504c32
BEGIN,UPLOAD,COMMIT,READ,FINISH,CLOSE=0x37,0x38,0x39,0x3a,0x3b,0x36
IDLE,UPLOADING,QUEUED,RUNNING,DONE,CLOSING,CLOSED,FAILED=range(8)
CHUNK=512


def packet(sequence,operation,prepared,request_id,timeout_us=0,data=b'',offset=0,total=0):
    if not 0<sequence<2**32 or not 0<request_id<2**64 or not 0<=offset<2**64:
        raise ValueError('Invalid sequence, request or offset')
    count=total if operation==BEGIN else len(data)
    return struct.pack('<4I4Q2IQ',MAGIC,sequence,operation,48,prepared['epoch'],
        prepared['generation'],request_id,timeout_us,count,0,offset)+data


def decode(frame,sequence,prepared,request_id):
    if len(frame)!=1040 or struct.unpack_from('<4I',frame)!=(MAGIC,sequence,0x80000034,1024):
        raise ValueError('Incomplete or different large runtime frame')
    raw=frame[16:]
    magic,version,state,epoch,generation,lease=struct.unpack_from('<6Q',raw)
    if (magic,version,epoch,generation,lease)!=(FRAME,1024<<32|2,prepared['epoch'],prepared['generation'],prepared['lease']):
        raise ValueError('Large runtime session differs')
    if raw[48:80].hex()!=prepared['build_id'] or struct.unpack_from('<Q',raw,80)[0]!=request_id:
        raise ValueError('Large runtime image or request differs')
    if hashlib.sha256(raw[:992]).digest()!=raw[992:]: raise ValueError('Large frame digest differs')
    total,offset,count=struct.unpack_from('<3Q',raw,232)
    if state not in (UPLOADING,QUEUED,RUNNING,DONE,CLOSED,FAILED) or count>CHUNK or offset>total or count>total-offset:
        raise ValueError('Invalid copied output extent or state')
    if any(raw[256+count:768]) or any(raw[800:992]): raise ValueError('Invalid large frame padding')
    reason,input_bytes,received,delivered=struct.unpack_from('<4Q',raw,768)
    result=dict(state=state,request_id=request_id,deadline=struct.unpack_from('<Q',raw,88)[0],
        model_sha256=raw[96:128].hex(),reason=reason,total=total,offset=offset,
        output=raw[256:256+count],input_bytes=input_bytes,received=received,delivered=delivered)
    if state in (DONE,CLOSED):
        start,end,job,jobs,interrupts=struct.unpack_from('<5Q',raw,128)
        if not start<=end<result['deadline'] or interrupts!=jobs+1 or state==DONE and (not job or not total) or state==CLOSED and (count or total):
            raise ValueError('Uncompleted large result')
        result.update(start=start,end=end,job=job,jobs=jobs,irq_completions=interrupts,
            input_sha256=raw[168:200].hex(),output_sha256=raw[200:232].hex())
    elif total or offset or count or any(raw[128:232]): raise ValueError('Pending or failed frame carries output')
    return result


def validate_inputs(inputs,oracles,model):
    if model.get('family')!='sh-nt' or not inputs or len(inputs)>16 or any(len(x)!=model['input']['bytes'] for x in inputs):
        raise ValueError('Inputs must match the fixed SH/NT graph')
    if oracles and (len(oracles)!=len(inputs) or any(len(x)!=model['reference']['bytes'] for x in oracles)):
        raise ValueError('Optional oracle extent differs')


def capture_device(device,same,deadline,prepared,evidence,record,save,inputs,oracles,model,*,durable):
    validate_inputs(inputs,oracles,model)
    sequence=0
    def exchange(operation,rid,data=b'',offset=0,timeout_us=0,total=0):
        nonlocal sequence
        sequence+=1
        request=packet(sequence,operation,prepared,rid,timeout_us,data,offset,total)
        same();left=int((deadline-time.monotonic())*1000)
        if left<=0: raise TimeoutError('Large host deadline')
        if device.write(2,request,timeout=min(left,5000))!=len(request): raise ValueError('Incomplete large request')
        parts=[];remaining=1040
        while remaining:
            same();left=int((deadline-time.monotonic())*1000)
            if left<=0: raise TimeoutError('Large response deadline')
            part=bytes(device.read(0x81,remaining,timeout=min(left,5000)))
            if not part or len(part)>remaining: raise ValueError('Incomplete large response')
            path=evidence/f'large-{sequence:05}-fragment-{len(parts):03}.bin';durable(path,part)
            record.setdefault('runtime_fragments',[]).append(dict(path=path.name,sha256=hashlib.sha256(part).hexdigest(),sequence=sequence));save()
            parts.append(part);remaining-=len(part)
        frame=b''.join(parts);value=decode(frame,sequence,prepared,rid)
        if value['state']==FAILED or value['model_sha256']!=model['model']['sha256']:
            raise ValueError('Large runtime retained or model differs')
        return value
    outputs=[]
    for rid,data in enumerate(inputs,1):
        durable(evidence/f'input-{rid:02}.bin',data)
        value=exchange(BEGIN,rid,timeout_us=5000000,total=len(data))
        if value['state']!=UPLOADING or value['received']: raise ValueError('Input begin was not admitted')
        for offset in range(0,len(data),CHUNK):
            part=data[offset:offset+CHUNK];value=exchange(UPLOAD,rid,part,offset)
            if value['state']!=UPLOADING or value['received']!=offset+len(part): raise ValueError('Input chunk was not copied')
        value=exchange(COMMIT,rid)
        for _ in range(256):
            if value['state']==DONE: break
            value=exchange(READ,rid)
            time.sleep(.005)
        else: raise TimeoutError('Large graph remained pending')
        chunks=[];offset=0;identity=None
        while offset<model['reference']['bytes']:
            if value['state']!=DONE or not value['output'] or value['offset']!=offset:
                value=exchange(READ,rid,offset=offset)
            if value['state']!=DONE or value['total']!=model['reference']['bytes'] or value['offset']!=offset or not value['output']:
                raise ValueError('Different large output chunk')
            current=(value['job'],value['jobs'],value['irq_completions'],value['input_sha256'],value['output_sha256'])
            if (identity is not None and identity!=current) or value['input_sha256']!=hashlib.sha256(data).hexdigest() or value['jobs']!=rid:
                raise ValueError('Large output identity changed')
            identity=current;part=value['output']
            durable(evidence/f'output-{rid:02}-{offset:08}.bin',part);chunks.append(part);offset+=len(part)
        output=b''.join(chunks)
        if hashlib.sha256(output).hexdigest()!=value['output_sha256']: raise ValueError('Copied full output digest differs')
        durable(evidence/f'output-{rid:02}.bin',output);outputs.append(output)
        exchange(FINISH,rid)
    rid=len(inputs)+1;value=exchange(CLOSE,rid,timeout_us=5000000)
    for _ in range(256):
        if value['state']==CLOSED: break
        time.sleep(.005);value=exchange(READ,rid)
    else: raise TimeoutError('Large CLOSE remained pending')
    if value['jobs']!=len(inputs): raise ValueError('CLOSE job count differs')
    match=None if not oracles else all(x==y for x,y in zip(outputs,oracles))
    record.update(npu_result='observed-success' if match is not False else 'observed-mismatch',
        runtime_jobs=len(inputs),runtime_close=True,oracle_match=match,
        manual_whole_board_power_removal_required=True);save()
    return dict(status='runtime-closed',jobs=len(inputs),oracle_match=match,normal_return=False)
