#!/usr/bin/env python3
"""Check the AVDTP SBC sink over the actual lower stack, SDP sink record and pinned libsbc."""
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
p.add_argument('--sbc-source',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True,exist_ok=False)
files=['include/a733_uart.h','drivers/uart/a733_uart.c','include/bt_h4.h','include/bt_hci.h','include/bt_memory.h',
       'drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','checks/bt-l2cap.c','checks/bt-avdtp.c','checks/bt-avdtp.py',
       'include/bt_avdtp.h','drivers/bluetooth/bt_avdtp.c','include/bt_sdp_server.h','drivers/bluetooth/bt_sdp_server.c',
       'include/bt_l2cap.h','drivers/bluetooth/bt_l2cap.c','include/bt_acl.h','drivers/bluetooth/bt_acl.c','apps/bluetooth/msbc-source.json']
manifest={}
for f in files:
    data=(root/f).read_bytes();dest=a.output/'inputs'/f;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
    manifest[f]=hashlib.sha256(data).hexdigest()
source=(a.source/'include/net/bluetooth/hci.h').read_bytes()
assert hashlib.sha256(source).hexdigest()=='9da36dad169c8c02ba3a676a3c884527e4efb7d152b975b131f33aaa59347bb7'
text=source.decode()
ref=[text[:text.index('#ifndef __HCI_H')], '#include <stdint.h>','typedef uint8_t __u8; typedef uint16_t __le16;',
     'typedef struct { uint8_t b[6]; } bdaddr_t;','#define __packed __attribute__((packed))']
for m in ['HCI_COMMAND_PKT','HCI_ACLDATA_PKT','HCI_SCODATA_PKT','HCI_EVENT_PKT','HCI_ISODATA_PKT',
        'HCI_OP_RESET','HCI_OP_READ_LOCAL_VERSION','HCI_OP_READ_LOCAL_COMMANDS','HCI_OP_READ_LOCAL_FEATURES',
        'HCI_OP_READ_BUFFER_SIZE','HCI_OP_READ_BD_ADDR','HCI_EV_CMD_COMPLETE','HCI_EV_CMD_STATUS','HCI_EV_DISCONN_COMPLETE','HCI_OP_CREATE_CONN','HCI_OP_DISCONNECT','HCI_EV_CONN_COMPLETE','HCI_EV_NUM_COMP_PKTS']:
    ref.append(re.search(r'^#define\s+'+m+r'\s+0x[0-9a-fA-F]+',text,re.M)[0])
for s in ['hci_command_hdr','hci_event_hdr','hci_acl_hdr','hci_sco_hdr','hci_iso_hdr','hci_ev_cmd_complete','hci_ev_cmd_status',
          'hci_rp_read_local_version','hci_rp_read_local_commands','hci_rp_read_local_features','hci_rp_read_buffer_size','hci_rp_read_bd_addr','hci_cp_create_conn','hci_cp_disconnect','hci_ev_conn_complete','hci_ev_disconn_complete','hci_comp_pkts_info']:
    ref.append(re.search(r'struct '+s+r' \{.*?\} __packed;',text,re.S)[0])
(a.output/'bt-ref.h').write_text('\n'.join(ref)+'\n')
l2text=(a.l2cap_source/'include/net/bluetooth/l2cap.h').read_bytes()
assert hashlib.sha256(l2text).hexdigest()=='e9419d18ce803ba930520bc0b7011da51df48c7e93d2760fde4e1e65035f2d4a'
l2text=l2text.decode()
l2ref=[l2text[:l2text.index('#ifndef __L2CAP_H')]]
for m in ['L2CAP_CONN_REQ','L2CAP_CONN_RSP','L2CAP_CONF_REQ','L2CAP_CONF_RSP','L2CAP_DISCONN_REQ','L2CAP_DISCONN_RSP','L2CAP_CID_SIGNALING','L2CAP_PSM_SDP','L2CAP_PSM_RFCOMM','L2CAP_ECHO_REQ']:
    l2ref.append(re.search(r'^#define\s+'+m+r'\s+0x[0-9a-fA-F]+',l2text,re.M)[0])
for name in ['l2cap_hdr','l2cap_cmd_hdr','l2cap_conn_req','l2cap_conn_rsp','l2cap_conf_req','l2cap_conf_rsp','l2cap_disconn_req']:
    l2ref.append(re.search(r'struct '+name+r' \{.*?\} __packed;',l2text,re.S)[0])
(a.output/'l2cap-ref.h').write_text('\n'.join(l2ref)+'\n')
sdp_text=(a.sdp_source/'lib/sdp.h').read_bytes()
assert hashlib.sha256(sdp_text).hexdigest()=='7195ad57a4a280195e3f223f5187eaf7c43753ade67250ac54ca08fec08b0267'
sdp_text=sdp_text.decode()
sdp_ref=[sdp_text[:sdp_text.index('#ifndef __SDP_H')]]
for macro in ['SDP_SVC_SEARCH_ATTR_REQ','SDP_SVC_SEARCH_ATTR_RSP','SDP_UINT16','SDP_UUID16','SDP_SEQ8']:
    sdp_ref.append(re.search(r'^#define\s+'+macro+r'\s+0x[0-9a-fA-F]+',sdp_text,re.M)[0])
