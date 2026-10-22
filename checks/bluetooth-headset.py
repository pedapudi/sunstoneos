#!/usr/bin/env python3
"""Compile the new native headset composition; run only reviewed new cases."""
import argparse
import hashlib
import json
import os
import shlex
import shutil
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--setup-only',action='store_true')
p.add_argument('--playback',action='store_true',help='New runtime caller PCM composition only')
p.add_argument('--msbc',action='store_true',help='Build the new actual mSBC stack composition')
p.add_argument('--repeated',action='store_true',help='Build only the new repeated headset composition')
p.add_argument('--recognition-profile',action='store_true')
p.add_argument('--run-only',action='store_true')
p.add_argument('--call-control',action='store_true',help='Build with the HFP call-control option')
p.add_argument('--speech-input',type=Path)
p.add_argument('--speech-output',type=Path)
p.add_argument('--case',action='append',choices=['success','retry','retry-limit','informational','cadence','bac','justworks','keyreplace','deadline','loss','handoff','security-pending','early-copy','speech','two','backlog','cancel','late-disconnect','revocation','resident','stop-in-poll','fragmented','crc','sequence','bad-bcs','no-capability','fallback','command-refusal','pcm-two','pcm-duplex','pcm-cancel','pcm-starve','pcm-partial','pcm-early-cancel','pcm-publication','pcm-cadence',
    'button','start','no-voice','early','pending-off','idle-stop','open-stop','setup-stop','ag-stop','window','peer-release','activations','bac-idle'])
a=p.parse_args();
if a.playback:a.msbc=True;a.recognition_profile=True
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
cache=Path('/home/sunil/.cache/sunstoneos');security=a.build/'bluetooth-application/security'
reference=cache/'evidence/bluetooth-native-20261017/composition'
cc='/usr/bin/clang-21';commands=[];executing_cases=False
common=['-std=c11','-Wall','-Wextra','-Wno-unused-function','-Wno-sign-compare','-Wno-for-loop-analysis','-g','-O1',
        '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer',
        '-DSUNSTONE_BLUETOOTH_HEADSET=1','-Inetwork/include','-Iinclude','-I'+str(reference),'-I'+str(security),'-I'+str(a.build/'bluetooth-application'),
        '-I'+str(cache/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/include'),
        '-I'+str(cache/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/portable/GCC/ARM_AARCH64_SRE'),
        '-I'+str(cache/'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source/include'),
        '-I'+str(cache/'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source/portable/Compiler/GCC')]
if a.recognition_profile:common.append('-DSUNSTONE_BLUETOOTH_RECOGNITION=1')
if a.call_control:common.append('-DSUNSTONE_BT_HFP_CALL_CONTROL=1')
sources=['drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','drivers/bluetooth/bt_acl.c',
         'drivers/bluetooth/bt_l2cap.c','drivers/bluetooth/bt_sdp.c','drivers/bluetooth/bt_sdp_server.c','drivers/bluetooth/bt_rfcomm.c',
         'drivers/bluetooth/bt_hfp_ag.c','drivers/bluetooth/bt_sco.c','drivers/bluetooth/bt_security.c',
         'drivers/storage/storage_digest.c','apps/bluetooth/result.c','checks/bluetooth-headset.c',
         *[str(security/name) for name in ('zephyr-ssp.c','zephyr-keys_br.c','zephyr-security-level.c')]]
if a.msbc:a.repeated=True
if a.repeated:
    assert a.recognition_profile
    sources[sources.index('checks/bluetooth-headset.c')]='checks/bluetooth-msbc.c' if a.msbc else 'checks/bluetooth-headset-repeated.c'
if a.msbc:
    sbc=cache/'sources/bluetooth-msbc'
    common+=['-I'+str(sbc),'-DSBC_HIGH_PRECISION=1','-DSUNSTONE_BLUETOOTH_MSBC=1']
    sources+=['apps/bluetooth/playback.c','apps/bluetooth/msbc.c',str(sbc/'sbc/sbc.c'),str(sbc/'sbc/sbc_primitives.c')]
if a.playback:
    sources[sources.index('checks/bluetooth-msbc.c')]='checks/bluetooth-playback.c'
    sources.remove('apps/bluetooth/playback.c')
def run(name,cmd):
    cmd=list(map(str,cmd));commands.append(cmd)
    r=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1','ASAN_OPTIONS':'halt_on_error=1','SUNSTONE_DUPLEX_PCM':str(out/'duplex-copied.pcm')})
    log=out/(name+'.log')
    if log.exists():
        prior=out/'prior'/hashlib.sha256(log.read_bytes()).hexdigest()/log.name;prior.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(log,prior)
    log.write_text(r.stdout+r.stderr)
    (out/('runtime-commands.json' if a.run_only or executing_cases else 'commands.json')).write_text(json.dumps(commands,indent=2)+'\n')
    return r.returncode
exe=out/'bluetooth-headset-model'
if not a.run_only:
    previous=json.loads((out/'setup-inputs.json').read_text()) if (out/'setup-inputs.json').exists() else {}
    def reusable(dep,obj):
        if not dep.exists() or not obj.exists():return False
        names=shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1])
        for name in [*names,str(obj)]:
            path=Path(name).resolve() if Path(name).is_absolute() else (ROOT/name).resolve()
            old=previous.get(str(path))
            if not old or hashlib.sha256(path.read_bytes()).hexdigest()!=old['sha256']:return False
        return True
    failures=[];objects=[]
    for i,source in enumerate(sources):
        obj=out/(str(i)+'.o');objects.append(obj)
        cmd=[cc,*common,'-MMD','-MF',out/(str(i)+'.d'),'-c',source,'-o',obj]
        if reusable(out/(str(i)+'.d'),obj):commands.append(list(map(str,cmd)))
        elif run('compile-'+str(i),cmd):failures.append(source)
    if failures:sys.exit('Setup errors before execution: '+', '.join(failures))
    if run('link',[cc,'-fsanitize=address,undefined','-Wl,--gc-sections',*objects,'-o',exe]):sys.exit('Fixture link failed before execution')
