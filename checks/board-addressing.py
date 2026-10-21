#!/usr/bin/env python3
"""Focused profile admission and host geometry checks; no hardware access."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import sys
import subprocess
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')
ROOT=Path(__file__).resolve().parents[1]

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

def host_checks():
    prepare=load('addressing_prepare',ROOT/'apps/board/prepare.py')
    observe=load('addressing_observe',ROOT/'apps/board/observe.py')
    raw=bytearray(1872);struct.pack_into('<QII',raw,0,0x3152464f52545353,4,1872)
    struct.pack_into('<4Q',raw,432,0x43010000,0x43020000,0x43030000,0x43040000)
    struct.pack_into('<4Q',raw,464,1708,24744,23288,341260)
    struct.pack_into('<H',raw,1336,7)
    session=bytearray(592);struct.pack_into('<6Q',session,0,0x4153373342535332,2,592,31,37,41)
    struct.pack_into('<QQ',session,352,0x43000000,len(raw));struct.pack_into('<I',session,400,3)
    def bind(profile):
        record=bytearray(session);struct.pack_into('<Q',record,360,len(profile))
        record[368:400]=hashlib.sha256(profile).digest()
        frame=struct.pack('<IIIIIIQQ',0x52414f42,1,3,0,2,7,31,37)+record[368:400]
        return record,frame
    def check(profile,valid):
        record,frame=bind(profile)
        for action in (lambda:prepare.profile_spans(profile,record),lambda:observe.decode(frame,record,profile)):
            try:action()
            except (RuntimeError,ValueError):assert not valid
            else:assert valid
    for policy in (0,1):
        struct.pack_into('<B4s3sII',raw,1856,policy,b'\x09\x00\x00\x09',bytes(3),0xffffffff if policy else 0,0xffffffff if policy else 0)
        check(raw,True)
    for offset,value in ((1856,2),(1861,1),(1862,1),(1863,1)):
        bad=bytearray(raw);bad[offset]=value;check(bad,False)
    for offset in (1864,1868):
        bad=bytearray(raw);struct.pack_into('<I',bad,offset,0);check(bad,False)
    bad=bytearray(raw);bad[1856]=0;check(bad,False)
    bad=bytearray(raw);struct.pack_into('<I',bad,8,2);check(bad,False)
    bad=bytearray(raw[:1856]);struct.pack_into('<II',bad,8,2,1856);check(bad,False)
    record,frame=bind(raw);bad=bytearray(raw);bad[1857]^=1
    try:observe.decode(frame,record,bad)
    except ValueError:pass
    else:raise AssertionError('changed DNS must invalidate authenticated profile')
    # Watchdog observation uses the distinct runtime profile version.
    runtime=bytearray(raw);struct.pack_into('<I',runtime,8,3)
    record,frame=bind(runtime);struct.pack_into('<3Q',record,0,0x41533733424f4131,1,592)
    struct.pack_into('<I',record,400,1);frame=bytearray(frame);struct.pack_into('<I',frame,8,0)
    assert observe.decode(frame,record,runtime)['verified_radio_bytes']==7

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('output','freertos','tcp'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--cc',default='/usr/bin/clang-21');a=parser.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    common=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_AIC8800_RSN=1',
        '-DSUNSTONE_BOARD_SUPERVISED=1','-DINCLUDE_xSemaphoreGetMutexHolder=1',
        '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
        '-Inetwork/include','-Iinclude','-Iruntime/include','-Iplatform/a733-smp-cached',
        '-I'+str(a.freertos/'include'),'-I'+str(a.freertos/'portable/GCC/ARM_AARCH64_SRE'),
        '-I'+str(a.tcp/'source/include'),'-I'+str(a.tcp/'source/portable/Compiler/GCC')]
    commands=[]
    def run(label,command):
        commands.append(list(map(str,command)))
        result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1'})
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0,label+': see preserved log'
    try:
        # Compile both inputs before executing any fixture.
        for name,source in [('profile','checks/board-addressing.c'),('digest','drivers/storage/storage_digest.c')]:
            run(name,[a.cc,*common,'-MMD','-MF',str(out/(name+'.d')),'-c',source,'-o',str(out/(name+'.o'))])
        run('link',[a.cc,'-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wl,--gc-sections',str(out/'profile.o'),str(out/'digest.o'),'-o',str(out/'board-addressing')])
        host_checks();(out/'host.log').write_text('PASS fixed/DHCP host admission, geometry and result decoding\n')
        run('profile-run',[str(out/'board-addressing')])
        (out/'result.json').write_text(json.dumps({'passed':True,'hardware_access':False,
            'scope':'Profile authentication, fixed/DHCP policy admission, supervised conversion and host geometry. Native CE and lower protocols are unused modeled boundaries.'},indent=2)+'\n')
    finally:
        sources={Path(__file__).resolve(),ROOT/'apps/board/prepare.py',ROOT/'apps/board/observe.py'}
        sources.update(ROOT/'tools'/name for name in ('check-a733-smp.py','check-a733-smp-supervised.py','check-a733-smp-observe.py'))
        for dep in out.glob('*.d'):
            sources.update((ROOT/Path(p)).resolve() for p in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
        manifest={}
        for source in sorted(sources):
            identity=hashlib.sha256(source.read_bytes()).hexdigest();target=out/'inputs'/identity/source.name
            target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
            manifest[str(source)]={'sha256':identity,'snapshot':str(target.relative_to(out))}
        (out/'inputs.json').write_text(json.dumps(manifest,indent=2)+'\n')
        (out/'tools.json').write_text(json.dumps({name:{'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()} for name,path in {'compiler':Path(a.cc).resolve(),'python':Path(sys.executable).resolve()}.items()},indent=2)+'\n')
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        (out/'artifacts.json').write_text(json.dumps({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.is_file()},indent=2)+'\n')
if __name__=='__main__':main()
