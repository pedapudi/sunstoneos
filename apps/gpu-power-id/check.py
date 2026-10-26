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
restores both written words before the image's own handler runs. With
--fw-start the image must be the firmware-start variant: the firmware
preparation, the start controller and the shared-memory adapter run as
linked, and every wrapper, SMC0 and GPU access is the next 32-bit access of
the case's expected sequence, after its progress record and log line. With
--fw-stop the image must be the firmware-stop variant: the same start, then
the control, the stop, the OFF and the park, with a model of the firmware's
side of the kernel CCB, the power-sync word and the interrupt status; after
the start every access must be an interrupt read or clear, a kick with its
read-back, or the next stop step. With --fw-job the image must be the
first-job variant: the firmware-stop variant with the compute job between
the health check and the stop, and a model of the firmware and GPU that
runs the job's client CCB commands. Without a
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
# pr-board: CORE6 reaches 8, every access, then OFF, supply end and park.
# pr-on-deadline: CORE6 does not reach 8 within 200 ms; no GPU-window access.
# pr-off-fault: every access, then the OFF fails.
PROBE_PATHS = {'pr-board': PROBE_ALL, 'pr-on-deadline': [], 'pr-bad-id': PROBE_ALL, 'pr-after-differs': PROBE_ALL,
               'pr-restore-ignored': PROBE_ALL, 'pr-sync-abort': list(range(10)), 'pr-early-stop': [],
               'pr-off-fault': PROBE_ALL, 'pr-mapping': [], 'pr-time-budget': [], 'pr-window-refused': list(range(5))}
PROBE_FAIL = 'GPU probe read: identity or restore not as expected, GPU left powered'
PROBE_REASONS = {'pr-board': None, 'pr-on-deadline': 'GPU ON deadline', 'pr-bad-id': PROBE_FAIL,
                 'pr-after-differs': PROBE_FAIL,
                 'pr-restore-ignored': PROBE_FAIL, 'pr-sync-abort': None, 'pr-early-stop': 'GPU ON poll',
                 'pr-off-fault': 'GPU OFF poll',
                 'pr-mapping': 'GPU probe read mapping', 'pr-time-budget': 'GPU probe read time budget',
                 'pr-window-refused': None}
