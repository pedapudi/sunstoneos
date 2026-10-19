#!/usr/bin/env python3
"""New I2S packaging/host cases only; all physical facts and USB are fixtures."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import sys
import types
import unicorn
from unicorn import arm64_const as regs
from elftools.elf.elffile import ELFFile
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def load(path):
    s=importlib.util.spec_from_file_location('i2s_host_cases',path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class Clock:
    value=100.
    def now(self):return self.value
    def pause(self,n):self.value+=max(.001,n)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('image','manifest','evidence','clang','lld','objcopy'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument("--duplex-only",action="store_true")
    p.add_argument("--native-output-fixture",type=Path)
    a=p.parse_args();out=a.evidence;out.mkdir(parents=True,exist_ok=True)
    host=load(ROOT/'tools/check-a733-i2s-supervised.py');prep=host.preparation;sup=host.supervisor
    paths=[Path(__file__),Path(host.__file__),Path(prep.__file__),Path(host.decoder.__file__),Path(sup.__file__),Path(sup.base.__file__),Path(sup.observer.__file__),ROOT/'checks/a733-smp-supervised-launch.py',ROOT/'platform/a733-smp-supervised/launch.c',ROOT/'platform/a733-smp-observe/launch.S',ROOT/'platform/a733-smp-observe/launch.ld',ROOT/'platform/a733-smp-cached/boot.h',a.image,a.manifest,a.clang,a.lld,a.objcopy]
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
    image=a.image.read_bytes();parts,address,identity,layout,pcm=prep.image_data(image)
    setup=out/'baseline';setup.mkdir(exist_ok=True)
    raw=bytearray(layout['size'])
    def put(name,value):
        f=layout['fields'][name];b=value if isinstance(value,bytes) else value.to_bytes(f['size'],'little');assert len(b)==f['size'];raw[f['offset']:f['offset']+f['size']]=b
    for n,v in {'magic':0x4153373341555331,'version':1,'bytes':layout['size'],'epoch':31,'generation':37,'lease':41,'shared.identity':37,'shared.pmic.identity':37,'shared.pmic.owner':4,'shared.usb.owner':5,'shared.usb.handoff':37,'dma.allowed.owner':6,'dma.allowed.handoff':41,'dma.evidence':41,'dma.established':0x5f,'dma.owned_channels':0xffff,'dma.service_channels':1,'dma.controller':0x04601000}.items():put(n,v)
    if a.duplex_only:
        for n,v in {'direction':2,'dma.service_channels':3,'dma.allowed.desired_cpu':3,'dma.allowed.route_change_mask':3}.items():put(n,v)
    put('image',identity);put('pcm',hashlib.sha256(pcm).digest())
    def write(name,value):
        data=value if isinstance(value,bytes) else json.dumps(value,sort_keys=True).encode();path=(setup/name).resolve();path.write_bytes(data);return {'path':str(path),'sha256':prep.sha(data)}
    proof=write('software-only.txt',b'SOFTWARE FIXTURE ONLY. No board ownership, power, mapping, drain or isolation established. Never deploy.\n')
    common={'identity':[0,0,0,0,0],'port':'software-only-never-deploy','image_sha256':prep.sha(image)}
    ddr=write('ddr.json',{**common,'schema':'a733-ddr-reservation-v1','ranges':[[0x41000000,0x42200000]],'exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'evidence':[proof]})
    conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
    platform=write('platform.json',{**common,'schema':'a733-smp-observer-platform-v1','conditions':dict.fromkeys(conditions,proof),'entry_conditions_established':True,'physical_coherence_result':'under-test'})
    profile={'epoch':31,'generation':37,'lease':41,'direction':2 if a.duplex_only else 0,'audio_cold':0}
    hashes={n+'_sha256':prep.sha(prep.field(raw,layout,n)) for n in ('shared','dma','inherited')}
    recovery=write('recovery.json',{**common,**profile,**hashes,'schema':'a733-i2s-initial-entry-v1','conditions':dict.fromkeys(prep.CONDITIONS,proof),'one_attempt':True,'whole_board_power_removal':True,'no_resource_reuse_before_power_removal':True})
    admission=write('admission.json',{**common,**profile,**hashes,'schema':'a733-i2s-supervised-entry-v1','ddr':ddr,'platform':platform,'recovery':recovery})
    for n,ref in [('memory',ddr),('recovery',recovery),('evidence',admission)]:put(n,bytes.fromhex(ref['sha256']))
    session=bytes(raw);write('session.bin',session)
    # Aggregate independent setup checks before compilation or cases.
    errors=[]
    for name,check in [('session',lambda:prep.session_profile(session,layout,identity,pcm)),('admission',lambda:prep.validate_admission(Path(admission['path']).read_bytes(),session,layout,identity,pcm,prep.sha(image))),('manifest',lambda:sup.require(json.loads(a.manifest.read_bytes())['image_sha256']==prep.sha(image),'Final manifest identity differs')),('PCM',lambda:sup.require(len(pcm)==16384 and len(session)==552,'Actual final geometry differs'))]:
        try:check()
        except BaseException as ex:errors.append(name+': '+str(ex))
    assert not errors,'; '.join(errors)
    bundle=out/'bundle'
    def package(d):
        if not bundle.exists():prep.prepare(a.image,a.manifest,setup/'session.bin',Path(admission['path']),bundle,a.clang,a.lld,a.objcopy)
        value=prep.validate_bundle(bundle);request,contract=prep.request_bytes(address,identity,session,profile)
        assert value['_frozen']['request.bin']==request and value['_frozen']['contract.bin']==contract
        assert value['segments'][-1]=={'name':'session.bin','address':0x41f00000}
        return {'image_sha256':value['image_sha256'],'launcher_sha256':value['launcher_sha256'],'fixture_not_physical':True}
    case('exact-package',package);prepared=prep.validate_bundle(bundle)
    if a.duplex_only:
        def duplex_capture(d):
            assert a.native_output_fixture,'Actual compiled USB producer required'
            with a.native_output_fixture.open('rb') as stream:
                elf=ELFFile(stream)
                segments=[(int(p['p_vaddr']),bytes(p.data()),int(p['p_memsz'])) for p in elf.iter_segments() if p['p_type']=='PT_LOAD']
                symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
            def produced(offset):
                uc=unicorn.Uc(unicorn.UC_ARCH_ARM64,unicorn.UC_MODE_ARM);pages=set()
                for address,data,size in segments:
                    for page in range(address&~4095,(address+size+4095)&~4095,4096):
                        if page not in pages:uc.mem_map(page,4096);pages.add(page)
                    uc.mem_write(address,data)
                uc.mem_map(0x41f00000,4096);uc.mem_write(0x41f00000,session)
                uc.mem_map(0x50000000,0x10000);uc.reg_write(regs.UC_ARM64_REG_SP,0x5000f000)
                uc.reg_write(regs.UC_ARM64_REG_X0,3+offset//4096)
                uc.hook_add(unicorn.UC_HOOK_INTR,lambda machine,*_:machine.emu_stop())
                uc.emu_start(symbols['i2s_check_run'],0,count=3000000)
                assert int.from_bytes(uc.mem_read(symbols['i2s_check_result'],4),'little')==1
                wire=bytes(uc.mem_read(symbols['wire'],4192))
                (d/f'producer-offset-{offset}.bin').write_bytes(wire);return wire
            frames={offset:produced(offset) for offset in (0,4096,8192,12288)}
            def frame(packet,timeout):
                (op,offset,count)=struct.unpack('<3I',packet)
                if op==1:return frames[offset]
                # STATUS shares the native completion metadata, without payload.
                value=bytearray(frames[0][:96]);struct.pack_into('<I',value,16,0);return bytes(value)
            record={};clock=Clock()
            host.capture(frame,session,d,record,lambda:None,150,pcm=pcm,direction=2,now=clock.now,pause=clock.pause)
            assert record['i2s_result']=='observed-success'
            assert record['copied_i2s_output']['duplex_completion']==dict(capture_bytes=16384,playback_bytes=16384,capture_periods=4,playback_periods=4,drained_directions=3)
            nominal=frame(struct.pack('<3I',0,0,0),100)
            for offset,value in ((76,0),(84,3),(88,1),(92,1)):
                broken=bytearray(nominal);struct.pack_into('<I',broken,offset,value)
                refused(lambda:host.decoder.decode(bytes(broken),session,0))
            refused(lambda:host.decoder.decode(nominal[:72],session,0))
            return {'duplex_counts':'both transfers and drains required','physical':False}
        case('duplex-copy-and-incomplete-count-refusal',duplex_capture)
        print('PASS producer-bound duplex package and copied completion counts; no physical access.')
        return
    def launcher(d):
        original=(ROOT/'checks/a733-smp-supervised-launch.py').read_text();start=original.index('    if args.frequency_only:');end=original.index('    result =',start)
        text=original[:start]+"    cases=[check('I2S sixty-second publication',12),check('wrong session size',3,mutation=(CONTRACT+184,1)),check('wrong session digest',3,mutation=(CONTRACT+192,0)),check('armed watchdog refuses',0,initial={MODE:0x41})]\n"+original[end:]
        text=text.replace('480000000','1440000000').replace('0x4153373353554333',hex(prep.CONTRACT_MAGIC));script=d/'launcher-cases.py';script.write_text(text)
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
    for filename in ('session.bin','admission.json','segment-0.bin','launch.bin'):
        def mutation(d,name=filename):
            path=bundle/name;b=path.read_bytes()
            try:path.write_bytes(bytes([b[0]^1])+b[1:]);refused(lambda:prep.validate_bundle(bundle))
            finally:path.write_bytes(b)
        case('mutated-'+filename,mutation)
    for n,v in [('shared.usb.deadline_us',1),('dma.established',0x7f),('dma.allowed.handoff',42)]:
        def typed(d,n=n,v=v):
            b=bytearray(session);f=layout['fields'][n];b[f['offset']:f['offset']+f['size']]=v.to_bytes(f['size'],'little');refused(lambda:prep.session_profile(bytes(b),layout,identity,pcm))
        case('typed-'+n,typed)
    def missing_prior_drain(d):
        cache={path:prepared['_frozen'][item['name']] for path,item in prepared['entry_evidence'].items()}
        admission_value=json.loads(prepared['_frozen']['admission.json']);facts_path=admission_value['recovery']['path']
        facts=json.loads(cache[facts_path]);facts['conditions'].pop('fifo-producers-quiescent-and-prior-dma-drained')
        facts_raw=json.dumps(facts,sort_keys=True).encode();cache[facts_path]=facts_raw
        admission_value['recovery']['sha256']=prep.sha(facts_raw);entry_raw=json.dumps(admission_value,sort_keys=True).encode()
        changed=bytearray(session)
        for name,value in [('recovery',hashlib.sha256(facts_raw).digest()),('evidence',hashlib.sha256(entry_raw).digest())]:
            f=layout['fields'][name];changed[f['offset']:f['offset']+f['size']]=value
        refused(lambda:prep.validate_admission(entry_raw,bytes(changed),layout,identity,pcm,prep.sha(image),cache,True))
        return {'refusal':'Missing prior drain despite consistent rebound evidence/session digests'}
    case('missing-initial-prior-drain',missing_prior_drain)
    def frame(op,offset,count,state=2,poison=False):
        payload=(pcm[offset:offset+count] if op==1 and state==2 else b'')
        return struct.pack('<6I2Q',0x53493241,1,state,offset,len(payload),16384,31,37)+(bytes(32) if poison else hashlib.sha256(session).digest())+payload
    def captures(d,mode):
        clock=Clock();record={'complete':False};calls=[];commits=[]
        def exchange(packet,timeout):
            op,offset,count=struct.unpack('<III',packet);calls.append((op,offset,count));f=frame(op,offset,count)
            if mode=='pending' and len(calls)==1:f=frame(op,offset,count,1)
            if mode=='failure':f=frame(op,offset,count,3)
            if mode=='wrong-session':f=frame(op,offset,count,poison=True)
            if mode=='regression' and op==1:f=frame(op,offset,count,1)
            if mode=='playback-mismatch' and op==1:f=f[:72]+bytes([f[72]^1])+f[73:]
            if mode=='short':f=b'bad'
            if mode=='transport':raise OSError('fixture transfer failure')
            if mode=='late':clock.value=200
            return f
        def save():
            if mode=='journal' and record.get('i2s_result')=='observed-success':raise OSError('fixture journal failure')
            commits.append(copy.deepcopy(record))
        error=None
        try:host.capture(exchange,session,d,record,save,150,pcm=pcm,direction=1 if mode in ('playback','playback-mismatch') else 0,now=clock.now,pause=clock.pause)
        except (ValueError,RuntimeError,OSError) as ex:error=ex
        if mode in ('capture','playback','pending'):
            assert not error and record['i2s_result']=='observed-success' and (d/'pcm-s16le-16000-mono.bin').read_bytes()==pcm
            assert record['copied_i2s_output']['observation']==('dma-memory-consumed' if mode=='playback' else 'captured-periods')
        elif mode=='failure':assert not error and len(calls)==1 and record['i2s_result']=='observed-failure'
        else:assert error and not any(x.get('i2s_result')=='observed-success' for x in commits)
        assert record['i2s_retention']=='whole-board-power-removal' and not record['complete']
        return {'requests':len(calls),'observed':record['i2s_result'],'error':str(error) if error else None}
    for mode in ('capture','playback','playback-mismatch','pending','failure','wrong-session','regression','short','transport','late','journal'):case('capture-'+mode,lambda d,m=mode:captures(d,m))
    def fragments(d,mode):
        calls=[];record={};clock=Clock();response=frame(0,0,0);chunks=[response[:17],response[17:]]
        def write(endpoint,packet,timeout):calls.append('write');return 11 if mode=='short-write' else 12
        def read(endpoint,count,timeout):
            calls.append('read')
            if mode=='fault' and len(chunks)==1:raise OSError('fixture midframe failure')
            return chunks.pop(0)
        device=types.SimpleNamespace(write=write,read=read);actual=host.capture
        def limited(exchange,*args,**kwargs):
            if mode=='success':assert exchange(struct.pack('<III',0,0,0),500)==response;return
            return actual(exchange,*args,**kwargs,now=clock.now,pause=clock.pause)
        with patch.object(host,'capture',limited),patch.object(host.time,'monotonic',clock.now):
            if mode=='success':host.capture_device(device,lambda:None,150,session,d,record,lambda:None,pcm=pcm,direction=0)
            else:
                try:host.capture_device(device,lambda:None,150,session,d,record,lambda:None,pcm=pcm,direction=0)
                except (ValueError,OSError):pass
                else:raise AssertionError('Transfer fault accepted')
        assert calls==(['write','read','read'] if mode!='short-write' else ['write'])
        if mode=='fault':assert (d/'i2s-fragment-001-001.bin').read_bytes()==response[:17]
        return {'calls':calls}
    for m in ('success','fault','short-write'):case('fragment-'+m,lambda d,m=m:fragments(d,m))
    def queue(d):
        for name in ['admission.json','prepared.json','launch.bin','image.elf']:shutil.copy2(bundle/name,d/name)
        def ref(path):return {'path':str(path.absolute()),'sha256':host.digest(path)}
        paths=host.software_sources();note=d/'model.txt';note.write_text('Software queue model only; never physical admission.\n')
        software={'schema':'a733-i2s-supervised-software-v1','physical_access':False,'admission_cases':True,'launcher_cases':True,'host_capture':True,'host_lifecycle':True,
            'image':ref(d/'image.elf'),'launcher':ref(d/'launch.bin'),'sources':{k:ref(v) for k,v in paths.items()},'evidence':[ref(note)]}
        prepared={'files':{n:host.digest(d/n) for n in ['image.elf','launch.bin','admission.json']},
            '_prepared_sha256':host.digest(d/'prepared.json'),'_source_hashes':{k:host.digest(v) for k,v in paths.items()}}
        value=d/'software.json';value.write_text(json.dumps(software))
        prerequisites={'supervised-ram-recovery':ref(d/'admission.json'),'i2s-supervised-software':ref(value)}
        submission={'prerequisites':list(prerequisites),'artifacts':[ref(d/n) for n in ['prepared.json','image.elf','launch.bin']]};review={'prerequisites':prerequisites}
        database=d/'queue.sqlite';db=sqlite3.connect(database);db.executescript('CREATE TABLE settings(key TEXT,value INTEGER);INSERT INTO settings VALUES("halted",0);CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT);')
        db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('i2s-test','claimed','coordinator',json.dumps(submission),json.dumps(review)));db.commit()
        host.claimed(database,'i2s-test','coordinator',d,prepared)
        try:host.claimed(database,'i2s-test','another owner',d,prepared)
        except ValueError:pass
        else:raise AssertionError('Foreign claim accepted')
        db.execute('UPDATE settings SET value=1');db.commit()
        try:host.claimed(database,'i2s-test','coordinator',d,prepared)
        except ValueError:pass
        else:raise AssertionError('Halted queue accepted')
        # Replacement refs match every current replacement file, but not the
        # initial snapshots whose bytes would be uploaded by shared preload.
        db.execute('UPDATE settings SET value=0')
        (d/'image.elf').write_bytes(b'Replacement software-only image, never executed')
        (d/'prepared.json').write_bytes(b'Replacement software-only metadata')
        software['image']=ref(d/'image.elf');value.write_text(json.dumps(software))
        review['prerequisites']['i2s-supervised-software']=ref(value)
        submission['artifacts']=[ref(d/n) for n in ['prepared.json','image.elf','launch.bin']]
        db.execute('UPDATE tests SET submission=?,review=?',(json.dumps(submission),json.dumps(review)));db.commit()
        try:host.claimed(database,'i2s-test','coordinator',d,prepared)
        except ValueError:pass
        else:raise AssertionError('Replacement review authorized earlier upload bytes')
        db.close();return {'claim':'exact owner and frozen reviewed executable/admission identities','refused':['foreign owner','halted queue','post-validation replacement metadata/image with matching replacement refs']}
    case('queue-admission',queue)

    def wiring(d):
        # Exercise canonical runner dispatch only; shared physical lifecycle is reused.
        args=['--bundle',str(bundle),'--fel','/no-device','--evidence',str(d/'physical-never-created'),'--database','/no-queue','--claim','fixture','--owner','fixture','--supervised-manual-reconnect']
        def run(*args,**kw):
            assert kw['result_key']=='i2s_result' and kw['capture_seconds']==59 and kw['product']=='sunstoneos native I2S' and kw['serial']=='sunstoneos-a733-i2s';assert args[2]['_frozen']['session.bin']==session;return 0
        with patch.object(sup,'run_session',run):assert host.main(args)==0
    case('canonical-runner-wiring',wiring)
    print(str(len(state['cases']))+' focused I2S host/launcher cases passed; no physical access.')
if __name__=='__main__':main()
