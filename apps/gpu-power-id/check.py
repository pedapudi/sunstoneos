#!/usr/bin/env python3
"""Focused actual-ELF consumer checks; device transitions are explicit models.

With --power-only the image must be the power-only variant: every case also
requires that no load or store reaches the GPU register window. With
--wrapper-read the image must be the wrapper-read variant: its only
GPU-window accesses are 32-bit loads of the four wrapper words, in their
order, each after its progress record and log line, and there is no store
to the window. With --probe-read the image must be the probe-read variant:
its only wrapper, SMC0 and GPU core accesses are 32-bit accesses of the rows
of its access table in their order, each after its progress record and log
line; the writes and the restore run with exceptions masked and with the
restore vectors installed, and a synchronous abort inside that window
restores both written words before the image's own handler runs. Without a
variant option the image must be the identity variant: in each of two ON/OFF
cycles its only GPU-window accesses are two 32-bit loads, 0x01800024 and
then 0x01800020. The PMIC, rail and GPU platform calls are modeled."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP
from unicorn.arm64_const import UC_ARM64_REG_VBAR_EL1, UC_ARM64_REG_PSTATE
from unicorn import UC_MEM_WRITE

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
# The probe-read variant's access table (probe_steps in probe-read.c):
# address, value, kind (0 read32, 1 write32, 2 write32 of the value read by
# the row named in value). Rows 5 to 14 form the write window.
SMC0_PAGE = 0x0a000000
PROBE_TABLE = [(0x01880014, 0, 0), (0x01880010, 0, 0), (0x0a0000b0, 0, 0), (0x01800024, 0, 0), (0x01800020, 0, 0),
               (0x0a0000b0, 2, 1), (0x0a0000b0, 0, 0), (0x01880014, 0, 1), (0x01880014, 0, 0), (0x01800024, 0, 0),
               (0x01800020, 0, 0), (0x01880014, 0, 2), (0x01880014, 0, 0), (0x0a0000b0, 2, 2), (0x0a0000b0, 0, 0)]
PROBE_MAGIC = 0x3130445250555047
# Modeled entry values of drm_ctrl, irq_ctrl and the SMC0 word.
PROBE_ENTRY = {0x01880014: 1, 0x01880010: 0, 0x0a0000b0: 5}
PROBE_ALL = list(range(15))
PROBE_PATHS = {'pr-board': PROBE_ALL, 'pr-core-on': PROBE_ALL, 'pr-bad-id': PROBE_ALL, 'pr-after-differs': PROBE_ALL,
               'pr-restore-ignored': PROBE_ALL, 'pr-sync-abort': list(range(10)), 'pr-early-stop': [],
               'pr-pending-early': [], 'pr-mapping': [], 'pr-time-budget': [], 'pr-window-refused': list(range(5))}
PROBE_FAIL = 'GPU probe read: identity or restore not as expected, GPU left powered'
PROBE_REASONS = {'pr-board': None, 'pr-core-on': None, 'pr-bad-id': PROBE_FAIL, 'pr-after-differs': PROBE_FAIL,
                 'pr-restore-ignored': PROBE_FAIL, 'pr-sync-abort': None, 'pr-early-stop': 'GPU ON poll',
                 'pr-pending-early': 'GPU probe read refused before clock and reset progress',
                 'pr-mapping': 'GPU probe read mapping', 'pr-time-budget': 'GPU probe read time budget',
                 'pr-window-refused': None}
PROBE_COMPLETE = ('COMPLETE GPU probe read: identity 36.56.104.183 before and after the secure configuration, '
                  'restored, GPU left powered')
NAMES = ['bb_power', 'a733_gpu_platform', 'a733_gpu_power', 'a733_pmic_consumer',
         'a733_power_domains', 'a733_power_domain', 'a733_power_domain_row']



def last_put(state):
    """Index of the last platform_puts call: the completion text follows the park."""
    return len(state['calls']) - 1 - state['calls'][::-1].index('platform_puts')


def run(image, case, power_only=False, wrapper=False, probe=False):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0x41000000, 0x1200000)
    u.mem_map(0x1800000, 0x1000)
    u.mem_map(WRAPPER_PAGE, 0x1000)
    for address, value in WRAPPER_VALUES.items():
        u.mem_write(address, value.to_bytes(4, 'little'))
    for page in SNAPSHOT_PAGES:
        u.mem_map(page, 0x1000)
    for address in SNAPSHOT:
        u.mem_write(address, (address ^ 0x5a5a0000).to_bytes(4, 'little'))
    for address, data in image.parts:
        u.mem_write(address, data)
    if probe:
        u.mem_map(SMC0_PAGE, 0x1000)
        for address, value in PROBE_ENTRY.items():
            u.mem_write(address, value.to_bytes(4, 'little'))
    if case == 'pr-table-mutant':
        # A seeded mutant: the SMC0 write stores 3, which the check must refuse.
        u.mem_write(image.address('probe_steps') + 5 * 12 + 4, (3).to_bytes(4, 'little'))
    if case == 'pr-restore-mutant':
        # A seeded mutant: the SMC0 restore takes drm_ctrl's entry value.
        u.mem_write(image.address('probe_steps') + 13 * 12 + 4, (0).to_bytes(4, 'little'))
    if case == 'wrapper-table-mutant':
        # A seeded mutant of the image: the second table entry names another
        # wrapper word, which the check must refuse at that read.
        u.mem_write(image.address('wrapper_words') + 4, (0x01880108).to_bytes(4, 'little'))
    u.reg_write(UC_ARM64_REG_SP, 0x421f0000)
    L = image.layouts
    gpu, bb = image.address('gpu'), image.address('bb_power')
    power = gpu + L['a733_gpu_platform']['power']
    row = power + L['a733_gpu_power']['row']
    consumer = gpu + L['a733_gpu_platform']['pmic']
    domains = bb + L['bb_power']['domains']
    domain5 = domains + L['a733_power_domains']['domain'] + 2 * 32
    # The wrapper-read client keeps 200 ms for ON and 100 ms after it.
    deadline = 250000 if case == 'pr-time-budget' else \
        3 * 10**6 if case == 'pr-window-refused' else 10**6 if wrapper or probe else 10000
    state = {'case': case, 'calls': [], 'gpu_reads': [], 'gpu_writes': [], 'clock_reads': 0, 'on_polls': 0,
             'snapshot_reads': [], 'translated': [], 'wrapper_reads': [], 'progress': [],
             'time': 100, 'messages': [], 'exit_status': None, 'failure': None,
             'pr': [], 'pr_values': {}, 'pr_accesses': [], 'rw_translated': []}
    def put(address, value, size=4):
        u.mem_write(address, value.to_bytes(size, 'little'))

    def field(base, typ, name, value, size=4):
        put(base + L[typ][name], value, size)

    def notes(idr0, misr, after_pwcr, after_policy, misr_late):
        base = power + L['a733_gpu_power']['core_note']
        for i, value in enumerate((idr0, misr, after_pwcr, after_policy, misr_late)):
            put(base + 4 * i, value)

    def clock_notes(pll=0xf9105300, locks=(3, 3), updates=(1, 1)):
        put(power + L['a733_gpu_power']['pll_word'], pll)
        for i in range(2):
            put(power + L['a733_gpu_power']['pll_lock_reads'] + 4 * i, locks[i])
            put(power + L['a733_gpu_power']['clock_update_reads'] + 4 * i, updates[i])

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
    put(0x1800020, 0 if case == 'pr-bad-id' else raw, 8)

    def call(name):
        state['calls'].append(name)
        if name == 'aarch64_device_translate' and power_only and u.reg_read(UC_ARM64_REG_X0) not in SNAPSHOT_PAGES:
            raise AssertionError('power-only variant translated a GPU address')
        if name == 'aarch64_device_translate' and wrapper and \
                u.reg_read(UC_ARM64_REG_X0) not in SNAPSHOT_PAGES + (WRAPPER_PAGE,):
            raise AssertionError('wrapper-read variant translated another GPU address')
        if name == 'aarch64_device_translate' and probe and \
                u.reg_read(UC_ARM64_REG_X0) not in SNAPSHOT_PAGES + (WRAPPER_PAGE, SMC0_PAGE, 0x1800000):
            raise AssertionError('probe-read variant translated another address')
        if name == 'bb_power_now':
            state['clock_reads'] += 1
            state['time'] += 1
            if case == 'read-crosses-deadline' and state['gpu_reads']:
                state['time'] = deadline
            if case == 'clock-regresses' and state['gpu_reads']:
                state['time'] = 99
            if case == 'on-deadline' and state['on_polls'] >= 3:
                state['time'] = deadline
            if case == 'pr-window-refused' and len(state['pr']) == 5:
                # 1.6 s after monitor entry (start_counter 0): the window would
                # end less than 500 ms before the monitor's 2 s deadline.
                state['time'] = max(state['time'], 1600000)
            ret(state['time'])
        elif name == 'a733_gpu_platform_poll' and probe:
            state['on_polls'] += 1
            state['time'] += 5000
            early = case == 'pr-early-stop'
            field(power, 'a733_gpu_power', 'core_seen', 0x1f if case in ('pr-early-stop', 'pr-pending-early') else 0xff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 10 if early else 6 if case == 'pr-pending-early' else 53)
            notes(0x10130101, 0, 0, 0, 0)
            field(gpu, 'a733_gpu_platform', 'phase', 7 if early else 3 if case == 'pr-core-on' else 2)
            field(gpu, 'a733_gpu_platform', 'token', 17, 8)
            field(gpu, 'a733_gpu_platform', 'delegated', 1)
            field(power, 'a733_gpu_power', 'phase', 5 if early else 2 if case == 'pr-core-on' else 1)
            field(power, 'a733_gpu_power', 'last_value', 8 if case == 'pr-core-on' else 0)
            ret((1 << 64) - 1 if early else 1 if case == 'pr-core-on' else 0)
        elif name == 'platform_exception':
            state['exception'] = True
            u.emu_stop()
        elif name == 'a733_gpu_platform_poll' and wrapper:
            state['on_polls'] += 1
            if case in ('wrapper-on', 'wrapper-off-fault'):
                field(power, 'a733_gpu_power', 'core_seen', 0xff)
                field(power, 'a733_gpu_power', 'core_policy', 8)
                field(power, 'a733_gpu_power', 'core_config', 0x101)
                notes(0x10130101, 0, 0, 8, 0x100)
                field(gpu, 'a733_gpu_platform', 'phase', 3)
                field(gpu, 'a733_gpu_platform', 'token', 17, 8)
                field(gpu, 'a733_gpu_platform', 'delegated', 1)
                field(power, 'a733_gpu_power', 'phase', 2)
                field(power, 'a733_gpu_power', 'last_value', 8)
                ret()
                return
            # The board's state: the power-up entry, every step through the
            # clock steps (seen 0xff) and a final poll that reads 0.
            early = case == 'wrapper-early-stop'
            field(power, 'a733_gpu_power', 'core_seen', 0x1f if early else 0xff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 10 if early else 53)
            notes(0x10130101, 0, 0, 0, 0)
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
                        notes(0x00031001, 0x100, 0, 0, 0)
                ret((1 << 64) - 1)
                return
            # The first ON enters from the power-up state, the second from
            # settled OFF with PWCR 0; each reaches status 8 once the GPU is
            # clocked, as the board run of 2026-10-25 showed for the first.
            field(power, 'a733_gpu_power', 'core_seen', 0xff)
            if state['on_polls'] == 1:
                field(power, 'a733_gpu_power', 'core_policy', 8)
                field(power, 'a733_gpu_power', 'core_config', 0x101)
            else:
                field(power, 'a733_gpu_power', 'core_policy', 0)
                field(power, 'a733_gpu_power', 'core_config', 0)
            notes(0x00031001, 0, 0, 8, 0x100)
            clock_notes()
            field(gpu, 'a733_gpu_platform', 'phase', 3)
            field(gpu, 'a733_gpu_platform', 'token', 17, 8)
            field(gpu, 'a733_gpu_platform', 'delegated', 1)
            # The controller's last read is the CORE status poll.
            field(power, 'a733_gpu_power', 'phase', 2)
            field(power, 'a733_gpu_power', 'last_value', 8)
            ret()
        elif name == 'aarch64_device_translate':
            address = u.reg_read(UC_ARM64_REG_X0)
            assert address in (0x1800024, 0x1800020, WRAPPER_PAGE) + SNAPSHOT_PAGES or \
                (probe and address in (SMC0_PAGE, 0x1800000))
            # Only the probe-read variant translates for a store, of the wrapper and SMC0 pages.
            assert u.reg_read(UC_ARM64_REG_X1) == 0 or (probe and address in (WRAPPER_PAGE, SMC0_PAGE) and
                                                        u.reg_read(UC_ARM64_REG_X1) == 1)
            state['rw_translated'].append([address, u.reg_read(UC_ARM64_REG_X1)])
            state['translated'].append(address)
            # The compiled mapping predicate consumes this native translation
            # result; wrong physical address must refuse before the MMIO read.
            target = u.reg_read(UC_ARM64_REG_X2)
            wrong = (case == 'bad-mapping' and address < 0x2000000) or \
                    (case == 'snapshot-mapping' and address == 0x07065000) or \
                    (case == 'wrapper-mapping' and address == WRAPPER_PAGE) or \
                    (case == 'pr-mapping' and address == SMC0_PAGE)
            u.mem_write(target, struct.pack('<QII', address + (4096 if wrong else 0), 0, 0))
            ret()
        elif name == 'a733_gpu_platform_off_poll':
            if case in ('off-fault', 'wrapper-off-fault'):
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
    if probe:
        # The probe-read image makes no OFF, so its link drops the OFF calls.
        modeled = [n for n in modeled if image.symbols.get(n, set()) - {0}] + ['platform_exception']
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

    vectors = image.address('probe_restore_vectors') if probe else None

    def pr_access(uc, access, address, size, value, arg):
        """One probe-read access: the table row its progress record names,
        after that row's log line, later than the previous access; the write
        window's rows run masked with the restore vectors installed."""
        write = access == UC_MEM_WRITE
        if state.get('in_handler'):
            # The restore vectors write both words back; nothing else.
            assert write and size == 4 and address in (0x01880014, 0x0a0000b0), hex(address)
            state['handler_writes'].append([hex(address), hex(value)])
            return
        magic, stage, count, stamp = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
        row = stage - 1
        expected = PROBE_TABLE[row] if 0 <= row < len(PROBE_TABLE) else None
        ok = expected is not None and expected[0] == address and magic == PROBE_MAGIC and stamp and \
            count == len(state['pr']) and (not state['pr'] or row > state['pr'][-1]) and size == 4
        if ok:
            kind = expected[2]
            store = expected[1] if kind == 1 else state['pr_values'].get(expected[1]) if kind == 2 else None
            ok = write == (kind in (1, 2)) and (not write or value == store)
        if not ok:
            state['refused'] = [hex(address), size, int(write), hex(value) if write else None, stage]
            uc.emu_stop()
            return
        window = row >= 5
        daif = (u.reg_read(UC_ARM64_REG_PSTATE) >> 6) & 15
        assert u.reg_read(UC_ARM64_REG_VBAR_EL1) == (vectors if window else image.address('_freertos_vector_table')), row
        assert daif == 15 or not window, (row, daif)
        if not state['pr']:
            assert state['rw_translated'][-5:] == [[WRAPPER_PAGE, 0], [WRAPPER_PAGE, 1], [SMC0_PAGE, 0],
                                                   [SMC0_PAGE, 1], [0x1800000, 0]], state['rw_translated']
            output = ''.join(state['messages'])
            assert state['snapshot_reads'] == SNAPSHOT * 2, state['snapshot_reads']
            for text in ('GPU SNAP n=1 a=0x2002b24', 'GPU power phase=', 'GPU CORE entry cycle=1', 'GPU PROBE core6 on='):
                assert text in output, (text, output)
        line = (f'GPU PROBE writing n={stage} a={address:#x} v={value:#x}\n' if write
                else f'GPU PROBE reading n={stage} a={address:#x}\n')
        assert state['messages'][-1] == line, (state['messages'][-1], line)
        if case == 'pr-after-differs' and row == 10:
            u.mem_write(0x1800020, bytes(4))
        if case == 'pr-restore-ignored' and row == 12:
            u.mem_write(0x01880014, bytes(4))
        if not write:
            state['pr_values'][row] = int.from_bytes(u.mem_read(address, 4), 'little')
        state['pr'].append(row)
        state['pr_accesses'].append([row, hex(address), int(write), hex(value) if write else hex(state['pr_values'][row])])
        if case == 'pr-sync-abort' and row == 9:
            # The second identity read takes a synchronous external abort.
            state['abort'] = True
            uc.emu_stop()

    def read(uc, access, address, size, value, arg):
        state['gpu_reads'].append([hex(address), size])
        assert not power_only, 'power-only variant read the GPU register window'
        assert not wrapper, 'wrapper-read variant read the GPU core window'
        # Two 32-bit loads per cycle, the high word first, as the vendor reads it.
        assert size == 4 and address == (0x1800024, 0x1800020)[(len(state['gpu_reads']) - 1) % 2], \
            (hex(address), size, state['gpu_reads'])

    def snapshot_read(uc, access, address, size, value, arg):
        assert size == 4 and address in SNAPSHOT, (hex(address), size)
        state['snapshot_reads'].append(address)

    def snapshot_write(uc, access, address, size, value, arg):
        raise AssertionError(f'write to a snapshot page at {address:#x}')

    for page in SNAPSHOT_PAGES:
        u.hook_add(UC_HOOK_MEM_READ, snapshot_read, begin=page, end=page + 0xfff)
        u.hook_add(UC_HOOK_MEM_WRITE, snapshot_write, begin=page, end=page + 0xfff)

    def write(uc, access, address, size, value, arg):
        state['gpu_writes'].append([address, size, value])
        raise AssertionError('GPU write in never-booted identity consumer')

    if probe:
        for page in (WRAPPER_PAGE, SMC0_PAGE, 0x1800000):
            u.hook_add(UC_HOOK_MEM_READ, pr_access, begin=page, end=page + 0xfff)
            u.hook_add(UC_HOOK_MEM_WRITE, pr_access, begin=page, end=page + 0xfff)
        # platform_init installed the image's vector table before the client ran.
        u.reg_write(UC_ARM64_REG_VBAR_EL1, image.address('_freertos_vector_table'))
        u.emu_start(image.address('gpu_power_id_client'), 0, count=400000)
        if state.get('abort'):
            # Exception entry to the current-EL SP_ELx synchronous slot, with
            # the masked DAIF the window set.
            state['in_handler'], state['handler_writes'] = True, []
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_VBAR_EL1) + 0x200)
            u.emu_start(u.reg_read(UC_ARM64_REG_VBAR_EL1) + 0x200, 0, count=10000)
        return check_probe(u, state, case, ''.join(state['messages']))
    u.hook_add(UC_HOOK_MEM_READ, read, begin=0x1800000, end=0x1800fff)
    u.hook_add(UC_HOOK_MEM_WRITE, write, begin=0x1800000, end=0x1800fff)
    u.hook_add(UC_HOOK_MEM_READ, wrapper_read, begin=WRAPPER_PAGE, end=WRAPPER_PAGE + 0xfff)
    u.hook_add(UC_HOOK_MEM_WRITE, write, begin=WRAPPER_PAGE, end=WRAPPER_PAGE + 0xfff)
    u.emu_start(image.address('gpu_power_id_client'), 0, count=100000)
    output = ''.join(state['messages'])
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
            for line in ['GPU CORE entry cycle=1 seen=255 policy=0x8 status=0x0 pwcr=0x101',
                         'GPU CORE inputs cycle=1 idr0=0x31001 misr=0x0 misr_late=0x100',
                         'GPU CORE after cycle=1 pwcr_status=0x0 policy_status=0x8',
                         'GPU clock cycle=1 pll=0xf9105300 lock1_reads=3 lock2_reads=3 update1_reads=1 '
                         'update2_reads=1',
                         'GPU CORE entry cycle=2 seen=255 policy=0x0 status=0x0 pwcr=0x0',
                         'GPU CORE inputs cycle=2 idr0=0x31001 misr=0x0 misr_late=0x100',
                         'GPU CORE after cycle=2 pwcr_status=0x0 policy_status=0x8',
                         'GPU clock cycle=2 pll=0xf9105300 lock1_reads=3 lock2_reads=3 update1_reads=1 '
                         'update2_reads=1',
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
    if case in ['normal', 'unexpected-id']:
        # Two cycles, each reading the identity as two 32-bit loads, high word first.
        assert state['gpu_reads'] == [['0x1800024', 4], ['0x1800020', 4]] * 2, state['gpu_reads']
        for n in (1, 2):
            assert f'GPU identity cycle={n} raw={(EXPECTED + (case == "unexpected-id")):#x} ' in output, output
        assert f'matches={0 if case == "unexpected-id" else 1}' in output
        assert state['calls'].index('bb_power_park') < len(state['calls']) - 1
        if case == 'normal':
            assert state['exit_status'] == 0 and state['failure'] is None
            assert 'COMPLETE GPU power identity: 36.56.104.183 in two ON/OFF cycles, provider parked' in output
            assert state['calls'].index('a733_gpu_platform_off_poll') < state['calls'].index('bb_power_park') < \
                last_put(state)
        else:
            assert state['failure'] == 'GPU identity not 36.56.104.183' and 'COMPLETE' not in output, state
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
        assert state['cycles'] == 2
    return state


def check_probe(u, state, case, output):
    """The probe-read variant's outcome for one case."""
    calls = state['calls']
    assert 'a733_gpu_platform_off_begin' not in calls and 'bb_power_park' not in calls, calls
    if case == 'pr-table-mutant':
        assert state.get('refused') == ['0xa0000b0', 4, 1, '0x3', 6] and state['pr'] == list(range(5)), state
        return state
    if case == 'pr-restore-mutant':
        assert state.get('refused') == ['0xa0000b0', 4, 1, '0x1', 14] and state['pr'] == list(range(13)), state
        return state
    assert 'refused' not in state, state['refused']
    rows = PROBE_PATHS[case]
    assert state['pr'] == rows, state['pr']
    assert state['failure'] == PROBE_REASONS[case], (state['failure'], output)
    magic, stage, count, _ = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
    values = struct.unpack('<15I', bytes(u.mem_read(PROGRESS + 32, 60)))
    if case != 'pr-time-budget':
        # The aborted read is the access in flight: named, not completed.
        done = len(rows) - (case == 'pr-sync-abort')
        assert magic == PROBE_MAGIC and stage == (rows[-1] + 1 if rows else 0) and count == done, (stage, count)
    high, low = (0, 0) if case == 'pr-bad-id' else (EXPECTED >> 32, EXPECTED & 0xffffffff)
    if case == 'pr-sync-abort':
        # The restore vectors wrote both words back, drm_ctrl first, before
        # the image's own handler ran; no other path was taken.
        assert state.get('exception') and state['handler_writes'] == [['0x1880014', '0x1'], ['0xa0000b0', '0x5']], state
        assert int.from_bytes(u.mem_read(0x01880014, 4), 'little') == 1
        assert int.from_bytes(u.mem_read(0x0a0000b0, 4), 'little') == 5
        assert state['exit_status'] is None and 'COMPLETE' not in output
        return state
    if rows and case != 'pr-window-refused':
        read = state['pr_values']
        after_low = 0 if case == 'pr-after-differs' else low
        restored_drm = 0 if case == 'pr-restore-ignored' else 1
        assert [read[r] for r in (0, 1, 2, 3, 4, 6, 8, 9, 10, 12, 14)] == \
            [1, 0, 5, high, low, 2, 0, high, after_low, restored_drm, 5], read
        assert [r[3] for r in state['pr_accesses'] if r[2]] == ['0x2', '0x0', '0x1', '0x5'], state['pr_accesses']
        assert values == (1, 0, 5, high, low, 2, 2, 0, 0, high, after_low, 1, restored_drm, 5, 5), values
        assert 'GPU PROBE window serror_pending=0' in output, output
        good = case in ('pr-board', 'pr-core-on')
        assert f'GPU PROBE result before={int(case != "pr-bad-id")} after={int(case not in ("pr-bad-id", "pr-after-differs"))} ' \
               f'restored={int(case != "pr-restore-ignored")} accesses=15' in output, output
    snapshots = 1 if case == 'pr-time-budget' else 2
    assert state['snapshot_reads'] == SNAPSHOT * snapshots, len(state['snapshot_reads'])
    if case == 'pr-window-refused':
        # Refused after the first identity read, before any write.
        assert state['exit_status'] == 7 and 'FAIL GPU probe read window refused' in output, output
        assert 'GPU PROBE window refused now_us=1600' in output and 'monitor_deadline_us=2000000' in output, output
    elif case in ('pr-board', 'pr-core-on'):
        assert state['exit_status'] == 0 and PROBE_COMPLETE in output, output
    else:
        assert state['exit_status'] is None and 'COMPLETE' not in output
    if case == 'pr-time-budget':
        assert 'a733_gpu_platform_begin' not in calls
    state['cycles'] = 0
    return state


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cases', nargs='+', help='Resume selected cases after a fixture-only failure')
    p.add_argument('--power-only', action='store_true', help='check the power-only variant')
    p.add_argument('--wrapper-read', action='store_true', help='check the wrapper-read variant')
    p.add_argument('--probe-read', action='store_true', help='check the probe-read variant')
    a = p.parse_args()
    assert a.power_only + a.wrapper_read + a.probe_read <= 1, 'choose one variant'
    # The shared owner added delays_ready in existing padding. Reused objects
    # retain the old DWARF member list; consume the actual replacement owner's
    # layout, and verify every old member and the total size still agree.
    image = harness.Image(a.image, layout_names=[n for n in NAMES if n != 'a733_power_domain'])
    # The variant is a compile option of main.c: only the identity variant
    # defines the identity read counter.
    identity = bool(image.symbols.get('id_reads', set()) - {0})
    wrapper_image = bool(image.symbols.get('wrapper_words', set()) - {0})
    probe_image = bool(image.symbols.get('probe_steps', set()) - {0})
    assert probe_image == a.probe_read, 'image variant does not match --probe-read'
    assert identity == (not a.power_only and not a.wrapper_read and not a.probe_read), \
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
    cases = ['normal', 'unexpected-id', 'on-fault', 'bad-mapping',
             'read-crosses-deadline', 'clock-regresses', 'off-fault', 'retained-token', 'counted-row', 'unparked', 'core-refused', 'core-partial']
    if a.power_only:
        cases = ['normal', 'on-fault', 'second-on-fault', 'on-deadline', 'snapshot-mapping', 'core-refused', 'core-partial',
                 'off-fault', 'retained-token', 'counted-row', 'unparked']
    if a.wrapper_read:
        cases = ['wrapper-platform-deadline', 'wrapper-client-deadline', 'wrapper-on', 'wrapper-early-stop',
                 'wrapper-mapping', 'wrapper-off-fault', 'wrapper-table-mutant']
    if a.probe_read:
        cases = [*PROBE_PATHS, 'pr-table-mutant', 'pr-restore-mutant']
    if a.cases:
        assert set(a.cases) <= set(cases)
        cases = a.cases
    result = {'physical_access': False,
              'variant': ('power-only' if a.power_only else 'wrapper-read' if a.wrapper_read
                          else 'probe-read' if a.probe_read else 'identity'),
              'scope': 'actual ELF client and ID access; modeled provider/power transitions',
              'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
              'cases': []}
    for case in cases:
        try:
            result['cases'].append(run(image, case, a.power_only, a.wrapper_read, a.probe_read))
        except Exception as error:
            result['failed_case'] = case
            result['fixture_error'] = repr(error)
            a.output.write_text(json.dumps(result, indent=2) + '\n')
            raise
        a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(cases)} focused client cases passed; no physical access')


if __name__ == '__main__':
    main()
