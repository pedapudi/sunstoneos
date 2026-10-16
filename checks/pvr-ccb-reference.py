#!/usr/bin/env python3
"""Compare queue ABI and initial publication with isolated pinned driver functions."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import subprocess


def function(source, name, kind):
    start = source.index('\n' + name + '(') + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return kind + '\n' + source[start:end] + '\n'


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define __aligned(n) __attribute__((aligned(n)))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define lockdep_assert_held(x) ((void)(x))
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
static bool warn_on(bool value) { assert(!value); return value; }
#define WARN_ON(x) warn_on(x)
static unsigned barriers, kicks;
#define mb() (++barriers)
#define wmb() (++barriers)
#define ROGUE_FWIF_KCCB_RTN_SLOT_NO_RESPONSE 0
#define PVR_FWIF_DM_GP 0
#define ROGUE_CR_MTS_SCHEDULE_DM_CLRMSK 0xfffffff0u
#define ROGUE_CR_MTS_SCHEDULE 0xb00u
'''

STRUCTURES = r'''
struct rogue_fwif_kccb_cmd { u32 cmd_type, kccb_flags; u8 cmd_data[80]; } __aligned(8);
struct pvr_ccb {
    u32 num_cmds, cmd_size;
    struct rogue_fwif_ccb_ctl *ctrl;
    void *ccb;
    int lock;
};
struct pvr_device {
    struct { struct pvr_ccb ccb; u32 *rtn; u32 reserved_count; } kccb;
    bool lost;
};
static void pvr_cr_write32(struct pvr_device *d, u32 offset, u32 value)
{
    assert(offset == 0xb00 && value == 0 && barriers >= 2);
    assert(d->kccb.ccb.ctrl->write_offset == 1);
    ++kicks;
}
'''

ENTRY = r'''
void reference(void *host_ctrl, void *host_commands, void *returns,
               void *firmware_ctrl, void *firmware_commands)
{
    memset(host_ctrl,0,144); memset(host_commands,0,128*88); memset(returns,0,128*4);
    memset(firmware_ctrl,0,144); memset(firmware_commands,0,32*88);
    struct pvr_device d={.kccb={.ccb={128,88,host_ctrl,host_commands,0},
        .rtn=returns,.reserved_count=1}};
    struct pvr_ccb f={32,88,firmware_ctrl,firmware_commands,0};
    ccb_ctrl_init(host_ctrl,&d.kccb.ccb);
    ccb_ctrl_init(firmware_ctrl,&f);
    struct rogue_fwif_kccb_cmd command={.cmd_type=0x2abc0073};
    u32 slot=99;
    barriers=kicks=0;
    pvr_kccb_send_cmd_reserved_powered(&d,&command,&slot);
    assert(slot==0 && kicks==1 && barriers==3 && d.kccb.reserved_count==0);
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sources', type=Path)
    ap.add_argument('evidence', type=Path)
    args = ap.parse_args()
    root, evidence = args.sources, args.evidence
    names = ['pvr_ccb.c', 'pvr_fw.c', 'pvr_fw.h', 'pvr_rogue_fwif.h',
             'pvr_rogue_fwif_check.h', 'pvr_rogue_cr_defs.h', 'pvr_rogue_fwif_common.h']
    sources = {name: (root / name).read_text() for name in names}
    hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in names}
    manifest = json.loads((root.parent / 'manifest.json').read_text())
    for name, digest in hashes.items():
        records = [r for r in manifest if r['path'] == 'imagination/' + name]
        assert len(records) == 1 and records[0]['sha256'] == digest
    checks = sources['pvr_rogue_fwif_check.h']
    expected = {
        'rogue_fwif_ccb_ctl': (144, {'write_offset': 0, 'read_offset': 128,
                                   'wrap_mask': 132, 'cmd_size': 136}),
        'rogue_fwif_kccb_cmd': (88, {'cmd_type': 0, 'kccb_flags': 4, 'cmd_data': 8}),
        'rogue_fwif_fwccb_cmd': (88, {'cmd_type': 0, 'fwccb_flags': 4, 'cmd_data': 8}),
    }
    assertions = ''
    for name, (size, offsets) in expected.items():
        assert re.search(rf'SIZE_CHECK\(struct {name}, {size}\);', checks)
        if name != 'rogue_fwif_fwccb_cmd':
            assertions += f'_Static_assert(sizeof(struct {name}) == {size}, "size");\n'
        for field, offset in offsets.items():
            assert re.search(rf'OFFSET_CHECK\(struct {name}, {field}, {offset}\);', checks)
            if name != 'rogue_fwif_fwccb_cmd':
                assertions += f'_Static_assert(offsetof(struct {name},{field}) == {offset}, "offset");\n'
    assert re.search(r'KCCB_NUMCMDS_LOG2_DEFAULT 7\b', sources['pvr_fw.h'])
    assert re.search(r'FWCCB_NUMCMDS_LOG2 5\b', sources['pvr_fw.h'])
    assert re.search(r'KCCB_CMD_HEALTH_CHECK = 115U', sources['pvr_rogue_fwif.h'])
    assert re.search(r'ROGUE_CMD_MAGIC_DWORD \(0x2ABC\)', sources['pvr_rogue_fwif.h'])
    assert re.search(r'MTS_SCHEDULE 0x0B00U', sources['pvr_rogue_cr_defs.h'])
    assert re.search(r'PVR_FWIF_DM_GP \(0\)', sources['pvr_rogue_fwif_common.h'])
    ctrl = re.search(r'struct rogue_fwif_ccb_ctl \{.*?\} __aligned\(8\);',
                     sources['pvr_rogue_fwif.h'], re.S).group()
    original = function(sources['pvr_ccb.c'], 'ccb_ctrl_init', 'static void')
    original += function(sources['pvr_ccb.c'], 'pvr_ccb_slot_available_locked', 'static bool')
    original += function(sources['pvr_fw.c'], 'pvr_fw_mts_schedule', 'void')
    original += function(sources['pvr_ccb.c'], 'pvr_kccb_send_cmd_reserved_powered', 'void')
    reference = evidence / 'reference.c'
    reference.write_text('/* Retained reference license: GPL-2.0-only OR MIT.\n'
                         ' * Copyright (c) 2023 Imagination Technologies Ltd. */\n' +
                         SHIM + ctrl + '\n' + STRUCTURES + assertions + original + ENTRY)
    command = ['clang-21', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
               str(reference), '-o', str(evidence / 'reference.so')]
    subprocess.run(command, check=True)
    buffers = [(C.c_uint8 * size)() for size in [144, 128 * 88, 128 * 4, 144, 32 * 88]]
    library = C.CDLL(str(evidence / 'reference.so'))
    library.reference.argtypes = [C.c_void_p] * 5
    library.reference.restype = None
    library.reference(*buffers)
    outputs = {}
    for i, buffer in enumerate(buffers):
        name = f'region-{i}.bin'
        assert (evidence / name).read_bytes() == bytes(buffer), name
        outputs[name] = hashlib.sha256(bytes(buffer)).hexdigest()
    (evidence / 'comparison.json').write_text(json.dumps(dict(
        result='pass', sources=hashes, compiler_command=command, abi=expected,
        outputs=outputs, command='health check', physical_access=False,
        reference_locks_and_barriers='host instrumentation'), indent=2) + '\n')
    print('PASS queue ABI and all five shared regions match pinned initialization/submission functions')


if __name__ == '__main__':
    main()
