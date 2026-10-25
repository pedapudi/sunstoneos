#!/usr/bin/env python3
"""Focused actual-ELF consumer checks; device transitions are explicit models.

With --power-only the image must be the power-only variant: every case also
requires that no load or store reaches the GPU register window. With
--wrapper-read the image must be the wrapper-read variant: its only
GPU-window accesses are 32-bit loads of the four wrapper words, in their
order, each after its progress record and log line, and there is no store
to the window. With --wrapper-write the image must be the wrapper-write
variant: its only GPU-window accesses are 32-bit accesses of the rows of its
access table in their order, each after its progress record and log line,
and each case's accesses are the rows its path allows."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP

EXPECTED = 36 << 48 | 56 << 32 | 104 << 16 | 183
# Words of the client's read-only prerequisite snapshot (snapshot_words in
# main.c) and the identity-mapped pages that hold them.
SNAPSHOT = [0x07066000 + o for o in (0, 4, 8, 0x10, 0x14, 0x18, 0x20, 0x24, 0x30, 0x34, 0x38, 0x3c, 0x160,
                                     0x164, 0x170, 0x174, 0xc00, 0xc04, 0xc10, 0xfb4)] + \
           [0x07065000 + o for o in (0, 8, 0x10, 0x14, 0x20, 0x170, 0x174, 0xc00, 0xc04, 0xc10)] + \
           [0x070101ac, 0x07010250, 0x020020e0, 0x02002b20, 0x02002b24]
SNAPSHOT_PAGES = (0x07066000, 0x07065000, 0x07010000, 0x02002000)
# The wrapper-read variant's four GPU wrapper words (wrapper_words in
# wrapper.c), their order, modeled values and its progress record.
WRAPPER = [0x01880104, 0x01880100, 0x01880018, 0x0188001c]
WRAPPER_VALUES = {0x01880104: 0x2, 0x01880100: 0x00200010, 0x01880018: 0x1, 0x0188001c: 0x10}
WRAPPER_PAGE = 0x01880000
WRAPPER_MAGIC = 0x3130505257555047
PROGRESS = 0x42000080
# The wrapper-write variant's access table (wrapper_steps in wrapper-write.c):
# address, value, write. Rows 0 and 1 read the entry words, rows 2 to 7 are
# the three stages' writes and readbacks, rows 8 to 11 the restore.
WRITE_TABLE = [(0x01880104, 0, 0), (0x01880100, 0, 0), (0x01880100, 0x00640064, 1), (0x01880100, 0, 0),
               (0x01880104, 0x100, 1), (0x01880104, 0, 0), (0x01880104, 0x107, 1), (0x01880104, 0, 0),
               (0x01880104, 0, 1), (0x01880104, 0, 0), (0x01880100, 0, 1), (0x01880100, 0, 0)]
WRITE_MAGIC = 0x3130575257555047
# The rows each wrapper-write case accesses, and how it ends.
WRITE_PATHS = {'ww-none': list(range(12)), 'ww-stage2': [0, 1, 2, 3, 4, 5, 8, 9, 10, 11], 'ww-on-first': [],
               'ww-entry-nonzero': [0, 1], 'ww-early-stop': [], 'ww-pending-early': [], 'ww-mapping': [],
               'ww-off-fault': list(range(12)), 'ww-abandon-refused': list(range(12)), 'ww-stage-fault': [0, 1, 2, 3],
               'ww-time-budget': [], 'ww-late': list(range(12)), 'ww-stage3': list(range(12)),
               'ww-readback': list(range(12))}
WRITE_REASONS = {'ww-none': 'GPU wrapper write: CORE6 not ON after three stages, OFF and provider parked',
                 'ww-stage2': None, 'ww-on-first': None,
                 'ww-entry-nonzero': 'GPU wrapper write refused: wrapper words not 0, OFF and provider parked',
                 'ww-early-stop': 'GPU ON poll', 'ww-pending-early': 'GPU wrapper write refused before the final CORE6 poll',
                 'ww-mapping': 'GPU wrapper mapping', 'ww-off-fault': 'GPU OFF poll', 'ww-abandon-refused': 'GPU OFF begin',
                 'ww-stage-fault': 'GPU ON poll', 'ww-time-budget': 'GPU wrapper write time budget',
                 'ww-late': None, 'ww-stage3': None,
                 'ww-readback': 'GPU wrapper write: CORE6 not ON after three stages, OFF and provider parked'}
# The CORE6 status, DISR and MISR words read after each stage's readback and after the restore.
CORE_WORDS = [0x07066008, 0x07066010, 0x07066014]
NAMES = ['bb_power', 'a733_gpu_platform', 'a733_gpu_power', 'a733_pmic_consumer',
         'a733_power_domains', 'a733_power_domain', 'a733_power_domain_row']


def last_put(state):
    """Index of the last platform_puts call: the completion text follows the park."""
    return len(state['calls']) - 1 - state['calls'][::-1].index('platform_puts')


def run(image, case, power_only=False, wrapper=False, writer=False):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0x41000000, 0x1200000)
    u.mem_map(0x1800000, 0x1000)
    u.mem_map(WRAPPER_PAGE, 0x1000)
    for address, value in WRAPPER_VALUES.items():
        u.mem_write(address, value.to_bytes(4, 'little'))
    if writer:
        # The board's entry words; the wrapper page holds what is written.
        u.mem_write(0x01880104, (2 if case == 'ww-entry-nonzero' else 0).to_bytes(4, 'little'))
        u.mem_write(0x01880100, bytes(4))
    for page in SNAPSHOT_PAGES:
        u.mem_map(page, 0x1000)
    for address in SNAPSHOT:
        u.mem_write(address, (address ^ 0x5a5a0000).to_bytes(4, 'little'))
    for address, data in image.parts:
        u.mem_write(address, data)
    if case == 'wrapper-table-mutant':
        # A seeded mutant of the image: the second table entry names another
        # wrapper word, which the check must refuse at that read.
        u.mem_write(image.address('wrapper_words') + 4, (0x01880108).to_bytes(4, 'little'))
    if case == 'ww-table-mutant':
        # A seeded mutant: stage 3 writes 0x10f, which the check must refuse.
        u.mem_write(image.address('wrapper_steps') + 6 * 12 + 4, (0x10f).to_bytes(4, 'little'))
    u.reg_write(UC_ARM64_REG_SP, 0x421f0000)
    L = image.layouts
    gpu, bb = image.address('gpu'), image.address('bb_power')
    power = gpu + L['a733_gpu_platform']['power']
    row = power + L['a733_gpu_power']['row']
    consumer = gpu + L['a733_gpu_platform']['pmic']
    domains = bb + L['bb_power']['domains']
    domain5 = domains + L['a733_power_domains']['domain'] + 2 * 32
    # The wrapper-read client keeps 200 ms for ON and 100 ms after it.
    deadline = 500000 if case == 'ww-time-budget' else 10**6 if wrapper or writer else 10000
    state = {'case': case, 'calls': [], 'gpu_reads': [], 'gpu_writes': [], 'clock_reads': 0, 'on_polls': 0,
             'snapshot_reads': [], 'translated': [], 'wrapper_reads': [], 'progress': [],
             'time': 100, 'messages': [], 'exit_status': None, 'failure': None, 'ww': [], 'ww_accesses': [],
             'events': [], 'rw_translated': []}

    def put(address, value, size=4):
        u.mem_write(address, value.to_bytes(size, 'little'))

    def field(base, typ, name, value, size=4):
        put(base + L[typ][name], value, size)

    def notes(idr0, misr, after_delays, after_pwcr, after_policy, misr_late, waits=(0, 0, 0)):
        base = power + L['a733_gpu_power']['core_note']
        for i, value in enumerate((idr0, misr, after_delays, after_pwcr, after_policy, misr_late)):
            put(base + 4 * i, value)
        for i, value in enumerate(waits):
            put(power + L['a733_gpu_power']['core_wait_us'] + 4 * i, value)

    def ret(value=1):
        u.reg_write(UC_ARM64_REG_X0, value)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    def text(address):
        value = bytearray()
        while (byte := u.mem_read(address + len(value), 1)[0]):
            value.append(byte)
        return value.decode()

    field(bb, 'bb_power', 'deadline', deadline, 8)
    field(row, 'a733_power_domain_row', 'domains', domains, 8)
    field(row, 'a733_power_domain_row', 'domain', 5)
    field(consumer, 'a733_pmic_consumer', 'rail', 32, 8)
    field(domain5, 'a733_power_domain', 'id', 5)
    raw = EXPECTED if case != 'unexpected-id' else EXPECTED + 1
    if case == 'legacy-id':
        raw = 36 << 16 | 56 << 8
        put(0x1800018, 104 << 8 | 183)
    put(0x1800020, raw, 8)

    def call(name):
        state['calls'].append(name)
        if name == 'aarch64_device_translate' and power_only and u.reg_read(UC_ARM64_REG_X0) not in SNAPSHOT_PAGES:
            raise AssertionError('power-only variant translated a GPU address')
        if name == 'aarch64_device_translate' and (wrapper or writer) and \
                u.reg_read(UC_ARM64_REG_X0) not in SNAPSHOT_PAGES + (WRAPPER_PAGE,):
            raise AssertionError('wrapper variant translated another GPU address')
        if name == 'bb_power_now':
            state['clock_reads'] += 1
            state['time'] += 1
            if case == 'read-crosses-deadline' and state['gpu_reads']:
                state['time'] = deadline
            if case == 'clock-regresses' and state['gpu_reads']:
                state['time'] = 99
            if case == 'on-deadline' and state['on_polls'] >= 3:
                state['time'] = deadline
            ret(state['time'])
        elif name == 'a733_gpu_platform_poll' and writer:
            state['on_polls'] += 1
            state['time'] += 5000
            early = case == 'ww-early-stop'
            field(power, 'a733_gpu_power', 'core_seen', 0x1f if case in ('ww-early-stop', 'ww-pending-early') else 0x1ff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 10 if early else 5 if case == 'ww-pending-early' else 38)
            notes(0x10130101, 0, 0, 0, 0, 0, (51, 51, 51))
            field(gpu, 'a733_gpu_platform', 'phase', 2)
            field(gpu, 'a733_gpu_platform', 'token', 17, 8)
            field(gpu, 'a733_gpu_platform', 'delegated', 1)
            field(power, 'a733_gpu_power', 'phase', 1)
            field(power, 'a733_gpu_power', 'last_value', 0)
            qchan = int.from_bytes(u.mem_read(0x01880104, 4), 'little')
            delay = int.from_bytes(u.mem_read(0x01880100, 4), 'little')
            if early or (case == 'ww-stage-fault' and delay == 0x00640064):
                # Admission refused the entry, or a status read failed.
                field(power, 'a733_gpu_power', 'phase', 5)
                field(power, 'a733_gpu_power', 'error', 5)
                field(gpu, 'a733_gpu_platform', 'phase', 7)
                ret((1 << 64) - 1)
                return
            if case == 'ww-on-first' or (case == 'ww-stage2' and qchan == 0x100) or \
                    (case == 'ww-stage3' and qchan == 0x107) or (case == 'ww-late' and len(state['ww']) == 12):
                field(gpu, 'a733_gpu_platform', 'phase', 3)
                field(power, 'a733_gpu_power', 'phase', 2)
                field(power, 'a733_gpu_power', 'last_value', 8)
                ret()
                return
            ret(0)
        elif name == 'a733_gpu_platform_abandon':
            if case == 'ww-abandon-refused':
                ret(0)
                return
            field(gpu, 'a733_gpu_platform', 'phase', 4)
            field(power, 'a733_gpu_power', 'phase', 3)
            ret()
        elif name == 'a733_gpu_platform_poll' and wrapper:
            state['on_polls'] += 1
            if case in ('wrapper-on', 'wrapper-off-fault'):
                field(power, 'a733_gpu_power', 'core_seen', 0x1ff)
                field(power, 'a733_gpu_power', 'core_policy', 8)
                field(power, 'a733_gpu_power', 'core_config', 0x101)
                notes(0x10130101, 0, 0, 0, 8, 0x100, (51, 51, 1))
                field(gpu, 'a733_gpu_platform', 'phase', 3)
                field(gpu, 'a733_gpu_platform', 'token', 17, 8)
                field(gpu, 'a733_gpu_platform', 'delegated', 1)
                field(power, 'a733_gpu_power', 'phase', 2)
                field(power, 'a733_gpu_power', 'last_value', 8)
                ret()
                return
            # The board's state: the power-up entry, every step through the
            # reset release (seen 0x1ff) and a final poll that reads 0.
            early = case == 'wrapper-early-stop'
            field(power, 'a733_gpu_power', 'core_seen', 0x1f if early else 0x1ff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 10 if early else 38)
            notes(0x10130101, 0, 0, 0, 0, 0, (51, 51, 51))
            if case == 'wrapper-client-deadline':
                # The client's own 200 ms bound ends the wait.
                state['time'] += 50000
                ret(0)
                return
            if state['on_polls'] < 2 and not early:
                ret(0)
                return
            # The platform's deadline cancelled the controller (error 6), or
            # admission refused the entry (error 5).
            field(power, 'a733_gpu_power', 'phase', 5)
            field(power, 'a733_gpu_power', 'error', 5 if early else 6)
            ret((1 << 64) - 1)
            return
        elif name == 'a733_gpu_platform_poll':
            state['on_polls'] += 1
            if case == 'on-deadline':
                ret(0)
                return
            if case in ['on-fault', 'core-refused', 'core-partial'] or (case == 'second-on-fault' and state['on_polls'] == 2):
                if case != 'on-fault':
                    # A refused entry has the policy, status, IDR0, MISR and
                    # PWCR reads (seen 0x1f) and stops at the admission step 10.
                    field(power, 'a733_gpu_power', 'core_seen', 0x1f if case == 'core-refused' else 1)
                    field(power, 'a733_gpu_power', 'core_policy', 8)
                    field(power, 'a733_gpu_power', 'core_config', 0x010101ff if case == 'core-refused' else 0)
                    field(power, 'a733_gpu_power', 'error', 5 if case == 'core-refused' else 1)
                    field(power, 'a733_gpu_power', 'index', 10 if case == 'core-refused' else 7)
                    field(power, 'a733_gpu_power', 'phase', 5)
                    if case == 'core-refused':
                        notes(0x00031001, 0x100, 0, 0, 0, 0)
                ret((1 << 64) - 1)
                return
            # The first ON enters from the power-up state and completes after
            # the PWCR write; the second enters from settled OFF with PWCR 0
            # and completes at the final poll.
            field(power, 'a733_gpu_power', 'core_seen', 0x1ff)
            if state['on_polls'] == 1:
                field(power, 'a733_gpu_power', 'core_policy', 8)
                field(power, 'a733_gpu_power', 'core_config', 0x101)
                notes(0x00031001, 0x100, 8, 8, 8, 0x100, (3, 1, 1))
            else:
                field(power, 'a733_gpu_power', 'core_policy', 0)
                field(power, 'a733_gpu_power', 'core_config', 0)
                notes(0x00031001, 0, 0, 0, 0, 0x100, (0, 0, 0))
            field(gpu, 'a733_gpu_platform', 'phase', 3)
            field(gpu, 'a733_gpu_platform', 'token', 17, 8)
            field(gpu, 'a733_gpu_platform', 'delegated', 1)
            # The controller's last read is the CORE status poll.
            field(power, 'a733_gpu_power', 'phase', 2)
            field(power, 'a733_gpu_power', 'last_value', 8)
            ret()
        elif name == 'aarch64_device_translate':
            address = u.reg_read(UC_ARM64_REG_X0)
            assert address in (0x1800020, 0x1800018, WRAPPER_PAGE) + SNAPSHOT_PAGES
            # Only the wrapper-write variant translates for a store, of the wrapper page.
            assert u.reg_read(UC_ARM64_REG_X1) == 0 or (writer and address == WRAPPER_PAGE and
                                                        u.reg_read(UC_ARM64_REG_X1) == 1)
            state['translated'].append(address)
            state['rw_translated'].append([address, u.reg_read(UC_ARM64_REG_X1)])
            # The compiled mapping predicate consumes this native translation
            # result; wrong physical address must refuse before the MMIO read.
            target = u.reg_read(UC_ARM64_REG_X2)
            wrong = (case == 'bad-mapping' and address < 0x2000000) or \
                    (case == 'snapshot-mapping' and address == 0x07065000) or \
                    (case in ('wrapper-mapping', 'ww-mapping') and address == WRAPPER_PAGE)
            u.mem_write(target, struct.pack('<QII', address + (4096 if wrong else 0), 0, 0))
            ret()
        elif name == 'a733_gpu_platform_off_poll':
            if case in ('off-fault', 'wrapper-off-fault', 'ww-off-fault'):
                ret((1 << 64) - 1)
                return
            field(gpu, 'a733_gpu_platform', 'phase', 6)
            field(gpu, 'a733_gpu_platform', 'token', 17 if case == 'retained-token' else 0, 8)
            field(gpu, 'a733_gpu_platform', 'delegated', 0)
            field(power, 'a733_gpu_power', 'phase', 4)
            field(row, 'a733_power_domain_row', 'state', 3)
            field(row, 'a733_power_domain_row', 'counted', case == 'counted-row')
            field(domain5, 'a733_power_domain', 'state', 1)
            ret()
        elif name == 'bb_power_park':
            field(bb, 'bb_power', 'parked', 0 if case == 'unparked' else 1)
            ret()
        elif name == 'bb_power_fail':
            state['failure'] = text(u.reg_read(UC_ARM64_REG_X0))
            u.emu_stop()
        elif name == 'platform_puts':
            state['messages'].append(text(u.reg_read(UC_ARM64_REG_X0)))
            ret()
        elif name == 'platform_exit':
            state['exit_status'] = u.reg_read(UC_ARM64_REG_X0)
            u.emu_stop()
        else:
            ret()

    modeled = ['bb_power_wait_ready', 'bb_power_now', 'vTaskDelay',
               'a733_gpu_platform_begin', 'a733_gpu_platform_poll', 'aarch64_device_translate',
               'a733_gpu_platform_off_begin', 'a733_gpu_platform_off_poll', 'bb_power_park',
               'a733_power_service_idle', 'bb_power_fail', 'platform_puts', 'platform_exit']
    if writer:
        modeled.append('a733_gpu_platform_abandon')
    hooks = {image.address(name): name for name in modeled}
    u.hook_add(UC_HOOK_CODE, lambda uc, address, size, arg: call(hooks[address]) if address in hooks else None)

    def wrapper_read(uc, access, address, size, value, arg):
        state['gpu_reads'].append([hex(address), size])
        assert wrapper, f'a GPU wrapper read at {address:#x} outside the wrapper-read variant'
        n = len(state['wrapper_reads']) + 1
        if not (size == 4 and n <= 4 and address == WRAPPER[n - 1]):
            state['refused'] = [hex(address), size, n]
            uc.emu_stop()
            return
        output = ''.join(state['messages'])
        if n == 1:
            # Everything already known is printed before the first wrapper read.
            assert len(state['snapshot_reads']) == 2 * len(SNAPSHOT), state['snapshot_reads']
            for text in ('GPU SNAP n=1 a=0x2002b24', 'GPU power phase=', 'GPU resources result=',
                         'GPU CORE entry cycle=1', 'GPU CORE inputs cycle=1', 'GPU CORE after cycle=1'):
                assert text in output, (text, output)
        # The progress record and the log name this read before it is issued.
        magic, stage, count, stamp = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
        assert magic == WRAPPER_MAGIC and stage == n and count == n - 1 and stamp, (magic, stage, count)
        assert state['messages'][-1] == f'GPU WRAPPER reading n={n} a={address:#x}\n', state['messages'][-1]
        state['wrapper_reads'].append(address)
        state['progress'].append([stage, count])

    def ww_access(uc, access, address, size, value, arg):
        """One wrapper-write access: the table row its progress record names,
        after that row's log line, later than the previous access."""
        write = access == UC_MEM_WRITE
        magic, stage, count, stamp = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
        row = stage - 1
        expected = WRITE_TABLE[row] if 0 <= row < len(WRITE_TABLE) else None
        if not (size == 4 and expected and expected[0] == address and expected[2] == write and
                (not write or expected[1] == value) and magic == WRITE_MAGIC and stamp and
                count == len(state['ww']) and (not state['ww'] or row > state['ww'][-1])):
            state['refused'] = [hex(address), size, int(write), hex(value) if write else None, stage]
            uc.emu_stop()
            return
        if case == 'ww-readback' and row == 3:
            # The register keeps 0: the stage-1 readback differs from its write.
            u.mem_write(address, bytes(4))
        if not state['ww']:
            # The page was checked for read and then write before the first access.
            assert state['rw_translated'][-2:] == [[WRAPPER_PAGE, 0], [WRAPPER_PAGE, 1]], state['rw_translated']
            # Everything already known is printed before the first wrapper access.
            output = ''.join(state['messages'])
            assert state['snapshot_reads'] == SNAPSHOT * 2, state['snapshot_reads']
            for text in ('GPU SNAP n=1 a=0x2002b24', 'GPU power phase=', 'GPU resources result=',
                         'GPU CORE entry cycle=1', 'GPU CORE inputs cycle=1', 'GPU CORE after cycle=1'):
                assert text in output, (text, output)
        line = (f'GPU WRAPPER writing n={stage} a={address:#x} v={value:#x}\n' if write
                else f'GPU WRAPPER reading n={stage} a={address:#x}\n')
        assert state['messages'][-1] == line, (state['messages'][-1], line)
        state['ww'].append(row)
        state['ww_accesses'].append([row, hex(address), int(write), hex(value) if write else None])
        # Each access happens before any OFF call, with the platform still starting or ON.
        phase = int.from_bytes(u.mem_read(gpu + L['a733_gpu_platform']['phase'], 4), 'little')
        assert phase in (2, 3) and 'a733_gpu_platform_abandon' not in state['calls'] and \
            'a733_gpu_platform_off_begin' not in state['calls'], (phase, state['calls'])
        state['events'].append(['ww', row])

    def read(uc, access, address, size, value, arg):
        state['gpu_reads'].append([hex(address), size])
        assert not power_only, 'power-only variant read the GPU register window'
        assert not wrapper, 'wrapper-read variant read the GPU core window'
        assert (address, size) in [(0x1800020, 8), (0x1800020, 4), (0x1800018, 4)]

    def snapshot_read(uc, access, address, size, value, arg):
        assert size == 4 and address in SNAPSHOT, (hex(address), size)
        state['snapshot_reads'].append(address)
        if state['ww']:
            state['events'].append(['core', address])

    def snapshot_write(uc, access, address, size, value, arg):
        raise AssertionError(f'write to a snapshot page at {address:#x}')

    for page in SNAPSHOT_PAGES:
        u.hook_add(UC_HOOK_MEM_READ, snapshot_read, begin=page, end=page + 0xfff)
        u.hook_add(UC_HOOK_MEM_WRITE, snapshot_write, begin=page, end=page + 0xfff)

    def write(uc, access, address, size, value, arg):
        state['gpu_writes'].append([address, size, value])
        raise AssertionError('GPU write in never-booted identity consumer')

    u.hook_add(UC_HOOK_MEM_READ, read, begin=0x1800000, end=0x1800fff)
    u.hook_add(UC_HOOK_MEM_WRITE, write, begin=0x1800000, end=0x1800fff)
    if writer:
        u.hook_add(UC_HOOK_MEM_READ, ww_access, begin=WRAPPER_PAGE, end=WRAPPER_PAGE + 0xfff)
        u.hook_add(UC_HOOK_MEM_WRITE, ww_access, begin=WRAPPER_PAGE, end=WRAPPER_PAGE + 0xfff)
    else:
        u.hook_add(UC_HOOK_MEM_READ, wrapper_read, begin=WRAPPER_PAGE, end=WRAPPER_PAGE + 0xfff)
        u.hook_add(UC_HOOK_MEM_WRITE, write, begin=WRAPPER_PAGE, end=WRAPPER_PAGE + 0xfff)
    u.emu_start(image.address('gpu_power_id_client'), 0, count=400000 if writer else 100000)
    output = ''.join(state['messages'])
    if writer:
        return check_writer(u, state, case, output)
    if wrapper and case == 'wrapper-table-mutant':
        assert state.get('refused') == ['0x1880108', 4, 2] and state['wrapper_reads'] == WRAPPER[:1], state
        state['gpu_reads'] = state['gpu_reads'][:1]
        return state
    assert 'refused' not in state, state['refused']
    if wrapper:
        assert not state['gpu_writes']
        reasons = {'wrapper-platform-deadline': 'GPU wrapper read: CORE6 not ON, four wrapper words read',
                   'wrapper-client-deadline': 'GPU wrapper read: CORE6 not ON, four wrapper words read',
                   'wrapper-early-stop': 'GPU wrapper read refused before clock and reset progress',
                   'wrapper-mapping': 'GPU wrapper mapping', 'wrapper-off-fault': 'GPU OFF poll', 'wrapper-on': None}
        assert state['failure'] == reasons[case], state['failure']
        reads = WRAPPER if case not in ('wrapper-early-stop', 'wrapper-mapping') else []
        assert state['wrapper_reads'] == reads and state['gpu_reads'] == [[hex(a), 4] for a in reads], state['gpu_reads']
        assert len(state['snapshot_reads']) == 2 * len(SNAPSHOT)
        if reads:
            for a in reads:
                assert f'GPU WRAPPER n={reads.index(a) + 1} a={a:#x} v={WRAPPER_VALUES[a]:#x}' in output, output
            assert 'GPU WRAPPER qchan core0_sw=0 core0_active=1 core0_accept=0 core0_deny=0 core1_sw=0 ' \
                   'core0_clk_en=0' in output, output
            assert 'GPU WRAPPER delay up=0x10 down=0x20 cores=1 primary=0x0 secondary=0x1' in output, output
        magic, stage, count, _ = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
        assert magic == WRAPPER_MAGIC and stage == len(reads) and count == len(reads), (stage, count)
        if case == 'wrapper-on':
            assert state['exit_status'] == 0
            assert 'COMPLETE GPU wrapper read: CORE status 8, four wrapper words, OFF and provider parked' in output
            calls = state['calls']
            assert calls.index('a733_gpu_platform_off_begin') < calls.index('bb_power_park')
        else:
            assert state['exit_status'] is None and 'COMPLETE' not in output
        if case in ('wrapper-platform-deadline', 'wrapper-client-deadline', 'wrapper-early-stop', 'wrapper-mapping'):
            assert 'a733_gpu_platform_off_begin' not in state['calls']
        state['cycles'] = state['calls'].count('a733_gpu_platform_off_begin')
        return state
    if power_only:
        assert not state['gpu_reads'] and not state['gpu_writes']
        if case == 'normal':
            assert state['exit_status'] == 0 and state['failure'] is None, state
            assert 'COMPLETE GPU power only: CORE status 8, OFF and provider parked' in output
            assert 'identity' not in output, output
            # Two full cycles, each reported, then one park and the completion text.
            for line in ['GPU CORE entry cycle=1 seen=511 policy=0x8 status=0x0 pwcr=0x101',
                         'GPU CORE inputs cycle=1 idr0=0x31001 misr=0x100 misr_late=0x100',
                         'GPU CORE after cycle=1 delays_status=0x8 delays_us=3 pwcr_status=0x8 pwcr_us=1 '
                         'policy_status=0x8 policy_us=1',
                         'GPU CORE entry cycle=2 seen=511 policy=0x0 status=0x0 pwcr=0x0',
                         'GPU CORE inputs cycle=2 idr0=0x31001 misr=0x0 misr_late=0x100',
                         'GPU CORE after cycle=2 delays_status=0x0 delays_us=0 pwcr_status=0x0 pwcr_us=0 '
                         'policy_status=0x0 policy_us=0',
                         'GPU power cycle=1 core_status=0x8', 'GPU power cycle=2 core_status=0x8']:
                assert line in output, (line, output)
            # Snapshots 0, 1 and 2, each of every word once, in order.
            assert state['snapshot_reads'] == SNAPSHOT * 3, state['snapshot_reads']
            for n in range(3):
                for address in SNAPSHOT:
                    assert f'GPU SNAP n={n} a={address:#x} v={address ^ 0x5a5a0000:#x}' in output
            calls = state['calls']
            starts = [i for i, c in enumerate(calls) if c == 'a733_gpu_platform_begin']
            stops = [i for i, c in enumerate(calls) if c == 'a733_gpu_platform_off_begin']
            assert len(starts) == len(stops) == 2 and starts[0] < stops[0] < starts[1] < stops[1], calls
            assert stops[1] < calls.index('bb_power_park') < last_put(state) and calls.count('bb_power_park') == 1
        else:
            reasons = {'core-refused': 'GPU ON poll', 'core-partial': 'GPU ON poll', 'on-fault': 'GPU ON poll',
                       'second-on-fault': 'GPU ON poll', 'off-fault': 'GPU OFF poll',
                       'on-deadline': 'GPU ON deadline', 'snapshot-mapping': 'GPU snapshot mapping',
                       'retained-token': 'GPU OFF ownership proof', 'counted-row': 'GPU OFF ownership proof',
                       'unparked': 'GPU provider park proof'}
            assert state['failure'] == reasons[case], state
            assert state['exit_status'] is None and 'COMPLETE' not in output
            if case in ['on-fault', 'core-refused', 'core-partial', 'on-deadline']:
                assert 'a733_gpu_platform_off_begin' not in state['calls']
                # The failed ON took its snapshot before the failure report.
                assert state['snapshot_reads'] == SNAPSHOT * 2, state['snapshot_reads']
            if case == 'snapshot-mapping':
                # A wrong translation of one page refuses before any snapshot read.
                assert not state['snapshot_reads'] and 'a733_gpu_platform_begin' not in state['calls']
            if case == 'second-on-fault':
                # The first cycle completed; the failed second ON reports its cycle.
                assert state['calls'].count('a733_gpu_platform_off_begin') == 1, state['calls']
                assert 'GPU CORE entry cycle=2' in output and 'bb_power_park' not in state['calls'], output
        state['cycles'] = state['calls'].count('a733_gpu_platform_off_begin')
        return state
    if case in ['normal', 'legacy-id', 'unexpected-id']:
        assert state['exit_status'] == 0 and state['failure'] is None
        assert 'COMPLETE GPU power identity: OFF and provider parked' in output
        assert f'matches={0 if case == "unexpected-id" else 1}' in output
        assert state['calls'].index('a733_gpu_platform_off_poll') < state['calls'].index('bb_power_park') < last_put(state)
        assert state['gpu_reads'] == ([['0x1800020', 8], ['0x1800020', 4], ['0x1800018', 4]] if case == 'legacy-id' else [['0x1800020', 8]])
    else:
        reasons = {'core-refused': 'GPU ON poll', 'core-partial': 'GPU ON poll', 'on-fault': 'GPU ON poll', 'bad-mapping': 'GPU identity mapping',
                   'read-crosses-deadline': 'GPU identity deadline/clock', 'clock-regresses': 'GPU identity deadline/clock',
                   'off-fault': 'GPU OFF poll', 'retained-token': 'GPU OFF ownership proof',
                   'counted-row': 'GPU OFF ownership proof', 'unparked': 'GPU provider park proof'}
        assert state['failure'] == reasons[case], state
        assert state['exit_status'] is None and 'COMPLETE' not in output
        if case in ['on-fault', 'core-refused', 'core-partial', 'bad-mapping']:
            assert not state['gpu_reads']
        if case in ['on-fault', 'core-refused', 'core-partial', 'bad-mapping', 'read-crosses-deadline', 'clock-regresses']:
            assert 'a733_gpu_platform_off_begin' not in state['calls']
    if case in ['core-refused', 'core-partial']:
        assert 'GPU CORE entry' in output and f'seen={31 if case == "core-refused" else 1}' in output
        assert 'GPU CORE after' in output and 'GPU CORE inputs' in output
        if case == 'core-refused':
            assert 'GPU power phase=5 error=5 step=10' in output and 'idr0=0x31001 misr=0x100' in output, output
        assert 'policy=0x8 status=0x0' in output, output
        assert 'GPU power' in output and 'a733_gpu_platform_off_begin' not in state['calls']
    assert not state['gpu_writes']
    state['cycles'] = state['calls'].count('a733_gpu_platform_off_begin')
    if case == 'normal':
        assert state['cycles'] == 1
    return state


