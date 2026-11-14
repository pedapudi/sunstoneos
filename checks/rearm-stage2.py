#!/usr/bin/env python3
"""Run the WDT0 arming replay's own bytes under Unicorn and compare its register accesses with the code it replays.

Usage: rearm-stage2.py --output DIR

tools/board_images.py builds boot/a733/rearm-stage2.S as the runner does for `--rearm-expiry stage2`. Each entry point
runs at its link address on an Armv8 AArch32 CPU model with the WDT0, ROM redirect and RTC pages mapped as MMIO, and
the check records every register load and store in order:

- the gate entry makes the WDT0 accesses of the card first stage's cold path (boot/a733/card-fel-gate.S, label cold):
  the mode read, then mode 0, output 2, configuration 1, mode 0xb1 and the restart key, and no other store;
- the stage-2 entry makes the accesses of stage 2's arm() for a consumed gate arming (boot/a733/stage2/loader.c and
  a733_stage2_watchdog_state in admission.c) in their order, with each access directly between two `dsb sy`, as rd()
  and wr() place them, and stops at the access where arm() would return 0 for each refusal.

The expected sequences are written out here from those sources, and the check also finds each source's own lines, so a
change to either source fails it until the replay and this list follow. No hardware access.
"""
import argparse
import json
from pathlib import Path
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import UC_CPU_ARM_MAX, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

if not __debug__:
    raise RuntimeError('Checks require assertions.')
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import board_images as images

GATE_RUNS = [['--output', '{output}']]
MODE, CFG, OCFG, RESTART = 0x02050014, 0x02050010, 0x02050018, 0x0205000c
FLAG, ENTRY, ALTERNATE, GP2, LAST = 0x070501c0, 0x070501c4, 0x070501d4, 0x07090108, 0x07090104
KEYED = (MODE, CFG, OCFG)
STOP, STACK = 0x40000000, 0x40080000
GATE = [('r', MODE), ('w', MODE, 0x16aa0000), ('w', OCFG, 0x16aa0002), ('w', CFG, 0x16aa0001), ('w', MODE, 0x16aa00b1),
        ('w', RESTART, 0x14af), ('r', MODE), ('r', CFG), ('r', OCFG)]
STATE = [('r', FLAG), ('r', ENTRY), ('r', ALTERNATE), ('r', GP2), ('r', MODE), ('r', CFG), ('r', OCFG)]
DISARM = [('w', MODE, 0x16aa0000), ('r', MODE)]
SETUP = [('w', OCFG, 0x16aa0002), ('w', CFG, 0x16aa0001), ('r', OCFG), ('r', CFG), ('r', MODE)]
ARM = [('w', MODE, 0x16aa0041), ('w', RESTART, 0x14af), ('w', LAST, 0x0c030e00)]
FINAL = [('r', MODE), ('r', CFG), ('r', OCFG)]
# The source lines the sequences above were written from.
SOURCES = {'boot/a733/card-fel-gate.S': ['ldr r2,=0x16aa0002', 'str r2,[r5,#4]', 'ldr r2,=0x16aa0001', 'str r2,[r5,#-4]',
                                         'ldr r2,=0x16aa0000|A733_CARD_FEL_COLD_WATCHDOG', 'ldr r2,=0x14af', 'str r2,[r5,#-8]'],
           'boot/a733/stage2/loader.c': ['wr(0x02050014, 0x16aa0000);', 'if (rd(0x02050014) & 1) return 0;',
                                         'wr(0x02050018, 0x16aa0002);', 'wr(0x02050010, 0x16aa0001);',
                                         "if ((rd(0x02050018) & 0xffff) != 2 || (rd(0x02050010) & 0xffff) != 1 ||",
                                         '((rd(0x02050014) & 1) && !gated)) return 0;', '*armed = counter();',
                                         'wr(0x02050014, 0x16aa0041);', 'wr(0x0205000c, 0x14af);', 'mark(0x0e00);',
                                         "return (rd(0x02050014) & 0xffff) == 0x41 && (rd(0x02050010) & 0xffff) == 1 &&"],
           'boot/a733/stage2/admission.c': ['return !rd(0x070501c0) && !rd(0x070501c4) && !rd(0x070501d4);',
                                            'if (!redirects_clear() || rd(RTC_GP2)) return 0;',
                                            'return (rd(WDT0_MODE) & 0xffff) == GATE_MODE && (rd(WDT0_CONFIG) & 0xffff) == 1 &&']}


