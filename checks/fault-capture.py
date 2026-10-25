#!/usr/bin/env python3
"""Execute the integrated OS image's fault capture and slot rotation.

Runs the linked image's own EL1 vectors, EL3 fault handler, capture leaf and
boot rotation under Unicorn with modeled system registers, on every core and
for every kind of park: a vector fault (EL1h, EL1t, EL0 and a direct fault
vector in each EL1 table), a deliberate exit with a stale x2, an exception
taken at EL3, a failed state check and an MPIDR that names no core. It checks
the pending byte, the decoded slot, the memory-write log and the instruction
counts against the budgets of docs/designs/cpu-fault-reset.md, and the
supervisor's added load by disassembly. It also compiles the single-core line
formatter on the host and compares it with checks/fault-decode.py.

With --baseline IMAGE it measures the same EL3 report path in an image built
without the capture. No hardware is accessed.
"""
import argparse
import hashlib
import importlib.util
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X29, UC_ARM64_REG_LR, UC_ARM64_REG_SP,
                                 UC_ARM64_REG_PC)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fault_decode', ROOT / 'checks/fault-decode.py')
decode = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decode)

SLOTS = 0x42001000
STAGE2 = 0x47008200
TCB = 0x41f80000
EL1_STACK = 0x41f90000
SENTINEL = 0x41fff000
RESIDENT_RESULT_MAGIC = 0x415337334f525231
REGS = [UC_ARM64_REG_X0 + i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_LR]


class Image:
    def __init__(self, path):
        with open(path, 'rb') as f:
            elf = ELFFile(f)
            self.segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments()
                             if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
            self.symbols, self.sizes = {}, {}
            for s in elf.get_section_by_name('.symtab').iter_symbols():
                if s.name and s['st_info']['bind'] == 'STB_GLOBAL':
                    self.symbols[s.name] = s['st_value']
                    self.sizes[s.name] = s['st_size']
        self.path = Path(path)
        self.decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.decoded = {}

    def machine(self):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x41000000, 0x1200000)
        uc.mem_map(STAGE2 & ~0xfff, 0x1000)
        for address, data in self.segments:
            uc.mem_write(address, data)
        return uc

    def insn(self, uc, address):
        if address not in self.decoded:
            self.decoded[address] = next(self.decoder.disasm(bytes(uc.mem_read(address, 4)), address))
        return self.decoded[address]


def run(image, uc, pc, regs, model, stops, limit=20000):
    """Run from pc until a WFE park, a stop symbol or the sentinel. System
    registers come from model; SMC enters EL3 vector slot 8 as the hardware
    does for an SMC from EL1."""
    trace, writes, reads, events = [], [], [], []
    state = {'stop': None, 'el': 1, 'cntpct': 1000}
    stop_at = {image.symbols[name]: name for name in stops if name in image.symbols}
    stop_at[SENTINEL] = 'return'

    def gpr(name):
        return 0 if name in ('xzr', 'wzr') else uc.reg_read(REGS[int(name[1:])])

    def code(cpu, address, size, unused):
        if address in stop_at:
            state['stop'] = stop_at[address]
            cpu.emu_stop()
            return
        if len(trace) >= limit:
            state['stop'] = 'limit'
            cpu.emu_stop()
            return
        trace.append(address)
        if 0 <= address - image.symbols['a733_cached_el3_vectors'] < 2048:
            state['el'] = 3
        i = image.insn(cpu, address)
        args = i.op_str.split(', ')
        if i.mnemonic == 'mrs':
            if args[1] == 'cntpct_el0':
                state['cntpct'] += 24
                value = state['cntpct']
            else:
                assert args[1] in model, (hex(address), i.op_str)
                value = model[args[1]]
            cpu.reg_write(REGS[int(args[0][1:])], value)
            cpu.reg_write(UC_ARM64_REG_PC, address + 4)
        elif i.mnemonic == 'msr':
            if args[0] == 'spsel':
                pass
            else:
                model[args[0]] = int(args[1][1:], 0) if args[1].startswith('#') else gpr(args[1])
            cpu.reg_write(UC_ARM64_REG_PC, address + 4)
        elif i.mnemonic in ('dsb', 'dmb', 'isb', 'sev', 'ic', 'tlbi'):
            events.append((len(trace) - 1, i.mnemonic + ' ' + i.op_str))
            cpu.reg_write(UC_ARM64_REG_PC, address + 4)
        elif i.mnemonic == 'wfe':
            state['stop'] = 'park'
            cpu.emu_stop()
        elif i.mnemonic == 'smc':
            events.append((len(trace) - 1, 'smc'))
            model['esr_el3'] = 0x5e000000
            model['elr_el3'] = address + 4
            state['el'] = 3
            state['smc_at'] = len(trace) - 1
            cpu.reg_write(UC_ARM64_REG_PC, image.symbols['a733_cached_el3_vectors'] + 8 * 128)

    def write(cpu, access, address, size, value, unused):
        writes.append((len(trace) - 1, address, size, value & ((1 << (8 * size)) - 1), state['el']))

    def read(cpu, access, address, size, value, unused):
        reads.append((len(trace) - 1, address, size, state['el']))

    uc.hook_add(UC_HOOK_CODE, code)
    uc.hook_add(UC_HOOK_MEM_WRITE, write)
    uc.hook_add(UC_HOOK_MEM_READ, read)
    for reg, value in regs.items():
        uc.reg_write(reg, value)
    uc.emu_start(pc, 0, count=limit + 10)
    return dict(stop=state['stop'], trace=trace, writes=writes, reads=reads, events=events,
                smc_at=state.get('smc_at'))


