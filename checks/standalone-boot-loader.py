#!/usr/bin/env python3
"""Execute the stage-2 monitor against modeled card images.

The actual boot0 parser and cleanup produce the inherited header. The linked
AArch64 monitor then runs from 0x47000000: cold-entry gate, stage-2 loader,
cold SD admission and card detection, SD core, SHA-256 and the ROM-return
request. System registers, the counter, the SD controller and card, and the
SMHC0 clock and PF bank registers that boot0 leaves are modeled.
Card images come from tools/boot-slots.py.
Remote and CPU/cache ownership are modeled as granted for card loading; reset policy
is modeled as refused to stop before OS entry. The actual admission predicates
and early ownership refusals are checked by remote-writers-model.py --stage2-build.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as a64

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


slots = module('boot_slots', 'tools/boot-slots.py')
cold = module('cold_entry_check', 'checks/cold-entry.py')

SDC0, RVBAR = 0x04020000, 0x08001000
CCU, PIO = 0x02002000, 0x02000000
SMHC0_CLK, SMHC0_BGR = CCU + 0xd00, CCU + 0xd0c
POW_MOD_SEL, POW_VAL, POW_VAL_SET_CTL = PIO + 0x40, PIO + 0x48, PIO + 0x70
PF_CFG0, PF_DAT, PF_PUL0 = PIO + 0x300, PIO + 0x310, PIO + 0x330
# The state boot0 leaves after reading the monitor package: SMHC0 bus open and
# its module clock running at a transfer rate (PERI0_400M / 4 here; the exact
# rate is not modeled), PF0..PF5 SDC0 function 2 with pull-up from the boot0
# header, PF6 at its IO-disabled reset value, and the PF bank at 3.3 V.
BOOT0_STATE = {SMHC0_CLK: 0x81000003, SMHC0_BGR: 0x00010001, POW_MOD_SEL: 0,
               POW_VAL: 0x01155950, POW_VAL_SET_CTL: 1, PF_CFG0: 0x0f222222, PF_PUL0: 0x555}
# Admission's writes: PF6 pull-up, PF6 input, then the module clock gated,
# moved to SYS_CLK24M / (2 * 30) and enabled.
SELECTED = [(PF_PUL0, 0x1555), (PF_CFG0, 0x00222222), (SMHC0_CLK, 0x01000003),
            (SMHC0_CLK, 0x0000011d), (SMHC0_CLK, 0x8000011d)]
SETTLE_TICKS = 24000
RAM, RAM_END = 0x41000000, 0x47400000
REPORT_MAGIC = int.from_bytes(b'AS73STG2', 'big')
COLD_MAGIC = 0x41533733434f4c44
SUNXI_MMC_ADMISSION = 4
HEADER_STATES = {0: 'unread', 1: 'valid', 2: 'read', 3: 'absent', 4: 'digest', 5: 'fields'}
LOAD_STATES = {0: 'none', 1: 'done', 2: 'read', 3: 'digest'}
CARD_SECTORS = 131072  # 64 MiB; the modeled CSD encodes (127 + 1) * 1024 sectors.


def field(words, first, count, value):
    for bit in range(count):
        position = first + bit
        word, shift = 3 - position // 32, position % 32
        words[word] = words[word] & ~(1 << shift) | ((value >> bit) & 1) << shift


class Card:
    """SD controller and SDHC card, following checks/sunxi-mmc.c for the legacy profile."""

    def __init__(self, image, fail_sector=None):
        assert len(image) == CARD_SECTORS * 512
        self.image, self.fail_sector, self.writes = image, fail_sector, 0
        self.reg = [0] * (0x304 // 4)
        for offset, value in ((0x300, 0x03050530), (0x5c, 0x81710331), (0x10c, 0x80001234),
                              (0x54, 0x12345676), (0x58, 0xffff), (0x104, 7), (0x140, 0xa5a40000)):
            self.reg[offset // 4] = value
        self.csd, self.cid = [0] * 4, [0x12345678 + i for i in range(4)]
        field(self.csd, 126, 2, 1)
        field(self.csd, 80, 4, 9)
        field(self.csd, 84, 12, 5)
        field(self.csd, 48, 22, CARD_SECTORS // 1024 - 1)
        self.state, self.app, self.ocr_calls, self.selected = 0, False, 0, False
        self.data, self.position, self.data_command = b'', 0, 0
        self.commands, self.reads, self.accesses = [], [], 0

    def read(self, uc, offset, size, user):
        assert size == 4 and offset <= 0x300 and not offset & 3, hex(offset)
        self.accesses += 1
        drained = self.position == len(self.data)
        if offset == 0x38 and self.data_command and drained:
            self.reg[0x38 // 4] |= 8 | ((1 << 14) if self.data_command & (1 << 12) else 0)
        if offset == 0x3c:
            return 4 if drained else 0
        if offset == 0x200:
            assert not drained
            value = int.from_bytes(self.data[self.position:self.position + 4], 'little')
            self.position += 4
            return value
        return self.reg[offset // 4]

    def write(self, uc, offset, size, value, user):
        assert size == 4 and offset <= 0x300 and not offset & 3 and offset != 0x200, hex(offset)
        self.accesses += 1
        self.writes += 1
        if offset == 0x38:
            assert not value & 0xffff0000
            self.reg[0x38 // 4] &= ~value
        elif offset == 0:
            self.reg[0] = value & ~7
            if value & 7:
                self.data, self.position = b'', 0
        else:
            self.reg[offset // 4] = value
        if offset == 0x18:
            self.command(value)

    def command(self, value):
        if value & (1 << 21):
            self.reg[0x18 // 4] = 0
            return
        assert not value & (1 << 10), 'write-direction command'
        number, argument = value & 63, self.reg[0x1c // 4]
        self.commands.append(number)
        self.reg[0x18 // 4] = value & ~(1 << 31)
        self.reg[0x38 // 4] = 4
        self.reg[0x20 // 4] = self.state << 9 | 0x100
        self.data, self.position, self.data_command = b'', 0, 0
        if number == 0:
            assert value & (1 << 15) and not argument
            self.state = 0
        elif number == 8:
            assert self.state == 0 and argument == 0x1aa
            self.reg[0x20 // 4] = 0x1aa
        elif number == 55:
            assert self.state == 0 and not argument
            self.app = True
            self.reg[0x20 // 4] |= 1 << 5
        elif number == 41:
            assert self.app and argument == 0x40300000
            self.app, self.ocr_calls = False, self.ocr_calls + 1
            self.reg[0x20 // 4] = 0x00300000 | 1 << 30 | ((1 << 31) if self.ocr_calls >= 2 else 0)
            self.state = 1 if self.ocr_calls >= 2 else 0
        elif number in (2, 9):
            assert self.state == (1 if number == 2 else 3) and argument == (0 if number == 2 else 0x12340000)
            for i, word in enumerate(self.cid if number == 2 else self.csd):
                self.reg[(0x2c - 4 * i) // 4] = word
            self.state = 2 if number == 2 else 3
        elif number == 3:
            assert self.state == 2
            self.state, self.reg[0x20 // 4] = 3, 0x12340700
        elif number == 7:
            assert self.state == 3 and argument == 0x12340000
            self.state, self.selected = 4, True
        elif number == 13:
            assert self.selected and argument == 0x12340000
            self.reg[0x20 // 4] = 0x900
        elif number in (17, 18):
            total = self.reg[0x14 // 4]
            assert self.selected and self.reg[0x10 // 4] == 512 and value & (1 << 9)
            assert 512 <= total <= 4096 and not total % 512 and (number == 18) == bool(value & (1 << 12))
            assert argument + total // 512 <= CARD_SECTORS
            self.reads.append((argument, total // 512))
            self.data = bytes(self.image[argument * 512:argument * 512 + total])
            self.data_command = value
            self.reg[0x20 // 4] = self.reg[0x24 // 4] = 0x900
            if argument == self.fail_sector:
                self.reg[0x38 // 4] |= 1 << 7  # Response CRC error.
        else:
            raise AssertionError('unsupported card command %d' % number)


class Board:
    """SMHC0 clock and bus registers and the PF bank, starting from `changes` over BOOT0_STATE.

    PF6 reads low while the card is present. After the card model has served
    `removed_after_reads` reads the card is gone; after `changed_after_reads`
    reads, as (count, address, value), another writer changes a register."""

    def __init__(self, card, counter, changes=None, stuck=(), present=True,
                 removed_after_reads=None, changed_after_reads=None):
        self.values = {**BOOT0_STATE, **(changes or {})}
        self.card, self.counter, self.stuck, self.present = card, counter, stuck, present
        self.removed_after_reads, self.changed_after_reads = removed_after_reads, changed_after_reads
        self.writes, self.detect_reads = [], []

    def page(self, base):
        def read(uc, offset, size, user):
            address = base + offset
            assert size == 4 and (address in self.values or address == PF_DAT), hex(address)
            reads = len(self.card.reads)
            if self.changed_after_reads and reads >= self.changed_after_reads[0]:
                self.values[self.changed_after_reads[1]] = self.changed_after_reads[2]
            if address == PF_DAT:
                self.detect_reads.append(self.counter())
                gone = self.removed_after_reads is not None and reads >= self.removed_after_reads
                return 0 if self.present and not gone else 1 << 6
            return self.values[address]

        def write(uc, offset, size, value, user):
            address = base + offset
            assert size == 4 and address in (SMHC0_CLK, PF_CFG0, PF_PUL0), hex(address)
            self.writes.append((address, value, self.counter()))
            if address not in self.stuck:
                self.values[address] = value
        return read, write


def symbols(path):
    """Return uniquely defined symbols; repeated local names are left out."""
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        found = {}
        for symbol in elf.get_section_by_name('.symtab').iter_symbols():
            if symbol.name and not symbol.name.startswith('$'):
                found.setdefault(symbol.name, set()).add(symbol['st_value'])
        table = {name: values.pop() for name, values in found.items() if len(values) == 1}
        assert elf['e_entry'] == table['a733_cold_image'] == 0x47000000
    return table


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='stage-2 CMake build directory')
    parser.add_argument('--boot0', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    raw = (args.build / 'a733-stage2.bin').read_bytes()
    table = symbols(args.build / 'a733-stage2.elf')
    assert table['a733_cold_report'] == 0x47008000
    assert table['a733_stage2_report'] == table['a733_stage2_bss_start'] == 0x47008200
    assert len(raw) <= 0x8000 and table['a733_stage2_stack_top'] <= 0x47400000
    inherited, predecessor = cold.inherited_payload(args.boot0.read_bytes(), raw, args.output)
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    system = {}
    for offset in range(0, len(raw) - 3, 4):
        instruction = next(disassembler.disasm(raw[offset:offset + 4], 0x47000000 + offset), None)
        if instruction and instruction.mnemonic in ('mrs', 'msr', 'wfi', 'wfe'):
            system[instruction.address] = (instruction.mnemonic, instruction.op_str.lower())

    # Card images are produced by the actual host tool on sparse ordinary files.
    work = args.output / 'cards'
    work.mkdir()

    def tool(argv):
        with (work / 'boot-slots.log').open('a') as log, contextlib.redirect_stdout(log):
            slots.main(argv)

    empty = work / 'empty.img'
    with empty.open('xb') as stream:
        stream.truncate(CARD_SECTORS * 512)
    allocation = work / 'allocation.json'
    tool(['allocate', '--backup', str(empty), '--slot-a', '40960:8192',
                '--slot-b', '49152:8192', '--output', str(allocation)])
    payloads = {}
    for name, size, seed in (('image1', 6000, 1), ('asset1', 3000, 2), ('image2', 7000, 3), ('asset2', 2500, 4)):
        data = bytes((i * 131 + seed * 17 + i // 251) & 255 for i in range(size))
        payloads[name] = data
        (work / name).write_bytes(data)

    def copy(source, name):
        target = work / name
        shutil.copyfile(source, target)
        return target

    def plan(source, number, name):
        tool(['plan', '--allocation', str(allocation), '--card-image', str(source),
                    '--image', '%s@0x41000000' % (work / ('image%d' % number)),
                    '--entry', '0x41000000', '--asset', '%s@0x44000000' % (work / ('asset%d' % number)),
                    '--output', str(work / name)])
        return work / name / 'plan.json'

    first = copy(empty, 'card-a.img')
    tool(['apply', '--plan', str(plan(empty, 1, 'plan-a')), '--card-image', str(first)])
    both = copy(first, 'card-ab.img')
    plan_b = plan(first, 2, 'plan-b')
    tool(['apply', '--plan', str(plan_b), '--card-image', str(both)])
    interrupted = copy(first, 'card-a-interrupted.img')
    tool(['apply', '--plan', str(plan_b), '--card-image', str(interrupted), '--stop-after', 'payload'])
    headers = {'A': json.loads((work / 'plan-a' / 'plan.json').read_text()),
               'B': json.loads(plan_b.read_text())}
    extent = lambda s, i: headers[s]['extents'][i]

    def image(path, *flips):
        data = bytearray(path.read_bytes())
        for sector, offset in flips:
            data[sector * 512 + offset] ^= 0x40
        return data

    windows = [(e['load'], e['load'] + e['bytes']) for s in 'AB' for e in headers[s]['extents']]
    cases = []

    def run(name, card_bytes, expected, fail_sector=None, registers=None, vector=False,
            counter_step=2400, expect_writes='all', sdc0=None, **board_state):
        card = Card(card_bytes, fail_sector)
        for offset, value in (sdc0 or {}).items():
            card.reg[offset // 4] = value
        board = Board(card, lambda: values['cntpct_el0'], **board_state)
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.mem_map(RAM, RAM_END - RAM)
        cpu.mem_map(RVBAR, 0x1000)
        cpu.mmio_map(SDC0, 0x1000, card.read, None, card.write, None)
        for base in (PIO, CCU):
            read, write = board.page(base)
            cpu.mmio_map(base, 0x1000, read, None, write, None)
        cpu.mem_write(0x47000000, inherited)
        cpu.mem_write(0x47008000, b'\xa5' * (table['a733_stage2_stack_top'] - 0x47008000))
        values = {'currentel': 12, 'mpidr_el1': 0x81000000, 'midr_el1': 0x412fd050,
                  'sctlr_el3': 0x30c50832, 'scr_el3': 0x30, 'cntfrq_el0': 0,
                  'cntpct_el0': 24000123, 'rvbar_el3': 0x47000000,
                  'esr_el3': 0x96000010, 'far_el3': 0xdead0000,
                  'elr_el3': table['a733_stage2_main'], 'spsr_el3': 0x3cd, **(registers or {})}
        controls, stops, foreign, report_writes, entered, unaligned = [], [], [], [], [], []
        cold_at_entry = []

        def instruction(u, address, size, context):
            mnemonic, operands = system[address]
            if mnemonic in ('wfi', 'wfe'):
                stops.append(mnemonic)
                u.emu_stop()
                return
            first_operand, second_operand = operands.split(', ')
            if mnemonic == 'mrs':
                value = values[second_operand]
                if second_operand == 'cntpct_el0':
                    values['cntpct_el0'] += counter_step  # 100 us per sample by default.
                u.reg_write(getattr(a64, 'UC_ARM64_REG_' + first_operand.upper()), value)
            else:
                assert first_operand in ('daifset', 'vbar_el3', 'rmr_el3'), first_operand
                controls.append([first_operand, second_operand if second_operand.startswith('#') else
                                 u.reg_read(getattr(a64, 'UC_ARM64_REG_' + second_operand.upper()))])
            u.reg_write(a64.UC_ARM64_REG_PC, address + 4)

        # The cold report is final once the loader starts; keep its bytes.
        def stage2(u, address, size, context):
            cold_at_entry.append(bytes(u.mem_read(0x47008000, 512)))

        def loaded(u, address, size, context):
            entered.append(address)
            u.emu_stop()

        def prerequisite(u, address, size, context):
            u.reg_write(a64.UC_ARM64_REG_X0, context)
            u.reg_write(a64.UC_ARM64_REG_PC, u.reg_read(a64.UC_ARM64_REG_LR))

        # With the MMU off every data access is Device memory, which faults
        # when unaligned; the emulator does not enforce that by itself.
        def aligned(u, access, address, size, value, context):
            if address % min(size, 8):
                unaligned.append([hex(address), size])

        def write(u, access, address, size, value, context):
            aligned(u, access, address, size, value, context)
            end = address + size
            storage = 0x47008200 if cold_at_entry else 0x47008000
            if 0x47008200 <= address < 0x47008400:
                report_writes.append(address)
            elif not (storage <= address and end <= table['a733_stage2_stack_top']
                      or 0x08001004 <= address and end <= 0x0800100c
                      or SDC0 <= address and end <= SDC0 + 0x1000
                      or address in (SMHC0_CLK, PF_CFG0, PF_PUL0) and size == 4
                      or any(lo <= address and end <= hi for lo, hi in windows)):
                foreign.append(hex(address))

        for address in system:
            cpu.hook_add(UC_HOOK_CODE, instruction, begin=address, end=address)
        cpu.hook_add(UC_HOOK_CODE, stage2, begin=table['a733_stage2_main'], end=table['a733_stage2_main'])
        for symbol, granted in (('a733_stage2_remote_writers_excluded', 1), ('a733_stage2_cpu_cache_owned', 1),
                                ('a733_stage2_reset_policy', 0)):
            cpu.hook_add(UC_HOOK_CODE, prerequisite, user_data=granted, begin=table[symbol], end=table[symbol])
        cpu.hook_add(UC_HOOK_CODE, loaded, begin=RAM, end=0x46ffffff)
        cpu.hook_add(UC_HOOK_MEM_WRITE, write)
        cpu.hook_add(UC_HOOK_MEM_READ, aligned)
        start = table['a733_cold_vectors'] + 512 if vector else 0x47000000
        cpu.emu_start(start, 0, count=60000000)
        assert stops and not entered and not foreign and not unaligned, (
            name, stops, entered, foreign[:4], unaligned[:4])
        assert bytes(cpu.mem_read(0x47000000, len(inherited))) == inherited
        assert not any(c[0] == 'sctlr_el3' for c in controls)
        cold_report = bytes(cpu.mem_read(0x47008000, 512))
        report = bytes(cpu.mem_read(0x47008200, 512))
        # Admission writes only PF6 and the SMHC0 module clock, in order, and
        # samples card detection only after the PF6 pull-up has settled.
        if isinstance(expect_writes, list):
            assert [w[:2] for w in board.writes] == expect_writes and board.detect_reads, (name, board.writes)
        elif expect_writes:
            assert [w[:2] for w in board.writes] == SELECTED[:len(board.writes)], (name, board.writes)
            if expect_writes == 'all':
                assert len(board.writes) == len(SELECTED) and board.detect_reads, (name, board.writes)
                assert board.detect_reads[0] - board.writes[1][2] >= SETTLE_TICKS, name
        else:
            assert not board.writes and not board.detect_reads, (name, board.writes)
        result = {'case': name, 'stops': stops, 'controls': controls, 'card_accesses': card.accesses,
                  'card_writes': card.writes, 'card_reads': len(card.reads),
                  'board_writes': [[hex(a), hex(v)] for a, v, _ in board.writes],
                  'detect_samples': len(board.detect_reads),
                  'counter_samples': (values['cntpct_el0'] - 24000123) // counter_step if counter_step else None}
        if expected is None:  # Refused before stage-2 storage or the loader.
            status = struct.unpack_from('<Q', cold_report, 24)[0]
            assert stops == ['wfe'] and card.accesses == 0
            assert report == b'\xa5' * 512 and not any(c[0] == 'rmr_el3' for c in controls)
            result['cold_status'] = status if cold_report[:8] != b'\xa5' * 8 else None
            cases.append(result)
            return result
        assert struct.unpack_from('<4Q', cold_report) == (COLD_MAGIC, 1, 512, 1)
        assert cold_at_entry == [cold_report], name
        fields = struct.unpack_from('<16Q', report)
        per_slot = [struct.unpack_from('<4Q', report, 128 + 32 * s) for s in range(2)]
        status, detail, selected, generation = fields[3], fields[4], fields[5], fields[6]
        assert fields[:3] == (REPORT_MAGIC, 1, 512) and report_writes[-1] == 0x47008200
        assert not any(report[192:]) and not any(report[112:128])
        assert controls[-1] == ['rmr_el3', 2] and stops == ['wfi']
        assert struct.unpack('<2I', cpu.mem_read(0x08001004, 8)) == (0x20, 0)
        assert fields[8] < fields[9] if counter_step else fields[8] == fields[9]
        result.update({'status': status, 'detail': detail, 'mmc_error': fields[10],
                       'selected': 'AB'[selected] if selected < 2 else None, 'generation': generation,
                       'slots': {s: {'header': HEADER_STATES.get(r[0], r[0]), 'generation': r[1],
                                     'load': LOAD_STATES[r[2]], 'extent': r[3]}
                                 for s, r in zip('AB', per_slot)},
                       'report_sha256': hashlib.sha256(report).hexdigest()})
        assert (status, result['selected']) == expected[:2], (name, result)
        if status == 1:
            assert fields[7] == CARD_SECTORS and detail == 0x41000000
            for e in headers[result['selected']]['extents']:
                assert bytes(cpu.mem_read(e['load'], e['bytes'])) == payloads[e['name']], (name, e)
        if expected[2]:
            # The host tool's selection must agree with the loader on the same bytes.
            path = args.output / (name + '.img')
            path.write_bytes(card_bytes)
            fd = os.open(path, os.O_RDONLY)
            try:
                host = slots.public(slots.inspect(fd, CARD_SECTORS))
            finally:
                os.close(fd)
            path.unlink()
            assert host['selected'] == result['selected'], (name, host)
            for s in 'AB':
                assert {k: host['slots'][s][k] for k in ('header', 'generation', 'load', 'extent')} \
                    == result['slots'][s], (name, s, host['slots'][s], result['slots'][s])
            result['host_tool_agrees'] = True
        cases.append(result)
        return result

    header_b = slots.HEADER_SECTORS['B']
    run('slot-a-only', image(first), (1, 'A', True))
    run('slot-b-newer', image(both), (1, 'B', True))
    run('slot-b-payload-digest', image(both, (extent('B', 1)['first_sector'], 7)), (1, 'A', True))
    run('slot-b-header-digest', image(both, (header_b, 16)), (1, 'A', True))
    run('slot-b-interrupted-before-header', image(interrupted), (1, 'A', True))
    run('no-valid-slot', image(first, (slots.HEADER_SECTORS['A'], 16)), (3, None, True))
    run('both-payloads-digest', image(both, (extent('A', 0)['first_sector'], 3),
                                      (extent('B', 0)['first_sector'], 3)), (4, None, True))
    forged = image(both)
    plan_data = headers['B']
    bad = [dict(e) for e in plan_data['extents']]
    bad[1]['load'] = 0x47000000 - 1024  # Asset would run into the monitor reservation.
    forged[header_b * 512:header_b * 512 + 512] = slots.pack_header('B', 2, 0x41000000, bad)
    run('slot-b-load-window', forged, (1, 'A', True))
    faulted = run('read-fault', image(both), (4, None, False),
                  fail_sector=extent('B', 0)['first_sector'])
    assert faulted['mmc_error'] == 6 and faulted['slots']['B']['load'] == 'read'
    # A PF6 left as an input with pull-up needs only the clock writes.
    run('pf6-already-input', image(both), (1, 'B', True), expect_writes=SELECTED[2:],
        changes={PF_CFG0: 0x00222222, PF_PUL0: 0x1555})
    # boot0 state that admission refuses before any write or SD command.
    for case, change in (('module-clock-gated', {SMHC0_CLK: 0x01000003}),
                         ('bus-reset-asserted', {SMHC0_BGR: 0x00000001}),
                         ('pf3-not-sdc0', {PF_CFG0: 0x0f223222}),
                         ('pf-pull-missing', {PF_PUL0: 0x551}),
                         ('pf6-output', {PF_CFG0: 0x01222222}),
                         ('pf-bank-1v8', {POW_VAL_SET_CTL: 0})):
        refused = run(case, image(both), (2, None, False), expect_writes=False, changes=change)
        assert refused['mmc_error'] == SUNXI_MMC_ADMISSION and refused['card_accesses'] == 0, case
    for case, registers in (('controller-command-pending', {0x18: 1 << 31}),
                            ('internal-dma-enabled', {0x80: 1 << 7})):
        refused = run(case, image(both), (2, None, False), expect_writes=False, sdc0=registers)
        assert refused['mmc_error'] == SUNXI_MMC_ADMISSION and not refused['card_writes'], case
    # No card: the writes happen, then card detection refuses.
    nocard = run('no-card', image(both), (2, None, False), present=False)
    assert nocard['mmc_error'] == SUNXI_MMC_ADMISSION and not nocard['card_writes']
    # A gated module clock that does not read back stops admission at that write.
    stuck = run('module-clock-not-read-back', image(both), (2, None, False), expect_writes=True,
                stuck=(SMHC0_CLK,))
    assert stuck['mmc_error'] == SUNXI_MMC_ADMISSION and len(stuck['board_writes']) == 3
    assert not stuck['card_writes']
    # Two header reads and the first slot B chunk, then card-detect reports no card.
    removed = run('card-removed', image(both), (4, None, False), removed_after_reads=3)
    assert removed['mmc_error'] == SUNXI_MMC_ADMISSION and removed['card_reads'] == 3
    assert removed['slots']['B']['load'] == 'read' and removed['slots']['A']['load'] == 'read'
    # Another writer restores a transfer clock after the same reads.
    changed = run('module-clock-changed', image(both), (4, None, False),
                  changed_after_reads=(3, SMHC0_CLK, 0x81000003))
    assert changed['mmc_error'] == SUNXI_MMC_ADMISSION and changed['card_reads'] == 3
    stopped = run('counter-stopped', image(both), (5, None, False), counter_step=0, expect_writes=False)
    assert stopped['card_accesses'] == 0
    run('wrong-el-parks', image(both), None, registers={'currentel': 4}, expect_writes=False)
    run('exception-parks', image(both), None, vector=True, expect_writes=False)
    assert cases[-1]['cold_status'] == 3 and cases[-2]['cold_status'] is None
    result = {'passed': True, 'physical_access': False, 'physical_boot_qualified': False,
              'monitor_sha256': hashlib.sha256(raw).hexdigest(),
              'inherited_monitor_sha256': hashlib.sha256(inherited).hexdigest(),
              'predecessor': predecessor, 'cases': cases,
              'limitations': [
                  'System registers, the 24 MHz counter, the SD controller and card, and the SMHC0 clock, bus and PF bank registers left by boot0 are modeled; the boot0 module clock value is representative, not measured.',
                  'The card supply is not modeled: admission relies on boot0 having read this card, which no model establishes.',
                  'No launch contract is built and the loaded image is never entered; remote-writer exclusion, memory ownership and the cold ROM return remain unqualified.',
                  'Card images are sparse ordinary files written by tools/boot-slots.py; no card or block device was opened.']}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS stage-2 monitor:', len(cases), 'instruction-model cases')


if __name__ == '__main__':
    main()