def run(binary, entry, registers, script=None):
    """Run ENTRY with REGISTERS; SCRIPT gives an address's successive read values instead. Return the access trace, the
    accessing PCs, the result block, the executed addresses and the final registers."""
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM_MAX)
    uc.mem_map(0x50000, 0x10000)
    uc.mem_map(STOP, 0x100000)
    uc.mem_write(images.LAUNCHER_SRAM[0], binary)
    registers = dict(registers)
    script = {k: list(v) for k, v in (script or {}).items()}
    trace, executed = [], []

    def read(uc, offset, size, base):
        trace.append(('r', base + offset, uc.reg_read(UC_ARM_REG_PC)))
        if script.get(base + offset):
            return script[base + offset].pop(0)
        return registers.get(base + offset, 0)

    def write(uc, offset, size, value, base):
        trace.append(('w', base + offset, value, uc.reg_read(UC_ARM_REG_PC)))
        registers[base + offset] = value & 0xffff if base + offset in KEYED else value
    for base in (0x02050000, 0x07050000, 0x07090000):
        uc.mmio_map(base, 0x1000, read, base, write, base)
    uc.hook_add(UC_HOOK_CODE, lambda uc, address, size, user: executed.append(address))
    uc.reg_write(UC_ARM_REG_SP, STACK)
    uc.reg_write(UC_ARM_REG_LR, STOP)
    uc.emu_start(entry, STOP, count=100000)
    assert uc.reg_read(UC_ARM_REG_PC) == STOP and uc.reg_read(UC_ARM_REG_SP) == STACK, 'The replay did not return.'
    result = bytes(uc.mem_read(images.REARM_RESULT, images.REARM_RESULT_BYTES))
    return [t[:-1] for t in trace], [t[-1] for t in trace], result, executed, registers


def instructions(binary):
    """Each word of BINARY decoded alone (the literal pools between the entry points stop a linear sweep)."""
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    base = images.LAUNCHER_SRAM[0]
    return {base + o: i.mnemonic + ' ' + i.op_str for o in range(0, len(binary), 4)
            for i in md.disasm(binary[o:o + 4], base + o)}


def bracketed(binary, pcs):
    """Each access instruction at PCS (the PC Unicorn reports is the access instruction) sits between two dsb sy."""
    text = instructions(binary)
    return [hex(pc) for pc in pcs if text.get(pc - 4) != 'dsb sy' or text.get(pc + 4) != 'dsb sy']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    for name, lines in SOURCES.items():
        text = ' '.join((ROOT / name).read_text().split())
        missing = [line for line in lines if ' '.join(line.split()) not in text]
        assert not missing, (name, missing)
    binary = images.build_rearm_stage2(a.output / 'build').read_bytes()
    cases = {}
    ready = {MODE: 0, CFG: 1, OCFG: 2}

    trace, pcs, result, _, registers = run(binary, images.REARM_GATE, {**ready, MODE: 0x41})
    assert trace == GATE, trace
    gate = images.rearm_results(result)['gate']
    assert gate == {'mode_at_entry': 0x41, 'mode': 0xb1, 'configuration': 1, 'output': 2, 'done': True}, gate
    cases['gate-cold-path-arming'] = len(trace)

    armed = {**ready, MODE: 0xb1}
    trace, pcs, result, executed, registers = run(binary, images.REARM_STAGE2, armed)
    assert trace == STATE + DISARM + SETUP + ARM + FINAL, trace
    stage2 = images.rearm_results(result)['stage2']
    assert stage2['status'] == 1 and stage2['final'] == [0x41, 1, 2] and registers[MODE] == 0x41, stage2
    assert not bracketed(binary, pcs), bracketed(binary, pcs)
    # The counter read with its isb between the setup reads and the arming store, as counter() sits in arm().
    text = instructions(binary)
    reads = [text[x] for x in executed]
    first = reads.index('mrrc p15, #0, r6, r7, c14')
    assert reads[first - 1] == 'isb sy' and first < reads.index('str r1, [r5]', first), reads[first - 3:first + 6]
    cases['stage2-arm-from-the-gate-arming'] = len(trace)

    # Each refusal stops where arm() returns 0; only the last two follow the disarm, and only the last leaves 0x41 armed.
    refusals = [('redirect-flag', {FLAG: [1]}, STATE[:1], 0x11),
                ('redirect-entry', {ENTRY: [0x20]}, STATE[:2], 0x11),
                ('alternate-flag', {ALTERNATE: [1]}, STATE[:3], 0x11),
                ('gp2-record', {GP2: [0x0c028000]}, STATE[:4], 0x11),
                ('mode-not-the-gate-arming', {MODE: [0x41]}, STATE[:5], 0x11),
                ('configuration-dcxo', {CFG: [0x101]}, STATE[:6], 0x11),
                ('output-differs', {OCFG: [0x1f]}, STATE, 0x11),
                ('disarm-ignored', {MODE: [0xb1, 0xb1]}, STATE + DISARM, 0x12),
                ('output-readback-differs', {OCFG: [2, 0x1f]}, STATE + DISARM + SETUP[:3], 0x13),
                ('final-mode-differs', {MODE: [0xb1, 0, 0, 0x40]}, STATE + DISARM + SETUP + ARM + FINAL[:1], 0x14)]
    for name, script, expected, status in refusals:
        trace, pcs, result, _, registers = run(binary, images.REARM_STAGE2, armed, script)
        assert trace == expected, (name, trace)
        assert images.rearm_results(result)['stage2']['status'] == status, (name, result.hex())
        assert not bracketed(binary, pcs), (name, bracketed(binary, pcs))
        assert any(t[:2] == ('w', MODE) and t[2] == 0x16aa0041 for t in trace) == (status == 0x14), name
        cases['refuses-' + name] = status
    (a.output / 'cases.json').write_text(json.dumps(cases, indent=1) + '\n')
    for name in cases:
        print('PASS', name)


if __name__ == '__main__':
    main()