def el3_model(core, mpidr=None):
    return {'mpidr_el1': mpidr if mpidr is not None else 0x81000000 | core << 8,
            'esr_el1': 0x96000045, 'elr_el1': 0x41012340 + core, 'far_el1': 0xdead0000 + core * 8,
            'spsr_el1': 0x3c5, 'esr_el3': 0, 'far_el3': 0xfa3, 'elr_el3': 0, 'tcr_el1': 0x80803519,
            'sctlr_el1': 0x30d01805, 'sctlr_el3': 0, 'tcr_el3': 0, 'mair_el3': 0, 'scr_el3': 0x535,
            'ttbr0_el3': 0, 'currentel': 12, 'daif': 0x3c0, 'tpidr_el1': core}


def prepare(image, uc, core, state=3, name=b'mimus-board\0'):
    records = image.symbols['a733_cached_records']
    epoch = 0x5eed0000 + core
    uc.mem_write(records + core * 128, struct.pack('<3Q', epoch, core, state))
    offset = struct.unpack('<Q', uc.mem_read(image.symbols['a733_fault_task_name_offset'], 8))[0]
    uc.mem_write(image.symbols['pxCurrentTCBs'] + core * 8, struct.pack('<Q', TCB))
    uc.mem_write(TCB + offset, name.ljust(16, b'\0')[:16])
    return epoch


def cases(image):
    s = image.symbols
    out = []
    for core in range(8):
        kinds = [
            ('vector-el1h', s['a733_cached_el1_vectors'] + 4 * 128, {'spsr_el1': 0x3c5}, 0x104),
            ('vector-el0', s['a733_cached_el1_vectors'] + 8 * 128, {'spsr_el1': 0x0, 'esr_el1': 0x92000007}, 0x108),
            ('vector-el1t', s['a733_cached_el1_vectors'] + 0 * 128, {'spsr_el1': 0x3c4}, 0x100),
            ('vector-direct', s['a733_cached_el1_vectors'] + 7 * 128, {'esr_el1': 0xbe000000}, 0x107),
            ('vector-cold', s['a733_cached_cold_vectors'] + 5 * 128, {'esr_el1': 0}, 0x105),
            ('exit-stale-x2', s['a733_cached_smc'], {}, 0),
            ('el3-origin', s['a733_cached_el3_vectors'] + 4 * 128, {'esr_el3': 0x96000010}, None),
            ('failed-validation', s['a733_cached_smc'], {}, None),
            ('bad-mpidr', s['a733_cached_smc'], {'mpidr_el1': 0x81010000 | core << 8}, None),
        ]
        for kind, pc, extra, marker in kinds:
            uc = image.machine()
            epoch = prepare(image, uc, core, state=0 if kind == 'failed-validation' else 3)
            model = el3_model(core)
            model.update(extra)
            regs = {UC_ARM64_REG_SP: EL1_STACK, UC_ARM64_REG_X0: 5, UC_ARM64_REG_X0 + 1: 3,
                    UC_ARM64_REG_X0 + 2: 0x1ff, UC_ARM64_REG_X0 + 18: 0x77}
            if kind == 'el3-origin':
                regs[UC_ARM64_REG_X0] = 0x11
            r = run(image, uc, pc, regs, model, ['a733_smp_stop', 'a733_storage_maintenance_stop'])
            r.update(core=core, kind=kind, marker=marker, epoch=epoch, model=model,
                     slot=bytes(uc.mem_read(SLOTS + core * 128, 128)),
                     record=bytes(uc.mem_read(s['a733_cached_records'] + core * 128, 128)))
            out.append(r)
    return out


