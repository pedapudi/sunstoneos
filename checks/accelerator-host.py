#!/usr/bin/env python3
"""Check the combined bundle and capture without opening a board or queue."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import time
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
tool=module('combined_capture_check',ROOT/'tools/check-a733-accelerators-supervised.py')
p=tool.preparation

def refused(call):
    try:call()
    except (ValueError,RuntimeError,TimeoutError,KeyError):return
    raise AssertionError('Invalid combined input admitted')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','root-session','output','package','npu-input-one','npu-input-two','npu-reference-one','npu-reference-two'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--case',action='append',choices=('admission','capture','refusals','bundle'))
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    image=a.image.read_bytes();parts,address,build,layout=p.image_data(image)
    raw=a.root_session.read_bytes();package=a.package.read_bytes();assert raw[layout[5]:]==package
    model=p.npu.selected_model(image);inputs=[a.npu_input_one.read_bytes(),a.npu_input_two.read_bytes()]
    references=[a.npu_reference_one.read_bytes(),a.npu_reference_two.read_bytes()]
    assert inputs[0]!=inputs[1] and all(len(v)==model['reference']['bytes'] for v in references)
    record_path=a.output/'results.json'
    result=json.loads(record_path.read_text()) if record_path.exists() else {'physical_access':False,'image_sha256':p.sha(image),'cases':{}}
    assert result['image_sha256']==p.sha(image),'Refuse evidence from a different ELF'
    def passed(name,details):
        result['cases'][name]=details;p.save_json(record_path,result);print('PASS',name,flush=True)
    selected=a.case or ['admission','capture','refusals','bundle']
    profile=p.session_profile(raw,layout)
    prepared={**profile,'build_id':build.hex(),'model_name':model['name'],'session_layout':list(layout),'_frozen':{'session.bin':raw}}
    count=struct.unpack_from('<I',package,36)[0]
    replacement=struct.pack('<'+'I'*(count+3),count,7,19,*[1000+11*i for i in range(count)])
    manifest={'target':'sunstone-a733-accelerators','execution_profile':p.PROFILE,
        'workload':'interleaved-gpu-compute-complete-nn','final_elf':p.sha(image),'build_id':build.hex()}
    if 'admission' in selected and 'admission' not in result['cases']:
        p.image_manifest_check(manifest,image,raw)
        session_profile=p.session_profile(raw,layout);request,contract=p.request_bytes(address,build,raw,session_profile)
        assert len(request)==128 and len(contract)==4096
        assert struct.unpack_from('<3Q',contract)==(0,1,224)
        assert struct.unpack_from('<2Q',contract,176)==(session_profile['supervisor'],len(raw)) and contract[192:224]==hashlib.sha256(raw).digest()
        assert p.validate_admission(b'{}',raw,build,layout,p.sha(image))=={}
        assert p.image_manifest_check({},image,raw)==p.PROFILE
        bad=bytearray(raw);bad[layout[5]]^=1;refused(lambda:p.session_profile(bytes(bad),layout))
        bad=bytearray(raw);bad[layout[1]+48]^=1
        refused(lambda:p.validate_admission(b'{}',bytes(bad),build,layout,p.sha(image)))
        passed('admission',{'passed':True,'dwarf_verified_npu_layout':list(p.NPU_LAYOUT),
            'combined_layout':list(layout),'direct_native_inputs':True})
    if 'capture' in selected and 'capture' not in result['cases']:
        native=module('combined_native_capture',ROOT/'checks/accelerator-runtime.py')
        native_image=native.large.Image(a.image);machine=native.Machine(native_image,package,dict(zip(inputs,references)))
        prepared.update(machine.prepared)
        before=machine.expected_gpu();out=a.output/'capture';out.mkdir(exist_ok=False);record={}
        def save():p.save_json(out/'record.json',record)
        output=tool.capture(machine.exchange,prepared,out,record,save,time.monotonic()+19,replacement,inputs,[],references)
        assert output['gpu_outputs']==[before,machine.expected_gpu()] and output['npu_outputs']==references
        assert machine.ginputs==[replacement] and machine.inputs==inputs and machine.closed and machine.gclosed
        assert record['accelerator_result']=='observed-success' and record['gpu_jobs']==record['npu_jobs']==2
        assert record['oracle_qualified']=={'gpu':False,'npu':True}
        p.save_json(a.output/'capture-prepared.json',{k:v for k,v in prepared.items() if k!='_frozen'})
        (a.output/'gpu-replacement.bin').write_bytes(replacement)
        passed('capture',{'passed':True,'frames':len(record['accelerator_frames']),'gpu_jobs':2,'npu_jobs':2,'both_healthy_close':True,
            'scope':'New capture callback runs actual linked clients and common USB response producer; RTOS, admission, cache/CPU/device effects, service arithmetic and saved SDK inference modeled. No startup or physical operation.'})
    if 'refusals' in selected and 'refusals' not in result['cases']:
        saved=json.loads((a.output/'capture/record.json').read_text());prepared.update(json.loads((a.output/'capture-prepared.json').read_text()))
        frames=[(a.output/'capture'/v['path']).read_bytes() for v in saved['accelerator_frames']]
        def replay(name,change=None,oracle=None,fail_save=False):
            out=a.output/name;out.mkdir(exist_ok=False);r={};cursor=0
            def exchange(request,capacity,left):
                nonlocal cursor
                assert left>0 and cursor<len(frames)
                assert hashlib.sha256(request).hexdigest()==saved['accelerator_frames'][cursor]['request_sha256']
                frame=frames[cursor];cursor+=1
                return change(frame,cursor) if change else frame
            def save():
                if fail_save and r.get('accelerator_result')=='observed-success':raise OSError('Modeled final persistence failure')
                p.save_json(out/'record.json',r)
            try:tool.capture(exchange,prepared,out,r,save,time.monotonic()+19,replacement,inputs,[],oracle or [])
            except (ValueError,RuntimeError,TimeoutError,OSError):pass
            else:raise AssertionError('Failed capture reported success')
            assert r['accelerator_result']=='unobserved'
            return r
        def corrupt(frame,index):
            if index==len(frames):
                data=bytearray(frame);data[-1]^=1;return bytes(data)
            return frame
        r=replay('bad-terminal-frame',change=corrupt);assert r['accelerator_frames']
        r=replay('bad-oracle',oracle=[bytes([references[0][0]^1])+references[0][1:],references[1]])
        assert len(r['copied_outputs']['gpu'])==len(r['copied_outputs']['npu'])==2
        r=replay('save-failure',fail_save=True);assert r['copied_outputs']
        refused(lambda:tool.validate_inputs(prepared,replacement[:-4],inputs))
        refused(lambda:tool.capture(lambda *x:(_ for _ in ()).throw(AssertionError('Exchange after expiry')),
            prepared,a.output,{},lambda:None,time.monotonic()-1,replacement,inputs))
        # Model short reads at the retained-handle boundary; the real protocol
        # validators and sequence run over the previously produced frames.
        class Device:
            def __init__(self):self.index=0;self.pending=b''
            def write(self,ep,request,timeout):
                assert ep==2 and timeout>0 and not self.pending
                assert hashlib.sha256(request).hexdigest()==saved['accelerator_frames'][self.index]['request_sha256']
                self.pending=frames[self.index];self.index+=1;return len(request)
            def read(self,ep,capacity,timeout):
                assert ep==0x81 and timeout>0
                size=min(257,len(self.pending)) if len(self.pending)>168 else len(self.pending)
                data=self.pending[:size];self.pending=self.pending[size:];assert len(data)<=capacity;return data
        out=a.output/'fragmented-device';out.mkdir(exist_ok=False);r={};device=Device()
        tool.capture_device(device,lambda:None,time.monotonic()+19,prepared,out,r,lambda:p.save_json(out/'record.json',r),replacement,inputs)
        assert r['accelerator_result']=='observed-success' and device.index==len(frames) and not device.pending
        passed('refusals',{'passed':True,'cases':['terminal corruption','oracle mismatch retains outputs','failed persistence refuses success','replacement bounds','expired session','fragmented NPU reads'],
            'scope':'Replay of actual native-producer frames for the new host failure and handle adapter paths; no device/queue access or additional native execution.'})
    if 'bundle' in selected and 'bundle' not in result['cases']:
        prepared_bundle=p.prepare(a.image,None,a.root_session,None,a.output/'bundle',
            '/usr/lib/llvm-21/bin/clang',Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld',
            '/usr/lib/llvm-21/bin/llvm-objcopy')
        assert prepared_bundle['entry_evidence']=={}
        bundle=a.output/'bundle';metadata=bundle/'prepared.json';before=metadata.read_bytes()
        changed=json.loads(before);changed['segments'][-1]['address']+=64;p.save_json(metadata,changed)
        refused(lambda:p.validate_bundle(bundle));metadata.write_bytes(before)
        for path in bundle.iterdir():
            if path.is_file() and path.name not in prepared_bundle['files'] and path.name!='prepared.json':
                original=path.read_bytes()
                try:
                    path.write_bytes(b'Changed archived build notes or source')
                    assert p.validate_bundle(bundle)['_frozen']==prepared_bundle['_frozen']
                finally:path.write_bytes(original)
        marker=bundle/'preserved.txt';marker.write_bytes(b'previous evidence')
        p.prepare(a.image,None,a.root_session,None,bundle,'/usr/lib/llvm-21/bin/clang',
            Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld','/usr/lib/llvm-21/bin/llvm-objcopy')
        assert marker.read_bytes()==b'previous evidence'
        passed('bundle',{'passed':True,'direct_native_inputs':True,'upload_tamper_refused':True,
            'existing_output_preserved':True,'archive_changes_accepted':True})

if __name__=='__main__':main()