def check_writer(u, state, case, output):
    """The wrapper-write variant's outcome for one case."""
    assert not state['gpu_reads'] and not state['gpu_writes'], 'a GPU core window access'
    calls = state['calls']
    if case == 'ww-table-mutant':
        # Refused at the mutated stage-3 write, after rows 0 to 5.
        assert state.get('refused') == ['0x1880104', 4, 1, '0x10f', 7] and state['ww'] == list(range(6)), state
        return state
    assert 'refused' not in state, state['refused']
    assert state['failure'] == WRITE_REASONS[case], (state['failure'], output)
    rows = WRITE_PATHS[case]
    assert state['ww'] == rows, state['ww']
    magic, stage, count, _ = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
    if case != 'ww-time-budget':
        assert magic == WRITE_MAGIC and stage == (rows[-1] + 1 if rows else 0) and count == len(rows), (stage, count)
    for row in rows:
        address, value, write = WRITE_TABLE[row]
        if not write:
            stored = 2 if case == 'ww-entry-nonzero' and row == 0 else WRITE_TABLE[row - 1][1] if row > 1 else 0
            stored = 0 if case == 'ww-readback' and row == 3 else stored
            assert f'GPU WRAPPER n={row + 1} a={address:#x} v={stored:#x}' in output, (row, output)
    # Snapshots 0 and 1; the three CORE6 words after each stage's readback
    # and after the restore, in order with the wrapper accesses; and snapshot
    # 2 after every OFF attempt.
    stages = {'ww-none': 3, 'ww-stage2': 2, 'ww-off-fault': 3, 'ww-abandon-refused': 3, 'ww-stage-fault': 1,
              'ww-late': 3, 'ww-stage3': 3, 'ww-readback': 3}.get(case, 0)
    snapshots = 1 if case == 'ww-time-budget' else 2
    restored = 11 in rows
    off = case in ('ww-none', 'ww-stage2', 'ww-on-first', 'ww-entry-nonzero', 'ww-off-fault', 'ww-abandon-refused',
                   'ww-late', 'ww-stage3', 'ww-readback')
    after = SNAPSHOT if off else []
    assert state['snapshot_reads'] == SNAPSHOT * snapshots + CORE_WORDS * (stages + restored) + after, \
        state['snapshot_reads']
    events = []
    for row in rows:
        events.append(['ww', row])
        if row in (3, 5, 7, 11):
            events += [['core', a] for a in CORE_WORDS]
    events += [['core', a] for a in after] if rows else []
    assert state['events'] == events, state['events']
    on_stage = {'ww-stage2': 2, 'ww-stage3': 3}.get(case, 0)
    for k in range(1, stages + 1):
        readback = 0 if case == 'ww-readback' and k == 1 else WRITE_TABLE[2 * k][1]
        assert f'GPU WRAPPER stage k={k} readback={readback:#x} ' in output, (k, output)
        if case != 'ww-stage-fault':
            assert f'GPU WRAPPER stage k={k} on={int(k == on_stage)} ' in output, (k, output)
    assert ('GPU WRAPPER restored status=' in output) == restored, output
    abandoned = case in ('ww-none', 'ww-entry-nonzero', 'ww-off-fault', 'ww-abandon-refused', 'ww-readback')
    assert ('a733_gpu_platform_abandon' in calls) == abandoned, calls
    completes = case in ('ww-stage2', 'ww-on-first', 'ww-late', 'ww-stage3')
    assert ('a733_gpu_platform_off_begin' in calls) == completes, calls
    parked = completes or case in ('ww-none', 'ww-entry-nonzero', 'ww-readback')
    assert ('bb_power_park' in calls) == parked, calls
    if parked:
        late = int(case == 'ww-late')
        assert f'GPU WRAPPER result stage={on_stage} on={int(completes)} late={late} accesses={len(rows)}' in output, \
            output
        # The restore precedes the OFF, and the park follows it.
        off = calls.index('a733_gpu_platform_abandon' if abandoned else 'a733_gpu_platform_off_begin')
        assert calls.index('a733_gpu_platform_off_poll') > off and calls.index('bb_power_park') > off
    if completes:
        assert state['exit_status'] == 0
        assert 'COMPLETE GPU wrapper write: CORE status 8, OFF and provider parked' in output, output
    else:
        assert state['exit_status'] is None and 'COMPLETE' not in output
    if case == 'ww-time-budget':
        assert 'a733_gpu_platform_begin' not in calls
    state['cycles'] = calls.count('a733_gpu_platform_off_begin') + calls.count('a733_gpu_platform_abandon')
    return state


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cases', nargs='+', help='Resume selected cases after a fixture-only failure')
    p.add_argument('--power-only', action='store_true', help='check the power-only variant')
    p.add_argument('--wrapper-read', action='store_true', help='check the wrapper-read variant')
    p.add_argument('--wrapper-write', action='store_true', help='check the wrapper-write variant')
    a = p.parse_args()
    assert a.power_only + a.wrapper_read + a.wrapper_write <= 1, 'choose one variant'
    # The shared owner added delays_ready in existing padding. Reused objects
    # retain the old DWARF member list; consume the actual replacement owner's
    # layout, and verify every old member and the total size still agree.
    image = harness.Image(a.image, layout_names=[n for n in NAMES if n != 'a733_power_domain'])
    # The variant is a compile option of main.c: only the identity variant
    # defines the identity read counter.
    identity = bool(image.symbols.get('id_reads', set()) - {0})
    wrapper_image = bool(image.symbols.get('wrapper_words', set()) - {0})
    writer_image = bool(image.symbols.get('wrapper_steps', set()) - {0})
    assert writer_image == a.wrapper_write, 'image variant does not match --wrapper-write'
    assert identity == (not a.power_only and not a.wrapper_read and not a.wrapper_write), \
        'image variant does not match the options'
    assert wrapper_image == a.wrapper_read, 'image variant does not match --wrapper-read'
    from elftools.elf.elffile import ELFFile
    layouts = []
    with a.image.open('rb') as stream:
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                attr = die.attributes
                if die.tag != 'DW_TAG_structure_type' or attr.get('DW_AT_name') is None or attr['DW_AT_name'].value != b'a733_power_domain':
                    continue
                if 'DW_AT_byte_size' not in attr: continue
                assert attr['DW_AT_byte_size'].value == 32
                layouts.append({c.attributes['DW_AT_name'].value.decode(): c.attributes['DW_AT_data_member_location'].value
                                for c in die.iter_children() if c.tag == 'DW_TAG_member'})
    current = next(x for x in layouts if 'delays_ready' in x)
    assert current['delays_ready'] == 12
    old_fields = {k: v for k, v in current.items() if k != 'delays_ready'}
    assert all({k: v for k, v in x.items() if k != 'delays_ready'} == old_fields for x in layouts)
    image.layouts['a733_power_domain'] = current
    cases = ['normal', 'legacy-id', 'unexpected-id', 'on-fault', 'bad-mapping',
             'read-crosses-deadline', 'clock-regresses', 'off-fault', 'retained-token', 'counted-row', 'unparked', 'core-refused', 'core-partial']
    if a.power_only:
        cases = ['normal', 'on-fault', 'second-on-fault', 'on-deadline', 'snapshot-mapping', 'core-refused', 'core-partial',
                 'off-fault', 'retained-token', 'counted-row', 'unparked']
    if a.wrapper_read:
        cases = ['wrapper-platform-deadline', 'wrapper-client-deadline', 'wrapper-on', 'wrapper-early-stop',
                 'wrapper-mapping', 'wrapper-off-fault', 'wrapper-table-mutant']
    if a.wrapper_write:
        cases = [*WRITE_PATHS, 'ww-table-mutant']
    if a.cases:
        assert set(a.cases) <= set(cases)
        cases = a.cases
    result = {'physical_access': False,
              'variant': ('power-only' if a.power_only else 'wrapper-read' if a.wrapper_read
                          else 'wrapper-write' if a.wrapper_write else 'identity'),
              'scope': 'actual ELF client and ID access; modeled provider/power transitions',
              'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
              'cases': []}
    for case in cases:
        try:
            result['cases'].append(run(image, case, a.power_only, a.wrapper_read, a.wrapper_write))
        except Exception as error:
            result['failed_case'] = case
            result['fixture_error'] = repr(error)
            a.output.write_text(json.dumps(result, indent=2) + '\n')
            raise
        a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(cases)} focused client cases passed; no physical access')


if __name__ == '__main__':
    main()
