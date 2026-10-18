#!/usr/bin/env python3
"""New mimus packaging/host cases only; all physical facts and USB are fixtures."""
import argparse
import copy
import hashlib
import importlib.util
import json
import inspect
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import sys
import types
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def load(path):
    s=importlib.util.spec_from_file_location('mimus_host_cases',path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class Clock:
    value=100.
    def now(self):return self.value
    def pause(self,n):self.value+=max(.001,n)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('image','manifest','evidence','clang','lld','objcopy'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--setup-only',action='store_true')
    a=p.parse_args();out=a.evidence;out.mkdir(parents=True,exist_ok=True)
    host=load(ROOT/'tools/check-a733-mimus-supervised.py');prep=host.preparation;sup=host.supervisor
    paths=[Path(__file__),Path(host.__file__),Path(prep.__file__),Path(sup.__file__),Path(sup.base.__file__),Path(sup.observer.__file__),ROOT/'checks/a733-smp-supervised-launch.py',ROOT/'platform/a733-smp-supervised/launch.c',ROOT/'platform/a733-smp-observe/launch.S',ROOT/'platform/a733-smp-observe/launch.ld',ROOT/'platform/a733-smp-cached/boot.h',a.image,a.manifest,a.clang,a.lld,a.objcopy]
    identities={str(x):host.digest(x) for x in paths};state_path=out/'results.json'
    state=json.loads(state_path.read_text()) if state_path.exists() else {'physical_access':False,'inputs':identities,'cases':{}}
    assert state['inputs']==identities,'Frozen batch inputs changed'
    def case(name,fn):
        if state['cases'].get(name,{}).get('result')=='pass':return
        d=out/(name+'-'+str(len(list(out.glob(name+'-*')))+1));d.mkdir()
        try:result=fn(d);state['cases'][name]={'result':'pass','evidence':str(d),**(result or {})}
        except BaseException as ex:state['cases'][name]={'result':'fail','error':str(ex),'evidence':str(d)};sup.save_json(state_path,state);raise
        sup.save_json(state_path,state)
    def refused(fn):
        try:fn()
        except (ValueError,RuntimeError,KeyError):return
        raise AssertionError('Expected refusal')
    # Construct one baseline once from the actual final ELF, before any mutations.
    image=a.image.read_bytes();parts,address,identity,layout=prep.image_data(image)
    setup=out/'baseline';setup.mkdir(exist_ok=True)
    raw=bytearray(704)
    struct.pack_into('<6Q',raw,0,0x415337334d494d32,2,704,31,37,41)
    raw[656:688]=identity;raw[48:80]=hashlib.sha256(image).digest();raw[184:216]=bytes(range(32))
    assets=Path.home()/'.cache/sunstoneos/inputs/mimus-bench/inputs.bin'
    asset_raw=assets.read_bytes();struct.pack_into('<3Q',raw,160,2,0,len(asset_raw));raw[216:248]=hashlib.sha256(asset_raw).digest()
    struct.pack_into('<Q',raw,416,37);struct.pack_into('<2Q',raw,512,4,5)
    struct.pack_into('<4I',raw,488,3300000,3300000,1200000,900000)
    struct.pack_into('<3I',raw,540,24000000,24000000,24000000);struct.pack_into('<I',raw,556,1)
    def write(name,value):
        data=value if isinstance(value,bytes) else json.dumps(value,sort_keys=True).encode();path=(setup/name).resolve();path.write_bytes(data);return {'path':str(path),'sha256':prep.sha(data)}
    proof=write('software-only.txt',b'SOFTWARE FIXTURE ONLY. No board ownership/power/coherence/isolation established. Never deploy.\n')
    common={'identity':[0,0,0,0,0],'port':'software-only-never-deploy','image_sha256':prep.sha(image)}
    ddr=write('ddr.json',{**common,'schema':'a733-ddr-reservation-v1','ranges':[[0x41000000,0x42200000],[0x43000000,0x45000000],[0x46000000,0x47000000]],'exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'evidence':[proof]})
    raw[424:456]=bytes.fromhex(ddr['sha256'])
    conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
    platform=write('platform.json',{**common,'schema':'a733-smp-observer-platform-v1','conditions':dict.fromkeys(conditions,proof),'entry_conditions_established':True,'physical_coherence_result':'under-test'})
    profile={'epoch':31,'generation':37,'lease':41}
    reference=Path.home()/'.cache/sunstoneos/evidence/mimus-host.json'
    reference_ref={'path':str(reference),'sha256':prep.digest(reference)}
    admission=write('admission.json',{**common,**profile,'configuration_sha256':prep.configuration_sha256(raw),
      'schema':'a733-mimus-supervised-entry-v1','ddr':ddr,'platform':platform,'recognition_reference':reference_ref,
      'conditions':dict.fromkeys(prep.CONDITIONS,proof),'one_attempt':True,'whole_board_power_removal':True,'no_resource_reuse_before_power_removal':True})
    raw[80:112]=bytes.fromhex(admission['sha256']);session=bytes(raw);write('session.bin',session)
    # Aggregate independent setup checks before compilation or cases.
    errors=[]
    for name,check in [('session',lambda:prep.session_profile(session,layout,identity)),('admission',lambda:prep.validate_admission(Path(admission['path']).read_bytes(),session,layout,identity,prep.sha(image))),('manifest',lambda:sup.require(json.loads(a.manifest.read_bytes())['image_sha256']==prep.sha(image),'Final manifest identity differs')),('assets',lambda:prep.validate_assets(asset_raw,session))]:
        try:check()
        except BaseException as ex:errors.append(name+': '+str(ex))
    for name,check in [
        ('capture-signature',lambda:sup.require(set(inspect.signature(host.capture).parameters)=={'exchange','session','evidence','record','save','deadline','now','pause','verify'},'Collector signature differs')),
        ('runner-prerequisites',lambda:sup.require(host.PREREQUISITES=={'supervised-ram-recovery','mimus-supervised-software'},'Lifecycle prerequisites differ')),
        ('launcher-source',lambda:sup.require(all(x in (ROOT/'platform/a733-smp-supervised/launch.c').read_text() for x in ('0x4153373353554c33','CONTRACT->reserved[0] && !CONTRACT->reserved[1]','20*A733_SMP_COUNTER_HZ')),'Maintained launcher baseline differs'))]:
        try:check()
        except BaseException as ex:errors.append(name+': '+str(ex))
    sup.save_json(out/'setup.json',{'physical_access':False,'mismatches':errors,'session_bytes':len(session),'asset_bytes':len(asset_raw),'image_sha256':prep.sha(image)})
    assert not errors,'; '.join(errors)
    if a.setup_only:return
    bundle=out/'bundle'
    def package(d):
        if not bundle.exists():prep.prepare(a.image,a.manifest,setup/'session.bin',Path(admission['path']),assets,bundle,a.clang,a.lld,a.objcopy)
        value=prep.validate_bundle(bundle);request,contract=prep.request_bytes(address,identity,session,profile)
        assert value['_frozen']['request.bin']==request and value['_frozen']['contract.bin']==contract
        assert value['segments'][-1]=={'name':'assets.bin','address':0x43000000}
        return {'image_sha256':value['image_sha256'],'launcher_sha256':value['launcher_sha256'],'fixture_not_physical':True}
    case('exact-package',package);prepared=prep.validate_bundle(bundle)
    def launcher(d):
        original=(ROOT/'checks/a733-smp-supervised-launch.py').read_text();start=original.index('    if args.frequency_only:');end=original.index('    result =',start)
        text=original[:start]+"    cases=[check('mimus 180-second publication',12),check('wrong session size',3,mutation=(CONTRACT+184,1)),check('wrong session digest',3,mutation=(CONTRACT+192,0)),check('armed watchdog refuses',0,initial={MODE:0x41})]\n"+original[end:]
        text=text.replace('480000000','4320000000').replace('(0x41000000, 0x1200000)', '(0x41000000, 0x1200000), (0x43000000, 0x2000000)').replace('0x4153373353554333',hex(prep.CONTRACT_MAGIC));script=d/'launcher-cases.py';script.write_text(text)
        command=[sys.executable,'-B',str(script),'--bundle',str(bundle),'--output',str(d/'result.json')];r=subprocess.run(command,capture_output=True,text=True);(d/'output.txt').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
        return {'cases':4,'check_sha256':prep.sha(text.encode())}
    case('launcher-new-abi',launcher)
    def frozen(d):
        before={Path(k):Path(k).read_bytes() for k in prepared['entry_evidence']}
        try:
            for p in before:p.write_bytes(b'changed external proof')
            assert prep.validate_bundle(bundle)['_frozen']==prepared['_frozen']
        finally:
            for p,b in before.items():p.write_bytes(b)
    case('frozen-entry',frozen)
    for filename in ('session.bin','admission.json','segment-0.bin','launch.bin','assets.bin'):
        def mutation(d,name=filename):
            path=bundle/name;b=path.read_bytes()
            try:path.write_bytes(bytes([b[0]^1])+b[1:]);refused(lambda:prep.validate_bundle(bundle))
            finally:path.write_bytes(b)
        case('mutated-'+filename,mutation)
    for offset,value in [(112,1),(256,1),(640,1),(160,1),(168,2)]:
        def typed(d,o=offset,v=value):
            bad=bytearray(session);struct.pack_into('<Q',bad,o,v);refused(lambda:prep.session_profile(bytes(bad),layout,identity))
        case('typed-'+str(offset),typed)
    def missing_retention(d):
        value=json.loads(Path(admission['path']).read_bytes());value['conditions'].pop('retain-all-resources-until-power-removal')
        entry=json.dumps(value,sort_keys=True).encode();changed=bytearray(session);changed[80:112]=hashlib.sha256(entry).digest()
        refused(lambda:prep.validate_admission(entry,bytes(changed),layout,identity,prep.sha(image)))
    case('missing-retention',missing_retention)
    def paced_package(d):
        changed=bytearray(session);paced_assets=Path.home()/'.cache/sunstoneos/evidence/mimus-paced-20261016/inputs.bin'
        value=paced_assets.read_bytes();struct.pack_into('<2Q',changed,168,1,len(value));changed[216:248]=hashlib.sha256(value).digest()
        entry=json.loads(Path(admission['path']).read_bytes());ref=Path.home()/'.cache/sunstoneos/evidence/mimus-paced-20261016/reference.json'
        entry['recognition_reference']={'path':str(ref),'sha256':prep.digest(ref)};entry['configuration_sha256']=prep.configuration_sha256(changed)
        encoded=json.dumps(entry,sort_keys=True).encode();changed[80:112]=hashlib.sha256(encoded).digest()
        (d/'session.bin').write_bytes(changed);(d/'admission.json').write_bytes(encoded)
        value=prep.prepare(a.image,a.manifest,d/'session.bin',d/'admission.json',paced_assets,d/'bundle',a.clang,a.lld,a.objcopy)
        assert value['_frozen']['assets.bin']==paced_assets.read_bytes()
        return {'asset_sha256':prep.digest(paced_assets),'reference_sha256':prep.digest(ref),'recognition_rerun':False}
    case('paced-pinned-package',paced_package)
    pixels=b'PASS mimus: copied software fixture only\n'*80
    def frame(offset,size=1024,finished=1,poison=False):
        payload=pixels[offset:offset+size]
        return struct.pack('<4I2Q',len(pixels),finished,offset,len(payload),31,37)+(bytes(32) if poison else hashlib.sha256(session).digest())+payload
    def captures(d,mode):
        clock=Clock();record={'complete':False};calls=[];commits=[];verified=[]
        def exchange(packet,timeout):
            offset,ack=struct.unpack('<II',packet);assert offset==ack;calls.append((offset,ack))
            if offset:assert (d/'recognition.log').read_bytes()==pixels[:offset], 'ack preceded durable bytes'
            f=frame(offset)
            if mode=='pending' and len(calls)==1:f=struct.pack('<4I2Q',0,0,0,0,31,37)+hashlib.sha256(session).digest()
            if mode=='failure':f=struct.pack('<4I2Q',0,2,0,0,31,37)+hashlib.sha256(session).digest()
            if mode=='wrong-session':f=frame(offset,poison=True)
            if mode=='regression' and len(calls)>1:f=frame(offset,finished=0)
            if mode=='short':f=b'bad'
            if mode=='transport':raise OSError('fixture transfer failure')
            if mode=='late':clock.value=200
            return f
        def save():
            if mode=='journal' and record.get('mimus_result')=='observed-success':raise OSError('fixture journal failure')
            commits.append(copy.deepcopy(record))
        def verify(path):
            assert path.read_bytes()==pixels;verified.append(True)
            if mode=='reference-mismatch':raise ValueError('fixture reference mismatch')
        error=None
        try:host.capture(exchange,session,d,record,save,150,now=clock.now,pause=clock.pause,verify=verify)
        except (ValueError,RuntimeError,OSError) as ex:error=ex
        if mode in ('capture','pending'):
            assert not error and record['mimus_result']=='observed-success' and verified
            assert (d/'recognition.log').read_bytes()==pixels and calls[-1][0]==len(pixels)
        elif mode=='failure':assert not error and len(calls)==1 and record['mimus_result']=='observed-failure'
        else:assert error and not any(x.get('mimus_result')=='observed-success' for x in commits)
        assert record['mimus_retention']=='whole-board-power-removal' and not record['complete']
        return {'requests':len(calls),'observed':record['mimus_result'],'error':str(error) if error else None}
    for mode in ('capture','pending','failure','wrong-session','regression','short','transport','late','journal','reference-mismatch'):case('capture-'+mode,lambda d,m=mode:captures(d,m))
    def fragments(d,mode):
        calls=[];record={};clock=Clock();response=frame(0,0);chunks=[response[:17],response[17:]]
        def write(endpoint,packet,timeout):calls.append('write');return 7 if mode=='short-write' else 8
        def read(endpoint,count,timeout):
            calls.append('read')
            if mode=='fault' and len(chunks)==1:raise OSError('fixture midframe failure')
            return chunks.pop(0)
        device=types.SimpleNamespace(write=write,read=read);actual=host.capture
        def limited(exchange,*args,**kwargs):
            if mode=='success':assert exchange(struct.pack('<II',0,0),500)==response;return
            return actual(exchange,*args,**kwargs,now=clock.now,pause=clock.pause)
        with patch.object(host,'capture',limited),patch.object(host.time,'monotonic',clock.now):
            if mode=='success':host.capture_device(device,lambda:None,150,session,d,record,lambda:None)
            else:
                try:host.capture_device(device,lambda:None,150,session,d,record,lambda:None)
                except (ValueError,OSError):pass
                else:raise AssertionError('Transfer fault accepted')
        assert calls==(['write','read','read'] if mode!='short-write' else ['write'])
        return {'calls':calls}
    for m in ('success','fault','short-write'):case('fragment-'+m,lambda d,m=m:fragments(d,m))
    def queue(d):
        for name in ['admission.json','prepared.json','launch.bin','image.elf']:shutil.copy2(bundle/name,d/name)
        def ref(path):return {'path':str(path.absolute()),'sha256':host.digest(path)}
        paths=host.software_sources();note=d/'model.txt';note.write_text('Software queue model only; never physical admission.\n')
        software={'schema':'a733-mimus-supervised-software-v1','physical_access':False,'admission_cases':True,'launcher_cases':True,'host_capture':True,'host_lifecycle':True,
            'image':ref(d/'image.elf'),'launcher':ref(d/'launch.bin'),'sources':{k:ref(v) for k,v in paths.items()},'evidence':[ref(note)]}
        prepared={'files':{n:host.digest(d/n) for n in ['image.elf','launch.bin','admission.json']},
            '_prepared_sha256':host.digest(d/'prepared.json'),'_source_hashes':{k:host.digest(v) for k,v in paths.items()}}
        value=d/'software.json';value.write_text(json.dumps(software))
        prerequisites={'supervised-ram-recovery':ref(d/'admission.json'),'mimus-supervised-software':ref(value)}
        submission={'prerequisites':list(prerequisites),'artifacts':[ref(d/n) for n in ['prepared.json','image.elf','launch.bin']]};review={'prerequisites':prerequisites}
        database=d/'queue.sqlite';db=sqlite3.connect(database);db.executescript('CREATE TABLE settings(key TEXT,value INTEGER);INSERT INTO settings VALUES("halted",0);CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT);')
        db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('mimus-test','claimed','coordinator',json.dumps(submission),json.dumps(review)));db.commit()
        host.claimed(database,'mimus-test','coordinator',d,prepared)
        try:host.claimed(database,'mimus-test','another owner',d,prepared)
        except ValueError:pass
        else:raise AssertionError('Foreign claim accepted')
        db.execute('UPDATE settings SET value=1');db.commit()
        try:host.claimed(database,'mimus-test','coordinator',d,prepared)
        except ValueError:pass
        else:raise AssertionError('Halted queue accepted')
        # Replacement refs match every current replacement file, but not the
        # initial snapshots whose bytes would be uploaded by shared preload.
        db.execute('UPDATE settings SET value=0')
        (d/'image.elf').write_bytes(b'Replacement software-only image, never executed')
        (d/'prepared.json').write_bytes(b'Replacement software-only metadata')
        software['image']=ref(d/'image.elf');value.write_text(json.dumps(software))
        review['prerequisites']['mimus-supervised-software']=ref(value)
        submission['artifacts']=[ref(d/n) for n in ['prepared.json','image.elf','launch.bin']]
        db.execute('UPDATE tests SET submission=?,review=?',(json.dumps(submission),json.dumps(review)));db.commit()
        try:host.claimed(database,'mimus-test','coordinator',d,prepared)
        except ValueError:pass
        else:raise AssertionError('Replacement review authorized earlier upload bytes')
        db.close();return {'claim':'exact owner and frozen reviewed executable/admission identities','refused':['foreign owner','halted queue','post-validation replacement metadata/image with matching replacement refs']}
    case('queue-admission',queue)

    def wiring(d):
        # Exercise canonical runner dispatch only; shared physical lifecycle is reused.
        args=['--bundle',str(bundle),'--fel','/no-device','--evidence',str(d/'physical-never-created'),'--database','/no-queue','--claim','fixture','--owner','fixture','--supervised-manual-reconnect']
        def run(*args,**kw):
            assert kw['result_key']=='mimus_result' and kw['capture_seconds']==179 and kw['product']=='sunstoneos native mimus' and kw['serial']=='sunstoneos-a733-mimus';assert args[2]['_frozen']['session.bin']==session;return 0
        with patch.object(sup,'run_session',run):assert host.main(args)==0
    case('canonical-runner-wiring',wiring)
    print(str(len(state['cases']))+' focused mimus host/launcher cases passed; no physical access.')
if __name__=='__main__':main()