def check_case(image, r):
    s = image.symbols
    core, kind = r['core'], r['kind']
    pending = s['a733_fault_pending']
    el3 = [w for w in r['writes'] if w[4] == 3]
    assert el3, (core, kind, 'no EL3 write')
    index = 0 if kind == 'bad-mpidr' else core
    first = el3[0]
    assert first[1:4] == (pending + index, 1, 1), (core, kind, first)
    # Instructions from the MPIDR decode (its ubfx) to the pending store.
    ubfx = [i for i, a in enumerate(r['trace']) if image.decoded[a].mnemonic == 'ubfx'
            and image.decoded[a].op_str.startswith('x19, x0')]
    decode_to_store = first[0] - ubfx[-1] if ubfx else None
    assert decode_to_store is not None and decode_to_store <= 6, (core, kind, decode_to_store)
    barrier_after = [e for e in r['events'] if e[0] > first[0]]
    assert barrier_after and barrier_after[0][1] == 'dsb sy', (core, kind, barrier_after[:1])
    slot_range = (SLOTS + core * 128, SLOTS + core * 128 + 128)
    record_range = (s['a733_cached_records'] + core * 128, s['a733_cached_records'] + core * 128 + 128)
    captured = kind.startswith('vector') or kind.startswith('exit')
    for w in el3[1:]:
        inside_slot = slot_range[0] <= w[1] < slot_range[1]
        inside_record = record_range[0] <= w[1] < record_range[1]
        assert (inside_slot and captured) or (inside_record and kind not in ('failed-validation', 'bad-mpidr')), \
            (core, kind, hex(w[1]))
        if inside_slot:
            assert w[2] == 8 and w[1] % 8 == 0, (core, kind, 'slot access', hex(w[1]), w[2])
    capture = [a for a in r['trace'] if s['a733_fault_capture'] <= a < s['a733_fault_capture'] +
               image.sizes['a733_fault_capture']]
    publication = [w[0] for w in el3 if w[1] == record_range[0] + 16 and w[3] in (4, 6)]
    entry = next(i for i, a in enumerate(r['trace']) if a == s['a733_cached_el3_vectors'] + 8 * 128 or
                 a == s['a733_cached_el3_vectors'] + 4 * 128)
    slot = decode.decode_slot(r['slot'])
    row = dict(core=core, kind=kind, stop=r['stop'], decode_to_pending_store=decode_to_store,
               capture_instructions=len(capture),
               entry_to_publication=publication[0] - entry if publication else None,
               el3_writes=len(el3), slot_state=slot['state'])
    if captured:
        assert len(capture) <= 250 and slot['state'] == 'valid', (core, kind, len(capture), slot['state'])
        assert slot['core'] == core and slot['epoch'] == r['epoch'], slot
        model = r['model']
        if kind.startswith('vector'):
            assert slot['kind'] == 'vector fault' and slot['vector_slot'] == r['marker'] & 0xff, (kind, slot)
            assert slot['status'] == 4 and slot['detail'] == model['esr_el1'], slot
            assert (slot['esr'], slot['elr'], slot['far'], slot['spsr']) == \
                (model['esr_el1'], model['elr_el1'], model['far_el1'], model['spsr_el1']), slot
            assert slot['tcb'] == TCB and slot['task'] == 'mimus-board', slot
        else:
            # A deliberate exit whose x2 still held 0x1ff is not a vector fault.
            assert slot['kind'] == 'deliberate exit' and slot['status'] == 5 and slot['detail'] == 3, slot
            assert not any((slot['esr'], slot['elr'], slot['far'], slot['spsr'], slot['tcb'], slot['task0'])), slot
        assert r['stop'] in ('park', 'a733_smp_stop', 'a733_storage_maintenance_stop'), r['stop']
        row['fields'] = sum(1 for k in ('info', 'status', 'detail', 'counter', 'epoch', 'esr', 'elr', 'far',
                                        'spsr', 'tcb', 'task0') if slot[k])
    else:
        assert not capture and slot['state'] == 'empty', (core, kind, slot['state'])
        assert r['stop'] == 'park', (core, kind, r['stop'])
    return row


