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
    """Bind actual typed native configuration to conspicuously nonphysical proofs."""
    profile=prep.session_profile(raw,layout);image_hash=sha(image);identity=[0,0,0,0,0];port='software-fixture-never-deploy'
    def write(name,value):
        path=(directory/name).resolve();data=value if isinstance(value,bytes) else json.dumps(value,sort_keys=True).encode()
        path.write_bytes(data);return {'path':str(path),'sha256':sha(data)}
    proof=write('not-hardware.txt',b'SOFTWARE FIXTURE ONLY: establishes no physical facts; never deploy.\n')
    common={'image_sha256':image_hash,'identity':identity,'port':port,'whole_platform':profile['whole_platform']}
    boot,boot_bytes,pll,pll_bytes=layout[1:5];boot_hash=sha(raw[boot:boot+boot_bytes]);pll_hash=sha(raw[pll:pll+pll_bytes])
    ddr=write('ddr.json',{**common,'schema':'a733-ddr-reservation-v1','ranges':[[0x41000000,0x42200000]],
        'exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'evidence':[proof]})
    conditions=('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram',
        'exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping',
        'physical-counter-continuity','npu-startup-sram-confinement-drain-ownership','pmic-bus-rail-pin-ownership')
    platform=write('platform.json',{**common,**profile,'schema':'a733-npu-convolution-platform-v1',
        'conditions':dict.fromkeys(conditions,proof),'entry_conditions_established':True,'physical_coherence_result':'under-test'})
    clock_conditions={'independent-dram-phy-sources','exclusive-pll-npu-consumers-and-writers',
        'no-autonomous-parent-selector-writers','enabled-retune-snapshot'}
    if struct.unpack_from('<I',raw,pll+24)[0]==3:
        clock_conditions.remove('enabled-retune-snapshot')
        clock_conditions.update({'live-disabled-pll-quiescent-on-snapshot',
            'native-dram-continuations-and-shared-sram-abandonment','supervised-cross-pll-disturbance-risk'})
    clock=write('clock.json',{**common,'pll_sha256':pll_hash,'conditions':dict.fromkeys(sorted(clock_conditions),proof)})
    power=write('power.json',{**common,'supply_uv':800000,'boot_sha256':boot_hash,
        'conditions':dict.fromkeys(('dcdc2-stable-800mv','authenticated-pmic-pin-inheritance'),proof)})
    supervision=write('supervision.json',{**common,'supervisor':profile['supervisor'],'one_attempt':True,
        'conditions':dict.fromkeys(('manual-whole-board-power-removal','retain-all-resources-until-power-removal','stop-after-first-failure'),proof)})
    admission={**common,**profile,'schema':'a733-npu-supervised-entry-v1','boot_sha256':boot_hash,'pll_sha256':pll_hash,
        'ddr':ddr,'platform':platform,'clock':clock,'power':power,'supervision':supervision}
    admission_ref=write('admission.json',admission);value=bytearray(raw);value[48:80]=build
    value[80:112]=bytes.fromhex(admission_ref['sha256'])
    for offset,ref in ((112,clock),(144,power),(176,supervision)):value[offset:offset+32]=bytes.fromhex(ref['sha256'])
    (directory/'session.bin').write_bytes(value)
    return directory/'session.bin',Path(admission_ref['path'])

def live_checks(image,session,directory):
    """Only changed mode admission; reuse launcher/USB evidence without execution."""
    directory.mkdir();prep=load('npu_live_prepare',ROOT/'apps/npu-convolution/prepare-supervised.py')
    image_raw=image.read_bytes();raw=session.read_bytes()
    _,_,build,layout=prep.image_data(image_raw)
    assert len(raw)==608 and struct.unpack_from('<I',raw,layout[3]+24)[0]==3
    packed,admission=admission_fixture(prep,image_raw,raw,layout,build,directory)
    raw=packed.read_bytes();admission_raw=admission.read_bytes();cache={}
    prep.validate_admission(admission_raw,raw,build,layout,sha(image_raw),cache)
    record=json.loads(admission_raw);clock=Path(record['clock']['path']);original=clock.read_bytes()
    refused=[]
    for condition in ('native-dram-continuations-and-shared-sram-abandonment','supervised-cross-pll-disturbance-risk'):
        facts=json.loads(original);del facts['conditions'][condition]
        changed=json.dumps(facts,sort_keys=True).encode();clock.write_bytes(changed)
        changed_record=copy.deepcopy(record);changed_record['clock']['sha256']=sha(changed)
        changed_admission=json.dumps(changed_record,sort_keys=True).encode()
        changed_raw=bytearray(raw);changed_raw[112:144]=hashlib.sha256(changed).digest()
        changed_raw[80:112]=hashlib.sha256(changed_admission).digest()
        try:prep.validate_admission(changed_admission,bytes(changed_raw),build,layout,sha(image_raw))
        except ValueError as error:
            assert str(error)=='clock facts do not cover this entry.',str(error)
            refused.append({'condition':condition,'reason':str(error)})
        else:raise AssertionError('Missing live-DRAM evidence was accepted: '+condition)
        finally:clock.write_bytes(original)
    invalid=bytearray(raw);struct.pack_into('<I',invalid,layout[3]+24,4)
    try:prep.session_profile(invalid,layout)
    except ValueError as error:
        assert str(error)=='Uploaded deadlines/PLL authority differ.',str(error)
        refused.append({'condition':'unknown-mode'})
    else:raise AssertionError('Unknown PLL mode accepted')
    result={'passed':True,'mode':3,'session_bytes':len(raw),'refused':refused,
        'image_sha256':sha(image_raw),'runner_sha256':sha(Path(__file__).read_bytes()),
        'prepare_sha256':sha(Path(prep.__file__).read_bytes()),'physical_access':False}
    (directory/'result.json').write_text(json.dumps(result,indent=2)+'\n');return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','session','evidence','clang','lld','objcopy'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--binding-only',action='store_true');parser.add_argument('--bundle',type=Path)
    a=parser.parse_args();e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=True)
    host=load('npu_focused_host',ROOT/'tools/check-a733-npu-supervised.py');prep=host.preparation;shared=host.supervisor
    if a.binding_only:
        assert a.bundle is not None
        binding_checks(host,a.bundle,e);return
    source_paths=[Path(__file__),Path(host.__file__),Path(prep.__file__),Path(host.protocol.__file__),
        ROOT/'apps/npu-convolution/artifacts.py',ROOT/'tools/usb-access.py',Path(shared.__file__),Path(shared.base.__file__),Path(shared.observer.__file__),
        ROOT/'checks/a733-smp-supervised-launch.py',ROOT/'platform/a733-smp-supervised/launch.c',ROOT/'platform/a733-smp-observe/launch.S',ROOT/'platform/a733-smp-observe/launch.ld',ROOT/'platform/a733-smp-cached/boot.h']
    inputs={str(p.resolve()):sha(p.read_bytes()) for p in source_paths+[a.image,a.manifest,a.session,a.clang,a.lld,a.objcopy]}
    fingerprint=sha(json.dumps(inputs,sort_keys=True).encode());state_path=e/'results.json'
    state=json.loads(state_path.read_text()) if state_path.exists() else {'physical_access':False,'cases':{},'inputs':inputs,'source_identity':fingerprint}
    assert state['source_identity']==fingerprint,'Frozen batch inputs changed; retain prior evidence and use a new batch.'
    snapshot=e/'inputs'
    if not snapshot.exists():
        snapshot.mkdir()
        for i,p in enumerate(source_paths+[a.image,a.manifest,a.session]):shutil.copy2(p,snapshot/(str(i)+'-'+p.name))
    def save():shared.save_json(state_path,state)
    def case(name,operation):
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
        start=original.index('    if args.frequency_only:');end=original.index('    result =',start)
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
    def frozen(directory):
        paths=[Path(p) for p in prepared['entry_evidence']];original={p:p.read_bytes() for p in paths}
        try:
            for p in paths:p.write_bytes(b'changed external file after freeze')
            assert prep.validate_bundle(bundle)['_frozen']==prepared['_frozen']
        finally:
            for p,data in original.items():p.write_bytes(data)
    case('frozen-proof-no-reread',frozen)
    for name in ('session.bin','admission.json','segment-0.bin','launch.bin'):
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
    for variation in ('deadline','established','supervisor'):
        def bad_session(directory,variation=variation):
            raw=bytearray(prepared['_frozen']['session.bin'])
            offset={'deadline':layout[3]+16,'established':layout[1]+12*8,'supervisor':208}[variation]
            struct.pack_into('<Q',raw,offset,1 if variation!='supervisor' else 0);refused(lambda:prep.session_profile(bytes(raw),layout))
        case('typed-session-'+variation,bad_session)
    for variation in ('missing-power-removal','wrong-platform','missing-pmic-pins'):
        def bad_admission(directory,variation=variation):
            frozen=prepared['_frozen'];record=json.loads(frozen['admission.json']);raw=bytearray(frozen['session.bin'])
            cache={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
            name='supervision' if variation=='missing-power-removal' else 'power'
            ref=record[name];facts=json.loads(cache[ref['path']])
            if variation=='missing-power-removal':facts['conditions'].pop('manual-whole-board-power-removal')
            elif variation=='missing-pmic-pins':facts['conditions'].pop('authenticated-pmic-pin-inheritance')
            else:facts['whole_platform']+=1
            cache[ref['path']]=json.dumps(facts,sort_keys=True).encode();ref['sha256']=sha(cache[ref['path']])
            offset=176 if name=='supervision' else 144;raw[offset:offset+32]=bytes.fromhex(ref['sha256'])
            admission=json.dumps(record,sort_keys=True).encode();raw[80:112]=hashlib.sha256(admission).digest()
            refused(lambda:prep.validate_admission(admission,bytes(raw),build,layout,sha(image),cache,True))
        case('admission-'+variation,bad_admission)
    def queue(directory):
        def ref(path):return {'path':str(path.resolve()),'sha256':sha(path.read_bytes())}
        proof=directory/'software-fixture.txt';proof.write_text('SOFTWARE FIXTURE ONLY; no qualification.\n')
        sources=dict(host.source_files());sources.update({'supervised-host-tool.py':Path(shared.__file__),
            'base-host-tool.py':Path(shared.base.__file__),'observer-host-tool.py':Path(shared.observer.__file__)})
        software={'schema':'a733-npu-supervised-software-v1','physical_access':False,
            **dict.fromkeys(('admission_cases','launcher_cases','host_capture','host_lifecycle'),True),
            'image':ref(bundle/'image.elf'),'launcher':ref(bundle/'launch.bin'),
            'sources':{k:ref(v) for k,v in sources.items()},'evidence':[ref(proof)]}
        software_path=directory/'software.json';shared.save_json(software_path,software)
        prerequisites={'supervised-ram-recovery':ref(bundle/'admission.json'),'npu-supervised-software':ref(software_path)}
        submission={'prerequisites':list(prerequisites),'artifacts':[ref(bundle/n) for n in ('prepared.json','image.elf','launch.bin')]}
        database=directory/'queue.db';db=sqlite3.connect(database)
        db.execute('CREATE TABLE settings(key TEXT,value TEXT)');db.execute('INSERT INTO settings VALUES(?,?)',('halted',''))
        db.execute('CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT)')
        db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('fixture','claimed','fixture-owner',json.dumps(submission),json.dumps({'prerequisites':prerequisites})));db.commit()
        host.claimed(database,'fixture','fixture-owner',bundle,prepared)
        refused(lambda:host.claimed(database,'fixture','other-owner',bundle,prepared))
        submission['prerequisites'].remove('supervised-ram-recovery')
        db.execute('UPDATE tests SET submission=?',(json.dumps(submission),));db.commit()
        refused(lambda:host.claimed(database,'fixture','fixture-owner',bundle,prepared));db.close()
        software['sources']['npu-host-tool.py']=ref(proof);refused(lambda:host.validate_software(software,bundle,prepared))
        return {'fixture_not_qualification':True}
    case('queue-and-source-admission',queue)
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
    # Shared discovery, refusal, failure and flock cases are reused from the
    # reviewed board-supervised-host batch; only the NPU result selector is new.
    for status in ('success','fault'):
        case('transport-npu-'+status,lambda d,s=status:transport(host,prepared,bundle,reference,d,s))
    save();print(json.dumps({'cases':len(state['cases']),'results':str(state_path),'physical_access':False}))


