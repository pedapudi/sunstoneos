#!/usr/bin/env python3
"""Capture one admitted supervised NPU board session; retain resources until full power removal.

Drives the supervised capture flow of tools/check-a733-smp-supervised.py with
the NPU board image's USB0 protocol (apps/npu-board/usb.c): STATUS, LOAD of
the bundle's catalogue model, UPLOAD of each input tensor, SUBMIT, READ of
each output tensor, RETURN and CLOSE. Every response is retained durably
before it is parsed. The outputs are compared with the bundle's references;
a mismatch keeps the copied outputs and refuses success. Talks to the board:
only the coordinator runs it, on a claimed queue entry.
"""
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
supervisor=module('npu_board_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('npu_board_capture_preparation',ROOT/'apps/npu-board/prepare.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','npu-board-supervised-software'}
PRODUCT='sunstoneos native NPU'
SERIAL='sunstoneos-a733-npu-board'
REQUEST_MAGIC,RESPONSE_MAGIC=0x3151424e,0x3152424e
STATUS,LOAD,UPLOAD,SUBMIT,READ,RETURN,CLOSE=range(1,8)
OK,NOT_READY=1,4
HEADER=64
CHUNK=4096

def request(operation,sequence,epoch,generation,argument=0,offset=0,payload=b''):
    return struct.pack('<4I2Q2I',REQUEST_MAGIC,operation,sequence,len(payload),epoch,generation,argument,offset)+bytes(24)+payload

def capture_device(device,same,deadline,session,evidence,record,save,*,prepared,now=time.monotonic,pause=time.sleep):
    same();require(now()<deadline,'NPU board descriptor deadline expired.')
    endpoints=[ep for ep in device.get_active_configuration()[(0,0)] if ep.bEndpointAddress==0x81]
    require(len(endpoints)==1 and endpoints[0].bmAttributes&3==2 and endpoints[0].wMaxPacketSize in (64,512),
        'NPU board bulk-IN packet geometry differs.')
    packet_size=endpoints[0].wMaxPacketSize
    epoch,generation=struct.unpack_from('<2Q',session,24)
    frozen=prepared['_frozen'];sequence=0;fragments=record.setdefault('npu_board_fragments',[])
    def remaining():
        same();left=int((deadline-now())*1000);require(left>0,'NPU board capture deadline expired.');return left
    def send(data):
        for first in range(0,len(data),512):
            packet=data[first:first+512];count=device.write(0x02,packet,timeout=min(500,remaining()))
            require(count==len(packet),'Incomplete NPU board write.')
    def exchange(operation,argument=0,offset=0,payload=b'',not_ready=False):
        nonlocal sequence
        sequence+=1;send(request(operation,sequence,epoch,generation,argument,offset,payload))
        frame=bytearray();expected=HEADER
        while len(frame)<expected:
            maximum=min(16384,((expected-len(frame)+packet_size-1)//packet_size)*packet_size)
            fragment=bytes(device.read(0x81,maximum,timeout=min(2000,remaining())))
            path=evidence/f'fragment-{len(fragments)+1:05}.bin';supervisor.durable_frame(path,fragment)
            fragments.append(dict(path=path.name,bytes=len(fragment),sha256=hashlib.sha256(fragment).hexdigest()));save()
            require(0<len(fragment)<=maximum,'Empty or oversized NPU board fragment.')
            frame.extend(fragment)
            if len(frame)>=HEADER and expected==HEADER:
                magic,op,seq,result,state,window,model,count=struct.unpack_from('<8I',frame)
                require((magic,op,seq)==(RESPONSE_MAGIC,operation,sequence) and struct.unpack_from('<2Q',frame,32)==(epoch,generation),
                    'NPU board response does not answer this request.')
                require(result==OK or (not_ready and result==NOT_READY),f'NPU board refused operation {operation}: result {result}.')
                require((operation==READ and count>0) or (operation!=READ and count==0),'Invalid NPU board response extent.')
                expected=HEADER+count
            require(len(frame)<=expected,'NPU board stream exceeds its declared extent.')
        return bytes(frame)
    # The image answers not-ready until its client has opened the NPU.
    while True:
        status=exchange(STATUS,not_ready=True)
        if struct.unpack_from('<I',status,12)[0]==OK:break
        remaining();pause(0.01)
    record['npu_board_status_hex']=status[:HEADER].hex();save()
    loaded=exchange(LOAD,prepared['model']);inputs,outputs=divmod(struct.unpack_from('<I',loaded,56)[0],1<<16)
    require(inputs==prepared['inputs'] and outputs==prepared['references'],'Loaded model tensors differ from the bundle.')
    for i in range(inputs):
        data=frozen[f'input-{i}.bin']
        for first in range(0,len(data),CHUNK):exchange(UPLOAD,i,first,data[first:first+CHUNK])
    submitted=exchange(SUBMIT);record['npu_board_job']=struct.unpack_from('<Q',submitted,48)[0];save()
    copies=[]
    for i in range(outputs):
        frame=exchange(READ,i);copies.append(frame[HEADER:])
        path=evidence/f'output-{i}.bin';supervisor.durable_frame(path,copies[-1])
    exchange(RETURN);closed=exchange(CLOSE)
    matched=all(copy==frozen[f'reference-{i}.bin'] for i,copy in enumerate(copies))
    record['npu_board_result']=dict(outputs=[hashlib.sha256(c).hexdigest() for c in copies],matched=matched,
        closed_state=struct.unpack_from('<I',closed,16)[0])
    save();require(matched,'NPU board outputs differ from the references; copies retained.')
    return record['npu_board_result']

def source_files():
    return [('npu-board-host-tool.py',Path(__file__)),('npu-board-preparation.py',Path(preparation.__file__)),
        ('npu-board-usb.c',ROOT/'apps/npu-board/usb.c'),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]

def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared NPU board metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid NPU board bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'NPU board input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    preparation.embedded_reference(frozen['image.elf'],frozen['session.bin'])
    prepared['_reference']=None
    prepared['_frozen']=frozen
    return prepared

def reference_bytes(value):
    return supervisor.reviewed_reference(value,'NPU board')

def software_sources():
    return supervisor.device_software_sources(source_files())

def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-npu-board-supervised-software-v1',
        ('admission_cases','launcher_cases','host_capture','host_lifecycle'),'NPU board',reference_bytes)

def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'NPU board','npu-board-supervised-software',PREREQUISITES,
        reference_bytes,validate_software)

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release NPU resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'NPU board entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,prepared=prepared),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=19,result_key='npu_result',
        source_files=source_files(),description='Supervised native NPU board; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