def synchronous_entry(image, faults=True):
    """Fatal entry never accesses SP; returning SVC preserves its input state."""
    rows = []
    for core in range(8):
        for vector, mode in ((4, 5), (8, 0)):
            for stack in ((0x40000000, EL1_STACK + 3) if faults else ()):
                uc = image.machine()
                epoch = prepare(image, uc, core)
                model = el3_model(core)
                model.update(spsr_el1=mode, esr_el1=0x96000045 if mode else 0x92000045)
                r = run(image, uc, image.symbols['a733_cached_el1_vectors'] + vector * 128,
                        {UC_ARM64_REG_SP: stack}, model,
                        ['a733_smp_stop', 'a733_storage_maintenance_stop'])
                assert not [w for w in r['writes'] if w[4] == 1], ('EL1 store', core, vector, stack)
                assert not [v for v in r['reads'] if v[3] == 1], ('EL1 load', core, vector, stack)
                slot = decode.decode_slot(bytes(uc.mem_read(SLOTS + core * 128, 128)))
                assert slot['state'] == 'valid' and slot['kind'] == 'vector fault', slot
                assert slot['epoch'] == epoch and slot['core'] == core and slot['vector_slot'] == vector, slot
                assert (slot['esr'], slot['elr'], slot['far'], slot['spsr']) == tuple(
                    model[n] for n in ('esr_el1', 'elr_el1', 'far_el1', 'spsr_el1')), slot
                rows.append(dict(core=core, vector=vector, stack=stack, kind='fault',
                                 el1_memory_accesses=0, instructions_to_smc=r['smc_at'] + 1))
            calls = [(0, 'FreeRTOS_SWI_Handler'), (119, 'a733_smp_exit_privileged')]
            if 'a733_observe_exception' in image.symbols:
                calls.append((120, 'a733_observe_exception'))
            for immediate, stop in calls:
                uc = image.machine()
                model = el3_model(core)
                model.update(esr_el1=(0x15 << 26) | immediate, spsr_el1=mode)
                registers = {reg: 0x1020304050600000 + n for n, reg in enumerate(REGS)}
                registers[UC_ARM64_REG_SP] = EL1_STACK
                r = run(image, uc, image.symbols['a733_cached_el1_vectors'] + vector * 128,
                        registers, model, [stop])
                assert r['stop'] == stop, r['stop']
                for reg, value in registers.items():
                    assert uc.reg_read(reg) == value, ('SVC register', core, vector, immediate, reg)
                assert model['tpidr_el1'] == core, ('SVC core index', core, model['tpidr_el1'])
                assert len(r['writes']) == 4 and len(r['reads']) == 4, ('SVC stack cost', r)
                rows.append(dict(core=core, vector=vector, kind='svc', immediate=immediate,
                                 instructions=len(r['trace']), loads=len(r['reads']), stores=len(r['writes'])))
    return rows