if not a.run_only:
    identities={}
    paths={Path(cc).resolve(),Path(__file__).resolve(),exe.resolve()}
    for dep in out.glob('*.d'):
        paths.add(dep.resolve());paths.add(dep.with_suffix(".o").resolve())
        for name in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]):
            path=Path(name);paths.add(path.resolve() if path.is_absolute() else (ROOT/path).resolve())
    paths.add((out/'commands.json').resolve())
    for path in paths:
        raw=path.read_bytes();identity=hashlib.sha256(raw).hexdigest();target=out/'inputs'/identity/path.name
        target.parent.mkdir(parents=True,exist_ok=True)
        if not target.exists():shutil.copyfile(path,target)
        identities[str(path)]={'sha256':identity,'snapshot':str(target.relative_to(out))}
    (out/'setup-inputs.json').write_text(json.dumps(identities,indent=2)+'\n')
if a.setup_only:sys.exit(0)
for name,identity in json.loads((out/'setup-inputs.json').read_text()).items():
    if hashlib.sha256(Path(name).read_bytes()).hexdigest()!=identity['sha256'] or hashlib.sha256((out/identity['snapshot']).read_bytes()).hexdigest()!=identity['sha256']:
        sys.exit('Refused changed fixture input: '+name)
executing_cases=True
for case in a.case or ('success','retry','retry-limit','justworks','keyreplace','deadline','loss','handoff','security-pending'):
    cmd=[exe,case]
    if case=='speech':
        assert a.speech_input and a.speech_output
        cmd += [a.speech_input,a.speech_output]
    if run(case,cmd):sys.exit('Stopped after new case failure: '+case)
print('New native headset composition cases passed; hardware is modeled.')
