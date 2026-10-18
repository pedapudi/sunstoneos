#!/usr/bin/env python3
"""One fixed native table-construction diagnostic, returning before cache publication."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import sqlite3
import struct
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
prior=module('startup_prior',ROOT/'tools/run-a64-entry-probe.py')
base=prior.base
supervised=module('startup_supervised',ROOT/'tools/check-a733-smp-supervised.py')
require,digest,save=base.require,base.digest,base.save_json
NATIVE=prior.NATIVE
LAUNCH='2b0641c2509963ad08d03e8498ee865b5d858c4536b9ea26fbd26ca576e698ea'
MAGIC=0x4137335354415254
REPORT=0x42002000
# Includes every byte written by actual stack/BSS initialization and admission.
DRAM=(('native-code',0x41000000,0x12078),('native-data',0x41200000,0x21),
      ('el1-bss',0x41201000,0x8fd10),('result',0x42000000,0x4000),
      ('contract',0x42004000,0x1000),('monitor-bss',0x42005000,0xa590),
      ('boot-stack',0x42010000,0x4000),('overlay',0x41020000,0x1000),
      ('tables',0x421f0000,0x5000))

def branch(source,target):
    offset=target-source;require(offset%4==0 and -(1<<27)<=offset<(1<<27),'Branch out of range.')
    return 0x14000000|((offset//4)&0x3ffffff)

def prepare(a):
    require(digest(a.observer)==NATIVE,'Only the unchanged reviewed native ELF is supported.')
    from elftools.elf.elffile import ELFFile
    with a.observer.open('rb') as f:
        elf=ELFFile(f);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    require(tuple(symbols[n] for n in prior.SYMBOLS)==prior.LAYOUT,'Native layout changed.')
    segments,_,_=base.image_parts(a.observer)
    a.build.mkdir(parents=True,exist_ok=False)
    commands=[[a.clang,'--target=aarch64-none-elf','-march=armv8-a','-c',ROOT/'boot/a733/native-startup-probe.S','-o',a.build/'overlay.o'],
        [a.lld,'-Ttext=0x41020000','-e','boot_checkpoint',a.build/'overlay.o','-o',a.build/'overlay.elf'],
        [a.objcopy,'-O','binary',a.build/'overlay.elf',a.build/'overlay.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (a.build/'build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'Overlay build failed; inspect log.')
    require(0<(a.build/'overlay.bin').stat().st_size<=0x1000,'Overlay exceeds reservation.')
    with (a.build/'overlay.elf').open('rb') as f:
        hooks={s.name:s['st_value'] for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()}
    code=bytearray(segments[0]['data']);patches=[]
    def patch(address,old,target):
        offset=address-0x41000000;require(struct.unpack_from('<I',code,offset)[0]==old,'Native instruction differs.')
        new=branch(address,target);struct.pack_into('<I',code,offset,new);patches.append([address,old,new])
    for address,old,name in [(0x410000d0,0xd102c3ff,'boot_checkpoint'),(0x41004b84,0xd5033f9f,'cached_checkpoint'),
                            (0x4100a09c,0x52801fe8,'stop_checkpoint'),(0x4100009c,0xd503205f,'park_checkpoint')]:
        patch(address,old,hooks[name])
    for slot in range(16):
        address=0x41008800+128*slot
        require(struct.unpack_from('<I',code,address-0x41000000)[0]==0xd2800012+(slot<<5),'Vector slot marker differs.')
        patch(address+4,branch(address+4,0x410082c8),hooks['fault_checkpoint'])
    payloads=[]
    for index,segment in enumerate(segments):
        if segment['data']:
            data=bytes(code) if index==0 else segment['data'];name=f'segment-{index}.bin'
            (a.build/name).write_bytes(data);payloads.append({'name':name,'address':segment['address'],'bytes':len(data)})
    save(a.build/'commands.json',[[str(x) for x in c] for c in commands])
    save(a.build/'manifest.json',{'schema':'a733-native-startup-probe-v1','physical_access':False,'native_sha256':NATIVE,
        'source_sha256':digest(ROOT/'boot/a733/native-startup-probe.S'),'host_sha256':digest(Path(__file__)),
        'patches':patches,'payloads':payloads,'hooks':{n:hooks[n] for n in ('boot_checkpoint','cached_checkpoint','stop_checkpoint','park_checkpoint','fault_checkpoint')},
        'files':{p.name:digest(p) for p in a.build.iterdir() if p.is_file()}})

def decode(raw,nonce):
    require(len(raw)==256,'Short startup report.');v=struct.unpack('<32Q',raw)
    require(v[:3]==(MAGIC,1,256) and v[3] in range(2,6) and raw[240:]==nonce,'Startup report identity differs.')
    names=('checkpoint','entered_c','entry_sp','CurrentEL','MPIDR_EL1','SCTLR_EL3','SCTLR_EL1','CNTFRQ_EL0','counter','status','detail','vector','esr','elr','far','terminal_sp')
    return dict(zip(names,v[3:19]))

def caller_interval(survey):
    require(survey.get('complete') is True and survey.get('sram_restored') is True and survey.get('rom_usable') is True,'Completed current returning survey required.')
    sp=survey['result']['sp'];require(type(sp) is int and 0x4003c<=sp<=0x100000 and sp%4==0,'Caller SP outside reviewed SRAM.')
    require(all(sp<=address or sp-60>=address+size for _,address,size in base.REGIONS),'Caller stack overlaps launcher reservations.')
    return sp-60,60

def claimed(a,expected):
    db=sqlite3.connect(a.database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'Queue halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(a.claim,)).fetchone()
        require(row and row['state']=='claimed' and row['owner']==a.owner,'Coordinator claim required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==set(review['prerequisites'])=={'supervised-ram-recovery','native-startup-software'},'Unexpected prerequisites.')
        _,software=prior.reference(review['prerequisites']['native-startup-software'])
        require(hashlib.sha256(software).hexdigest()==expected[a.software.resolve()],'Reviewed software differs.')
        covered={}
        for item in submission['artifacts']:
            path,data=prior.reference(item);covered[path]=hashlib.sha256(data).hexdigest()
        require(all(covered.get(path)==identity for path,identity in expected.items()),'Queue covers different inputs.')
        _,raw=prior.reference(review['prerequisites']['supervised-ram-recovery']);return json.loads(raw),hashlib.sha256(raw).hexdigest()
    finally:db.close()

def run(a):
    a.build=a.build.resolve();raw=(a.build/'manifest.json').read_bytes();m=json.loads(raw);manifest_hash=hashlib.sha256(raw).hexdigest()
    require(m['schema']=='a733-native-startup-probe-v1' and m['native_sha256']==NATIVE,'Wrong startup image.')
    require(m['source_sha256']==digest(ROOT/'boot/a733/native-startup-probe.S') and m['host_sha256']==digest(Path(__file__)),'Implementation changed.')
    frozen={name:(a.build/name).read_bytes() for name in m['files']}
    require(all(Path(n).name==n and hashlib.sha256(b).hexdigest()==m['files'][n] for n,b in frozen.items()),'Frozen build changed.')
    prepared=supervised.validate_bundle(a.bundle)
    require(prepared['image_sha256']==NATIVE and prepared['launcher_sha256']==LAUNCH,'Native/launcher identity differs.')
    survey_raw=a.survey.read_bytes();survey=json.loads(survey_raw);caller=caller_interval(survey)
    require(all(survey['result']['launcher_gates'].values()),'Actual A32 launcher gates fail; establish cache-off state separately.')
    software_raw=a.software.read_bytes();software=json.loads(software_raw)
    require(software.get('schema')=='a733-native-startup-software-v1' and software.get('physical_access') is False and
            software.get('manifest_sha256')==manifest_hash and software.get('instruction_cases') is True,'Focused instruction evidence required.')
    expected={a.build/'manifest.json':manifest_hash,a.software.resolve():hashlib.sha256(software_raw).hexdigest(),
              a.survey.resolve():hashlib.sha256(survey_raw).hexdigest(),(a.bundle/'prepared.json').resolve():prepared['_prepared_sha256']}
    recovery,recovery_hash=claimed(a,expected);require(digest(a.fel)==base.FEL_HASH,'Pinned FEL executable required.')
    a.evidence.mkdir(parents=True,exist_ok=False)
    record={'physical_access':True,'complete':False,'launch_attempted':False,'transactions':[],'claim':a.claim,'owner':a.owner,
        'manifest_sha256':expected[a.build/'manifest.json'],'recovery_sha256':recovery_hash,'caller_interval':caller,'sram_after_warm_return':'Owned by new ROM; old bytes are evidence only.'}
    def journal():save(a.evidence/'session.json',record)
    access=module('startup_access',ROOT/'tools/usb-access.py');sys.path.insert(0,str(Path.home()/'git/mimus/tools'));import usb_guard
    usb_guard.check_health()
    with access.board_lock() as node:
        access.require_session(node);access.require_rom(node);identity=access.identity(node);port=access.target(node);config={'port':port.name,'sysfs':str(port.resolve())};failed=False;dirty=False
        require(survey.get('identity')==identity and survey.get('port')==port.name and recovery.get('identity')==identity and recovery.get('port')==port.name,'Survey/recovery covers another live instance.')
        supervised.validate_admission(json.loads(prepared['_frozen']['admission.json']),NATIVE,identity,port.name)
        record.update(initial_identity=identity,initial_node=str(node),port=port.name);journal()
        def transaction(*arguments,pass_fds=()):
            nonlocal failed
            require(not failed and access.identity(node)==identity,'Transport failed or instance changed; stop.');access.require_rom(node)
            entry={'arguments':list(map(str,arguments))};record['transactions'].append(entry);journal()
            try:
                result=subprocess.run([str(a.fel),'-d',f'{int(node.parent.name)}:{int(node.name)}',*map(str,arguments)],capture_output=True,text=True,timeout=15,pass_fds=pass_fds)
                entry.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr);require(result.returncode==0,'FEL transaction failed.')
            except BaseException as error:failed=True;entry['failure']=str(error);journal();raise
            journal();return result.stdout
        def transfer(address,data,destination):
            with base.sealed_source(data) as (path,fd):transaction('write',hex(address),path,'read',hex(address),str(len(data)),destination,pass_fds=(fd,))
            require(destination.read_bytes()==data,'Transfer comparison differs.')
        try:
            require('soc=00001903(A733)' in transaction('version'),'Not an A733 ROM instance.')
            words=transaction('readl','0x20','readl','0x24','readl','0x28','readl','0x3c','readl','0x40')
            require([int(v,16) for v in words.split()]==[0xeb00008f,0xe3a00911,0xe3a0193d,0xe3a0605c,0xea00004d],'Forced-FEL destination changed.')
            backups={}
            for name,address,size in (*DRAM,*base.REGIONS,('caller-evidence',*caller)):
                path=a.evidence/(name+'-before.bin');transaction('read',hex(address),str(size),path)
                data=path.read_bytes();require(len(data)==size,'Short backup.');base.sync_file(path);backups[name]=data
            nonce=secrets.token_bytes(16);initial=bytearray(0x4000);initial[0x2000+240:0x2000+256]=nonce;record['nonce']=nonce.hex();dirty=True;journal()
            uploads=[(s['address'],frozen[s['name']],s['name']) for s in m['payloads']]
            uploads += [(0x41020000,frozen['overlay.bin'],'overlay.bin'),(0x42000000,bytes(initial),'result.bin'),
                        (0x42004000,prepared['_frozen']['contract.bin'],'contract.bin'),(0x52c00,prepared['_frozen']['request.bin'],'request.bin'),
                        (0x52d00,bytes(128),'launch-report.bin'),(0x52000,prepared['_frozen']['launch.bin'],'launch.bin')]
            for address,data,name in uploads:transfer(address,data,a.evidence/('upload-'+name))
            _,current_hash=claimed(a,expected);require(current_hash==recovery_hash,'Recovery review changed.')
            with usb_guard.reenumeration('sunstoneos-a733','Native table construction checkpoint and forced-FEL return',2):
                record['launch_attempted']=True;journal();transaction('exe','0x52000')
                deadline=time.monotonic()+10;old=identity
                while True:
                    candidate=access.session_node(config)
                    if candidate is not None and access.identity(candidate)!=old and os.access(candidate,os.R_OK|os.W_OK):break
                    require(time.monotonic()<deadline,'No new FEL instance; no protocol retry.');time.sleep(.05)
                node=candidate;identity=access.identity(node);access.require_session(node);access.require_rom(node)
                record.update(returned_identity=identity,returned_node=str(node));journal()
            require('soc=00001903(A733)' in transaction('version'),'Return is not A733 ROM service.')
            for name,address,size in [('report',REPORT,256),('native-result',0x42000000,128),('admission',0x42004000,224),('tables',0x421f0000,0x5000)]:
                transaction('read',hex(address),str(size),a.evidence/(name+'.bin'));base.sync_file(a.evidence/(name+'.bin'))
            error=None
            try:record['result']=decode((a.evidence/'report.bin').read_bytes(),nonce)
            except (ValueError,struct.error) as e:error=e
            for name,address,size in DRAM:transfer(address,backups[name],a.evidence/(name+'-restored.bin'))
            dirty=False;record.update(rom_usable=True,dram_restored=True,complete=error is None);journal()
            if error:raise error
        except BaseException as error:
            record.update(failure=str(error),transport_failed=failed)
            if dirty or failed:
                try:base.stop_after_failure(record,journal,lambda:access.quarantine_instance(node,'Native startup diagnostic stopped; no retry; coordinator recovery required.',identity),error)
                finally:
                    if dirty and not record['launch_attempted']:base.hold_ownership()
            else:journal()
            raise
    return 0

def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='operation',required=True)
    build=sub.add_parser('prepare')
    for name in ('build','observer','clang','lld','objcopy'):build.add_argument('--'+name,type=Path,required=True)
    live=sub.add_parser('run')
    for name in ('build','bundle','software','survey','database','fel','evidence'):live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--claim',required=True);live.add_argument('--owner',required=True);a=p.parse_args()
    if a.operation=='prepare':prepare(a);return 0
    return run(a)
if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,OSError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
