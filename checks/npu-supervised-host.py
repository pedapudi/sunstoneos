#!/usr/bin/env python3
"""Focused NPU supervised packaging/capture checks; fixtures establish no hardware facts.

USB, FEL, discovery and guard calls are mocked. Ownership checks use a private
real flock and explicit returning hold sentinels. No physical lock is opened.
"""
import argparse
from contextlib import ExitStack, nullcontext, contextmanager
import copy
import fcntl
import hashlib
import importlib.util
import io
import json
from pathlib import Path
from harness import artifact_refused
import shutil
import sqlite3
import struct
import subprocess
import sys
import types
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def sha(data):return hashlib.sha256(data).hexdigest()
class Clock:
    def __init__(self):self.value=100.
    def now(self):return self.value
    def pause(self,n):self.value+=max(.01,n)

def frame(host,prepared,challenge,sequence,reference,status=0,armed=1000):
    raw=bytearray(512);complete=status in (0,2)
    struct.pack_into('<QII8Q',raw,0,host.protocol.FRAME_MAGIC,1,512,status,
        prepared['epoch'],prepared['generation'],prepared['lease'],2000 if complete else 0,
        3000 if complete else 0,armed+480000000,7 if complete else 0)
    raw[80:112]=challenge;raw[112:144]=bytes.fromhex(prepared['build_id'])
    struct.pack_into('<5Q',raw,144,0x03600000,prepared['npu_evidence'],prepared['command_admission'],prepared['usb_evidence'],armed)
    raw[184:280]=b''.join(bytes.fromhex(h) for n,h in host.protocol.ARTIFACTS.values())
    if complete:
        output=reference if status==0 else bytes([reference[0]^1])+reference[1:]
        raw[280:352]=output;raw[352:384]=hashlib.sha256(output).digest();struct.pack_into('<Q',raw,384,72)
    if status==3:struct.pack_into('<Q',raw,392,17)
    raw[480:]=hashlib.sha256(raw[:480]).digest()
    return struct.pack('<4I',host.protocol.MESSAGE_MAGIC,sequence,0x80000033,512)+raw

