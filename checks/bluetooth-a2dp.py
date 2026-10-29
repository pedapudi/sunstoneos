#!/usr/bin/env python3
"""Check the optional native finite A2DP consumer without board access."""
import argparse
import hashlib
import json
import pathlib
import shlex
import subprocess
import os
from harness import SANITIZE, environment
p=argparse.ArgumentParser()
p.add_argument('--build',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True,exist_ok=False)
cache=pathlib.Path('/home/sunil/.cache/sunstoneos');sbc=cache/'sources/bluetooth-msbc'
for item in json.loads((root/'apps/bluetooth/msbc-source.json').read_text())['sources']:
    assert hashlib.sha256((sbc/item['path']).read_bytes()).hexdigest()==item['sha256']
cc='/usr/bin/clang-21';commands=[];log=''
def run(command):
    global log
    command=list(map(str,command));commands.append(command)
    result=subprocess.run(command,cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=environment())
    log+=result.stdout;(a.output/'log.txt').write_text(log)
    (a.output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    if result.returncode:print(result.stdout);raise SystemExit(result.returncode)
base=['-std=c11','-D_DEFAULT_SOURCE','-g','-O1','-ffunction-sections','-fdata-sections',*SANITIZE,'-DSBC_HIGH_PRECISION=1']
run([cc,*base,'-I'+str(sbc),'checks/bluetooth-a2dp-encode.c',sbc/'sbc/sbc.c',sbc/'sbc/sbc_primitives.c','-lm','-o',a.output/'encode'])
for shape in ('joint','short'):run([a.output/'encode',shape,a.output/(shape+'.sbc')])
reference=cache/'evidence/bluetooth-native-20261017/composition';security=a.build/'bluetooth-application/security'
include=['-Inetwork/include','-Iinclude','-Iapps/bluetooth','-I'+str(sbc),'-I'+str(reference),'-I'+str(security),'-I'+str(a.build/'bluetooth-application'),
 '-I'+str(cache/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/include'),
 '-I'+str(cache/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/portable/GCC/ARM_AARCH64_SRE'),
 '-I'+str(cache/'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source/include'),
 '-I'+str(cache/'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source/portable/Compiler/GCC')]
usb=(root/'apps/bluetooth/usb0.c').read_text();start=usb.index('static void pcm_exchange(');end=usb.index('\n}\n#endif',start)+2
(a.output/'a2dp-pcm-exchange.h').write_text(usb[start:end]+'\n')
flags=[*base,*include,'-I'+str(a.output),'-DSUNSTONE_BLUETOOTH_HEADSET=1','-DSUNSTONE_BLUETOOTH_A2DP=1']
sources=['drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','drivers/bluetooth/bt_acl.c','drivers/bluetooth/bt_l2cap.c',
 'drivers/bluetooth/bt_sdp.c','drivers/bluetooth/bt_sdp_server.c','drivers/bluetooth/bt_rfcomm.c','drivers/bluetooth/bt_hfp_ag.c','drivers/bluetooth/bt_sco.c','drivers/bluetooth/bt_security.c',
 'drivers/bluetooth/bt_avdtp.c','drivers/storage/storage_digest.c','apps/bluetooth/result.c','apps/bluetooth/a2dp.c','checks/bluetooth-a2dp-native.c',
 *[str(security/n) for n in ('zephyr-ssp.c','zephyr-keys_br.c','zephyr-security-level.c')]]
objects=[]
for i,source in enumerate(sources+[str(sbc/'sbc/sbc.c'),str(sbc/'sbc/sbc_primitives.c')]):
    obj=a.output/(str(i)+'.o');objects.append(obj)
    extra=['-Dmalloc=bluetooth_a2dp_alloc','-Dfree=bluetooth_a2dp_free','-include','apps/bluetooth/a2dp.h'] if i>=len(sources) else []
    command=[cc,*flags,*extra,'-MMD','-MF',a.output/(str(i)+'.d'),'-c',source,'-o',obj]
    run(command)
run([cc,'-fsanitize=address,undefined','-Wl,--gc-sections',*objects,'-o',a.output/'native'])
cases=[('success','joint'),('crc','joint'),('invalid','joint'),('success','short'),('gap','joint'),('revocation','joint'),('peer-close','joint'),('success','single')]
for case,shape in cases:
    command=[a.output/'native',case,a.output/('short.sbc' if shape=='single' else shape+'.sbc'),shape]
    if case=='success':
        capture=a.output/('captured-'+shape);capture.mkdir();command.append(capture)
    run(command)
    if case=='success':run([__import__('sys').executable,'-B','checks/bluetooth-a2dp-host.py','--capture',capture,'--output',a.output/('host-'+shape)])
inputs={}
for dependency in a.output.glob('*.d'):
    for name in shlex.split(dependency.read_text().replace('\\\n',' ').split(':',1)[1]):
        path=pathlib.Path(name);path=path if path.is_absolute() else root/path
        inputs[str(path)]=hashlib.sha256(path.read_bytes()).hexdigest()
inputs[str(pathlib.Path(__file__).resolve())]=hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()
(a.output/'results.json').write_text(json.dumps({'result':'PASS; actual native finite A2DP path over modeled providers, no physical radio','inputs':inputs,'cases':len(cases)},indent=2)+'\n')
print(log)
