#!/usr/bin/env python3
"""Compare shutdown commands and idle/reset sequencing with pinned driver functions."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import subprocess


def function(source,name,result):
    start=re.search(r'\n'+name+r'\(',source).start()+1
    brace=source.index('{',start); depth,end=1,brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return result+'\n'+source[start:end]+'\n'


SHIM=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define __aligned(n) __attribute__((aligned(n)))
#define PVR_HAS_FEATURE(d,f) ((d)->f)
#define PVR_FW_PROCESSOR_TYPE_META 0
#define PVR_FW_PROCESSOR_TYPE_MIPS 1
#define META_CR_TxVECINT_BHALT 0
#define POLL_TIMEOUT_USEC 1000000
#define ROGUE_FWIF_KCCB_CMD_POW (0x2abc0000u|107u)
#include "pvr_rogue_cr_defs.h"
struct pvr_device { bool pbe2_in_xe,xt_top_infrastructure; struct { int processor_type; } fw_dev; };
struct event { u32 kind,offset,width; u64 mask,value; };
static struct event events[32];
static unsigned count;
static void event(u32 kind,u32 off,u32 width,u64 mask,u64 value) {
    assert(count<32); events[count++]=(struct event){kind,off,width,mask,value};
}
static int pvr_cr_poll_reg32(struct pvr_device *d,u32 off,u32 value,u32 mask,u32 timeout) {
    (void)d; assert(timeout==1000000); event(0,off,4,mask,value); return 0;
}
static int pvr_cr_poll_reg64(struct pvr_device *d,u32 off,u64 value,u64 mask,u32 timeout) {
    (void)d; assert(timeout==1000000); event(0,off,8,mask,value); return 0;
}
static void pvr_cr_write32(struct pvr_device *d,u32 off,u32 value) { (void)d; event(1,off,4,0,value); }
static void pvr_cr_write64(struct pvr_device *d,u32 off,u64 value) { (void)d; event(1,off,8,0,value); }
static int pvr_meta_cr_read32(struct pvr_device *d,u32 off,u32 *v) { (void)d;(void)off;(void)v;assert(0);return -1; }
'''
COMMANDS=r'''
struct rogue_fwif_kccb_cmd {
    u32 cmd_type,kccb_flags;
    union { struct rogue_fwif_power_request pow_data; unsigned char reserved[80]; } cmd_data;
} __aligned(8);
_Static_assert(sizeof(struct rogue_fwif_kccb_cmd)==88,"command bytes");
_Static_assert(offsetof(struct rogue_fwif_kccb_cmd,cmd_data)==8,"command payload");
_Static_assert(sizeof(struct rogue_fwif_power_request)==8,"power bytes");
_Static_assert(offsetof(struct rogue_fwif_power_request,power_req_data)==4,"request data");
static struct rogue_fwif_kccb_cmd commands[2];
static unsigned command_count;
static int pvr_power_send_command(struct pvr_device *d,struct rogue_fwif_kccb_cmd *in) {
    (void)d; assert(command_count<2);
    struct rogue_fwif_kccb_cmd *out=&commands[command_count++];
    memset(out,0,sizeof(*out)); out->cmd_type=in->cmd_type;
    out->cmd_data.pow_data.pow_type=in->cmd_data.pow_data.pow_type;
    /* Read only fields assigned by the original functions. Other native bytes must be zero. */
    if(in->cmd_data.pow_data.pow_type==ROGUE_FWIF_POW_OFF_REQ)
        out->cmd_data.pow_data.power_req_data.forced=in->cmd_data.pow_data.power_req_data.forced;
    else out->cmd_data.pow_data.power_req_data.pow_request_type=in->cmd_data.pow_data.power_req_data.pow_request_type;
    return 0;
}
'''
ENTRY=r'''
u32 reference(u32 pbe,u32 xt,struct event *out,void *out_commands) {
    struct pvr_device d={pbe,xt,{PVR_FW_PROCESSOR_TYPE_MIPS}};
    count=command_count=0;
    assert(!pvr_power_request_idle(&d)); assert(!pvr_power_request_pwr_off(&d));
    assert(!pvr_fw_stop(&d));
    memcpy(out,events,count*sizeof(*events)); memcpy(out_commands,commands,sizeof(commands));
    return count;
}
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sources',type=Path); ap.add_argument('firmware',type=Path); ap.add_argument('evidence',type=Path)
    a=ap.parse_args(); root,e=a.sources,a.evidence
    names=['pvr_power.c','pvr_fw_startstop.c','pvr_rogue_fwif.h','pvr_rogue_fwif_check.h','pvr_rogue_cr_defs.h']
    sources={n:(root/n).read_text() for n in names}
    hashes={n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in names}
    manifest=json.loads((root.parent/'manifest.json').read_text())
    for name,digest in hashes.items():
        rows=[r for r in manifest if r['path']=='imagination/'+name]
        assert len(rows)==1 and rows[0]['sha256']==digest
    h=sources['pvr_rogue_fwif.h']; checks=sources['pvr_rogue_fwif_check.h']
    assert 'ROGUE_FWIF_KCCB_CMD_POW = 107U' in h
    assert 'OFFSET_CHECK(struct rogue_fwif_kccb_cmd, cmd_data, 8);' in checks
    assert 'SIZE_CHECK(struct rogue_fwif_kccb_cmd, 88);' in checks
    assert 'OFFSET_CHECK(struct rogue_fwif_power_request, power_req_data, 4);' in checks
    code='/* SPDX-License-Identifier: MIT\n * Copyright (c) 2023 Imagination Technologies Ltd.\n * Retained terms: LICENSE.pvr. */\n'+SHIM
    for name in ['rogue_fwif_power_type','rogue_fwif_power_force_idle_type']:
        code+=re.search(r'enum '+name+r' \{.*?\};',h,re.S).group()+'\n'
    code+=re.search(r'struct rogue_fwif_power_request \{.*?\n\};',h,re.S).group()+'\n'+COMMANDS
    for name in ['pvr_power_request_idle','pvr_power_request_pwr_off']:
        code+=function(sources['pvr_power.c'],name,'static int')
    code+=function(sources['pvr_fw_startstop.c'],'pvr_fw_stop','int')+ENTRY
    (e/'pvr_rogue_cr_defs.h').write_text(sources['pvr_rogue_cr_defs.h'])
    (e/'LICENSE.pvr').write_bytes((Path(__file__).resolve().parents[1]/'drivers/gpu/LICENSE.pvr').read_bytes())
    ref=e/'reference.c'; ref.write_text(code)
    command=['clang-21','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',str(ref),'-o',str(e/'reference.so')]
    subprocess.run(command,check=True)
    class Event(C.Structure):
        _fields_=[('kind',C.c_uint32),('offset',C.c_uint32),('width',C.c_uint32),('mask',C.c_uint64),('value',C.c_uint64)]
    lib=C.CDLL(str(e/'reference.so'))
    lib.reference.argtypes=[C.c_uint32,C.c_uint32,C.POINTER(Event),C.c_void_p]; lib.reference.restype=C.c_uint32
    firmware=a.firmware.read_bytes()
    assert hashlib.sha256(firmware).hexdigest()=='1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2'
    results=[]
    for i,raw in enumerate([firmware,(e/'variant.fw').read_bytes()]):
        pbe=bool(raw[122880+48+4]&1); xt=bool(raw[122880+48+8]&32)
        out=(Event*32)(); commands=(C.c_uint8*176)(); count=lib.reference(pbe,xt,out,commands)
        trace=json.loads((e/f'trace-{i}.json').read_text())
        upstream=[{k:getattr(v,k) for k in ['kind','offset','width','mask','value']} for v in out[:count]]
        # The A733 register sequence is the vendor DDK's RGXStop for a MIPS core
        # (docs/designs/gpu-firmware-stop.md): it first acknowledges a pending
        # MIPS interrupt (kinds 2 and 3: read 0x8a8, clear 0x8b0), polls SLC_IDLE
        # after the first Sidekick poll, clears only the thread-0 DM
        # associations, and asserts no SOFT_RESET. Upstream's other operations
        # stay, in its order.
        thread1=[x for x in upstream if x['kind']==1 and x['offset'] in (0xb48,0xb38)]
        reset=[x for x in upstream if x['offset']==0x100]
        assert len(thread1)==2 and reset==[upstream[-1]] and reset[0]['kind']==1
        kept=[x for x in upstream if x not in thread1 and x is not reset[0]]
        assert kept[0]==dict(kind=0,offset=0x3c8,width=4,mask=0x78,value=0x78)
        expected=[dict(kind=2,offset=0x8a8,width=4,mask=0,value=0),dict(kind=3,offset=0x8b0,width=4,mask=0,value=1),
                  kept[0],dict(kind=0,offset=0x3898,width=4,mask=0xff,value=0xff)]+kept[1:]
        assert trace['plan']==expected,(trace['plan'],expected)
        # The traces record a pending interrupt, so the clear is made.
        assert len(trace['accesses'])==len(trace['plan'])
        for operation,access in zip(trace['plan'],trace['accesses']):
            assert access['offset']==operation['offset'] and access['width']==operation['width']
            assert access['write']==int(operation['kind'] in (1,3))
            if operation['kind'] in (1,3): assert access['value']==operation['value']
            elif operation['kind']==0: assert access['value']&operation['mask']==operation['value']
        assert bytes(commands)==(e/'commands.bin').read_bytes()
        results.append(dict(synthetic=bool(i),pbe2_in_xe=pbe,xt_top_infrastructure=xt,reference_operations=count,
            native_operations=len(trace['plan']),firmware_sha256=hashlib.sha256(raw).hexdigest()))
    # The stop without the firmware's acknowledgements (trace 2, pinned
    # firmware): the same vendor plan, then upstream's final soft reset with
    # the mask of the firmware's pbe2_in_xe feature.
    raw=firmware; pbe=bool(raw[122880+48+4]&1); xt=bool(raw[122880+48+8]&32)
    out=(Event*32)(); commands=(C.c_uint8*176)(); count=lib.reference(pbe,xt,out,commands)
    upstream=[{k:getattr(v,k) for k in ['kind','offset','width','mask','value']} for v in out[:count]]
    reset=upstream[-1]; assert reset['offset']==0x100 and reset['kind']==1 and reset['width']==8
    hard=json.loads((e/'trace-2.json').read_text())
    first=json.loads((e/'trace-0.json').read_text())['plan']
    assert hard['plan']==first+[reset],(hard['plan'][-1],reset)
    results.append(dict(unacknowledged=True,reset_value=reset['value'],native_operations=len(hard['plan'])))
    (e/'comparison.json').write_text(json.dumps(dict(result='pass',sources=hashes,compiler_command=command,
        variants=results,command_comparison='Original functions assign defined fields; unused reference bytes zeroed in capture shim.',
        native_differences='Vendor RGXStop: interrupt acknowledgement, early SLC_IDLE poll, thread-0 DM associations only, no SOFT_RESET; mandatory platform isolation callback',physical_access=False),indent=2)+'\n')
    print('PASS shutdown commands match the pinned functions and the idle plans match them with the vendor RGXStop differences, for both feature profiles; the unacknowledged plan adds upstream\'s final soft reset')


if __name__=='__main__': main()
