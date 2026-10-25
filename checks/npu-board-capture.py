#!/usr/bin/env python3
"""Exercise the actual NPU host callback and retained supervisor with modeled USB only."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('npu_capture',ROOT/'tools/check-a733-npu-board-supervised.py')
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)

class Device:
    def __init__(self,mps,output,mode):
        self.mps=mps;self.output=output;self.mode=mode;self.queue=[];self.requests=[];self.pending=bytearray()
    def get_active_configuration(self):
        return {(0,0):[SimpleNamespace(bEndpointAddress=0x81,bmAttributes=2,wMaxPacketSize=self.mps)]}
    def write(self,endpoint,data,timeout):
        assert endpoint==2 and timeout>0 and not self.queue
        self.pending.extend(data)
        if len(self.pending)<64:return len(data)
        magic,op,seq,n,epoch,generation=struct.unpack_from('<4I2Q',self.pending)
        if len(self.pending)<64+n:return len(data)
        assert magic==host.REQUEST_MAGIC and len(self.pending)==64+n
        self.pending.clear();self.requests.append(op)
        payload=self.output if op==host.READ else b''
        if self.mode=='wrong-output' and payload:payload=bytes([payload[0]^1])+payload[1:]
        state=host.CLOSED if op==host.CLOSE and self.mode!='wrong-close' else 2
        raw=bytearray(64);struct.pack_into('<8I2Q',raw,0,host.RESPONSE_MAGIC,op,seq,host.OK,state,0,0,len(payload),epoch,generation)
        if op==host.LOAD:struct.pack_into('<I',raw,56,0x10001)
        if op==host.SUBMIT:struct.pack_into('<Q',raw,48,123)
        raw+=payload
        self.queue.extend(bytes(raw[i:i+self.mps]) for i in range(0,len(raw),self.mps))
        if len(raw)%self.mps==0:self.queue.append(b'x' if self.mode=='nonempty-zlp' else None if self.mode=='missing-zlp' else b'')
        return len(data)
    def read(self,endpoint,maximum,timeout):
        assert endpoint==0x81 and maximum>=self.mps and timeout>0
        value=self.queue.pop(0)
        if value is None:raise TimeoutError('modeled missing terminal packet')
        assert len(value)<=maximum
        return value

def run(directory,mps,length,mode):
    directory.mkdir();output=bytes(i%251 for i in range(length));device=Device(mps,output,mode)
    session=bytearray(1000);struct.pack_into('<2Q',session,24,17,19)
    prepared={'model':0,'inputs':1,'references':1,'epoch':17,'generation':19,'_backup_hashes':{},
              '_frozen':{'input-0.bin':bytes(192),'reference-0.bin':output}}
    record={'result_key':'npu_result'};clock=[1.];saves=[];quarantined=[];held=[];transactions=[]
    def save():
        if mode=='late-final' and 'npu_board_result' in record:clock[0]=21.
        saves.append(json.loads(json.dumps(record)))
    def same():
        if mode=='changed-final' and 'npu_board_result' in record:raise RuntimeError('modeled instance change')
    durable=host.supervisor.durable_frame
    def write(path,data):
        if mode=='durability' and path.name=='output-0.bin':raise OSError('modeled fsync failure')
        durable(path,data)
    def transaction(*args):transactions.append(args);return '0'
    def observe(started):
        return host.capture_device(device,same,20.,session,directory,record,save,prepared=prepared,now=lambda:clock[0])
    error=None
    try:
        with patch.object(host.supervisor.base,'preload',lambda *a:None),patch.object(host.supervisor,'durable_frame',write):
            host.supervisor.launch_once(transaction,observe,directory,prepared,directory,record,save,lambda:None,result_key='npu_result')
    except (RuntimeError,OSError,ValueError) as e:error=str(e)
    finally:
        host.supervisor.finish_after_observation(record,save,lambda:quarantined.append(True),lambda:held.append(True))
    assert quarantined==[True] and held==[True] and record['manual_reconnect_required']
    assert transactions==[('readl','0x02050014'),('exe','0x52000')]
    if mode=='success':
        assert error is None and record['complete'] and record['npu_result']=='observed-success'
        assert record['npu_board_result']['closed_state']==host.CLOSED and not device.queue
        assert (directory/'output-0.bin').read_bytes()==output
        assert device.requests==[host.STATUS,host.LOAD,host.UPLOAD,host.SUBMIT,host.READ,host.RETURN,host.CLOSE]
    else:
        assert error and record.get('npu_result')!='observed-success' and not record.get('complete')
    for fragment in record.get('npu_board_fragments',[]):
        raw=(directory/fragment['path']).read_bytes()
        assert len(raw)==fragment['bytes'] and hashlib.sha256(raw).hexdigest()==fragment['sha256']
    if mode=='nonempty-zlp':assert (directory/record['npu_board_fragments'][-1]['path']).read_bytes()==b'x'
    return {'packet_size':mps,'output_bytes':length,'mode':mode,'pass':True,'error':error,
            'fragments':len(record.get('npu_board_fragments',[])),
            'zero_packets':sum(f['bytes']==0 for f in record.get('npu_board_fragments',[])),
            'ownership_retained':bool(held)}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False)
    cases=[(mps,length,'success') for mps in (64,512) for length in (72,448,960)]
    cases += [(mps,448,mode) for mps in (64,512) for mode in ('missing-zlp','nonempty-zlp','wrong-output','wrong-close','durability','late-final','changed-final')]
    results=[run(a.output/str(i),*case) for i,case in enumerate(cases)]
    value={'physical_access':False,'pass':True,'cases':results,'host_sources':{k:host.digest(v) for k,v in host.software_sources().items()},
           'check_sha256':host.digest(Path(__file__)),'boundary':'Actual capture and supervisor; modeled USB, FEL preload, clock and ownership hold.'}
    (a.output/'result.json').write_text(json.dumps(value,indent=2)+'\n');print(json.dumps({'pass':True,'cases':len(results)}))
if __name__=='__main__':main()