def name_bounds(image):
    """A TCB outside image RAM or misaligned leaves the name empty."""
    rows = []
    for tcb, expect in ((0x41f80000, 'kept'), (0x41f80004, 'misaligned'), (0x40000000, 'outside'),
                        (0x41fffff0, 'outside'), (0, 'null')):
        uc = image.machine()
        prepare(image, uc, 2)
        uc.mem_write(image.symbols['pxCurrentTCBs'] + 16, struct.pack('<Q', tcb))
        model = el3_model(2)
        run(image, uc, image.symbols['a733_cached_el1_vectors'] + 4 * 128, {UC_ARM64_REG_SP: EL1_STACK},
            model, ['a733_smp_stop', 'a733_storage_maintenance_stop'])
        slot = decode.decode_slot(bytes(uc.mem_read(SLOTS + 2 * 128, 128)))
        assert slot['state'] == 'valid' and slot['tcb'] == tcb, slot
        assert (slot['task'] == 'mimus-board') == (expect == 'kept'), (hex(tcb), slot['task'])
        rows.append(dict(tcb=tcb, name=slot['task']))
    return rows


def rotation(image, captured):
    """a733_fault_boot over valid, incomplete, corrupt and empty slots."""
    s = image.symbols
    rows = []
    for copied in (True, False):
        uc = image.machine()
        result = [RESIDENT_RESULT_MAGIC, 0xe90c, 7, 5, 5, 3, 100, 0, 200] + [0] * 7
        report = [0x4153373353544732 if copied else 0, 1, 512, 6] + [0] * 20 + result
        uc.mem_write(STAGE2, struct.pack('<40Q', *report))
        slots = bytearray(1024)
        slots[1 * 128:2 * 128] = captured[1]
        slots[6 * 128:7 * 128] = captured[6]
        slots[2 * 128:3 * 128] = struct.pack('<4Q', decode.SLOT_OPEN, 0, 0x50001, 4).ljust(128, b'\0')
        slots[3 * 128:4 * 128] = hashlib.sha512(b'power-on').digest() * 2
        bad = bytearray(captured[4])
        bad[40] ^= 1
        slots[4 * 128:5 * 128] = bad
        uc.mem_write(SLOTS, bytes(slots))
        r = run(image, uc, s['a733_fault_boot'], {UC_ARM64_REG_LR: SENTINEL,
                UC_ARM64_REG_SP: s['__a733_smp_boot_stack_top']}, {}, [], limit=100000)
        assert r['stop'] == 'return', r['stop']
        previous = s['a733_fault_previous']
        stack = (s['__a733_smp_boot_stack_top'] - 4096, s['__a733_smp_boot_stack_top'])

        def region(address):
            if STAGE2 <= address < STAGE2 + 512: return 'stage2'
            if SLOTS <= address < SLOTS + 1024: return 'slots'
            if previous <= address < previous + 1160: return 'previous'
            if stack[0] <= address < stack[1]: return 'stack'
            return 'other'
        totals = {}
        for kind, log in (('read', r['reads']), ('write', r['writes'])):
            for access in log:
                address, size = access[1], access[2]
                where = region(address)
                if where != 'stack':
                    assert size == 8 and address % 8 == 0, ('Device access', kind, hex(address), size)
                assert where != 'other' and not (kind == 'write' and where == 'stage2'), (kind, hex(address))
                totals[f'{kind}_{where}'] = totals.get(f'{kind}_{where}', 0) + size
        assert bytes(uc.mem_read(SLOTS, 1024)) == bytes(1024), 'slots not cleared'
        data = bytes(uc.mem_read(previous, 1160))
        report_out = decode.decode_report(data)
        assert report_out['valid_mask'] == 0b01000010 and report_out['incomplete_mask'] == 0b00000100, report_out
        assert report_out['result_copied'] == copied
        assert data[8:136] == (struct.pack('<16Q', *result) if copied else bytes(128))
        assert data[136 + 1 * 128:136 + 2 * 128] == captured[1] and data[136 + 3 * 128:136 + 4 * 128] == bytes(128)
        assert data[136 + 4 * 128:136 + 5 * 128] == bytes(128), 'corrupt slot copied'
        assert totals.get('write_previous') == 1160 and totals.get('write_slots') == 1024, totals
        rows.append(dict(result_copied=copied, instructions=len(r['trace']), **totals))
    return rows


