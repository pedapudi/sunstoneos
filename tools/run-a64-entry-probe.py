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
import struct
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('a64_probe_base',ROOT/'tools/check-a733-smp.py')
require,digest,save=base.require,base.digest,base.save_json
returning=module('probe_returning',ROOT/'tools/a733-returning-host.py')
NATIVE='b6b1f9115cf36644f070407002d5d8b3150e24fa522e6f01f4e04a9390afde50'
SYMBOLS=('__a733_smp_boot_stack_top','__monitor_bss_start','__monitor_bss_end','__tables_start','__tables_end','__bss_start','__bss_end')
LAYOUT=(0x42014000,0x42005000,0x4200f590,0x421f0000,0x421f5000,0x41201000,0x41290d10)
MAGIC=0x413634454e545259

def prepare(a):
    from elftools.elf.elffile import ELFFile
    with a.observer.open('rb') as f:
        symbols={s.name:s['st_value'] for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()}
    require(tuple(symbols[n] for n in SYMBOLS)==LAYOUT,'Native ELF layout changed.')
    a.build=returning.artifact_directory(a.build);shutil.copy2(ROOT/'boot/a733/a64-entry-probe.S',a.build/'probe.S')
    commands=[[a.clang,'--target=aarch64-none-elf','-march=armv8-a','-c',a.build/'probe.S','-o',a.build/'probe.o'],
        [a.lld,'-Ttext=0x41000000','-e','_start',a.build/'probe.o','-o',a.build/'probe.elf'],
        [a.objcopy,'-O','binary',a.build/'probe.elf',a.build/'probe.bin']]
    for command in commands:
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (a.build/'build.log').open('a') as f:f.write(result.stdout+result.stderr)
        require(result.returncode==0,'Probe build failed; inspect log.')
    require(0<(a.build/'probe.bin').stat().st_size<=0x10000,'Probe exceeds reviewed code interval.')
    save(a.build/'commands.json',[[str(v) for v in c] for c in commands])
    save(a.build/'manifest.json',{'schema':'a733-a64-entry-probe-v1','physical_access':False,'native_sha256':digest(a.observer),
        'layout':dict(zip(SYMBOLS,LAYOUT)),'source_sha256':digest(ROOT/'boot/a733/a64-entry-probe.S'),'host_sha256':digest(Path(__file__)),
        'files':{p.name:digest(p) for p in a.build.iterdir() if p.is_file()}})

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
    """Route legacy launch commands through the native probe contract."""
    sys.path.insert(0, str(ROOT/'tools'))
    import board
    return board.legacy_run(a, image=a.build/'probe.elf')

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='operation',required=True)
    build=sub.add_parser('prepare')
    for name in ('build','observer','clang','lld','objcopy'):build.add_argument('--'+name,type=Path,required=True)
    live=sub.add_parser('run')
    for name in ('build','fel','evidence'):live.add_argument('--'+name,type=Path,required=True)
    for name in ('software','database','survey'):live.add_argument('--'+name,type=Path,help=argparse.SUPPRESS)
    live.add_argument('--claim',help=argparse.SUPPRESS);live.add_argument('--owner',help=argparse.SUPPRESS);a=p.parse_args(argv)
    if a.operation=='prepare':prepare(a);return 0
    return run(a)
if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,OSError,RuntimeError,struct.error) as error:sys.exit(f'Stopped: {error}')