def transport(npu,prepared,bundle,reference,directory,status):
    """Exercise actual shared run_session with NPU protocol and private flock."""
    host=npu.supervisor;clock=Clock();events=[];phase=[False];packet=[b''];old=Path('/fixture-usb/101/010');native=Path('/fixture-usb/101/011')
    port=directory/'port';port.mkdir()
    for name,value in [('bcdDevice','0101'),('product',npu.protocol.PRODUCT),('serial',npu.protocol.SERIAL)]: (port/name).write_text(value)
    identity=lambda node:[0,0 if node==old else 1,0,0,0]
    access=types.SimpleNamespace(discover=lambda:old,require_session=lambda n:None,require_rom=lambda n:None,
        identity=identity,target=lambda n:port,session_node=lambda c:native if phase[0] else old,
        quarantine_instance=lambda *a:events.append('quarantine'))
    fel=directory/'fixture-fel';fel.write_bytes(b'NOT EXECUTABLE: software FEL fixture')
    guard_source=directory/'fixture-guard.py';guard_source.write_text('# Fixture only.\n')
    guard=types.ModuleType('usb_guard');guard.__file__=str(guard_source);guard.check_health=lambda:None;guard.reenumeration=lambda *a:nullcontext()
    def fake_fel(command,**kwargs):
        assert command[:3]==[str(fel),'-d','101:10'];args=command[3:];events.append(args)
        if args==['version']:text='soc=00001903(A733)'
        elif args==['readl','0x02050014']:text='0'
        else:assert args==['exe','0x52000'];phase[0]=True;text=''
        return types.SimpleNamespace(returncode=0,stdout=text,stderr='')
    def preload(*args):prepared['_backup_hashes']={};events.append('preload-reused')
    def write(endpoint,data,timeout):assert endpoint==2;packet[0]=bytes(data);events.append('usb-write');return len(data)
    def read(endpoint,length,timeout):
        assert endpoint==0x81 and length==528;events.append('usb-read')
        return frame(npu,prepared,packet[0][32:64],struct.unpack_from('<I',packet[0],4)[0],reference,0 if status=='success' else 3)
    device=types.SimpleNamespace(bcdDevice=0x0101,set_configuration=lambda n:None,write=write,read=read)
    usb=types.ModuleType('usb');core=types.ModuleType('usb.core');usb.core=core
    def find(**kw):assert kw=={'idVendor':0x1f3a,'idProduct':0xefe8,'bus':101,'address':11};return device
    core.find=find;lock=directory/'private.lock'
    @contextmanager
    def board_lock():
        with open(lock,'a') as owned:
            fcntl.flock(owned,fcntl.LOCK_EX|fcntl.LOCK_NB)
            yield old
    access.board_lock=board_lock
    def hold():
        with open(lock,'a') as other:
            try:fcntl.flock(other,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:events.append('held-private-flock')
            else:raise AssertionError('Ownership released before hold')
    real_finish,real_stop=host.finish_after_observation,host.base.stop_after_failure
    def finish(record,save,quarantine):return real_finish(record,save,quarantine,hold=hold)
    def stop(record,save,quarantine,error):return real_stop(record,save,quarantine,error,hold=hold)
    real_capture=npu.capture
    def capture(*args,**kw):return real_capture(*args,**kw,now=clock.now,pause=clock.pause)
    settings=types.SimpleNamespace(fel=fel,evidence=directory/'runtime',claim='fixture',owner='fixture')
    patches=[patch.object(host,'module',lambda name,path:access),
        patch.object(host.base,'FEL_HASH',sha(fel.read_bytes())),patch.object(host.base,'preload',preload),
        patch.object(host.subprocess,'run',fake_fel),patch.object(host.os,'access',lambda *a:True),
        patch.object(host.time,'monotonic',clock.now),patch.object(host.time,'sleep',clock.pause),
        patch.object(host,'finish_after_observation',finish),patch.object(host.base,'stop_after_failure',stop),
        patch.object(npu,'capture',capture),patch.dict(sys.modules,{'usb':usb,'usb.core':core,'usb_guard':guard})]
    with ExitStack() as stack:
        for item in patches:stack.enter_context(item)
        code=host.run_session(settings,bundle,prepared,lambda:None,lambda i,p:None,
            lambda dev,same,deadline,record,save:npu.capture_device(dev,same,deadline,prepared,directory/'runtime',record,save),
            product=npu.protocol.PRODUCT,serial=npu.protocol.SERIAL,bcd_device=0x0101,capture_seconds=.1,result_key='npu_result')
    record=json.loads((directory/'runtime/session.json').read_text())
    assert code==(0 if status=='success' else 2) and record['complete']==(status=='success')
    assert record['npu_result']=='observed-'+status and 'quarantine' in events and 'held-private-flock' in events
    assert events.count('usb-write')==events.count('usb-read')==1 and events.count(['exe','0x52000'])==1
    return {'events':events,'code':code,'record':record,'preload':'Reused prior transport evidence; fixture stub only.'}

def binding_checks(host,bundle,directory):
    """Only changed frozen queue/software binding; reuse 33 prior cases."""
    bundle=bundle.resolve();prepared=host.preparation.validate_bundle(bundle);results={}
    def ref(path):return {'path':str(path.resolve()),'sha256':sha(path.read_bytes())}
    def refused(name,operation):
        try:operation()
        except (ValueError,RuntimeError):results[name]='pass';return
        raise AssertionError('Expected frozen binding refusal: '+name)
    proof=directory/'fixture.txt';proof.write_text('Software fixture only; no physical qualification.\n')
    sources=dict(host.source_files());shared=host.supervisor
    sources.update({'supervised-host-tool.py':Path(shared.__file__),'base-host-tool.py':Path(shared.base.__file__),'observer-host-tool.py':Path(shared.observer.__file__)})
    software={'schema':'a733-npu-supervised-software-v1','physical_access':False,
        **dict.fromkeys(('admission_cases','launcher_cases','host_capture','host_lifecycle'),True),
        'image':ref(bundle/'image.elf'),'launcher':ref(bundle/'launch.bin'),
        'sources':{k:ref(v) for k,v in sources.items()},'evidence':[ref(proof)]}
    software_path=directory/'software-fixture.json';shared.save_json(software_path,software)
    prerequisites={'supervised-ram-recovery':ref(bundle/'admission.json'),'npu-supervised-software':ref(software_path)}
    submission={'prerequisites':list(prerequisites),'artifacts':[ref(bundle/n) for n in ('prepared.json','image.elf','launch.bin')]}
    database=directory/'queue.db';db=sqlite3.connect(database)
    db.execute('CREATE TABLE settings(key TEXT,value TEXT)');db.execute('INSERT INTO settings VALUES(?,?)',('halted',''))
    db.execute('CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT)')
    db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('fixture','claimed','fixture-owner',json.dumps(submission),json.dumps({'prerequisites':prerequisites})));db.commit()
    host.claimed(database,'fixture','fixture-owner',bundle,prepared);results['unchanged-frozen-claim']='pass'
    for name in ('prepared.json','admission.json'):
        path=bundle/name;original=path.read_bytes()
        try:
            path.write_bytes(original+b'\n')
            # Rebind queue artifacts too: two matching live paths must not admit.
            submission['artifacts']=[ref(bundle/n) for n in ('prepared.json','image.elf','launch.bin')]
            prerequisites['supervised-ram-recovery']=ref(bundle/'admission.json')
            db.execute('UPDATE tests SET submission=?,review=?',(json.dumps(submission),json.dumps({'prerequisites':prerequisites})));db.commit()
            initial={**prepared['files'],'prepared.json':prepared['_prepared_sha256']}
            # Simulate a file replacement after the initial current-file scan;
            # review refs must still bind the initially admitted bytes directly.
            real_digest=host.digest
            with patch.object(host,'digest',lambda path:initial[Path(path).name] if Path(path).parent==bundle and Path(path).name in initial else real_digest(path)):
                refused('changed-frozen-'+name,lambda:host.claimed(database,'fixture','fixture-owner',bundle,prepared))
        finally:path.write_bytes(original)
    for key,name in (('image','image.elf'),('launcher','launch.bin')):
        path=bundle/name;original=path.read_bytes()
        try:
            path.write_bytes(bytes([original[0]^1])+original[1:]);changed=copy.deepcopy(software);changed[key]=ref(path)
            refused('software-rebound-'+key,lambda:host.validate_software(changed,bundle,prepared))
        finally:path.write_bytes(original)
    db.close()
    shared.save_json(directory/'results.json',{'physical_access':False,'cases':results,'reused_cases':33,
        'sources':{str(p):sha(p.read_bytes()) for p in (Path(__file__),Path(host.__file__),Path(host.preparation.__file__))},
        'initial_prepared_sha256':prepared['_prepared_sha256'],'image_sha256':prepared['image_sha256'],'launcher_sha256':prepared['launcher_sha256']})
    print(json.dumps({'binding_cases':len(results),'results':str(directory/'results.json')}))

if __name__=='__main__':main()