def supervisor(image, baseline):
    """The renewal pass adds one plain 64-bit load of a733_fault_pending and no barrier."""
    out = {}
    for label, img in (('changed', image), ('baseline', baseline)):
        if img is None or 'board_resident_renew' not in img.symbols:
            continue
        start, size = img.symbols['board_resident_renew'], img.sizes['board_resident_renew']
        code = next(seg for seg in img.segments if seg[0] <= start < seg[0] + len(seg[1]))
        body = code[1][start - code[0]:start - code[0] + size]
        insns = list(img.decoder.disasm(body, start))
        pages, loads = {}, []
        for i in insns:
            ops = i.op_str.split(', ')
            if i.mnemonic == 'adrp':
                pages[ops[0]] = int(ops[1][1:], 0)
            elif i.mnemonic.startswith('ld') and '[' in i.op_str:
                m = re.search(r'\[(x\d+)(?:, #(-?0x[0-9a-f]+|-?\d+))?\]', i.op_str)
                if m and m.group(1) in pages:
                    loads.append((i.mnemonic, pages[m.group(1)] + int(m.group(2) or '0', 0)))
        barriers = [i.mnemonic for i in insns if i.mnemonic in ('dmb', 'dsb', 'isb') or
                    i.mnemonic.startswith(('ldar', 'ldax', 'stlr', 'stlx', 'cas', 'swp'))]
        out[label] = dict(instructions=len(insns), barriers=barriers,
                          pending_loads=[m for m, a in loads if a == img.symbols.get('a733_fault_pending')])
    if 'changed' in out:
        assert out['changed']['pending_loads'] == ['ldr'] and not out['changed']['barriers'], out['changed']
    else:
        out['changed'] = 'supervised lifetime: no renewal'
    return out


def line_format(work):
    """The C formatter and the decoder produce the same line within 160 bytes."""
    source = work / 'line.c'
    source.write_text('#include <stdio.h>\n#include "a733_fault.h"\nint main(void){\n'
                      ' unsigned long long w[16];char line[A733_FAULT_LINE_BYTES];\n'
                      ' while(fread(w,8,16,stdin)==16){unsigned n=a733_fault_line((const void *)w,line);\n'
                      '  if(n>=A733_FAULT_LINE_BYTES||line[n])return 1;fputs(line,stdout);}\n return 0;}\n')
    binary = work / 'line'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(ROOT / 'include'), str(source), '-o', str(binary)], check=True)
    records = [
        [0, 0, 0x00ff01, 1, 0, 9, 0, 0x96000045, 0x41001234, 0xdead, 0x3c5, 0x41300000,
         int.from_bytes(b'IDLE0\0\0\0', 'little'), 0, 0, 0],
        [0, 0, 0x07ff01, 1, 0, 9, 0, (1 << 64) - 1, (1 << 64) - 1, (1 << 64) - 1, (1 << 64) - 1, 0,
         int.from_bytes(b'ABCDEFGH', 'little'), int.from_bytes(b'IJKLMNOP', 'little'), 0, 0],
        [0, 0, 0x03ff01, 1, 0, 9, 0, 0, 0, 0, 0, 0, int.from_bytes(b'\x01\xffab\0zz\0', 'little'), 0, 0, 0],
    ]
    data = b''.join(struct.pack('<16Q', *r) for r in records)
    lines = subprocess.run([str(binary)], input=data, capture_output=True, check=True).stdout.decode()
    expected = ''.join(decode.fault_line(r) for r in records)
    assert lines == expected, (lines, expected)
    longest = max(len(line) + 2 for line in lines.splitlines())  # newline and NUL
    assert longest <= 160, longest
    return dict(lines=lines.splitlines(), longest_bytes=longest)


