#!/usr/bin/env python3
"""Build a Bluetooth return HCI/ACL/UART model using wire structures from pinned Linux source."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
GATE_RUNS = [
    ['--source', '{cache}/sources/bluetooth-hci', '--output', '{output}', '--spec', '{cache}/sources/bluetooth-incoming/hci-Core-54.html', '--security', '{cache}/sources/bluetooth-security-zephyr-v4.2.1'],
]
p=argparse.ArgumentParser()
p.add_argument('--source',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
p.add_argument('--security',type=pathlib.Path,required=True)
p.add_argument('--spec',type=pathlib.Path,required=True)
a=p.parse_args()
a.output=a.output.resolve();a.source=a.source.resolve();a.spec=a.spec.resolve()
root=pathlib.Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True,exist_ok=False)
files=['include/bt_security.h','include/bt_security_port.h','drivers/bluetooth/bt_security.c','checks/npu-startup-host/FreeRTOS.h','checks/npu-startup-host/task.h','apps/bluetooth/security-source.py','apps/bluetooth/security-source.json','checks/bt-acl-incoming.c','checks/bt-config-fixture.h','include/a733_uart.h','drivers/uart/a733_uart.c','include/bt_h4.h','include/bt_hci.h','include/bt_memory.h','include/a733_range.h',
       'drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','checks/bt-security-return.c','checks/bt-security-return.py','include/bt_acl.h','drivers/bluetooth/bt_acl.c']
manifest={}
for f in files:
    data=(root/f).read_bytes();dest=a.output/'inputs'/f;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
    manifest[f]=hashlib.sha256(data).hexdigest()
source=(a.source/'include/net/bluetooth/hci.h').read_bytes()
assert hashlib.sha256(source).hexdigest()=='9da36dad169c8c02ba3a676a3c884527e4efb7d152b975b131f33aaa59347bb7'
text=source.decode()
ref=[text[:text.index('#ifndef __HCI_H')], '#include <stdint.h>','typedef uint8_t __u8; typedef uint16_t __le16;',
     'typedef struct { uint8_t b[6]; } bdaddr_t;','#define __packed __attribute__((packed))']
macros=['HCI_COMMAND_PKT','HCI_ACLDATA_PKT','HCI_SCODATA_PKT','HCI_EVENT_PKT','HCI_ISODATA_PKT',
        'HCI_OP_RESET','HCI_OP_READ_LOCAL_VERSION','HCI_OP_READ_LOCAL_COMMANDS','HCI_OP_READ_LOCAL_FEATURES',
        'HCI_OP_READ_BUFFER_SIZE','HCI_OP_READ_BD_ADDR','HCI_EV_CMD_COMPLETE','HCI_EV_CMD_STATUS','HCI_EV_DISCONN_COMPLETE','HCI_OP_CREATE_CONN','HCI_OP_DISCONNECT','HCI_EV_CONN_COMPLETE','HCI_EV_NUM_COMP_PKTS','HCI_EV_CONN_REQUEST','HCI_OP_ACCEPT_CONN_REQ','HCI_OP_REJECT_CONN_REQ','HCI_OP_CREATE_CONN_CANCEL','HCI_OP_WRITE_SCAN_ENABLE']
for m in macros:
    ref.append(re.search(r'^#define\s+'+m+r'\s+0x[0-9a-fA-F]+',text,re.M)[0])
for s in ['hci_command_hdr','hci_event_hdr','hci_acl_hdr','hci_sco_hdr','hci_iso_hdr','hci_ev_cmd_complete','hci_ev_cmd_status',
          'hci_rp_read_local_version','hci_rp_read_local_commands','hci_rp_read_local_features','hci_rp_read_buffer_size','hci_rp_read_bd_addr','hci_cp_create_conn','hci_cp_disconnect','hci_ev_conn_complete','hci_ev_disconn_complete','hci_comp_pkts_info','hci_ev_conn_request','hci_cp_accept_conn_req','hci_cp_reject_conn_req','hci_cp_create_conn_cancel']:
    ref.append(re.search(r'struct '+s+r' \{.*?\} __packed;',text,re.S)[0])
(a.output/'bt-ref.h').write_text('\n'.join(ref)+'\n')
(a.output/'source-hci.h').write_bytes(source)
shutil.copy2(a.source/'sources.json',a.output/'sources.json')
spec=a.spec.read_bytes()
assert hashlib.sha256(spec).hexdigest()=='66d656a886aa0f0aa3e62baddffe5c517ce2fcda9886002c58208fe3f000b0e6'
(a.output/'hci-Core-54.html').write_bytes(spec)
manifest['spec']={'url':'https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html','sha256':hashlib.sha256(spec).hexdigest(),'sections':['7.1.7','7.1.8','7.1.9','7.3.2','7.3.18','7.7.3','7.7.4']}
(a.output/'input-hashes.json').write_text(json.dumps(manifest,indent=2)+'\n')
commands=[]
def run(args):
    commands.append([str(x) for x in args])
    result=subprocess.run(args,cwd=a.output/'inputs',stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    with (a.output/'commands.jsonl').open('a') as log: log.write(json.dumps({'args':commands[-1],'returncode':result.returncode})+'\n')
    with (a.output/'build.log').open('a') as log: log.write(result.stdout)
    if result.returncode: raise SystemExit(result.stdout)
    return result.stdout
cc=shutil.which('clang-21');assert cc
import sys
run([sys.executable,'apps/bluetooth/security-source.py','--source',str(a.security.resolve()),'--output',str(a.output/'security')])
security_sources=[str(a.output/'security'/('zephyr-'+name+'.c')) for name in ['ssp','keys_br','security-level']]
exe=a.output/'bt-security-return-model'
log=run([cc,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-Wno-unused-parameter','-Wno-for-loop-analysis','-Wno-sign-compare','-Ichecks/npu-startup-host','-I'+str(a.output/'security'),'-Iinclude','-I'+str(a.output),
         'drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','drivers/bluetooth/bt_acl.c','drivers/bluetooth/bt_security.c',*security_sources,'checks/bt-security-return.c','-o',str(exe)])
for case in range(6):log+=run([str(exe),str(case)])
mutations=[
    ('configuration-admission','!s->stopping&&!s->configured&&!s->count','!s->configured&&!s->count',0,'!s.count'),
    ('disconnect-revocation','c->authenticated=0;c->key_verified=0;c->encrypt=0;',
     'c->key_verified=0;c->encrypt=0;',3,'!s.connections[0].authenticated'),
]
manifest['mutations']=[]
for label,old,new,case,expected in mutations:
    original=(root/'drivers/bluetooth/bt_security.c').read_text();assert original.count(old)==1
    changed=a.output/(label+'.c');changed.write_text(original.replace(old,new))
    binary=a.output/(label+'-model')
    run([cc,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
         '-Wno-unused-parameter','-Wno-for-loop-analysis','-Wno-sign-compare','-Ichecks/npu-startup-host',
         '-I'+str(a.output/'security'),'-Iinclude','-I'+str(a.output),
         'drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c',
         'drivers/bluetooth/bt_acl.c',str(changed),*security_sources,'checks/bt-security-return.c','-o',str(binary)])
    result=subprocess.run([str(binary),str(case)],cwd=a.output/'inputs',capture_output=True,text=True)
    (a.output/(label+'.log')).write_text(result.stdout+result.stderr)
    assert result.returncode!=0 and expected in result.stderr,(label,result.returncode,result.stderr)
    manifest['mutations'].append({'name':label,'returncode':result.returncode,'assertion':expected})

objects=[]
for name in ['bt_security']:
    obj=a.output/(name+'.o');objects.append(obj)
    log+=run([cc,'--target=aarch64-none-elf','-std=c11','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-O2','-Ichecks/npu-startup-host','-I'+str(a.output/'security'),'-I'+str(pathlib.Path.home()/'.cache/sunstoneos/host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include'),'-Iinclude',
              '-c','drivers/bluetooth/'+name+'.c','-o',str(obj)])
(a.output/'model.log').write_text(log)
manifest['commands']=commands
manifest['artifacts']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in [exe,*objects,a.output/'bt-ref.h']}
manifest['compiler']={'path':cc,'sha256':hashlib.sha256(pathlib.Path(cc).read_bytes()).hexdigest()}
manifest['result']='Pinned Zephyr SSP/key composition, six ASan/UBSan return cases, two rejected mutations and AArch64 security compilation passed.'
(a.output/'results.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(log,end='');print(manifest['result'])
