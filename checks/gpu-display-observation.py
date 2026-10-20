#!/usr/bin/env python3
"""Replay actual copied native observations through the supervised host callback."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
tool=module('gpu_display_observation_host',ROOT/'tools/check-a733-gpu-display-supervised.py')
p=tool.preparation

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','pending','terminal','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--case',action='append',choices=('capture','refusals'))
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    image=a.image.read_bytes();session=a.session.read_bytes();_,_,build=p.image_data(image)
    prepared={**p.session_profile(session[24:568]),'build_id':build.hex(),'_frozen':{'session.bin':session}}
    frames=[a.pending.read_bytes(),a.terminal.read_bytes()]
    identities={'image_sha256':p.sha(image),'session_sha256':p.sha(session),'reply_sha256':[p.sha(v) for v in frames]}
    result_path=a.output/'result.json';record=json.loads(result_path.read_text()) if result_path.exists() else {**identities,'physical_access':False,'cases':{}}
    assert all(record[k]==v for k,v in identities.items()),'Refuse stale image/session/native-producer records'
    def passed(name,value):record['cases'][name]=value;p.save_json(result_path,record);print('PASS',name,flush=True)
    protocol=module('gpu_display_observation_codec',ROOT/'apps/gpu-display/capture.py')
    class Clock:
        now=0.
        def __call__(self):return self.now
        def sleep(self,seconds):self.now+=seconds
    def run(name,*,offset=None,wrong_input=False,short=False,late=False,fail_save=False,transport=False):
        out=a.output/name;out.mkdir(exist_ok=False);clock=Clock();observed={};same_calls=[]
        class Device:
            writes=0;reads=0
            def write(self,endpoint,data,timeout):
                assert endpoint==2 and timeout>0 and self.writes==self.reads
                self.writes+=1;assert self.writes<=2
                assert bytes(data)==protocol.request(self.writes,prepared['epoch'],prepared['generation'])
                return len(data)
            def read(self,endpoint,capacity,timeout):
                assert endpoint==0x81 and capacity==1025 and timeout>0 and self.writes==self.reads+1
                self.reads+=1
                if transport:raise OSError('Modeled transport failure')
                raw=bytearray(frames[self.reads-1])
                if offset is not None:raw[offset]^=1
                if wrong_input and self.reads==2:raw[80]^=1
                if late:clock.now=20.
                return bytes(raw[:512] if short else raw)
        device=Device()
        def save():
            if fail_save and observed.get('gpu_display_result')=='observed-success':raise OSError('Modeled terminal persistence failure')
            p.save_json(out/'record.json',observed)
        failed=False
        try:
            result=tool.capture_device(device,lambda:same_calls.append(True),19.,prepared,out,observed,save,clock=clock,sleep=clock.sleep)
        except (OSError,ValueError,RuntimeError,TimeoutError):failed=True;result=None
        assert len(same_calls)==device.writes+device.reads
        return failed,result,observed,device
    selected=a.case or ['capture','refusals']
    if 'capture' in selected and 'capture' not in record['cases']:
        failed,result,observed,device=run('capture')
        assert not failed and observed['gpu_display_result']=='observed-success' and device.reads==device.writes==2
        assert [v['sha256'] for v in observed['gpu_display_frames']]==identities['reply_sha256']
        assert result['state']==2 and len(result['frames'])==2 and len(result['presentations'])==3
        assert (result['terminal']['lease'],result['terminal']['build_id'],result['terminal']['session_sha256'])==(prepared['lease'],build.hex(),p.sha(session))
        passed('capture',{'passed':True,'copied_replies':2,'frame_evidence_records':2,'presentation_records':3,
            'scope':'Actual native-producer reply bytes replayed unchanged through new supervisor callback, codec and retained-device wrapper. USB transfer/completion and host clock modeled; no native, hardware, queue or USB execution.'})
    if 'refusals' in selected and 'refusals' not in record['cases']:
        cases=[]
        for name,kwargs in (('wrong-lease',{'offset':816}),('wrong-build',{'offset':824}),('wrong-session',{'offset':856}),
            ('wrong-caller-input',{'wrong_input':True}),('short-reply',{'short':True}),('late-reply',{'late':True}),
            ('transport-failure',{'transport':True}),('terminal-save-failure',{'fail_save':True})):
            failed,_,observed,device=run(name,**kwargs)
            assert failed and observed['gpu_display_result']=='unobserved'
            assert observed['manual_whole_board_power_removal_required'] is True
            if name in ('wrong-lease','wrong-build','wrong-session','short-reply','late-reply','transport-failure'):assert device.reads==device.writes==1
            if name=='terminal-save-failure':assert observed['copied_observation']
            cases.append(name)
        passed('refusals',{'passed':True,'cases':cases,
            'scope':'Mutations of native-produced records and modeled transport/time/persistence faults. No additional native execution, retry, provider release or physical claim.'})

if __name__=='__main__':main()
