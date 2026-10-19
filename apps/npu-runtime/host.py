#!/usr/bin/env python3
"""Bounded same-model runtime requests and copied numerical results."""
import hashlib,json,struct,time
MAGIC=0x53534f53
FRAME=0x415337334e505232
SUBMIT,READ,CLOSE=0x34,0x35,0x36
IDLE,UPLOAD,QUEUED,RUNNING,DONE,CLOSING,CLOSED,FAILED=range(8)
def packet(sequence,operation,prepared,request_id,timeout_us=0,data=b''):
 if not 0<sequence<2**32 or not 0<request_id<2**64:raise ValueError('Invalid runtime sequence/request identity')
 return struct.pack('<4I4Q2IQ',MAGIC,sequence,operation,48,prepared['epoch'],prepared['generation'],request_id,timeout_us,len(data),0,0)+data

def decode(frame,sequence,prepared,request_id):
 if len(frame)!=1040 or struct.unpack_from('<4I',frame)!=(MAGIC,sequence,0x80000034,1024):raise ValueError('Incomplete/different runtime response')
 raw=frame[16:];magic,version,state,epoch,generation,lease=struct.unpack_from('<6Q',raw)
 if (magic,version,epoch,generation,lease)!=(FRAME,1024<<32|1,prepared['epoch'],prepared['generation'],prepared['lease']):raise ValueError('Runtime response identity differs')
 if raw[48:80].hex()!=prepared['build_id'] or struct.unpack_from('<Q',raw,80)[0]!=request_id:raise ValueError('Runtime image/request differs')
 if hashlib.sha256(raw[:992]).digest()!=raw[992:]:raise ValueError('Runtime frame digest differs')
 count=struct.unpack_from('<Q',raw,232)[0]
 if count>512 or state not in (IDLE,QUEUED,RUNNING,DONE,CLOSED,FAILED):raise ValueError('Unsupported runtime result')
 result=dict(state=state,request_id=request_id,deadline=struct.unpack_from('<Q',raw,88)[0],model_sha256=raw[96:128].hex(),reason=struct.unpack_from('<Q',raw,752)[0])
 if state in (DONE,CLOSED):
  start,end,job,jobs,interrupts=struct.unpack_from('<5Q',raw,128)
  if not start<=end<result['deadline'] or interrupts!=jobs+1 or state==DONE and (not job or not count) or state==CLOSED and count:raise ValueError('Uncompleted runtime result')
  output=raw[240:240+count]
  if state==DONE and hashlib.sha256(output).digest()!=raw[200:232]:raise ValueError('Copied output digest differs')
  result.update(start=start,end=end,job=job,jobs=jobs,irq_completions=interrupts,input_sha256=raw[168:200].hex(),output=output)
 elif count or any(raw[128:752]):raise ValueError('Pending/failure carries numerical output')
 return result

def validate_inputs(inputs,oracles,model):
 if not inputs or len(inputs)>16 or any(len(x)!=model['input']['bytes'] for x in inputs):raise ValueError('Runtime inputs must match selected shape, at most16 jobs')
 if oracles and (len(oracles)!=len(inputs) or any(len(x)!=model['reference']['bytes'] for x in oracles)):raise ValueError('Optional oracle extent differs')
def capture_device(device,same,deadline,prepared,evidence,record,save,inputs,oracles,model,*,durable):
 validate_inputs(inputs,oracles,model)
 sequence=0
 def exchange(op,rid,data=b'',timeout_us=0):
  nonlocal sequence
  sequence+=1;request=packet(sequence,op,prepared,rid,timeout_us,data)
  same();left=int((deadline-time.monotonic())*1000)
  if left<=0:raise TimeoutError('Runtime host deadline')
  if device.write(2,request,timeout=min(left,5000))!=len(request):raise ValueError('Incomplete runtime upload')
  chunks=[];remaining=1040
  while remaining:
   same();left=int((deadline-time.monotonic())*1000)
   if left<=0:raise TimeoutError('Runtime response deadline')
   part=bytes(device.read(0x81,remaining,timeout=min(left,5000)))
   if not part or len(part)>remaining:raise ValueError('Incomplete runtime response')
   fragment=evidence/f'runtime-{sequence:03}-fragment-{len(chunks):03}.bin';durable(fragment,part)
   record.setdefault('runtime_fragments',[]).append({'path':fragment.name,'sha256':hashlib.sha256(part).hexdigest(),'sequence':sequence});save()
   chunks.append(part);remaining-=len(part)
  frame=b''.join(chunks);path=evidence/f'runtime-{sequence:03}.bin';durable(path,frame)
  record.setdefault('runtime_frames',[]).append({'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()});save()
  value=decode(frame,sequence,prepared,rid)
  if value['model_sha256']!=model['model']['sha256'] or value['state']==FAILED:raise ValueError('NPU runtime retained or model differs')
  return value
 outputs=[]
 for i,data in enumerate(inputs,1):
  durable(evidence/f'input-{i:02}.bin',data)
  exchange(SUBMIT,i,data,5000000)
  for _ in range(256):
   value=exchange(READ,i)
   if value['state']==DONE:break
   time.sleep(.005)
  else:raise TimeoutError('Runtime job remained pending')
  if value['input_sha256']!=hashlib.sha256(data).hexdigest() or len(value['output'])!=model['reference']['bytes'] or value['jobs']!=i:raise ValueError('Different runtime input/output/job count')
  durable(evidence/f'output-{i:02}.bin',value['output']);outputs.append(value['output'])
 value=exchange(CLOSE,len(inputs)+1,timeout_us=5000000)
 for _ in range(256):
  if value['state']==CLOSED:break
  time.sleep(.005);value=exchange(READ,len(inputs)+1)
 else:raise TimeoutError('Runtime CLOSE remained pending')
 if value['jobs']!=len(inputs):raise ValueError('CLOSE job count differs')
 oracle_match=None if not oracles else all(x==y for x,y in zip(outputs,oracles))
 record.update(npu_result=('observed-success' if oracle_match is not False else 'observed-mismatch'),runtime_jobs=len(inputs),runtime_close=True,oracle_match=oracle_match,manual_whole_board_power_removal_required=True);save()
 return dict(status='runtime-closed',jobs=len(inputs),oracle_match=oracle_match,normal_return=False)
