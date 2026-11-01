#!/usr/bin/env python3
"""Build or execute one returning A32 SRAM survey; never performs an RMR transition."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import sys
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('launch_survey_base',ROOT/'tools/check-a733-smp.py')
require,digest,save=base.require,base.digest,base.save_json
returning=module('probe_returning',ROOT/'tools/a733-returning-host.py')
REGIONS=(('code',0x85000,0x400),('report',0x86000,128))
MAGIC=0x3153564c

def prepare(args):
    out=returning.artifact_directory(args.build.resolve());args.build=out
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

def capture_caller(transaction, evidence):
    """Read caller state in this locked session and restore the survey's SRAM.

    The survey never uses the incoming stack. Transport failures propagate
    immediately; restoration is attempted only after successful report transfer.
    """
    build = SimpleNamespace(build=evidence/'caller-survey-build',
        clang=Path(os.environ.get('SUNSTONE_HOST_CLANG','/usr/bin/clang-21')),
        lld=Path(os.environ.get('SUNSTONE_HOST_LLD',str(Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21'))),
        objcopy=Path('/usr/lib/llvm-21/bin/llvm-objcopy'))
    prepare(build)
    backups={}
    for name,address,size in REGIONS:
        destination=evidence/('caller-'+name+'-before.bin')
        transaction('read',hex(address),size,destination)
        backups[name]=destination.read_bytes()
        require(len(backups[name])==size,'Short caller-survey SRAM backup.')
        base.sync_file(destination)
    def transfer(address,data,name):
        destination=evidence/name
        with base.sealed_source(data) as (source,descriptor):
            transaction('write',hex(address),source,'read',hex(address),len(data),destination,pass_fds=(descriptor,))
        require(destination.read_bytes()==data,'Caller-survey SRAM comparison differs.')
    nonce=secrets.token_bytes(8)
    transfer(REGIONS[0][1],(build.build/'launch-survey.bin').read_bytes(),'caller-code-upload.bin')
    transfer(REGIONS[1][1],bytes(120)+nonce,'caller-report-upload.bin')
    transaction('exe',hex(REGIONS[0][1]))
    report=evidence/'caller-report.bin'
    transaction('read',hex(REGIONS[1][1]),128,report)
    raw=report.read_bytes()
    base.sync_file(report)
    for name,address,_ in REGIONS:
        transfer(address,backups[name],'caller-'+name+'-restored.bin')
    return decode(raw,nonce)


def caller_interval(result):
    """Check the native launcher's 60-byte push before any launcher upload."""
    sp=result['sp']
    require(type(sp) is int and 0x4003c<=sp<=0x100000 and sp%4==0,'Caller SP outside SRAM or misaligned.')
    require(all(sp<=address or sp-60>=address+size for _,address,size in base.REGIONS),
            'Caller stack overlaps launcher reservations.')
    require(all(result['launcher_gates'].values()),'Live A32 launcher state is unsafe.')
    return sp-60,60


def run(args):
    """Route legacy launch commands through the native probe contract."""
    sys.path.insert(0, str(ROOT/'tools'))
    import board
    return board.legacy_run(args, image=args.build/'launch-survey.elf')

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='operation',required=True)
    build=sub.add_parser('prepare');build.add_argument('--build',type=Path,required=True)
    for name in ('clang','lld','objcopy'):build.add_argument('--'+name,type=Path,required=True)
    live=sub.add_parser('run')
    for name in ('fel','build','evidence'):live.add_argument('--'+name,type=Path,required=True)
    for name in ('software','database'):live.add_argument('--'+name,type=Path,help=argparse.SUPPRESS)
    live.add_argument('--claim',help=argparse.SUPPRESS);live.add_argument('--owner',help=argparse.SUPPRESS)
    args=parser.parse_args(argv)
    if args.operation=='prepare':prepare(args);return 0
    return run(args)
if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,OSError,RuntimeError,struct.error) as error:sys.exit(f'Stopped: {error}')