(a.output/'sdp-ref.h').write_text('\n'.join(sdp_ref)+'\n')
for name in ['sources.json']:
    shutil.copy2(a.source/name,a.output/'hci-sources.json');shutil.copy2(a.l2cap_source/name,a.output/'l2cap-sources.json')
    shutil.copy2(a.sdp_source/name,a.output/'sdp-sources.json')
# The pinned libsbc copy is verified against the existing source record.
sbc_record=json.loads((root/'apps/bluetooth/msbc-source.json').read_text())
sbc=a.output/'sbc'
for item in sbc_record['sources']:
    data=(a.sbc_source/item['path']).read_bytes();assert hashlib.sha256(data).hexdigest()==item['sha256'],item['path']
    dest=sbc/item['path'];dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
# The SBC information element layout in the driver must match libsbc's A2DP
# configuration bits: frequency and mode share byte 0, the rest byte 1.
sbc_c=(sbc/'sbc/sbc.c').read_text()
bits={m[0]:eval(m[1]) for m in re.findall(r'#define\s+(A2DP_\w+)\s+(\(1 << \d\)|\d)',sbc_c)}
assert bits=={'A2DP_SAMPLING_FREQ_16000':8,'A2DP_SAMPLING_FREQ_32000':4,'A2DP_SAMPLING_FREQ_44100':2,'A2DP_SAMPLING_FREQ_48000':1,
    'A2DP_CHANNEL_MODE_MONO':8,'A2DP_CHANNEL_MODE_DUAL_CHANNEL':4,'A2DP_CHANNEL_MODE_STEREO':2,'A2DP_CHANNEL_MODE_JOINT_STEREO':1,
    'A2DP_BLOCK_LENGTH_4':8,'A2DP_BLOCK_LENGTH_8':4,'A2DP_BLOCK_LENGTH_12':2,'A2DP_BLOCK_LENGTH_16':1,
    'A2DP_SUBBANDS_4':2,'A2DP_SUBBANDS_8':1,'A2DP_ALLOCATION_SNR':2,'A2DP_ALLOCATION_LOUDNESS':1},bits
layout=re.search(r'#if __BYTE_ORDER == __LITTLE_ENDIAN\s*struct a2dp_sbc \{(.*?)\}',sbc_c,re.S)[1]
assert re.findall(r'uint8_t (\w+)(?::(\d))?;',layout)==[('channel_mode','4'),('frequency','4'),('allocation_method','2'),
    ('subbands','2'),('block_length','4'),('min_bitpool',''),('max_bitpool','')]
base=(root/'checks/bt-l2cap.c').read_text()
base=base.replace('output[48]','output[1536]').replace('output_length[48]','output_length[1536]').replace('outputs<48','outputs<1536')
(a.output/'avdtp-l2cap-fixture.h').write_text(base)
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
sanitize=['-fsanitize=address,undefined','-g']
log=''
objects=[]
for name in ['sbc','sbc_primitives']:
    obj=a.output/(name+'.o');objects.append(obj)
    log+=run([cc,'-std=gnu11','-O1','-w','-DSBC_HIGH_PRECISION=1',*sanitize,'-I'+str(sbc),'-c',str(sbc/'sbc'/(name+'.c')),'-o',str(obj)])
exe=a.output/'bt-avdtp-model'
log+=run([cc,'-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror',*sanitize,'-Iinclude','-I'+str(a.output),'-I'+str(sbc),
         'drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c','drivers/bluetooth/bt_hci.c','drivers/bluetooth/bt_acl.c',
         'drivers/bluetooth/bt_l2cap.c','drivers/bluetooth/bt_sdp_server.c','drivers/bluetooth/bt_avdtp.c','checks/bt-avdtp.c',
         *[str(o) for o in objects],'-lm','-o',str(exe)])
log+=run([str(exe)])
native=a.output/'bt_avdtp.o'
log+=run([cc,'--target=aarch64-none-elf','-std=c11','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-O2','-Iinclude',
          '-c','drivers/bluetooth/bt_avdtp.c','-o',str(native)])
(a.output/'model.log').write_text(log)
manifest['commands']=commands
manifest['artifacts']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in [exe,native,*objects,a.output/'bt-ref.h',a.output/'l2cap-ref.h',a.output/'sdp-ref.h',a.output/'avdtp-l2cap-fixture.h']}
manifest['compiler']={'path':cc,'sha256':hashlib.sha256(pathlib.Path(cc).read_bytes()).hexdigest()}
manifest['result']='AVDTP SBC sink over actual UART/HCI/ACL/L2CAP and SDP with pinned libsbc frames, ASan/UBSan and native AVDTP compilation passed; no physical radio or source device.'
(a.output/'results.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(log,end='');print(manifest['result'])
