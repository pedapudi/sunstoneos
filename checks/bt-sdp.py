#!/usr/bin/env python3
"""Check native SDP against the real lower stack and pinned reference header."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
p=argparse.ArgumentParser()
p.add_argument('--source',type=pathlib.Path,required=True)
p.add_argument('--l2cap-source',type=pathlib.Path,required=True)
p.add_argument('--sdp-source',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True,exist_ok=False)
files=['include/a733_uart.h','drivers/uart/a733_uart.c','include/bt_h4.h','include/bt_hci.h','include/bt_memory.h',
       'drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','checks/bt-l2cap.c','checks/bt-sdp.c','checks/bt-sdp.py','include/bt_sdp.h','drivers/bluetooth/bt_sdp.c','include/bt_l2cap.h','drivers/bluetooth/bt_l2cap.c','include/bt_acl.h','drivers/bluetooth/bt_acl.c']
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
        'HCI_OP_READ_BUFFER_SIZE','HCI_OP_READ_BD_ADDR','HCI_EV_CMD_COMPLETE','HCI_EV_CMD_STATUS','HCI_EV_DISCONN_COMPLETE','HCI_OP_CREATE_CONN','HCI_OP_DISCONNECT','HCI_EV_CONN_COMPLETE','HCI_EV_NUM_COMP_PKTS']
for m in macros:
    ref.append(re.search(r'^#define\s+'+m+r'\s+0x[0-9a-fA-F]+',text,re.M)[0])
for s in ['hci_command_hdr','hci_event_hdr','hci_acl_hdr','hci_sco_hdr','hci_iso_hdr','hci_ev_cmd_complete','hci_ev_cmd_status',
          'hci_rp_read_local_version','hci_rp_read_local_commands','hci_rp_read_local_features','hci_rp_read_buffer_size','hci_rp_read_bd_addr','hci_cp_create_conn','hci_cp_disconnect','hci_ev_conn_complete','hci_ev_disconn_complete','hci_comp_pkts_info']:
    ref.append(re.search(r'struct '+s+r' \{.*?\} __packed;',text,re.S)[0])
(a.output/'bt-ref.h').write_text('\n'.join(ref)+'\n')
(a.output/'source-hci.h').write_bytes(source)
shutil.copy2(a.source/'sources.json',a.output/'sources.json')
l2source=(a.l2cap_source/'include/net/bluetooth/l2cap.h').read_bytes()
assert hashlib.sha256(l2source).hexdigest()=='e9419d18ce803ba930520bc0b7011da51df48c7e93d2760fde4e1e65035f2d4a'
l2text=l2source.decode()
l2ref=[l2text[:l2text.index('#ifndef __L2CAP_H')]]
for m in ['L2CAP_CONN_REQ','L2CAP_CONN_RSP','L2CAP_CONF_REQ','L2CAP_CONF_RSP','L2CAP_DISCONN_REQ','L2CAP_DISCONN_RSP','L2CAP_CID_SIGNALING','L2CAP_PSM_SDP','L2CAP_PSM_RFCOMM','L2CAP_ECHO_REQ']:
    l2ref.append(re.search(r'^#define\s+'+m+r'\s+0x[0-9a-fA-F]+',l2text,re.M)[0])
for name in ['l2cap_hdr','l2cap_cmd_hdr','l2cap_conn_req','l2cap_conn_rsp','l2cap_conf_req','l2cap_conf_rsp','l2cap_disconn_req']:
    l2ref.append(re.search(r'struct '+name+r' \{.*?\} __packed;',l2text,re.S)[0])
(a.output/'l2cap-ref.h').write_text('\n'.join(l2ref)+'\n')
(a.output/'source-l2cap.h').write_bytes(l2source)
shutil.copy2(a.l2cap_source/'sources.json',a.output/'l2cap-sources.json')
sdp_source=(a.sdp_source/'lib/sdp.h').read_bytes()
assert hashlib.sha256(sdp_source).hexdigest()=='7195ad57a4a280195e3f223f5187eaf7c43753ade67250ac54ca08fec08b0267'
sdp_text=sdp_source.decode()
sdp_ref=[sdp_text[:sdp_text.index('#ifndef __SDP_H')]]
for macro in ['SDP_PSM','HANDSFREE_SVCLASS_ID','L2CAP_UUID','RFCOMM_UUID','SDP_SVC_SEARCH_ATTR_REQ','SDP_SVC_SEARCH_ATTR_RSP','SDP_ERROR_RSP','SDP_UINT8','SDP_UINT16','SDP_UINT32','SDP_UUID16','SDP_UUID32','SDP_UUID128','SDP_SEQ8','SDP_ALT8','SDP_TEXT_STR16','SDP_ATTR_RECORD_HANDLE','SDP_ATTR_SVCLASS_ID_LIST','SDP_ATTR_PROTO_DESC_LIST','SDP_ATTR_PFILE_DESC_LIST','SDP_ATTR_SUPPORTED_FEATURES']:
    sdp_ref.append(re.search(r'^#define\s+'+macro+r'\s+0x[0-9a-fA-F]+',sdp_text,re.M)[0])
sdp_ref.append(re.search(r'typedef struct \{\s*uint8_t\s+pdu_id;.*?sdp_pdu_hdr_t;',sdp_text,re.S)[0])
(a.output/'sdp-ref.h').write_text('\n'.join(sdp_ref)+'\n')
(a.output/'source-sdp.h').write_bytes(sdp_source)
shutil.copy2(a.sdp_source/'sources.json',a.output/'sdp-sources.json')
commands=[]
def run(args):
    commands.append([str(x) for x in args])
    try:
        return subprocess.check_output(args,cwd=root,stderr=subprocess.STDOUT,text=True)
    except subprocess.CalledProcessError as error:
        (a.output/'failure.log').write_text(error.output)
        (a.output/'failed-commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        print(error.output,end='')
        raise
cc=shutil.which('clang-21');assert cc
exe=a.output/'bt-sdp-model'
log=run([cc,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-Iinclude','-I'+str(a.output),
         'drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','drivers/bluetooth/bt_acl.c','drivers/bluetooth/bt_l2cap.c','drivers/bluetooth/bt_sdp.c','checks/bt-sdp.c','-o',str(exe)])
log+=run([str(exe)])

objects=[]
for name in ['bt_sdp']:
    obj=a.output/(name+'.o');objects.append(obj)
    log+=run([cc,'--target=aarch64-none-elf','-std=c11','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-O2','-Iinclude',
              '-c','drivers/bluetooth/'+name+'.c','-o',str(obj)])
(a.output/'model.log').write_text(log)
manifest['commands']=commands
manifest['artifacts']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in [exe,*objects,a.output/'bt-ref.h',a.output/'l2cap-ref.h',a.output/'sdp-ref.h']}
manifest['compiler']={'path':cc,'sha256':hashlib.sha256(pathlib.Path(cc).read_bytes()).hexdigest()}
manifest['result']='SDP source-byte comparison, actual UART/HCI/ACL/L2CAP composition with ASan/UBSan and native SDP compilation passed; prior suites not executed.'
(a.output/'results.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(log,end='');print(manifest['result'])