PROBE_COMPLETE = ('COMPLETE GPU probe read: identity 36.56.104.183 before and after the secure configuration, '
                  'restored, OFF and provider parked')
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
            # The board's ON: CORE6 status 8 at the third poll; pr-on-deadline
            # never reaches it, pr-early-stop is refused at admission.
            early, on = case == 'pr-early-stop', case != 'pr-on-deadline' and state['on_polls'] >= 3
            field(power, 'a733_gpu_power', 'core_seen', 0x31f if early else 0xfff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 10 if early else 57)
            notes(0x10130101, 0, 0, 8 if on else 0, 0x100 if on else 0)
            field(gpu, 'a733_gpu_platform', 'phase', 7 if early else 3 if on else 2)
            field(gpu, 'a733_gpu_platform', 'token', 17, 8)
            field(gpu, 'a733_gpu_platform', 'delegated', 1)
            field(power, 'a733_gpu_power', 'phase', 5 if early else 2 if on else 1)
            field(power, 'a733_gpu_power', 'last_value', 8 if on else 0)
            ret((1 << 64) - 1 if early else 1 if on else 0)
        elif name == 'platform_exception':
            state['exception'] = True
            u.emu_stop()
        elif name == 'a733_gpu_platform_poll' and wrapper:
            state['on_polls'] += 1
            if case in ('wrapper-on', 'wrapper-off-fault'):
                field(power, 'a733_gpu_power', 'core_seen', 0xfff)
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
            # clock steps (seen 0xfff) and a final poll that reads 0.
            early = case == 'wrapper-early-stop'
            field(power, 'a733_gpu_power', 'core_seen', 0x31f if early else 0xfff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 10 if early else 57)
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
                    # PWCR reads and the two gate notes (seen 0x31f) and stops at the admission step 10.
                    field(power, 'a733_gpu_power', 'core_seen', 0x31f if case == 'core-refused' else 1)
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
            field(power, 'a733_gpu_power', 'core_seen', 0xfff)
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
            if case in ('off-fault', 'wrapper-off-fault', 'pr-off-fault'):
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
        elif name == 'a733_gpu_platform_supply_end':
            # The supply consumer's completed DCDC4 sample ends here.
            ret(1)
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
               'a733_gpu_platform_off_begin', 'a733_gpu_platform_off_poll', 'a733_gpu_platform_supply_end',
               'bb_power_park',
               'a733_power_service_idle', 'bb_power_fail', 'platform_puts', 'platform_exit']
    if probe:
        # The probe-read image makes no OFF, so its link drops the OFF calls.
        modeled = [n for n in modeled if image.symbols.get(n, set()) - {0}] + ['platform_exception']
    elif wrapper:
        modeled = [n for n in modeled if n != 'a733_gpu_platform_supply_end']
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
            for line in ['GPU CORE entry cycle=1 seen=4095 policy=0x8 status=0x0 pwcr=0x101',
                         'GPU CORE inputs cycle=1 idr0=0x31001 misr=0x0 misr_late=0x100',
                         'GPU CORE after cycle=1 pwcr_status=0x0 policy_status=0x8',
                         'GPU clock cycle=1 pll=0xf9105300 lock1_reads=3 lock2_reads=3 update1_reads=1 '
                         'update2_reads=1',
                         'GPU CORE entry cycle=2 seen=4095 policy=0x0 status=0x0 pwcr=0x0',
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
            assert state['calls'].index('a733_gpu_platform_off_poll') < \
                state['calls'].index('a733_gpu_platform_supply_end') < state['calls'].index('bb_power_park') < \
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
        assert 'GPU CORE entry' in output and f'seen={799 if case == "core-refused" else 1}' in output
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


# The firmware-start variant (fw-start.c). Rows of its fws_rows table:
# address, value, kind (0 read32, 1 write32).
FWS_ROWS = [(0x01800024, 0, 0), (0x01800020, 0, 0), (0x01880014, 0, 0), (0x0a0000b0, 0, 0),
            (0x0a0000b0, 2, 1), (0x0a0000b0, 0, 0), (0x01880014, 0, 1), (0x01880014, 0, 0)]
FWS_MAGIC = 0x3130535746555047
GPU = 0x01800000
GPU_PAGES = (GPU, GPU + 0x3000, GPU + 0xa000)
NC_BASE, NC_PAGES = 0x41c00000, 45
# Shared resources in the order of enum pvr_fw_resource, in pages; the
# utilization buffer is resource 10 and system init the last.
NC_ALLOCATION = [1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 1, 12, 12, 1, 1, 1, 1]
UTILIZATION = NC_BASE + 4096 * sum(NC_ALLOCATION[:10])
SYSTEM_INIT = NC_BASE + 4096 * sum(NC_ALLOCATION[:18])
STARTED = SYSTEM_INIT + 352
# The modeled CR timer: bit 31 copied into bit 63.
TIMER = (0x80001234, 0x87654321)
# The 29-step start of pvr_gpu_start.c for the pinned firmware: its register
# accesses as the reference comparison verified them
# (evidence/gpu-start-20261025/trace-0.json, without SOFT_RESET2), with
# None for the four boot addresses the image's own MIPS plan supplies.
# Each is (write, offset, width, value).
FWS_START = [(1, 0xa100, 4, 0), (0, 0xa100, 4, None),
             (1, 0x100, 8, 0xffeffffffffffc3d), (0, 0x100, 8, None),
             (1, 0x100, 8, 0xffeffe071ffffc3d), (0, 0x100, 8, None),
             (1, 0x100, 8, 0x100000000), (0, 0x100, 8, None),
             (0, 0x3800, 4, None), (1, 0x3800, 4, 0x200001),
             (1, 0x810, 4, 0x1cf80), (1, 0x818, 8, 0x1fc00001), (1, 0x820, 8, 'code'), (1, 0x858, 8, 1),
             (1, 0x860, 8, 'code'), (1, 0x828, 8, 0x1fc01001), (1, 0x830, 8, 'data'), (1, 0x838, 8, 0x1fc02001),
             (1, 0x840, 8, 'exceptions'), (1, 0xb50, 8, 0), (1, 0x8c8, 4, 0), (1, 0x38c0, 8, 0x8882af00),
             (1, 0x100, 8, 0), (0, 0x100, 8, None)]
FWS_AFTER = [(0x100, 8), (0x810, 4), (0x818, 8), (0x820, 8), (0x858, 8), (0x860, 8), (0x828, 8),
             (0x830, 8), (0x838, 8), (0x840, 8), (0xb50, 8), (0x8c8, 4)]
FWS_COMPLETE = ('COMPLETE GPU firmware start: started word 1 after the timer record and the secure configuration, '
                'GPU left running for the power removal')
FWS_REASONS = {'fs-board': None, 'fs-timer-wrap': None, 'fs-timeout': 'GPU firmware start: started word not 1, GPU left running',
               'fs-stale': 'GPU firmware start: started word not 1, GPU left running',
               'fs-bad-flag': 'GPU firmware start: started word not 1, GPU left running',
               'fs-on-deadline': 'GPU ON deadline',
               'fs-bad-id': 'GPU firmware start: identity not 36.56.104.183, nothing written, GPU left powered',
               'fs-refused': None, 'fs-mapping': 'GPU firmware start mapping',
               'fs-memory': 'GPU firmware start preparation, nothing powered', 'fs-row-mutant': None}


# The firmware-stop variant (fw-start.c built with FWS_STOP). Shared
# resources it reads besides system init: the queues, the power-sync word,
# system data, OS data and OS init, each at the first page of its allocation.
def nc_page(resource):
    return NC_BASE + 4096 * sum(NC_ALLOCATION[:resource])


HOST_CONTROL, HOST_COMMANDS, RETURNS, FW_CONTROL, FW_COMMANDS = (nc_page(i) for i in range(5))
POWER_SYNC, SYSTEM_DATA, OS_DATA, OS_INIT = nc_page(5), nc_page(8), nc_page(15), nc_page(17)
MAGIC = 0x2abc0000
STOP_PAGE = GPU + 0x1000
FWX_COMPLETE = ('COMPLETE GPU firmware stop: started, health check executed, forced idle and power off '
                'acknowledged, idle, OFF and provider parked')
FWX_CONTROL_FAIL = 'GPU firmware control: compatibility or health check not complete, GPU left running'
FWX_STOP_FAIL = 'GPU firmware stop: not complete, GPU left powered'
# Without the firmware's acknowledgements, the stop of upstream's hard reset:
# the register sequence, then the OFF when every idle poll matches.
FWX_HARD = 'GPU firmware stop: not acknowledged, idle checks met, GPU OFF and provider parked'
FWX_HARD_BUSY = 'GPU firmware stop: not acknowledged and the GPU not idle, GPU left powered'
FWX_REASONS = {'fx-board': None, 'fx-health-return': None, 'fx-incompatible': FWX_CONTROL_FAIL, 'fx-no-health': FWX_CONTROL_FAIL,
               'fx-event': FWX_CONTROL_FAIL, 'fx-denied': FWX_HARD, 'fx-no-ack': FWX_HARD,
               'fx-no-ack-busy': FWX_HARD_BUSY, 'fx-off-fault': FWX_STOP_FAIL,
               'fx-idle-held': 'GPU firmware stop: idle check not met after the power-off acknowledgement, '
                               'GPU OFF and provider parked'}
# The stop's register plan (pvr_gpu_stop.c, the vendor RGXStop) as 32-bit
# accesses: (write, offset). The interrupt clear appears only after a pending
# status; a poll repeats its read until it matches.
FWX_STOP_PLAN = [(0, 0x8a8), (1, 0x8b0), (0, 0x3c8), (0, 0x3898), (1, 0xb40), (1, 0xb30), (0, 0x1358),
                 (0, 0x1350), (0, 0x1320), (0, 0x1338), (0, 0x3874), (0, 0x3870), (0, 0x3898), (0, 0x3c8),
                 (0, 0x3c8)]


# The first-job variant (fw-start.c built with FWS_STOP and FWS_JOB): its six
# firmware objects follow the 45 shared pages, at firmware addresses
# 0xc0800000 + n * 0x10000; object 2 is the client CCB control, 3 its ring
# and 5 the timeline.
JOB_OBJECTS = [NC_BASE + 4096 * (45 + i) for i in range(6)]
JOB_ADDRESS = [0xc0800000 + i * 0x10000 for i in range(6)]
JOB_SEED, JOB_FACTOR = 0x12345678, 0x9e3779b1
JOB_WORDS = [((JOB_SEED + (i + 1) * JOB_FACTOR) ^ (0x9e3779b9 + (i << 16))) & 0xffffffff for i in range(32)]
JOB_GUARD = 0x6d35a7c1
FWJ_COMPLETE = ('COMPLETE GPU first job: 32 words as computed, context cleaned up, firmware stopped, OFF and '
                'provider parked')
FWJ_NOT_COMPLETE = 'GPU first job: not complete, GPU OFF and provider parked'
FWJ_REASONS = {'fj-board': None, 'fj-kick-word': None, 'fj-stats-event': None,
               'fj-no-fence': FWJ_NOT_COMPLETE, 'fj-wrong-output': FWJ_NOT_COMPLETE,
               'fj-guard': FWJ_NOT_COMPLETE,
               'fj-cleanup-no-word': 'GPU first job: 32 words as computed, context cleanup not confirmed, GPU OFF '
                                     'and provider parked',
               'fj-reset-event': FWJ_NOT_COMPLETE, 'fj-board-1025': FWJ_NOT_COMPLETE,
               'fj-restart-event': FWX_HARD, 'fj-kick-bad-word': FWX_HARD, 'fj-no-ack': FWX_HARD,
               'fj-not-idle': FWX_HARD_BUSY}
# Cases in which the CDM command leaves the output as the CPU filled it, as
# on port 3-1 (board-session-20261025-22, s22-gfj-run).
FWJ_NO_OUTPUT = ('fj-no-fence', 'fj-board-1025', 'fj-restart-event', 'fj-kick-bad-word', 'fj-no-ack', 'fj-not-idle')
# Cases in which the timeline is never written either.
FWJ_NO_TIMELINE = ('fj-no-fence', 'fj-no-ack', 'fj-not-idle')
# The context reset notification of the model: context id 1, reason 1
# (guilty lockup), data master 3, job reference 1, flags 1 (page fault),
# a page catalogue address and a fault address.
FWJ_RESET = struct.pack('<IIIIIIQQ', 1, 1, 3, 1, 1, 0, 0x41100000, 0x8001000000)


def split(write, offset, width, value):
    """One register access as the vendor's 32-bit accesses: OSWriteHWReg64
    writes the low word first, OSReadHWReg64 reads the high word first."""
    if width == 4:
        return [(write, GPU + offset, value)]
    if write:
        return [(1, GPU + offset, value & 0xffffffff), (1, GPU + offset + 4, value >> 32)]
    return [(0, GPU + offset + 4, None), (0, GPU + offset, None)]


def fws_expected(boot, timer_reads):
    """Every 32-bit access of a complete run, in order: (write, address, value or None)."""
    rows = [(k, a, v if k else None) for a, v, k in FWS_ROWS]
    sequence = rows[:4] + split(0, 0x160, 8, None) * timer_reads + rows[4:]
    for write, offset, width, value in FWS_START:
        if isinstance(value, str):
            value = boot[value]
        sequence += split(write, offset, width, value)
    for offset, width in FWS_AFTER:
        sequence += split(0, offset, width, None)
    return sequence


def fws_boot(u, image):
    """The image's MIPS wrapper plan (struct pvr_mips_boot): the eleven writes
    and the three boot addresses among them."""
    raw = bytes(u.mem_read(image.address('boot'), 8 + 16 * 11))
    writes = [struct.unpack_from('<IIQ', raw, 8 + 16 * i) for i in range(11)]
    return writes, {'code': writes[2][2], 'data': writes[6][2], 'exceptions': writes[8][2]}


def fws_case_sequence(case, boot, timer_reads):
    """The accesses a case may make, in order. A stale started word stops
    the controller before its first register access; the report reads follow."""
    expected = fws_expected(boot, timer_reads)
    if case == 'fs-stale':
        start = expected.index((1, GPU + 0xa100, 0))
        after = sum((split(0, o, w, None) for o, w in FWS_AFTER), [])
        return expected[:start] + after
    return expected


def run_fws(image, case, stop=False, unchecked=False, job=False):
    """One firmware-start case, or with stop a firmware-stop case. Only the
    client's platform calls are modeled: the ON and OFF, the clock,
    translations, cache maintenance and the log; for the stop variant also
    the firmware's side of the kernel CCB, the power-sync word and the
    interrupt status. The firmware preparation, the start, control and stop
    controllers and the shared-memory adapter run as linked."""
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0x41000000, 0x1200000)
    pages = GPU_PAGES + (WRAPPER_PAGE, SMC0_PAGE) + ((STOP_PAGE,) if stop else ())
    for page in pages + SNAPSHOT_PAGES:
        u.mem_map(page, 0x1000)
    for address, data in image.parts:
        u.mem_write(address, data)
    for address in SNAPSHOT:
        u.mem_write(address, (address ^ 0x5a5a0000).to_bytes(4, 'little'))
    raw = EXPECTED if case != 'fs-bad-id' else EXPECTED + 1
    u.mem_write(0x1800020, raw.to_bytes(8, 'little'))
    # The board's entry values (board-session-20261025-14): drm_ctrl 1,
    # irq_ctrl 0, SMC0 +0xb0 0.
    u.mem_write(0x01880014, (1).to_bytes(4, 'little'))
    timer_reads = 2 if case == 'fs-timer-wrap' else 1
    if case == 'fs-row-mutant':
        # A seeded mutant: the SMC0 write stores 3, which the check must refuse.
        u.mem_write(image.address('fws_rows') + 4 * 12 + 4, (3).to_bytes(4, 'little'))
    u.reg_write(UC_ARM64_REG_SP, 0x421f0000)
    L = image.layouts
    gpu, bb = image.address('gpu'), image.address('bb_power')
    power = gpu + L['a733_gpu_platform']['power']
    deadline = 1500000
    state = {'case': case, 'calls': [], 'time': 100, 'messages': [], 'exit_status': None, 'failure': None,
             'accesses': [], 'on_polls': 0, 'snapshot_reads': [], 'started_reads': 0, 'released': None,
             'cache': [], 'record_at': None, 'gpu_reads': [], 'gpu_writes': [], 'steps': [],
             'commands': [], 'irq_reads': [], 'client_commands': []}
    row = power + L['a733_gpu_power']['row']
    consumer = gpu + L['a733_gpu_platform']['pmic']
    domains = bb + L['bb_power']['domains']
    domain5 = domains + L['a733_power_domains']['domain'] + 2 * 32

    def put(address, value, size=4):
        u.mem_write(address, value.to_bytes(size, 'little'))

    def field(base, typ, name, value, size=4):
        put(base + L[typ][name], value, size)

    field(bb, 'bb_power', 'deadline', deadline, 8)
    if stop:
        # The OFF proof's entry state, as the power-only check models it.
        field(row, 'a733_power_domain_row', 'domains', domains, 8)
        field(row, 'a733_power_domain_row', 'domain', 5)
        field(consumer, 'a733_pmic_consumer', 'rail', 32, 8)
        field(domain5, 'a733_power_domain', 'id', 5)
        # Idle status words that the stop polls (SIDEKICK_IDLE, SLC_IDLE);
        # the BIF and SLC status words read 0.
        put(GPU + 0x3c8, 0x7f)
        put(GPU + 0x3898, 0xff)
        if case == 'fx-idle-held':
            put(GPU + 0x1358, 1)
        if case in ('fx-no-ack-busy', 'fj-not-idle'):
            # SIDEKICK_IDLE with a unit busy: the first idle poll never matches.
            put(GPU + 0x3c8, 0x07)

    def ret(value=1):
        u.reg_write(UC_ARM64_REG_X0, value)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    def text(address):
        value = bytearray()
        while (byte := u.mem_read(address + len(value), 1)[0]):
            value.append(byte)
        return value.decode()

    def call(name):
        state['calls'].append(name)
        if name == 'bb_power_now':
            # The started-word wait spins on the clock; the clock runs 25 us
            # per read once the firmware processor is released.
            state['time'] += 25 if state['released'] is not None else 1
            if case == 'fs-refused' and len(state['accesses']) == 4:
                state['time'] = max(state['time'], 450000)
            ret(state['time'])
        elif name == 'vTaskDelay':
            state['time'] += 1000
            ret()
        elif name == 'a733_gpu_platform_poll':
            state['on_polls'] += 1
            state['time'] += 5000
            on = case != 'fs-on-deadline' and state['on_polls'] >= 3
            field(power, 'a733_gpu_power', 'core_seen', 0xfff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 57)
            field(gpu, 'a733_gpu_platform', 'phase', 3 if on else 2)
            field(power, 'a733_gpu_power', 'phase', 2 if on else 1)
            field(power, 'a733_gpu_power', 'last_value', 8 if on else 0)
            ret(1 if on else 0)
        elif name == 'aarch64_device_translate':
            address, write, target = (u.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
            page = address & ~0xfff
            if NC_BASE <= address < NC_BASE + 0x200000:
                attributes = 0xff if case == 'fs-memory' and page == STARTED & ~0xfff else 0x44
            elif 0x41000000 <= address < NC_BASE:
                attributes = 0xff
            else:
                assert page in pages + SNAPSHOT_PAGES, hex(address)
                attributes = 0
            wrong = case == 'fs-mapping' and page == SMC0_PAGE
            u.mem_write(target, struct.pack('<QII', address + (4096 if wrong else 0), attributes, 3))
            ret()
        elif name == 'aarch64_device_cache_stride':
            ret(64)
        elif name == 'aarch64_device_cache':
            state['cache'].append([u.reg_read(UC_ARM64_REG_X0), u.reg_read(UC_ARM64_REG_X1),
                                   u.reg_read(UC_ARM64_REG_X2), len(state['accesses'])])
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
        elif name == 'platform_exception':
            state['exception'] = True
            u.emu_stop()
        elif name == 'a733_gpu_platform_off_poll':
            if case == 'fx-off-fault':
                ret((1 << 64) - 1)
                return
            field(gpu, 'a733_gpu_platform', 'phase', 6)
            field(gpu, 'a733_gpu_platform', 'token', 0, 8)
            field(gpu, 'a733_gpu_platform', 'delegated', 0)
            field(power, 'a733_gpu_power', 'phase', 4)
            field(row, 'a733_power_domain_row', 'state', 3)
            field(row, 'a733_power_domain_row', 'counted', 0)
            field(domain5, 'a733_power_domain', 'state', 1)
            ret()
        elif name == 'bb_power_park':
            field(bb, 'bb_power', 'parked', 1)
            ret()
        else:
            ret()

    modeled = ['bb_power_wait_ready', 'bb_power_now', 'vTaskDelay', 'a733_gpu_platform_begin',
               'a733_gpu_platform_poll', 'aarch64_device_translate', 'aarch64_device_cache_stride',
               'aarch64_device_cache', 'bb_power_fail', 'platform_puts', 'platform_exit', 'platform_exception']
    if stop:
        modeled += ['a733_gpu_platform_off_begin', 'a733_gpu_platform_off_poll', 'a733_gpu_platform_supply_end',
                    'bb_power_park', 'a733_power_service_idle']
    for name in modeled:
        address = image.address(name)
        u.hook_add(UC_HOOK_CODE, lambda uc, a, size, n: call(n), name, begin=address, end=address)

    def access(uc, kind, address, size, value, arg):
        """One wrapper, SMC0 or GPU access: a 32-bit access named by its
        progress record and log line, then modeled."""
        write = kind == UC_MEM_WRITE
        n = len(state['accesses']) + 1
        magic, named, count, at, stored, step = struct.unpack('<6Q', bytes(u.mem_read(PROGRESS, 48)))
        line = (f'GPU FWS writing n={n} a={address:#x} v={value:#x}\n' if write
                else f'GPU FWS reading n={n} a={address:#x}\n')
        # In the stop variant a read that repeats the previous read is a poll,
        # named by the progress record alone: since that read, at most its
        # changed value was printed.
        previous = state['accesses'][-1] if state['accesses'] else None
        since = state['messages'][state.get('printed', 0):]
        quiet = stop and not write and previous == [0, address, None] and \
            not any(m.startswith(('GPU FWS reading', 'GPU FWS writing', 'GPU FWS n=')) and
                    not m.startswith(f'GPU FWS n={n - 1} v=') for m in since)
        if not (size == 4 and magic == FWS_MAGIC and named == n and count == n - 1 and at == address and
                (not write or stored == value) and (quiet or (state['messages'] and state['messages'][-1] == line))):
            state['refused'] = [hex(address), size, int(write), hex(value) if write else None, n]
            uc.emu_stop()
            return
        if 'expected' not in state:
            state['expected'] = fws_case_sequence(case, fws_boot(u, image)[1], timer_reads)
        if stop and n > len(state['expected']):
            # After the start: the control's interrupt status and clear, the
            # kernel CCB kick with its read-back, and the stop's registers.
            last = state['accesses'][-1]
            kick = last[0] == 1 and last[1] == GPU + 0xb00
            ok = (address == GPU + 0xb00 and not write) if kick else \
                (not write and address == GPU + 0x8a8) or \
                (write and address == GPU + 0x8b0 and value == 1 and state['irq_reads'] and state['irq_reads'][-1] & 1) or \
                (write and address == GPU + 0xb00 and value == 0 and not 100 <= step < 200) or \
                (100 <= step < 200 and (int(write), address - GPU) in FWX_STOP_PLAN and (not write or value == 0 or
                                                                                    address == GPU + 0x8b0))
            if not ok:
                state['refused'] = [hex(address), size, int(write), hex(value) if write else None, n]
                uc.emu_stop()
                return
        else:
            expected = state['expected'][n - 1] if n <= len(state['expected']) else None
            if expected is None or expected[:2] != (int(write), address) or \
                    (expected[2] is not None and expected[2] != value):
                state['refused'] = [hex(address), size, int(write), hex(value) if write else None, n]
                uc.emu_stop()
                return
        (state['gpu_writes'] if write else state['gpu_reads']).append([hex(address), size])
        state['accesses'].append([int(write), address, value if write else None])
        state['steps'].append(step)
        state['printed'] = len(state['messages'])
        if stop:
            fws_firmware(u, state, case, write, address, value, put, image if job else None)
        if address == 0x0a0000b0 and not write:
            # SMC0 +0xb0 read 0 at entry and 0 right after a write of 2 on the board.
            put(address, 0)
        if address == GPU + 0x164 and not write:
            wrap = case == 'fs-timer-wrap' and state['accesses'].count([0, address, None]) == 1
            put(GPU + 0x164, TIMER[0] ^ (0x80000000 if wrap else 0))
            put(GPU + 0x160, TIMER[1])
        if address == GPU + 0x104 and write and value == 0 and \
                int.from_bytes(u.mem_read(GPU + 0x100, 4), 'little') == 0:
            state['released'] = n

    for page in pages:
        u.hook_add(UC_HOOK_MEM_READ, access, begin=page, end=page + 0xfff)
        u.hook_add(UC_HOOK_MEM_WRITE, access, begin=page, end=page + 0xfff)

    def started(uc, kind, address, size, value, arg):
        if kind == UC_MEM_WRITE:
            return
        state['started_reads'] += 1
        value = 0
        if case == 'fs-stale':
            value = 1
        elif state['released'] is not None and state['started_reads'] >= 3 and case != 'fs-timeout':
            value = 2 if case == 'fs-bad-flag' else 1
            if stop and 'booted' not in state:
                # The firmware's boot: its compatibility record at OS init
                # +40 (the fields the control checks) and power state ON.
                state['booted'] = True
                record = bytearray(64)
                struct.pack_into('<IIQIIQ', record, 0, 2 if case == 'fx-incompatible' else 3, 0,
                                 0x00240038006800b7, 3, 0, 0x00240038006800b7)
                record[56] = 1
                u.mem_write(OS_INIT + 40, bytes(record))
                put(SYSTEM_DATA + 8, 1)
        put(STARTED, value)

    u.hook_add(UC_HOOK_MEM_READ, started, begin=STARTED, end=STARTED + 3)
    u.hook_add(UC_HOOK_MEM_WRITE, started, begin=STARTED, end=STARTED + 3)

    def sequence_word(uc, kind, address, size, value, arg):
        if kind == UC_MEM_WRITE and value == 1:
            state['record_at'] = len(state['accesses'])

    u.hook_add(UC_HOOK_MEM_WRITE, sequence_word, begin=UTILIZATION + 10240, end=UTILIZATION + 10243)

    def snapshot_read(uc, kind, address, size, value, arg):
        assert size == 4 and address in SNAPSHOT, (hex(address), size)
        state['snapshot_reads'].append(address)

    for page in SNAPSHOT_PAGES:
        u.hook_add(UC_HOOK_MEM_READ, snapshot_read, begin=page, end=page + 0xfff)
    u.reg_write(UC_ARM64_REG_VBAR_EL1, image.address('_freertos_vector_table'))
    u.emu_start(image.address('gpu_power_id_client'), 0, count=60000000)
    if unchecked:
        return state
    if job:
        return check_fwj(u, image, state, case, timer_reads)
    return check_fwx(u, image, state, case, timer_reads) if stop else check_fws(u, image, state, case, timer_reads)


def reproduce_job(image, log, case):
    """Run one firmware model case on a first-job image and compare the
    control, job and failure lines with a board kernel.log."""
    state = run_fws(image, case, True, True, True)
    model = ''.join(state['messages']).splitlines()
    model += ['FAIL ' + state['failure']] if state['failure'] else []
    board = Path(log).read_text().splitlines()
    import re
    strip = lambda line: re.sub(r' waited_us=\d+', '', line)
    compared = {}
    for prefix in ('GPU FWS start result=', 'GPU FWS control result=', 'GPU FWS compat ', 'GPU FWS kccb ',
                   'GPU FWJ job ', 'GPU FWJ output ', 'GPU FWJ cleanup ', 'FAIL '):
        ours = [strip(l) for l in model if l.startswith(prefix)]
        theirs = [strip(l) for l in board if l.startswith(prefix)]
        compared[prefix.strip()] = {'model': ours, 'board': theirs, 'equal': ours == theirs}
    return {'case': case, 'failure': state['failure'], 'exit_status': state['exit_status'], 'lines': compared,
            'equal': all(v['equal'] for v in compared.values())}


def reproduce(image, log):
    """Run the board's firmware model (fx-board) on a firmware-stop image and
    compare the control and failure lines with a board kernel.log. The
    waited time differs between the model's clock and the board's."""
    state = run_fws(image, 'fx-board', True, True)
    model = ''.join(state['messages']).splitlines()
    # bb_power_fail prints its FAIL line on the board; the model records it.
    model += ['FAIL ' + state['failure']] if state['failure'] else []
    board = Path(log).read_text().splitlines()
    import re
    strip = lambda line: re.sub(r' waited_us=\d+', '', line)
    compared = {}
    for prefix in ('GPU FWS start result=', 'GPU FWS control result=', 'GPU FWS compat ', 'FAIL '):
        ours = [strip(l) for l in model if l.startswith(prefix)]
        theirs = [strip(l) for l in board if l.startswith(prefix)]
        compared[prefix.strip()] = {'model': ours, 'board': theirs, 'equal': ours == theirs}
    return {'failure': state['failure'], 'exit_status': state['exit_status'], 'lines': compared,
            'equal': all(v['equal'] for v in compared.values())}


def fws_job_gpu(u, state, case, image, kick, put):
    """The firmware and GPU for one client KICK: run the client CCB commands
    from the control's read offset to the KICK's write offset. The CDM
    command writes the 32 result words, by the job's formula, into the
    output that the job's own descriptor names, between its guards; the
    UPDATE command writes its value to the timeline its payload names."""
    word = lambda a: int.from_bytes(u.mem_read(a, 4), 'little')
    context, offset, mask = struct.unpack_from('<III', kick, 8)
    assert context == JOB_ADDRESS[0] and mask == 1023, (hex(context), mask)
    read = word(JOB_OBJECTS[2] + 4)
    while read != offset:
        kind, size = struct.unpack('<II', bytes(u.mem_read(JOB_OBJECTS[3] + read, 8)))
        payload = bytes(u.mem_read(JOB_OBJECTS[3] + read + 40, size))
        state['client_commands'].append(kind - MAGIC)
        if kind == MAGIC | 0x8000 | 205 and case not in FWJ_NO_OUTPUT:
            output = struct.unpack('<Q', bytes(u.mem_read(image.address('job_backing') + 5 * 32, 8)))[0]
            for i, value in enumerate(JOB_WORDS):
                if case == 'fj-wrong-output' and i == 7:
                    value ^= 1
                put(output + 64 + 4 * i, value)
            if case == 'fj-guard':
                put(output + 64 + 128, 0)
        elif kind == MAGIC | 213 and case not in FWJ_NO_TIMELINE:
            address, value = struct.unpack_from('<II', payload)
            assert address == JOB_ADDRESS[5], hex(address)
            put(JOB_OBJECTS[5], value)
        read = (read + 40 + size) & 1023
    put(JOB_OBJECTS[2] + 4, read)


def fws_firmware(u, state, case, write, address, value, put, image=None):
    """The firmware's side of one register access of the stop variant: a
    kernel CCB kick runs every pending command, advances the read offset,
    counts it executed and raises the interrupt; a health check leaves the
    power state IDLE (3), a power request sets its power state and the
    power-sync word. As on port 3-1 (board-session-20261025-18, s18-gfx-run)
    no return word is written, except in fx-health-return, which writes 1
    for every command. Clearing the interrupt event clears the status."""
    if not write and address == GPU + 0x8a8:
        state['irq_reads'].append(int.from_bytes(u.mem_read(address, 4), 'little'))
    if write and address == GPU + 0x8b0:
        put(GPU + 0x8a8, 0)
    if not (write and address == GPU + 0xb00):
        return
    word = lambda a: int.from_bytes(u.mem_read(a, 4), 'little')
    host_write, read = word(HOST_CONTROL), word(HOST_CONTROL + 128)
    while read != host_write:
        command = bytes(u.mem_read(HOST_COMMANDS + read * 88, 88))
        kind, pow_type, request = struct.unpack_from('<I4xII', command)
        state['commands'].append([kind - MAGIC, pow_type, request])
        if kind == MAGIC | 115 and case == 'fx-no-health':
            return  # Never consumed.
        if kind == MAGIC | 101:
            fws_job_gpu(u, state, case, image, command, put)
            if case == 'fj-stats-event':
                u.mem_write(FW_COMMANDS, struct.pack('<I', MAGIC | 107) + bytes(84))
                put(FW_CONTROL, 1)
            if case in ('fj-reset-event', 'fj-board-1025', 'fj-restart-event'):
                event = 109 if case == 'fj-restart-event' else 105
                u.mem_write(FW_COMMANDS, struct.pack('<II', MAGIC | event, 0) + FWJ_RESET + bytes(40))
                put(FW_CONTROL, 1)
        if kind == MAGIC | 106 and case != 'fj-cleanup-no-word':
            # The cleanup's return word, as upstream waits for it.
            put(RETURNS + read * 4, 1)
        if kind == MAGIC | 115:
            put(SYSTEM_DATA + 8, 3)
            if case == 'fx-event':
                # One firmware event: the control has no handler for it.
                u.mem_write(FW_COMMANDS, struct.pack('<I', MAGIC | 101) + bytes(84))
                put(FW_CONTROL, 1)
        elif kind == MAGIC | 107:
            off = pow_type == 1
            if not (off and case in ('fx-no-ack', 'fx-no-ack-busy')) and case not in ('fj-no-ack', 'fj-not-idle'):
                put(SYSTEM_DATA + 8, 2 if not off else 2 if case == 'fx-denied' else 0)
                put(POWER_SYNC, 1)
        if case == 'fx-health-return' or (kind == MAGIC | 101 and case == 'fj-kick-word'):
            put(RETURNS + read * 4, 1)
        if kind == MAGIC | 101 and case == 'fj-kick-bad-word':
            put(RETURNS + read * 4, 8)
        put(OS_DATA + 568, word(OS_DATA + 568) + 1)
        read = (read + 1) & 127
        put(HOST_CONTROL + 128, read)
    put(GPU + 0x8a8, 1)


def check_fwx(u, image, state, case, timer_reads):
    """The firmware-stop variant's outcome for one case."""
    output = ''.join(state['messages'])
    calls = state['calls']
    accesses = state['accesses']
    assert not state.get('exception'), output[-2000:]
    assert 'refused' not in state, (state['refused'], output[-3000:])
    assert state['failure'] == FWX_REASONS[case], (state['failure'], output[-3000:])
    writes, boot = fws_boot(u, image)
    start = fws_case_sequence(case, boot, timer_reads)
    got = [(w, a, v) for w, a, v in accesses]
    # The start is the firmware-start variant's, access for access.
    assert len(got) >= len(start) and all(g[:2] == e[:2] and (e[2] is None or g[2] == e[2])
                                         for g, e in zip(got, start)), 'start sequence'
    assert 'GPU FWS start result=0 phase=3 error=0 ' in output, output[-2500:]
    post = list(zip(got[len(start):], state['steps'][len(start):]))
    kicks = [i for i, ((w, a, v), _) in enumerate(post) if w and a == GPU + 0xb00]
    # Each kick is read back at once.
    assert all(post[i + 1][0][:2] == (0, GPU + 0xb00) for i in kicks), post
    # The stop's register accesses follow its plan; a poll may repeat a read.
    stop_steps = [((w, a - GPU), step) for (w, a, v), step in post if 100 <= step < 200]
    stop_accesses = [x for x, _ in stop_steps]
    plan = [x for x in FWX_STOP_PLAN if x != (1, 0x8b0) or (1, 0x8b0) in stop_accesses]
    # A poll repeats its read within one step; a 64-bit poll repeats both halves.
    compressed, seen = [], set()
    for access, step in stop_steps:
        if (access, step) not in seen:
            compressed.append(access)
            seen.add((access, step))
    held = case == 'fx-idle-held'
    returned = int(case == 'fx-health-return')
    if case in ('fx-board', 'fx-health-return', 'fx-idle-held', 'fx-off-fault'):
        assert state['commands'] == [[115, 0, 0], [107, 2, 1], [107, 1, 1]], state['commands']
        assert len(kicks) == 3, kicks
        expected_plan = plan[:plan.index((0, 0x1358)) + 1] if held else plan
        assert compressed == expected_plan, (compressed, expected_plan)
        # The health check completes by the executed count, with or without
        # its return word.
        assert f'GPU FWS control result=0 phase=4 error=0 health=1 health_status={returned:#x} ' in output, \
            output[-2500:]
        assert 'GPU FWS compat w0=0x3 w16=0x3 updated=0x1 kccb_done=0x1 pow_state=0x3' in output, output[-2500:]
        assert f'GPU FWS kccb write=0x1 read=0x1 rtn0={returned:#x} executed_before=0x0 executed_after=0x1' \
            in output, output[-2500:]
        assert 'GPU FWS power ack_idle=0x1 ack_off=0x1 state_idle=0x2 state_off=0x0 status_idle=' \
               f'{returned:#x} status_off={returned:#x} ' in output, output[-2500:]
        assert 'GPU FWS after kccb_done=0x3 pow_state=0x0 kccb_read=0x3' in output, output[-2500:]
    hard = case in ('fx-denied', 'fx-no-ack')
    if case in ('fx-board', 'fx-health-return', 'fx-idle-held') or hard:
        # The OFF follows the last stop access; the supply end and the park follow it.
        assert calls.index('a733_gpu_platform_off_begin') < calls.index('a733_gpu_platform_off_poll') < \
            calls.index('a733_gpu_platform_supply_end') < calls.index('bb_power_park'), calls
        assert calls.count('a733_gpu_platform_off_begin') == 1
    else:
        assert 'a733_gpu_platform_supply_end' not in calls and 'bb_power_park' not in calls, calls
        assert ('a733_gpu_platform_off_begin' in calls) == (case == 'fx-off-fault'), calls
    if case in ('fx-board', 'fx-health-return'):
        assert state['exit_status'] == 0 and FWX_COMPLETE in output, output[-2000:]
        assert 'GPU FWS stop result=0 phase=10 error=0 step=14 count=14 ' in output, output[-2500:]
    else:
        assert state['exit_status'] is None and 'COMPLETE' not in output, output[-2000:]
    if case in ('fx-incompatible', 'fx-no-health', 'fx-event'):
        errors = {'fx-incompatible': 4, 'fx-no-health': 3, 'fx-event': 7}
        assert f'GPU FWS control result=3 phase=5 error={errors[case]} ' in output, output[-2500:]
        assert not stop_accesses and 'GPU FWS stop' not in output
        assert state['commands'] == ([] if case == 'fx-incompatible' else [[115, 0, 0]]), state['commands']
    if case in ('fx-denied', 'fx-no-ack', 'fx-no-ack-busy'):
        # The power-off request was sent; its acknowledgement was refused or
        # never came. The stop then runs without the acknowledgements: the
        # register plan, all of it when every idle poll matches, then the OFF.
        assert state['commands'] == [[115, 0, 0], [107, 2, 1], [107, 1, 1]], state['commands']
        assert 'GPU FWS stop result=3 phase=11 ' in output and 'ack_off=0x0 ' in output, output[-2500:]
        assert all(step >= 150 for _, step in stop_steps), stop_steps
        if hard:
            assert compressed == plan or compressed == [x for x in plan if x != (1, 0x8b0)], (compressed, plan)
            assert 'GPU FWS stop result=0 phase=10 error=0 step=14 count=14 ' in output and \
                ' unacknowledged=1' in output, output[-2500:]
        else:
            assert compressed in ([(0, 0x8a8), (0, 0x3c8)], [(0, 0x8a8), (1, 0x8b0), (0, 0x3c8)]), compressed
            assert 'a733_gpu_platform_off_begin' not in calls
    state['cycles'] = calls.count('a733_gpu_platform_off_begin')
    state['memory_flushes'] = state.pop('cache')
    return state


def check_fwj(u, image, state, case, timer_reads):
    """The first-job variant's outcome for one case."""
    output = ''.join(state['messages'])
    calls = state['calls']
    accesses = state['accesses']
    assert not state.get('exception'), output[-2000:]
    assert 'refused' not in state, (state['refused'], output[-3000:])
    assert state['failure'] == FWJ_REASONS[case], (state['failure'], output[-3000:])
    writes, boot = fws_boot(u, image)
    start = fws_case_sequence(case, boot, timer_reads)
    got = [(w, a, v) for w, a, v in accesses]
    assert len(got) >= len(start) and all(g[:2] == e[:2] and (e[2] is None or g[2] == e[2])
                                         for g, e in zip(got, start)), 'start sequence'
    assert 'GPU FWS control result=0 phase=4 error=0 health=1 health_status=0x0 ' in output, output[-2500:]
    # The job's image computes the formula's words.
    assert f'expected0={JOB_WORDS[0]:#x} expected31={JOB_WORDS[31]:#x}' in output, output[-2500:]
    # Every buffer the GPU reads or writes is the image's: the output in its
    # payload, the timeline and client CCB in its NC pages.
    output_cpu = struct.unpack('<Q', bytes(u.mem_read(image.address('job_backing') + 5 * 32, 8)))[0]
    payload = image.address('job_payload')
    assert payload <= output_cpu < payload + 88 * 4096 and payload + 88 * 4096 <= NC_BASE
    post = list(zip(got[len(start):], state['steps'][len(start):]))
    kicks = [i for i, ((w, a, v), _) in enumerate(post) if w and a == GPU + 0xb00]
    assert all(post[i + 1][0][:2] == (0, GPU + 0xb00) for i in kicks), post
    stop_accesses = [(w, a - GPU) for (w, a, v), step in post if 100 <= step < 200]
    hard = FWJ_REASONS[case] in (FWX_HARD, FWX_HARD_BUSY)
    if hard:
        # The control failed, or the firmware acknowledged no request: the
        # stop runs without the acknowledgements, the register plan and,
        # when every idle poll matches, the OFF.
        kinds = [c[0] for c in state['commands']]
        assert kinds == ([115, 102, 101] if case in ('fj-restart-event', 'fj-kick-bad-word') else
                         [115, 102, 101, 107]), state['commands']
        assert stop_accesses and all(step >= 150 for (w, a, v), step in post if 100 <= step < 200), post
        assert ' unacknowledged=1' in output, output[-2500:]
        assert ('a733_gpu_platform_off_begin' in calls) == (case != 'fj-not-idle'), calls
        if case != 'fj-not-idle':
            assert 'GPU FWS stop result=0 phase=10 error=0 step=14 count=14 ' in output, output[-2500:]
            assert calls.index('a733_gpu_platform_off_begin') < calls.index('a733_gpu_platform_supply_end') < \
                calls.index('bb_power_park'), calls
        assert state['exit_status'] is None and 'COMPLETE' not in output
        if case in ('fj-restart-event', 'fj-kick-bad-word'):
            errors = {'fj-restart-event': 6, 'fj-kick-bad-word': 5}
            assert 'GPU FWJ job complete=0 outcome=4 stage=4 phase=4 error=0 client=7 client_error=0 kick_status=0x0 ' \
                   'timeline=0x1\n' in output, output[-2500:]
            assert f'GPU FWJ events control=5 control_error={errors[case]} n=0 last=0x0 ' in output, output[-2500:]
            assert 'GPU FWJ cleanup status=0xffffffff events=0 kccb_done=0x3' in output, output[-2500:]
        state['cycles'] = calls.count('a733_gpu_platform_off_begin')
        state['memory_flushes'] = state.pop('cache')
        return state
    reset = case in ('fj-reset-event', 'fj-board-1025')
    if reset:
        # The firmware reported a context reset: the job ends as failed with
        # the notification's words, the context is cleaned up, and the stop
        # and OFF run with the acknowledgements.
        assert 'GPU FWJ job complete=0 outcome=5 stage=4 ' in output, output[-2500:]
        assert 'GPU FWJ events control=4 control_error=0 n=1 last=0x2abc0069 read=0x2abc0069 resets=1 unexpected=0' \
            in output, output[-2500:]
        assert 'GPU FWJ reset ctx=0x1 reason=1 dm=3 ref=0x1 flags=0x1 pc=0x41100000 fault=0x8001000000' \
            in output, output[-2500:]
        if case == 'fj-board-1025':
            # The words are the CPU's fill: the complement of each expected word.
            inverse = [w ^ 0xffffffff for w in (JOB_WORDS[0], JOB_WORDS[1], JOB_WORDS[31])]
            assert f'GPU FWJ output w0={inverse[0]:#x} w1={inverse[1]:#x} w31={inverse[2]:#x} guard={JOB_GUARD:#x}' \
                in output, output[-2500:]
        assert 'GPU FWJ cleanup status=0x1 events=1 kccb_done=0x4' in output, output[-2500:]
    complete = case in ('fj-board', 'fj-kick-word', 'fj-stats-event', 'fj-cleanup-no-word')
    kinds = [c[0] for c in state['commands']]
    # The MMU cache command precedes the KICK: PT, PD, PC, TLB and the
    # interrupt, the MMU sync object, update value 0.
    assert state['commands'][1] == [102, 0x400001f, 0xc0109000], state['commands']
    # The cleanup follows whenever the firmware has finished with the
    # context: the timeline reached, or the context reset.
    cleaned = case not in FWJ_NO_TIMELINE
    assert kinds == ([115, 102, 101, 106, 107, 107] if cleaned else [115, 102, 101, 107, 107]), state['commands']
    assert len(kicks) == len(kinds), kicks
    assert state['client_commands'] == [0x8000 | 205, 213], state['client_commands']
    timeline = int.from_bytes(u.mem_read(JOB_OBJECTS[5], 4), 'little')
    assert timeline == (0 if case == 'fj-no-fence' else 1), timeline
    if reset:
        pass
    elif complete:
        assert f'GPU FWJ job complete=1 outcome=1 stage=4 phase=7 error=0 client=2 client_error=0 kick_status=' \
               f'{int(case == "fj-kick-word"):#x} timeline=0x1' in output, output[-2500:]
        assert f'GPU FWJ output w0={JOB_WORDS[0]:#x} w1={JOB_WORDS[1]:#x} w31={JOB_WORDS[31]:#x} ' \
               f'guard={JOB_GUARD:#x}' in output, output[-2500:]
        cleanup = 0 if case == 'fj-cleanup-no-word' else 1
        assert f'GPU FWJ cleanup status={cleanup:#x} events={int(case == "fj-stats-event")} kccb_done=0x4' \
            in output, output[-2500:]
    else:
        # The job stays waiting for its timeline, with no transport fault,
        # so the shared kernel CCB still serves the stop.
        outcomes = {'fj-no-fence': 2, 'fj-wrong-output': 3, 'fj-guard': 3}
        assert f'GPU FWJ job complete=0 outcome={outcomes[case]} stage=4 phase=5 error=0 client=2 client_error=0 ' \
            in output, output[-2500:]
        assert f'GPU FWJ cleanup status={1 if cleaned else 0xffffffff:#x}' in output, output[-2500:]
    # The stop and OFF run whatever the job's result.
    assert 'GPU FWS power ack_idle=0x1 ack_off=0x1 state_idle=0x2 state_off=0x0 ' in output, output[-2500:]
    assert 'GPU FWS stop result=0 phase=10 error=0 step=14 count=14 ' in output, output[-2500:]
    assert calls.index('a733_gpu_platform_off_begin') < calls.index('a733_gpu_platform_supply_end') < \
        calls.index('bb_power_park'), calls
    if case in ('fj-board', 'fj-kick-word', 'fj-stats-event'):
        assert state['exit_status'] == 0 and FWJ_COMPLETE in output, output[-2000:]
    else:
        assert state['exit_status'] is None and 'COMPLETE' not in output
    state['cycles'] = calls.count('a733_gpu_platform_off_begin')
    state['memory_flushes'] = state.pop('cache')
    return state


def check_fws(u, image, state, case, timer_reads):
    """The firmware-start variant's outcome for one case."""
    output = ''.join(state['messages'])
    calls = state['calls']
    accesses = state['accesses']
    assert not state.get('exception'), output[-2000:]
    # The image never powers the GPU off: the lifetime ends with it on.
    assert not {'a733_gpu_platform_off_begin', 'bb_power_park'} & set(calls), calls
    if case == 'fs-row-mutant':
        assert state.get('refused') == ['0xa0000b0', 4, 1, '0x3', 7] and len(accesses) == 6, state.get('refused')
        state['cycles'] = 0
        return state
    assert 'refused' not in state, (state['refused'], output[-3000:])
    assert state['failure'] == FWS_REASONS[case], (state['failure'], output[-3000:])
    writes, boot = fws_boot(u, image)
    fw = image.address('fw_memory')
    prepared = case != 'fs-memory'
    # The image's MIPS plan: its three boot remaps point into its own
    # firmware pages; a refused preparation leaves the plan empty.
    for value in boot.values():
        assert (value & 0xfff == 12 and fw <= value & ~0xfff < fw + 34 * 4096) if prepared else not value, hex(value)
    assert not prepared or (writes[4][2] == boot['code'] and len(set(boot.values())) == 3), writes
    expected = fws_expected(boot, timer_reads)
    got = [(w, a, v) for w, a, v in accesses]
    assert ('a733_gpu_platform_begin' in calls) == prepared, calls
    if case in ('fs-memory', 'fs-on-deadline', 'fs-mapping'):
        assert not accesses, accesses
    elif case in ('fs-bad-id', 'fs-refused'):
        assert got == [(k, a, None) for k, a, _ in expected[:4]], got
    else:
        # Every access of the case's sequence, each already matched when made.
        assert len(got) == len(fws_case_sequence(case, boot, timer_reads)), len(got)
        # The timer record precedes the first secure-configuration write,
        # and the publication the first GPU register write.
        first_secure = 4 + 2 * timer_reads
        assert state['record_at'] == first_secure, (state['record_at'], first_secure)
        flushes = state['cache']
        assert [f[0] for f in flushes] == [1, 1, 1] and [f[3] for f in flushes] == [first_secure + 4] * 3, flushes
        assert [[f[1], f[2]] for f in flushes] == [[fw, 102400], [fw + 102400, 20480], [fw + 122880, 16384]], flushes
        record = struct.unpack('<QQQQII', bytes(u.mem_read(UTILIZATION + 40, 40)))
        cr = ((TIMER[0] << 32) | TIMER[1]) & 0xffffffffffff
        assert record[1:] == (0, cr, 266305015, 1008000000, 0), record
        assert int.from_bytes(u.mem_read(UTILIZATION + 10240, 4), 'little') == 1
        assert int.from_bytes(u.mem_read(UTILIZATION, 40), 'little') == 0
        # System init carries the core clock; the runtime configuration too.
        assert int.from_bytes(u.mem_read(SYSTEM_INIT + 344, 4), 'little') == 1008000000
        assert int.from_bytes(u.mem_read(SYSTEM_INIT + 356, 4), 'little') == 1
        assert f'GPU FWS timer cr={cr:#x} ' in output and 'k=266305015 clock_hz=1008000000 reads=' \
            f'{timer_reads}' in output, output
        assert 'GPU FWS secure smc0_entry=0x0 smc0_after=0x0 drm_entry=0x1 drm_after=0x0' in output, output
        # Neither secure-configuration word is written back.
        assert [a for w, a, v in got if w and a in (0x0a0000b0, 0x01880014)] == [0x0a0000b0, 0x01880014]
        results = {'fs-board': 'result=0 phase=3 error=0', 'fs-timer-wrap': 'result=0 phase=3 error=0',
                   'fs-timeout': 'result=3 phase=4 error=2', 'fs-stale': 'result=3 phase=4 error=4 step=1',
                   'fs-bad-flag': 'result=3 phase=4 error=5'}
        assert f'GPU FWS start {results[case]} ' in output, output[-2500:]
        assert 'GPU FWS shared started=' in output and output.count('GPU FWS register offset=') == len(FWS_AFTER)
    if case in ('fs-board', 'fs-timer-wrap'):
        assert state['exit_status'] == 0 and FWS_COMPLETE in output, output[-2000:]
    elif case == 'fs-refused':
        assert state['exit_status'] == 7 and 'FAIL GPU firmware start refused: deadline too near, nothing written' in output
        assert 'GPU FWS refused now_us=45' in output and 'session_deadline_us=1500000' in output, output[-1000:]
    else:
        assert state['exit_status'] is None and 'COMPLETE' not in output
    assert state['snapshot_reads'] == SNAPSHOT * (0 if case == 'fs-memory' else 2), len(state['snapshot_reads'])
    state['cycles'] = 0
    state['memory_flushes'] = state.pop('cache')
    return state


def check_probe(u, state, case, output):
    """The probe-read variant's outcome for one case."""
    calls = state['calls']
    # OFF, the supply end and the park follow the window only on a complete run.
    complete = case == 'pr-board'
    assert ('bb_power_park' in calls) == complete and ('a733_gpu_platform_supply_end' in calls) == complete, calls
    assert ('a733_gpu_platform_off_begin' in calls) == (case in ('pr-board', 'pr-off-fault')), calls
    if complete:
        assert calls.index('a733_gpu_platform_off_poll') < calls.index('a733_gpu_platform_supply_end') < \
            calls.index('bb_power_park') < last_put(state)
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
        assert f'GPU PROBE result before={int(case != "pr-bad-id")} after={int(case not in ("pr-bad-id", "pr-after-differs"))} ' \
               f'restored={int(case != "pr-restore-ignored")} accesses=15' in output, output
    snapshots = 1 if case == 'pr-time-budget' else 2
    assert state['snapshot_reads'] == SNAPSHOT * snapshots, len(state['snapshot_reads'])
    if case == 'pr-window-refused':
        # Refused after the first identity read, before any write.
        assert state['exit_status'] == 7 and 'FAIL GPU probe read window refused' in output, output
        assert 'GPU PROBE window refused now_us=1600' in output and 'monitor_deadline_us=2000000' in output, output
    elif case == 'pr-board':
        assert state['exit_status'] == 0 and PROBE_COMPLETE in output, output
        assert 'GPU PROBE core6 on=1 status=0x8' in output, output
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
    p.add_argument('--fw-start', action='store_true', help='check the firmware-start variant')
    p.add_argument('--fw-stop', action='store_true', help='check the firmware-stop variant')
    p.add_argument('--fw-job', action='store_true', help='check the first-job variant')
    p.add_argument('--board-log', type=Path,
                   help='with --fw-stop: reproduce this board kernel.log with the board firmware model instead; '
                        'with --fw-job: with each of --board-cases')
    p.add_argument('--board-cases', nargs='+', help='with --fw-job and --board-log: the firmware model cases')
    a = p.parse_args()
    assert a.power_only + a.wrapper_read + a.probe_read + a.fw_start + a.fw_stop + a.fw_job <= 1, 'choose one variant'
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
    fws_image = bool(image.symbols.get('fws_rows', set()) - {0})
    stop_image = bool(image.symbols.get('fws_stop_run', set()) - {0})
    job_image = bool(image.symbols.get('job_backing', set()) - {0})
    assert job_image == a.fw_job, 'image variant does not match --fw-job'
    assert fws_image == (a.fw_start or a.fw_stop or a.fw_job) and stop_image == (a.fw_stop or a.fw_job), \
        'image variant does not match --fw-start or --fw-stop'
    assert identity == (not a.power_only and not a.wrapper_read and not a.probe_read and not fws_image), \
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
    if a.board_log and a.fw_job:
        assert a.board_cases and set(a.board_cases) <= set(FWJ_REASONS), '--board-log --fw-job needs --board-cases'
        values = [reproduce_job(image, a.board_log, case) for case in a.board_cases]
        a.output.write_text(json.dumps({'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
                                        'physical_access': False, 'board_log': str(a.board_log),
                                        'board_log_sha256': hashlib.sha256(a.board_log.read_bytes()).hexdigest(),
                                        'cases': values}, indent=2) + '\n')
        for v in values:
            print(v['case'], 'reproduces the board log' if v['equal'] else 'does NOT reproduce the board log')
        return
    if a.board_log:
        assert a.fw_stop, '--board-log needs --fw-stop'
        value = reproduce(image, a.board_log)
        value.update(image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(), physical_access=False,
                     board_log=str(a.board_log), board_log_sha256=hashlib.sha256(a.board_log.read_bytes()).hexdigest())
        a.output.write_text(json.dumps(value, indent=2) + '\n')
        print('board log reproduced' if value['equal'] else 'board log NOT reproduced')
        return
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
    if a.fw_start:
        cases = list(FWS_REASONS)
    if a.fw_stop:
        cases = list(FWX_REASONS)
    if a.fw_job:
        cases = list(FWJ_REASONS)
    if a.cases:
        assert set(a.cases) <= set(cases)
        cases = a.cases
    result = {'physical_access': False,
              'variant': ('power-only' if a.power_only else 'wrapper-read' if a.wrapper_read
                          else 'probe-read' if a.probe_read else 'fw-start' if a.fw_start
                          else 'fw-stop' if a.fw_stop else 'fw-job' if a.fw_job else 'identity'),
              'scope': 'actual ELF client and ID access; modeled provider/power transitions',
              'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
              'cases': []}
    for case in cases:
        try:
            result['cases'].append(run_fws(image, case, a.fw_stop or a.fw_job, False, a.fw_job)
                                   if a.fw_start or a.fw_stop or a.fw_job else
                                   run(image, case, a.power_only, a.wrapper_read, a.probe_read))
        except Exception as error:
            result['failed_case'] = case
            result['fixture_error'] = repr(error)
            a.output.write_text(json.dumps(result, indent=2) + '\n')
            raise
        a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(cases)} focused client cases passed; no physical access')


if __name__ == '__main__':
    main()
