#!/usr/bin/env python3
"""One stackless A64 entry capture and the established forced-FEL return."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import sqlite3
import struct
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('a64_probe_base',ROOT/'tools/check-a733-smp.py')
require,digest,save=base.require,base.digest,base.save_json
NATIVE='b6b1f9115cf36644f070407002d5d8b3150e24fa522e6f01f4e04a9390afde50'
SYMBOLS=('__a733_smp_boot_stack_top','__monitor_bss_start','__monitor_bss_end','__tables_start','__tables_end','__bss_start','__bss_end')
LAYOUT=(0x42014000,0x42005000,0x4200f590,0x421f0000,0x421f5000,0x41201000,0x41290d10)
MAGIC=0x413634454e545259

def prepare(a):
    require(digest(a.observer)==NATIVE,'Probe layout requires the preserved native image.')
    from elftools.elf.elffile import ELFFile
    with a.observer.open('rb') as f:
        symbols={s.name:s['st_value'] for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()}
    require(tuple(symbols[n] for n in SYMBOLS)==LAYOUT,'Native ELF layout changed.')
    a.build.mkdir(parents=True,exist_ok=False);shutil.copy2(ROOT/'boot/a733/a64-entry-probe.S',a.build/'probe.S')
    commands=[[a.clang,'--target=aarch64-none-elf','-march=armv8-a','-c',a.build/'probe.S','-o',a.build/'probe.o'],
        [a.lld,'-Ttext=0x41000000','-e','_start',a.build/'probe.o','-o',a.build/'probe.elf'],
        [a.objcopy,'-O','binary',a.build/'probe.elf',a.build/'probe.bin']]
    for command in commands:
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (a.build/'build.log').open('a') as f:f.write(result.stdout+result.stderr)
        require(result.returncode==0,'Probe build failed; inspect log.')
    require(0<(a.build/'probe.bin').stat().st_size<=0x10000,'Probe exceeds reviewed code interval.')
    save(a.build/'commands.json',[[str(v) for v in c] for c in commands])
    save(a.build/'manifest.json',{'schema':'a733-a64-entry-probe-v1','physical_access':False,'native_sha256':NATIVE,
        'layout':dict(zip(SYMBOLS,LAYOUT)),'source_sha256':digest(ROOT/'boot/a733/a64-entry-probe.S'),'host_sha256':digest(Path(__file__)),
        'files':{p.name:digest(p) for p in a.build.iterdir() if p.is_file()}})

def reference(item):
    p=Path(item['path']);require(p.is_absolute(),'Absolute review reference required.');data=p.read_bytes()
    require(hashlib.sha256(data).hexdigest()==item['sha256'],'Review reference changed.');return p.resolve(),data

def claimed(a,expected):
    db=sqlite3.connect(a.database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'Queue halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(a.claim,)).fetchone()
        require(row and row['state']=='claimed' and row['owner']==a.owner,'Matching coordinator claim required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==set(review['prerequisites'])=={'supervised-ram-recovery','a64-entry-probe-software'},'Probe prerequisites differ.')
        software_path,software=reference(review['prerequisites']['a64-entry-probe-software'])
        require(software_path==a.software.resolve() and hashlib.sha256(software).hexdigest()==expected[a.software.resolve()],'Reviewed software differs.')
        covered={}
        for item in submission['artifacts']:
            path,data=reference(item);covered[path]=hashlib.sha256(data).hexdigest()
        require(all(covered.get(path)==identity for path,identity in expected.items()),'Queue covers different frozen probe inputs.')
        path,data=reference(review['prerequisites']['supervised-ram-recovery']);return json.loads(data),hashlib.sha256(data).hexdigest()
    finally:db.close()

def decode(raw,nonce,pre):
    require(len(raw)==256,'Short probe report.');v=struct.unpack('<32Q',raw)
    require(v[:4]==(MAGIC,1,256,4) and raw[240:]==nonce and v[13]==pre and not v[14] and not v[29],'Probe identity/stage differs.')
    require(v[27] in (0,1),'Invalid exception indicator.')
    if not v[27]:require(v[4]==12 and tuple(v[20:27])==LAYOUT,'Probe state/layout differs.')
    else:require(v[18] in (1,2,3),'Unknown exception slot.')
    names=('CurrentEL','MIDR_EL1','MPIDR_EL1','CNTFRQ_EL0','counter_post','RVBAR_EL3','SCTLR_EL3','SCTLR_EL1','SCR_EL3')
    result=dict(zip(names,v[4:13]));result.update(counter_pre=v[13],counter_second=v[28],exception=bool(v[27]),esr=v[15],elr=v[16],far=v[17],vector=v[18],entry_sp=v[19],layout=dict(zip(SYMBOLS,v[20:27])))
    if not v[27]:result['entry_invariants']={'el3':v[4]==12,'primary':not v[6]&~0xc1000000,'sctlr3':not v[10]&0x02001005,'counter_monotonic':v[28]>=v[8]>=pre}
    if not v[27]:result['cached_initial_sctlr1_predicate']=not bool(v[11]&0x02001005)
    return result

def run(a):
    a.build=a.build.resolve();manifest_raw=(a.build/'manifest.json').read_bytes();m=json.loads(manifest_raw)
    require(m['schema']=='a733-a64-entry-probe-v1' and m['native_sha256']==NATIVE and m['layout']==dict(zip(SYMBOLS,LAYOUT)),'Probe build identity differs.')
    require(m['source_sha256']==digest(ROOT/'boot/a733/a64-entry-probe.S') and m['host_sha256']==digest(Path(__file__)),'Probe implementation changed.')
    frozen={name:(a.build/name).read_bytes() for name in m['files']}
    require(all(Path(name).name==name and hashlib.sha256(data).hexdigest()==m['files'][name] for name,data in frozen.items()),'Probe build changed.')
    software_raw=a.software.read_bytes();software=json.loads(software_raw)
    require(software.get('schema')=='a733-a64-entry-probe-software-v1' and software.get('return_case') is True and software.get('physical_access') is False and software.get('binary_sha256')==m['files']['probe.bin'],'Instruction/report-return evidence required.')
    survey_raw=a.survey.read_bytes();survey=json.loads(survey_raw)
    require(survey.get('complete') is True and survey.get('sram_restored') is True and survey.get('rom_usable') is True and survey.get('result'),'Completed returning survey required.')
    pre=survey['result']['counter_second'];require(type(pre) is int and 0<=pre<2**64,'Invalid pre-transition counter.')
    expected={(a.build/'manifest.json').resolve():hashlib.sha256(manifest_raw).hexdigest(),(a.build/'probe.bin').resolve():m['files']['probe.bin'],a.survey.resolve():hashlib.sha256(survey_raw).hexdigest(),a.software.resolve():hashlib.sha256(software_raw).hexdigest()}
    recovery,recovery_hash=claimed(a,expected);require(digest(a.fel)==base.FEL_HASH,'Pinned FEL executable required.')
    a.evidence.mkdir(parents=True,exist_ok=False)
    record={'complete':False,'launch_attempted':False,'transactions':[],'claim':a.claim,'owner':a.owner,'physical_access':True,'survey_sha256':expected[a.survey.resolve()],'recovery_sha256':recovery_hash,'binary_sha256':m['files']['probe.bin']}
    def journal():save(a.evidence/'session.json',record)
    access=module('a64_probe_access',ROOT/'tools/usb-access.py');sys.path.insert(0,str(Path.home()/'git/mimus/tools'));import usb_guard
    usb_guard.check_health()
    with access.board_lock() as node:
        access.require_session(node);access.require_rom(node);identity=access.identity(node);port=access.target(node);config={'port':port.name,'sysfs':str(port.resolve())};failed=False;dirty=False
        require(survey.get('identity')==identity and survey.get('port')==port.name and recovery.get('identity')==identity and recovery.get('port')==port.name,'Survey/recovery covers another live instance.')
        record.update(initial_identity=identity,initial_node=str(node),port=port.name);journal()
        def transaction(*arguments,pass_fds=()):
            nonlocal failed
            require(not failed and access.identity(node)==identity,'Transport failed or instance changed; stop.')
            access.require_rom(node);entry={'arguments':list(map(str,arguments))};record['transactions'].append(entry);journal()
            try:
                result=subprocess.run([str(a.fel),'-d',f'{int(node.parent.name)}:{int(node.name)}',*map(str,arguments)],capture_output=True,text=True,timeout=15,pass_fds=pass_fds)
                entry.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr);require(result.returncode==0,'FEL transaction failed.')
            except BaseException as error:failed=True;entry['failure']=str(error);journal();raise
            journal();return result.stdout
        def transfer(address,data,destination):
            with base.sealed_source(data) as (path,fd):transaction('write',hex(address),path,'read',hex(address),str(len(data)),destination,pass_fds=(fd,))
            require(destination.read_bytes()==data,'Upload/restoration comparison differs.')
        try:
            require('soc=00001903(A733)' in transaction('version'),'Not an A733 ROM instance.')
            words=transaction('readl','0x20','readl','0x24','readl','0x28','readl','0x3c','readl','0x40')
            require([int(v,16) for v in words.split()]==[0xeb00008f,0xe3a00911,0xe3a0193d,0xe3a0605c,0xea00004d],'Forced-FEL ROM destination changed.')
            backups={}
            for name,address,size in [('code',0x41000000,len(frozen['probe.bin'])),('report',0x42000000,256)]:
                path=a.evidence/(name+'-before.bin');transaction('read',hex(address),str(size),path);backups[name]=path.read_bytes();require(len(backups[name])==size,'Short backup.');base.sync_file(path)
            nonce=secrets.token_bytes(16);initial=bytearray(256);struct.pack_into('<Q',initial,104,pre);initial[240:]=nonce;record['nonce']=nonce.hex();dirty=True;journal()
            transfer(0x41000000,frozen['probe.bin'],a.evidence/'code-upload.bin');transfer(0x42000000,bytes(initial),a.evidence/'report-upload.bin')
            current,current_hash=claimed(a,expected);require(current_hash==recovery_hash,'Recovery review changed.')
            with usb_guard.reenumeration('sunstoneos-a733','A64 entry capture and existing forced-FEL return',2):
                record['launch_attempted']=True;journal();transaction('reset64','0x41000000')
                deadline=time.monotonic()+10;old=identity
                while True:
                    candidate=access.session_node(config)
                    if candidate is not None and access.identity(candidate)!=old and os.access(candidate,os.R_OK|os.W_OK):break
                    require(time.monotonic()<deadline,'No new FEL instance before deadline; no protocol retry.');time.sleep(.05)
                node=candidate;identity=access.identity(node);access.require_session(node);access.require_rom(node)
                record.update(returned_identity=identity,returned_node=str(node));journal()
            require('soc=00001903(A733)' in transaction('version'),'Return is not an A733 ROM service.')
            transaction('read','0x42000000','256',a.evidence/'report.bin');base.sync_file(a.evidence/'report.bin');raw=(a.evidence/'report.bin').read_bytes();error=None
            try:record['result']=decode(raw,nonce,pre)
            except (ValueError,struct.error) as e:error=e
            transfer(0x41000000,backups['code'],a.evidence/'code-restored.bin');transfer(0x42000000,backups['report'],a.evidence/'report-restored.bin');dirty=False
            record.update(rom_usable=True,dram_restored=True,complete=error is None);journal()
            if error:raise error
        except BaseException as error:
            record.update(failure=str(error),transport_failed=failed)
            if dirty or failed:
                try:base.stop_after_failure(record,journal,lambda:access.quarantine_instance(node,'A64 probe stopped; no automatic retry; coordinator recovery required.',identity),error)
                finally:
                    if dirty and not record['launch_attempted']:base.hold_ownership()
            else:journal()
            raise
    return 0

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='operation',required=True)
    build=sub.add_parser('prepare')
    for name in ('build','observer','clang','lld','objcopy'):build.add_argument('--'+name,type=Path,required=True)
    live=sub.add_parser('run')
    for name in ('build','software','survey','database','fel','evidence'):live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--claim',required=True);live.add_argument('--owner',required=True);a=p.parse_args(argv)
    if a.operation=='prepare':prepare(a);return 0
    return run(a)
if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,OSError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