def admission_fixture(prep,image,raw,layout,build,directory):
    """Reuse native typed inputs without external admission records."""
    value=bytearray(raw);value[48:80]=build
    path=directory/'session.bin';path.write_bytes(value)
    return path,None

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','evidence','clang','lld','objcopy'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--manifest',type=Path)
    parser.add_argument('--case',action='append',help='Run a named case; exact-package always supplies its bundle')
    parser.add_argument('--binding-only',action='store_true');parser.add_argument('--bundle',type=Path)
    a=parser.parse_args();e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=True)
    host=load('npu_focused_host',ROOT/'tools/check-a733-npu-supervised.py');prep=host.preparation;shared=host.supervisor
    if a.binding_only:
        assert a.bundle is not None
        binding_checks(host,a.bundle,e);return
    source_paths=[Path(__file__),Path(host.__file__),Path(prep.__file__),Path(host.protocol.__file__),
        ROOT/'apps/npu-convolution/artifacts.py',ROOT/'tools/usb-access.py',Path(shared.__file__),Path(shared.base.__file__),Path(shared.observer.__file__),
        ROOT/'checks/a733-smp-supervised-launch.py',ROOT/'platform/a733-smp-supervised/launch.c',ROOT/'platform/a733-smp-observe/launch.S',ROOT/'platform/a733-smp-observe/launch.ld',ROOT/'platform/a733-smp-cached/boot.h']
    inputs={str(p.resolve()):sha(p.read_bytes()) for p in source_paths+[a.image,a.session,a.clang,a.lld,a.objcopy]}
    fingerprint=sha(json.dumps(inputs,sort_keys=True).encode());state_path=e/'results.json'
    state=json.loads(state_path.read_text()) if state_path.exists() else {'physical_access':False,'cases':{},'inputs':inputs,'source_identity':fingerprint}
    assert state['source_identity']==fingerprint,'Frozen batch inputs changed; retain prior evidence and use a new batch.'
    snapshot=e/'inputs'
    if not snapshot.exists():
        snapshot.mkdir()
        for i,p in enumerate(source_paths+[a.image,a.session]):shutil.copy2(p,snapshot/(str(i)+'-'+p.name))
    def save():shared.save_json(state_path,state)
    def case(name,operation):
        if a.case and name!='exact-package' and name not in a.case:return
        if state['cases'].get(name,{}).get('result')=='pass':return
        attempt=1
        while (e/(name+'-'+str(attempt))).exists():attempt+=1
        directory=e/(name+'-'+str(attempt));directory.mkdir()
        try:result=operation(directory);state['cases'][name]={'result':'pass','evidence':str(directory),**(result or {})}
        except BaseException as error:
            state['cases'][name]={'result':'fail','evidence':str(directory),'error':str(error)};save();raise
        save()
    def refused(operation):
        try:operation()
        except (ValueError,RuntimeError,KeyError):return
        raise AssertionError('Expected refusal')
    image=a.image.read_bytes();parts,address,build,layout=prep.image_data(image)
    from elftools.elf.elffile import ELFFile
    elf=ELFFile(io.BytesIO(image));symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
    reference_address=int(symbols['a733_npu_application_reference']['st_value'])
    reference=next(p['data'][reference_address-p['address']:reference_address-p['address']+72] for p in parts if p['address']<=reference_address<p['address']+len(p['data']))
    bundle=e/'bundle'
    def package(directory):
        if bundle.exists():bundle.rename(directory/'prior-incomplete-bundle')
        session,admission=admission_fixture(prep,image,a.session.read_bytes(),layout,build,directory)
        prepared=prep.prepare(a.image,a.manifest,session,admission,bundle,a.clang,a.lld,a.objcopy)
        return {'image_sha256':prepared['image_sha256'],'launcher_sha256':prepared['launcher_sha256'],'layout':list(layout),'fixture_not_physical':True}
    case('exact-package',package);prepared=prep.validate_bundle(bundle)
    def launcher(directory):
        original=(ROOT/'checks/a733-smp-supervised-launch.py').read_text()
        start=original.index('    if args.accelerator_profile:\n        cases');end=original.index('    result =',start)
        derived=original[:start]+"    cases=[check('NPU contract publication and handoff',12)]\n"+original[end:]
        assert derived.count('0x4153373353554333')==1
        derived=derived.replace('0x4153373353554333',hex(prep.CONTRACT_MAGIC))
        script=directory/'npu-contract-publication.py';script.write_text(derived)
        command=[sys.executable,str(script),'--bundle',str(bundle),'--output',str(directory/'result.json')]
        result=subprocess.run(command,capture_output=True,text=True)
        (directory/'output.txt').write_text(result.stdout+result.stderr)
        assert result.returncode==0,result.stderr
        return {'original_check_sha256':sha(original.encode()),'derived_check_sha256':sha(derived.encode()),
            'scope':'One new contract publication case; unchanged watchdog/frequency/refusal cases reused.'}
    case('npu-launcher-publication',launcher)
    def repeated_output(directory):
        sentinel=bundle/'prior-evidence.txt';sentinel.write_bytes(b'preserved')
        session,_=admission_fixture(prep,image,a.session.read_bytes(),layout,build,directory)
        artifact_refused(lambda:prep.prepare(a.image,None,session,None,bundle,a.clang,a.lld,a.objcopy),bundle)
    case('existing-output-refused-without-changes',repeated_output)
    def archive_changes(directory):
        for path in bundle.iterdir():
            if path.is_file() and path.name not in prepared['files'] and path.name!='prepared.json':
                original=path.read_bytes()
                try:
                    path.write_bytes(b'Changed archived build notes or source')
                    assert prep.validate_bundle(bundle)['_frozen']==prepared['_frozen']
                finally:path.write_bytes(original)
        return {'archived_source_manifest_log_not_authority':True}
    case('archived-build-files-independent',archive_changes)
    for name in ('session.bin','segment-0.bin','launch.bin'):
        def mutation(directory,name=name):
            path=bundle/name;data=path.read_bytes()
            try:path.write_bytes(bytes([data[0]^1])+data[1:]);refused(lambda:prep.validate_bundle(bundle))
            finally:path.write_bytes(data)
        case('mutated-'+name,mutation)
    phoff=int(elf['e_phoff']);entsize=int(elf['e_phentsize']);loads=[i for i,p in enumerate(elf.iter_segments()) if p['p_type']=='PT_LOAD']
    for kind in ('outside-ddr','writable-executable','session-overlap','executable-overflow','overlap'):
        def geometry(directory,kind=kind):
            data=bytearray(image);off=phoff+loads[0]*entsize
            if kind=='writable-executable':struct.pack_into('<I',data,off+4,7)
            elif kind=='executable-overflow':struct.pack_into('<Q',data,off+40,0x200001)
            elif kind=='overlap':
                assert len(loads)>1;off=phoff+loads[1]*entsize;struct.pack_into('<2Q',data,off+16,0x41000000,0x41000000)
            else:
                target=0x43000000 if kind=='outside-ddr' else prep.SESSION_ADDRESS
                if kind=='session-overlap':struct.pack_into('<I',data,off+4,4)
                struct.pack_into('<2Q',data,off+16,target,target)
            refused(lambda:prep.image_data(bytes(data)))
        case('geometry-'+kind,geometry)
    def artifact_mutation(directory):
        data=bytearray(image);p=next(p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and p['p_vaddr']<=reference_address<p['p_vaddr']+p['p_filesz'])
        data[int(p['p_offset'])+reference_address-int(p['p_vaddr'])]^=1;refused(lambda:prep.image_data(bytes(data)))
    case('embedded-artifact-changed',artifact_mutation)
    for variation in ('deadline','owner-collision','supervisor'):
        def bad_session(directory,variation=variation):
            raw=bytearray(prepared['_frozen']['session.bin']);boot=layout[1]
            if variation=='deadline':struct.pack_into('<Q',raw,boot+layout[4]+16,1)
            elif variation=='owner-collision':raw[boot+7*8:boot+8*8]=raw[boot+5*8:boot+6*8]
            else:struct.pack_into('<Q',raw,208,0)
            refused(lambda:prep.session_profile(bytes(raw),layout))
        case('typed-session-'+variation,bad_session)
    def direct_session(directory):
        raw=prepared['_frozen']['session.bin']
        assert prep.validate_admission(b'{}',raw,build,layout,sha(image))=={}
        assert prep.image_manifest_check({},image,raw)==prepared['execution_profile']
        bad=bytearray(raw);bad[48]^=1
        refused(lambda:prep.validate_admission(b'{}',bad,build,layout,sha(image)))
        bad=bytearray(raw);struct.pack_into('<I',bad,layout[1]+layout[3]+12,1)
        refused(lambda:prep.validate_admission(b'{}',bad,build,layout,sha(image)))
    case('direct-session-and-clock-validation',direct_session)
    def capture_case(directory,variation):
        clock=Clock();calls=[];record={'npu_result':'unobserved'};challenge=bytes(range(1,33));saved=[]
        statuses=[1,0] if variation in ('pending-success','timing-change') else [2] if variation=='mismatch' else [3] if variation=='fault' else [0]
        def exchange(packet,timeout):
            calls.append(packet);sequence=len(calls);assert len(packet)==64 and 0<timeout<=500
            data=frame(host,prepared,challenge,sequence,reference,statuses[sequence-1],1001 if variation=='timing-change' and sequence==2 else 1000)
            if variation=='stale':data=frame(host,{**prepared,'lease':prepared['lease']+1},challenge,sequence,reference)
            if variation=='malformed':data=data[:-1]
            if variation=='digest':data=data[:-1]+bytes([data[-1]^1])
            if variation=='late':clock.value=101
            return data
        def save_record():
            if variation=='save-failure' and 'durable_result' in record:raise OSError('fixture save failure')
            saved.append(copy.deepcopy(record))
        error=None
        try:result=host.capture(exchange,prepared,challenge,directory,record,save_record,101,now=clock.now,pause=clock.pause)
        except (ValueError,RuntimeError,OSError) as caught:error=caught
        bad=variation in ('stale','malformed','digest','late','save-failure','timing-change')
        assert bool(error)==bad and len(calls)==len(statuses)
        assert len(list(directory.glob('npu-*.bin')))==len(calls),'Raw frame not durable before interpretation'
        if bad:assert record['npu_result']=='unobserved' and 'durable_result' not in record
        else:assert record['npu_result']=='observed-'+result['status'] and saved[-1]['durable_result']
        if variation=='save-failure':assert (directory/'output.bin').read_bytes()==reference
        return {'calls':len(calls),'record':record,'error':str(error) if error else None}
    for variation in ('pending-success','mismatch','fault','stale','malformed','digest','late','save-failure','timing-change'):
        case('capture-'+variation,lambda d,v=variation:capture_case(d,v))
    def transfer(directory,variation):
        calls=[];clock=Clock();device=types.SimpleNamespace()
        def write(endpoint,packet,timeout):
            calls.append('write')
            if variation=='late':clock.value=102
            return len(packet)-1 if variation=='partial' else len(packet)
        device.write=write;device.read=lambda *a,**kw:calls.append('read')
        real_capture=host.capture
        with patch.object(host,'capture',lambda *args,**kw:real_capture(*args,**kw,now=clock.now,pause=clock.pause)),patch.object(host.time,'monotonic',clock.now):
            refused(lambda:host.capture_device(device,lambda:None,101,prepared,directory,{},lambda:None))
        assert calls==['write'];return {'calls':calls}
    for variation in ('partial','late'):case('transfer-'+variation,lambda d,v=variation:transfer(d,v))
    save();print(json.dumps({'cases':len(state['cases']),'results':str(state_path),'physical_access':False}))




def binding_checks(host,bundle,directory):
    prepared=host.preparation.validate_bundle(bundle)
    raw=prepared['_frozen']['session.bin']
    assert host.preparation.validate_admission(b'{}',raw,bytes.fromhex(prepared['build_id']),
        prepared['session_layout'],prepared['image_sha256'])=={}
    host.supervisor.save_json(directory/'results.json',{'passed':True,'physical_access':False,
        'direct_native_session':True})

if __name__=='__main__':main()