def sources():
    """Constants shared with the stage-2 loader and the single-core patch context."""
    loader = (ROOT / 'boot/a733/stage2/loader.c').read_text()
    header = (ROOT / 'include/a733_fault.h').read_text()
    magic = re.search(r'#define REPORT_MAGIC UINT64_C\((0x[0-9a-f]+)\)', loader).group(1)
    assert f'A733_FAULT_STAGE2_MAGIC UINT64_C({magic})' in header
    assert 'ENTERED = 6' in loader and 'A733_FAULT_STAGE2_ENTERED 6u' in header
    assert re.search(r'struct \{ uint64_t header, generation, load, extent; \} slot\[2\];\s+uint64_t reserved1\[40\];',
                     loader), 'stage-2 report layout changed'
    with tempfile.TemporaryDirectory() as work:
        subprocess.run(['patch', '-F0', '--silent', '--output', str(Path(work) / 'out.c'),
                        str(ROOT / 'platform/a733/platform.c'),
                        str(ROOT / 'apps/storage-read-single-core/platform.patch')], check=True)
    return dict(stage2_magic=magic, single_core_patch='applies with fuzz 0')


def baseline_path(image):
    """EL3 report from entry to publication for a deliberate exit."""
    uc = image.machine()
    records = image.symbols['a733_cached_records']
    uc.mem_write(records + 3 * 128, struct.pack('<3Q', 1, 3, 3))
    model = el3_model(3)
    r = run(image, uc, image.symbols['a733_cached_smc'], {UC_ARM64_REG_X0: 5, UC_ARM64_REG_X0 + 1: 3},
            model, ['a733_smp_stop', 'a733_storage_maintenance_stop'])
    entry = next(i for i, a in enumerate(r['trace']) if a == image.symbols['a733_cached_el3_vectors'] + 8 * 128)
    publication = [w[0] for w in r['writes'] if w[1] == records + 3 * 128 + 16 and w[3] in (4, 6)]
    fields = sorted({w[1] - records - 3 * 128 for w in r['writes'] if w[4] == 3 and records + 3 * 128 <= w[1]
                     < records + 4 * 128})
    return dict(entry_to_publication=publication[0] - entry, record_offsets_written=fields,
                pending_store='a733_fault_pending' in image.symbols)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image = Image(args.image)
    baseline = Image(args.baseline) if args.baseline else None
    for name in ('a733_fault_capture', 'a733_fault_boot', 'a733_fault_pending', 'a733_fault_previous'):
        assert name in image.symbols, name
    runs = cases(image)
    rows = [check_case(image, r) for r in runs]
    captured = {r['core']: r['slot'] for r in runs if r['kind'] == 'vector-el1h'}
    result = dict(
        passed=True, physical_access=False,
        image=str(args.image), image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),
        parks=len(rows), parks_marking_pending=len(rows),
        max_decode_to_pending_store=max(r['decode_to_pending_store'] for r in rows),
        max_capture_instructions=max(r['capture_instructions'] for r in rows),
        max_entry_to_publication=max(r['entry_to_publication'] for r in rows if r['entry_to_publication']),
        cases=rows, synchronous_entry=synchronous_entry(image), name_bounds=name_bounds(image),
        rotation=rotation(image, captured),
        supervisor=supervisor(image, baseline), line=None, sources=sources(),
        sizes={n: image.sizes.get(n) for n in ('a733_fault_capture', 'a733_fault_boot', 'a733_fault_previous',
                                              'a733_fault_pending', 'board_resident_renew')},
        limit='Modeled system registers, memory and SMC entry; no cache, timing or board behavior.')
    with tempfile.TemporaryDirectory() as work:
        result['line'] = line_format(Path(work))
    result['exit_path'] = baseline_path(image)
    if baseline:
        result['baseline'] = baseline_path(baseline)
        result['baseline']['sizes'] = {n: baseline.sizes.get(n) for n in ('board_resident_renew',)}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"{len(rows)} parks on 8 cores: every entry stores its pending byte at most "
          f"{result['max_decode_to_pending_store']} instructions after the MPIDR decode")
    print(f"capture: at most {result['max_capture_instructions']} instructions; entry to publication "
          f"{result['max_entry_to_publication']} (exit path {result['exit_path']['entry_to_publication']}"
          + (f", baseline {result['baseline']['entry_to_publication']})" if baseline else ')'))
    for row in result['rotation']:
        print('rotation:', {k: v for k, v in row.items()})
    print('supervisor:', result['supervisor'])
    print('PASS: fault capture, rotation and report decode')


if __name__ == '__main__':
    main()
