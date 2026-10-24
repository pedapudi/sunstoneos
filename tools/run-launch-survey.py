#!/usr/bin/env python3
"""Build or execute one returning A32 SRAM survey; never performs an RMR transition."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import shutil
import sqlite3
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('launch_survey_base',ROOT/'tools/check-a733-smp.py')
require,digest,save=base.require,base.digest,base.save_json
REGIONS=(('code',0x85000,0x400),('report',0x86000,128))
MAGIC=0x3153564c

def prepare(args):
    out=args.build.resolve();out.mkdir(parents=True,exist_ok=False)
    source=ROOT/'boot/a733/launch-survey.S';shutil.copy2(source,out/'launch-survey.S')
    commands=[[args.clang,'--target=arm-none-eabi','-march=armv8-a','-marm','-c',out/'launch-survey.S','-o',out/'launch-survey.o'],
        [args.lld,'-Ttext=0x85000','-e','_start',out/'launch-survey.o','-o',out/'launch-survey.elf'],
        [args.objcopy,'-O','binary',out/'launch-survey.elf',out/'launch-survey.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'Survey build failed; inspect retained log.')
    require(0<(out/'launch-survey.bin').stat().st_size<=0x400,'Survey exceeds reviewed SRAM extent.')
    save(out/'commands.json',[[str(x) for x in command] for command in commands])
    save(out/'manifest.json',{'schema':'a733-launch-survey-v1','physical_access':False,
        'source_sha256':digest(source),'host_sha256':digest(Path(__file__)),
        'tools':{str(p):digest(p) for p in (args.clang,args.lld,args.objcopy)},
        'files':{p.name:digest(p) for p in out.iterdir() if p.is_file()}})

def decode(raw,nonce):
    require(len(raw)==128,'Incomplete survey report.');v=struct.unpack('<32I',raw)
    require(v[:4]==(MAGIC,1,128,1) and raw[120:128]==nonce and not v[29],'Survey identity/completion differs.')
    result=dict(zip(('cpsr','sctlr','scr','mpidr','cntfrq','rmr','counter_lo','counter_hi','counter2_lo','counter2_hi','sp','lr'),v[4:16]))
    require(v[6]==0,'Unavailable SCR field changed.');result['scr']=None;result['scr_available']=False
    result['registers']=list(v[16:29]);first=v[10]|v[11]<<32;second=v[12]|v[13]<<32
    result['counter_first']=first;result['counter_second']=second
    result['launcher_gates']={'svc':v[4]&31==19,'little_endian':not v[4]&512,
        'mmu_caches_endian_off':not v[5]&0x02001005,
        'primary_mpidr':not v[7]&~0xc1000000,'frequency':v[8] in (0,24000000),'counter_monotonic':second>=first}
    return result

def claimed(args,manifest_hash,binary_hash,software_hash):
    db=sqlite3.connect(args.database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'Queue halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(args.claim,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==args.owner,'Matching coordinator claim required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==set(review['prerequisites'])=={'supervised-ram-recovery','launch-survey-software'},'Survey prerequisites differ.')
        refs={k:base.artifact(v) for k,v in review['prerequisites'].items()}
        require(refs['launch-survey-software']==args.software.resolve() and review['prerequisites']['launch-survey-software']['sha256']==software_hash,'Queue covers other survey software.')
        covered={(base.artifact(v),v['sha256']) for v in submission['artifacts']}
        require({((args.build/'manifest.json').resolve(),manifest_hash),((args.build/'launch-survey.bin').resolve(),binary_hash)}<=covered,'Queue covers different survey bytes.')
        recovery_ref=review['prerequisites']['supervised-ram-recovery']
        recovery_raw=Path(recovery_ref['path']).read_bytes()
        require(hashlib.sha256(recovery_raw).hexdigest()==recovery_ref['sha256'],'Recovery record changed.')
        return json.loads(recovery_raw),recovery_ref['sha256']
    finally:db.close()

def run(args):
    args.build=args.build.resolve();metadata=(args.build/'manifest.json').read_bytes();manifest=json.loads(metadata)
    require(manifest['schema']=='a733-launch-survey-v1' and manifest['physical_access'] is False,'Wrong survey build.')
    require(manifest['source_sha256']==digest(ROOT/'boot/a733/launch-survey.S') and manifest['host_sha256']==digest(Path(__file__)),'Survey implementation changed.')
    frozen={name:(args.build/name).read_bytes() for name in manifest['files']}
    require(all(Path(name).name==name and hashlib.sha256(data).hexdigest()==manifest['files'][name] for name,data in frozen.items()),'Survey build bytes changed.')
    software_raw=args.software.read_bytes();software=json.loads(software_raw);software_hash=hashlib.sha256(software_raw).hexdigest()
    require(software.get('schema')=='a733-launch-survey-software-v1' and software.get('binary_sha256')==manifest['files']['launch-survey.bin'] and
        software.get('return_preserved') is True and software.get('physical_access') is False,'Instruction/return evidence required.')
    require(digest(args.fel)==base.FEL_HASH,'FEL executable differs from pinned implementation.')
    recovery,recovery_hash=claimed(args,hashlib.sha256(metadata).hexdigest(),manifest['files']['launch-survey.bin'],software_hash)
    args.evidence.mkdir(parents=True,exist_ok=False)
    record={'physical_access':True,'complete':False,'transactions':[],'claim':args.claim,'owner':args.owner,
        'manifest_sha256':hashlib.sha256(metadata).hexdigest(),'software_sha256':software_hash,'one_attempt':True,'launch_attempted':False,'device_access_attempted':False,'recovery_sha256':recovery_hash}
    def journal():save(args.evidence/'session.json',record)
    access=module('launch_survey_access',ROOT/'tools/usb-access.py')
    sys.path.insert(0,str(Path.home()/'git/mimus/tools'));import usb_guard
    usb_guard.check_health()
    with access.board_lock() as node:
        access.require_rom(node);identity=access.identity(node);port=access.target(node).name
        require(recovery.get('identity')==identity and recovery.get('port')==port,'Recovery review covers another live instance.')
        record.update(node=str(node),identity=identity,port=port)
        failed=False
        def transaction(*commands,pass_fds=()):
            nonlocal failed
            require(not failed and access.identity(node)==identity,'Transport failed or instance changed; no further requests.')
            access.require_rom(node);entry={'arguments':list(map(str,commands))};record['transactions'].append(entry);journal()
            try:
                record['device_access_attempted']=True
                result=subprocess.run([str(args.fel),'-d',f'{int(node.parent.name)}:{int(node.name)}',*map(str,commands)],capture_output=True,text=True,timeout=15,pass_fds=pass_fds)
                entry.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr)
                require(result.returncode==0,'Survey FEL transaction failed.')
            except BaseException as error:failed=True;entry['failure']=str(error);journal();raise
            journal();return result.stdout
        def transfer(address,data,readback):
            with base.sealed_source(data) as (path,fd):transaction('write',hex(address),path,'read',hex(address),str(len(data)),readback,pass_fds=(fd,))
            require(readback.read_bytes()==data,'Survey transfer comparison failed.')
        dirty=False
        try:
            require('soc=00001903(A733)' in transaction('version'),'Not an A733 ROM instance.')
            backups={}
            for name,address,size in REGIONS:
                path=args.evidence/(name+'-before.bin');transaction('read',hex(address),str(size),path);backups[name]=path.read_bytes();require(len(backups[name])==size,'Short SRAM backup.')
            nonce=secrets.token_bytes(8);initial=bytes(120)+nonce;record['nonce']=nonce.hex();journal();dirty=True
            transfer(0x85000,frozen['launch-survey.bin'],args.evidence/'code-upload.bin')
            transfer(0x86000,initial,args.evidence/'report-upload.bin')
            current,current_hash=claimed(args,hashlib.sha256(metadata).hexdigest(),manifest['files']['launch-survey.bin'],software_hash)
            require(current_hash==recovery_hash and current.get('identity')==identity and current.get('port')==port,'Recovery approval changed before execution.')
            record['launch_attempted']=True;record['execution_intent']=True;journal();transaction('exe','0x85000')
            transaction('read','0x86000','128',args.evidence/'report.bin')
            # A successful report read establishes a responsive ROM service.
            # Restore SRAM even when architectural report validation refuses.
            raw=(args.evidence/'report.bin').read_bytes();validation_error=None
            try:record['result']=decode(raw,nonce)
            except (ValueError,struct.error) as error:validation_error=error
            for name,address,size in REGIONS:transfer(address,backups[name],args.evidence/(name+'-restored.bin'))
            dirty=False;record.update(sram_restored=True,rom_usable=True,complete=validation_error is None);journal()
            if validation_error:raise validation_error
        except BaseException as error:
            record.update(failure=str(error),transport_failed=failed)
            if dirty or failed or (record['device_access_attempted'] and not record.get('sram_restored')):
                try:base.stop_after_failure(record,journal,lambda:access.quarantine_instance(node,'Survey stopped; no retries; manual recovery review required.',identity),error)
                finally:
                    if not record['launch_attempted']:base.hold_ownership()
            else:journal()
            raise
    return 0

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='operation',required=True)
    build=sub.add_parser('prepare');build.add_argument('--build',type=Path,required=True)
    for name in ('clang','lld','objcopy'):build.add_argument('--'+name,type=Path,required=True)
    live=sub.add_parser('run')
    for name in ('fel','build','software','database','evidence'):live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--claim',required=True);live.add_argument('--owner',required=True)
    args=parser.parse_args(argv)
    if args.operation=='prepare':prepare(args);return 0
    return run(args)
if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,OSError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
