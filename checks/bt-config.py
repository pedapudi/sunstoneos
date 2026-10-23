#!/usr/bin/env python3
"""Build a focused controller configuration and ACL handoff model using wire structures from pinned Linux source."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
p=argparse.ArgumentParser()
p.add_argument('--source',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
p.add_argument('--spec',type=pathlib.Path,required=True)
a=p.parse_args()
a.output=a.output.resolve();a.source=a.source.resolve();a.spec=a.spec.resolve()
root=pathlib.Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True,exist_ok=False)
files=['checks/bt-hfp-ag.c','checks/bt-hfp-ag.py','checks/bt-config-fixture.h','include/a733_uart.h','drivers/uart/a733_uart.c','include/bt_h4.h','include/bt_hci.h','include/bt_memory.h','include/a733_range.h',
       'drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','checks/bt-acl-incoming.c','checks/bt-acl-incoming.py','checks/bt-config.c','checks/bt-config.py','include/bt_acl.h','drivers/bluetooth/bt_acl.c']
files += ['include/'+name+'.h' for name in ['bt_l2cap','bt_sdp','bt_rfcomm','bt_sdp_server','bt_hfp_ag']]
files += ['drivers/bluetooth/'+name+'.c' for name in ['bt_l2cap','bt_sdp','bt_rfcomm','bt_sdp_server','bt_hfp_ag']]
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
        'HCI_OP_READ_BUFFER_SIZE','HCI_OP_READ_BD_ADDR','HCI_EV_CMD_COMPLETE','HCI_EV_CMD_STATUS','HCI_EV_DISCONN_COMPLETE','HCI_OP_CREATE_CONN','HCI_OP_DISCONNECT','HCI_EV_CONN_COMPLETE','HCI_EV_NUM_COMP_PKTS','HCI_EV_CONN_REQUEST','HCI_OP_ACCEPT_CONN_REQ','HCI_OP_REJECT_CONN_REQ']
for m in macros:
    ref.append(re.search(r'^#define\s+'+m+r'\s+0x[0-9a-fA-F]+',text,re.M)[0])
for s in ['hci_command_hdr','hci_event_hdr','hci_acl_hdr','hci_sco_hdr','hci_iso_hdr','hci_ev_cmd_complete','hci_ev_cmd_status',
          'hci_rp_read_local_version','hci_rp_read_local_commands','hci_rp_read_local_features','hci_rp_read_buffer_size','hci_rp_read_bd_addr','hci_cp_create_conn','hci_cp_disconnect','hci_ev_conn_complete','hci_ev_disconn_complete','hci_comp_pkts_info','hci_ev_conn_request','hci_cp_accept_conn_req','hci_cp_reject_conn_req']:
    ref.append(re.search(r'struct '+s+r' \{.*?\} __packed;',text,re.S)[0])
(a.output/'bt-ref.h').write_text('\n'.join(ref)+'\n')
(a.output/'source-hci.h').write_bytes(source)
shutil.copy2(a.source/'sources.json',a.output/'sources.json')
spec=a.spec.read_bytes()
assert hashlib.sha256(spec).hexdigest()=='66d656a886aa0f0aa3e62baddffe5c517ce2fcda9886002c58208fe3f000b0e6'
(a.output/'hci-Core-54.html').write_bytes(spec)
manifest['spec']={'url':'https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html','sha256':hashlib.sha256(spec).hexdigest(),'sections':['6.27','7.3.1','7.3.3','7.3.13','7.3.14','7.3.17','7.3.18','7.3.19','7.3.20']}
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
exe=a.output/'bt-config-model'
log=run([cc,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-Iinclude','-I'+str(a.output),
         'drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','drivers/bluetooth/bt_acl.c','checks/bt-config.c','-o',str(exe)])
log+=run([str(exe)])

objects=[]
for name in ['bt_hci','bt_acl','bt_l2cap','bt_sdp','bt_rfcomm','bt_sdp_server','bt_hfp_ag']:
    obj=a.output/(name+'.o');objects.append(obj)
    log+=run([cc,'--target=aarch64-none-elf','-std=c11','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-O2','-Iinclude',
              '-c','drivers/bluetooth/'+name+'.c','-o',str(obj)])
(a.output/'model.log').write_text(log)
manifest['commands']=commands
manifest['artifacts']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in [exe,*objects,a.output/'bt-ref.h']}
manifest['compiler']={'path':cc,'sha256':hashlib.sha256(pathlib.Path(cc).read_bytes()).hexdigest()}
manifest['result']='Pinned protocol/source review, ASan/UBSan configuration and incoming handoff composition, and native HCI/ACL/L2CAP/SDP/RFCOMM/SDP-server/AG consumer compilation passed.'
(a.output/'results.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(log,end='');print(manifest['result'])
