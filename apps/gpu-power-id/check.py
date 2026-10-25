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
--vendor-state the image must be the vendor-state variant: it prints the
monitor's EL3 security record and refuses an incomplete one or a security
switch other than 7; it raises DCDC4 from selector 0x1e to 0x2e under an
exclusive token, refuses a refused, failed or wrong readback, releases the
token and waits at least 1.2 ms before GPU power-on; it refuses the identity
read unless the controller set PLL_GPU0 to 1008 MHz; and its only GPU-window
accesses are the two identity loads, each after its progress record and log
line. The PMIC, rail and GPU platform calls are modeled."""
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
# The vendor-clk variant's two identity reads, high word (+0x24) first.
VCLK_WORDS = [0x01800024, 0x01800020]
VCLK_MAGIC = 0x31304b4c43555047
VCLK_PATHS = {'vc-board': 2, 'vc-core-on': 2, 'vc-bad-id': 2, 'vc-early-stop': 0, 'vc-pending-early': 0,
              'vc-mapping': 0, 'vc-time-budget': 0}
VCLK_REASONS = {'vc-board': None, 'vc-core-on': None,
                'vc-bad-id': 'GPU vendor-clk: identity not 36.56.104.183, GPU left powered',
                'vc-early-stop': 'GPU ON poll', 'vc-pending-early': 'GPU vendor-clk refused before clock and reset progress',
                'vc-mapping': 'GPU vendor-clk mapping', 'vc-time-budget': 'GPU vendor-clk time budget'}
VCLK_COMPLETE = 'COMPLETE GPU vendor-clk: identity 36.56.104.183 with the vendor clock and reset order, GPU left powered'
# The vendor-state variant's two identity reads, its progress magic, the EL3
# record its monitor leaves, and the result of each case.
VST_WORDS = [0x01800024, 0x01800020]
VST_MAGIC = 0x3130545356555047
VST_EL3 = 0x42010000
VST_EL3_MAGIC = 0x3130455356555047
VST_EL3_WRITES = ([0x02054004 + 0x10 * n for n in range(14)] + [0x02054104 + 0x10 * n for n in range(8)] +
                  [0x07002004, 0x07002014, 0x07002024, 0x07010290, 0x02003f00, 0x020540e0])
VST_PATHS = {'vs-board': 2, 'vs-core-on': 2, 'vs-bad-id': 2, 'vs-el3-missing': 0, 'vs-el3-switch': 0,
             'vs-write-refused': 0, 'vs-write-retained': 0, 'vs-readback': 0, 'vs-clock-fault': 0,
             'vs-pending-early': 0, 'vs-mapping': 0, 'vs-time-budget': 0}
VST_REASONS = {'vs-board': None, 'vs-core-on': None,
               'vs-bad-id': 'GPU vendor-state: identity not 36.56.104.183, GPU left powered',
               'vs-el3-missing': 'GPU vendor-state: EL3 security record incomplete',
               'vs-el3-switch': 'GPU vendor-state: CCU security switch did not read 7',
               'vs-write-refused': 'GPU vendor-state DCDC4 write refused',
               'vs-write-retained': 'GPU vendor-state DCDC4 write not complete',
               'vs-readback': 'GPU vendor-state DCDC4 readback not 960 mV',
               'vs-clock-fault': 'GPU ON poll',
               'vs-pending-early': 'GPU vendor-state refused before the GPU clock reached 1008 MHz',
               'vs-mapping': 'GPU vendor-state mapping', 'vs-time-budget': 'GPU vendor-state time budget'}
# The last call each case reaches among the DCDC4 and GPU steps.
VST_LAST = {'vs-el3-missing': None, 'vs-el3-switch': None, 'vs-time-budget': None,
            'vs-write-refused': 'a733_pmic_submit_write', 'vs-write-retained': 'a733_pmic_result',
            'vs-readback': 'a733_pmic_result'}
VST_COMPLETE = ('COMPLETE GPU vendor-state: identity 36.56.104.183 at 1008 MHz from the GPU PLL, DCDC4 960 mV '
                'and the BL31 security setup, GPU left powered')
VST_CALLS = ['a733_rail_acquire', 'a733_rail_poll', 'a733_pmic_submit_write', 'a733_pmic_result',
             'a733_pmic_reap', 'a733_rail_release']
NAMES = ['bb_power', 'a733_gpu_platform', 'a733_gpu_power', 'a733_pmic_consumer',
         'a733_power_domains', 'a733_power_domain', 'a733_power_domain_row']
VST_NAMES = ['a733_pmic_snapshot', 'a733_pmic_request', 'vendor_state_clock']


def last_put(state):
    """Index of the last platform_puts call: the completion text follows the park."""
    return len(state['calls']) - 1 - state['calls'][::-1].index('platform_puts')


def run(image, case, power_only=False, wrapper=False, probe=False, vclk=False, vst=False):
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
    deadline = 250000 if case in ('pr-time-budget', 'vc-time-budget', 'vs-time-budget') else \
        3 * 10**6 if case == 'pr-window-refused' else 10**6 if wrapper or probe or vclk or vst else 10000
    state = {'case': case, 'calls': [], 'gpu_reads': [], 'gpu_writes': [], 'clock_reads': 0, 'on_polls': 0,
             'snapshot_reads': [], 'translated': [], 'wrapper_reads': [], 'progress': [],
             'time': 100, 'messages': [], 'exit_status': None, 'failure': None,
             'pr': [], 'pr_values': {}, 'pr_accesses': [], 'rw_translated': [], 'vc': [],
             'vs': [], 'vs_args': {}, 'vs_stages': {}, 'vs_messages': {}, 'vs_results': 0}
    if vst:
        # The monitor's EL3 record: each write's entry value and value after.
        n = len(VST_EL3_WRITES)
        after = [0x17 if a == 0x07010290 else 0 if a == 0x02003f00 and case == 'vs-el3-switch' else
                 7 if a == 0x02003f00 else 0xffffffff for a in VST_EL3_WRITES]
        record = struct.pack('<QII', 0 if case == 'vs-el3-missing' else VST_EL3_MAGIC,
                             n - (case == 'vs-el3-missing'), 0)
        record += struct.pack(f'<{n}I', *VST_EL3_WRITES) + struct.pack(f'<{n}I', *range(n)) + struct.pack(f'<{n}I', *after)
        u.mem_write(VST_EL3, record)

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
    put(0x1800020, 0 if case in ('pr-bad-id', 'vc-bad-id', 'vs-bad-id') else raw, 8)

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
        if name == 'aarch64_device_translate' and vclk and \
                u.reg_read(UC_ARM64_REG_X0) not in SNAPSHOT_PAGES + (0x1800000,):
            raise AssertionError('vendor-clk variant translated another address')
        if name == 'aarch64_device_translate' and vst and \
                u.reg_read(UC_ARM64_REG_X0) not in SNAPSHOT_PAGES + (0x1800000,):
            raise AssertionError('vendor-state variant translated another address')
        if vst and name in VST_CALLS + ['a733_gpu_platform_begin']:
            # The progress record names the step that each call belongs to.
            magic, stage = struct.unpack('<2Q', bytes(u.mem_read(PROGRESS, 16)))
            assert magic == VST_MAGIC, hex(magic)
            state['vs_stages'].setdefault(name, stage)
            state['vs_messages'].setdefault(name, len(state['messages']))
        if vst and name in VST_CALLS:
            x = [u.reg_read(UC_ARM64_REG_X0 + i) for i in range(8)]
            state['vs_args'].setdefault(name, x)
            if name == 'a733_rail_acquire':
                assert x[1] == 3 and x[2] == 0 and x[3] == 1 << 3, ('DCDC4 exclusive token', x)
                put(x[5], 33, 8); put(x[5] + 8, 3, 8); put(x[5] + 16, 0, 8); put(x[5] + 24, 1 << 3, 8)
                ret(1)                                   # PENDING
            elif name == 'a733_pmic_submit_write':
                assert x[1] == 3 and x[2] == state['vs_args']['a733_rail_acquire'][5] and x[3] == 3 and \
                    x[4] == 0x1e and x[5] == 0x2e, ('DCDC4 write arguments', x)
                put(x[7] + L['a733_pmic_request']['id'], 9, 8)
                ret(5 if case == 'vs-write-refused' else 0)  # INVALID or PENDING
            elif name == 'a733_pmic_result':
                state['vs_results'] += 1
                state['time'] += 300
                if state['vs_results'] == 1:
                    ret(0)                               # PENDING
                    return
                if case == 'vs-write-retained':
                    ret(6)                               # RETAINED
                    return
                snap, S = x[2], L['a733_pmic_snapshot']
                good = case != 'vs-readback'
                for key, value, size in (('rail', 3, 4), ('page_before', 0, 1), ('chip', 3, 1), ('revision', 0, 1),
                                         ('enable', 0x1f, 1), ('selector', 0x2e if good else 0x1e, 1),
                                         ('page_after', 0, 1), ('enabled', 1, 4), ('decoded', 1, 4),
                                         ('programmed_uv', 960000 if good else 800000, 4)):
                    put(snap + S[key], value, size)
                state['written_at'] = state['time']
                ret(2)                                   # COMPLETE
            elif name == 'a733_pmic_reap':
                ret(2)                                   # COMPLETE
            else:
                ret(0)                                   # READY
            return
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
        elif name == 'a733_gpu_platform_poll' and vst:
            state['on_polls'] += 1
            state['time'] += 5000
            fault = case == 'vs-clock-fault'
            early = case == 'vs-pending-early'
            field(power, 'a733_gpu_power', 'core_seen', 0x1f if early or fault else 0x1ff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 23 if fault else 20 if early else 53)
            field(power, 'a733_gpu_power', 'error', 8 if fault else 0)
            notes(0x10130101, 0, 0, 0, 0, 0, (0, 51, 51))
            clock = image.address('vendor_state_clock')
            C = L['vendor_state_clock']
            for key, value in (('pll_last', 0xe1104500 if fault or early else 0xf9105300),
                               ('pll_steps', 2 if fault else 4 if early else 10)):
                put(clock + C[key], value)
            put(clock + C['lock_reads'], 0 if fault else 3); put(clock + C['lock_reads'] + 4, 0 if fault or early else 3)
            field(gpu, 'a733_gpu_platform', 'phase', 7 if fault else 3 if case == 'vs-core-on' else 2)
            field(power, 'a733_gpu_power', 'phase', 5 if fault else 2 if case == 'vs-core-on' else 1)
            field(power, 'a733_gpu_power', 'last_value', 8 if case == 'vs-core-on' else 0)
            ret((1 << 64) - 1 if fault else 1 if case == 'vs-core-on' else 0)
        elif name == 'bb_power_pause':
            # One tick of the DCDC4 waits.
            state['time'] += 1000
            ret()
        elif name == 'a733_gpu_platform_begin' and vst:
            state['begin_at'] = state['time']
            ret()
        elif name == 'a733_gpu_platform_poll' and vclk:
            state['on_polls'] += 1
            state['time'] += 5000
            early = case == 'vc-early-stop'
            field(power, 'a733_gpu_power', 'core_seen', 0x1f if case in ('vc-early-stop', 'vc-pending-early') else 0x1ff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 10 if early else 3 if case == 'vc-pending-early' else 34)
            notes(0x10130101, 0, 0, 0, 0, 0, (0, 51, 51))
            field(gpu, 'a733_gpu_platform', 'phase', 7 if early else 3 if case == 'vc-core-on' else 2)
            field(power, 'a733_gpu_power', 'phase', 5 if early else 2 if case == 'vc-core-on' else 1)
            field(power, 'a733_gpu_power', 'last_value', 8 if case == 'vc-core-on' else 0)
            ret((1 << 64) - 1 if early else 1 if case == 'vc-core-on' else 0)
        elif name == 'a733_gpu_platform_poll' and probe:
            state['on_polls'] += 1
            state['time'] += 5000
            early = case == 'pr-early-stop'
            field(power, 'a733_gpu_power', 'core_seen', 0x1f if case in ('pr-early-stop', 'pr-pending-early') else 0x1ff)
            field(power, 'a733_gpu_power', 'core_policy', 8)
            field(power, 'a733_gpu_power', 'core_config', 0x101)
            field(power, 'a733_gpu_power', 'index', 10 if early else 5 if case == 'pr-pending-early' else 38)
            notes(0x10130101, 0, 0, 0, 0, 0, (51, 51, 51))
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
            assert address in (0x1800020, 0x1800018, WRAPPER_PAGE) + SNAPSHOT_PAGES or \
                (probe and address in (SMC0_PAGE, 0x1800000)) or ((vclk or vst) and address == 0x1800000)
            # Only the probe-read variant translates for a store, of the wrapper and SMC0 pages.
            assert u.reg_read(UC_ARM64_REG_X1) == 0 or (probe and address in (WRAPPER_PAGE, SMC0_PAGE) and
                                                        u.reg_read(UC_ARM64_REG_X1) == 1)
            state['rw_translated'].append([address, u.reg_read(UC_ARM64_REG_X1)])
            state['translated'].append(address)
            # The compiled mapping predicate consumes this native translation
            # result; wrong physical address must refuse before the MMIO read.
            target = u.reg_read(UC_ARM64_REG_X2)
            wrong = (case == 'bad-mapping' and address < 0x2000000) or \
                    (case in ('vc-mapping', 'vs-mapping') and address == 0x1800000) or \
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
    if vclk or vst:
        # The vendor-clk and vendor-state images make no OFF; drop the absent OFF calls.
        modeled = [n for n in modeled if image.symbols.get(n, set()) - {0}]
    if vst:
        modeled += VST_CALLS + ['bb_power_pause']
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
        assert (address, size) in [(0x1800020, 8), (0x1800020, 4), (0x1800018, 4)]

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

    def vc_read(uc, access, address, size, value, arg):
        state['gpu_reads'].append([hex(address), size])
        n = len(state['vc']) + 1
        if not (size == 4 and n <= 2 and address == VCLK_WORDS[n - 1]):
            state['refused'] = [hex(address), size, n]; uc.emu_stop(); return
        magic, stage, count, stamp = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
        assert magic == VCLK_MAGIC and stage == n and count == n - 1 and stamp, (magic, stage, count)
        assert state['messages'][-1] == f'GPU VCLK reading n={n} a={address:#x}\n', state['messages'][-1]
        if n == 1:
            out = ''.join(state['messages'])
            assert len(state['snapshot_reads']) == 2 * len(SNAPSHOT), state['snapshot_reads']
            for text in ('GPU SNAP n=1 a=0x2002b24', 'GPU power phase=', 'GPU CORE entry cycle=1', 'GPU VCLK core6 on='):
                assert text in out, (text, out)
        state['vc'].append(address)

    def vs_read(uc, access, address, size, value, arg):
        state['gpu_reads'].append([hex(address), size])
        n = len(state['vs']) + 1
        if not (size == 4 and n <= 2 and address == VST_WORDS[n - 1]):
            state['refused'] = [hex(address), size, n]; uc.emu_stop(); return
        magic, stage, count, stamp = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
        assert magic == VST_MAGIC and stage == 5 + n and count == n - 1 and stamp, (magic, stage, count)
        assert state['messages'][-1] == f'GPU VST reading n={n} a={address:#x}\n', state['messages'][-1]
        if n == 1:
            out = ''.join(state['messages'])
            assert len(state['snapshot_reads']) == 2 * len(SNAPSHOT), state['snapshot_reads']
            for text in ('GPU SNAP n=1 a=0x2002b24', 'GPU power phase=', 'GPU CORE entry cycle=1',
                         'GPU VST clock pll=0xf9105300 pll_checks=10', 'GPU VST core6 on='):
                assert text in out, (text, out)
        state['vs'].append(address)

    if vst:
        u.hook_add(UC_HOOK_MEM_READ, vs_read, begin=0x1800000, end=0x1800fff)
        u.hook_add(UC_HOOK_MEM_WRITE, write, begin=0x1800000, end=0x1800fff)
        u.emu_start(image.address('gpu_power_id_client'), 0, count=1000000)
        return check_vst(u, state, case, ''.join(state['messages']))
    if vclk:
        u.hook_add(UC_HOOK_MEM_READ, vc_read, begin=0x1800000, end=0x1800fff)
        u.hook_add(UC_HOOK_MEM_WRITE, write, begin=0x1800000, end=0x1800fff)
        u.emu_start(image.address('gpu_power_id_client'), 0, count=200000)
        return check_vclk(u, state, case, ''.join(state['messages']))
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


def check_vclk(u, state, case, output):
    """The vendor-clk variant's outcome for one case."""
    calls = state['calls']
    assert not state['gpu_writes'], state['gpu_writes']
    assert 'a733_gpu_platform_off_begin' not in calls and 'bb_power_park' not in calls, calls
    assert 'refused' not in state, state['refused']
    assert state['failure'] == VCLK_REASONS[case], (state['failure'], output)
    reads = VCLK_WORDS[:VCLK_PATHS[case]]
    assert state['vc'] == reads and state['gpu_reads'] == [[hex(a), 4] for a in reads], state['gpu_reads']
    magic, stage, count, _ = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
    if case != 'vc-time-budget':
        assert magic == VCLK_MAGIC and stage == len(reads) and count == len(reads), (stage, count)
    if reads:
        identity = 0 if case == 'vc-bad-id' else EXPECTED
        assert f'GPU VCLK identity raw={identity:#x} ' in output and f'matches={int(identity == EXPECTED)}' in output, output
    if case in ('vc-board', 'vc-core-on'):
        assert state['exit_status'] == 0 and VCLK_COMPLETE in output, output
    else:
        assert state['exit_status'] is None and 'COMPLETE' not in output
    if case == 'vc-time-budget':
        assert 'a733_gpu_platform_begin' not in calls
    state['cycles'] = 0
    return state


def check_vst(u, state, case, output):
    """The vendor-state variant's outcome for one case."""
    calls = state['calls']
    assert not state['gpu_writes'], state['gpu_writes']
    assert 'a733_gpu_platform_off_begin' not in calls and 'bb_power_park' not in calls, calls
    assert 'refused' not in state, state['refused']
    assert state['failure'] == VST_REASONS[case], (state['failure'], output)
    reads = VST_WORDS[:VST_PATHS[case]]
    assert state['vs'] == reads and state['gpu_reads'] == [[hex(a), 4] for a in reads], state['gpu_reads']
    magic, stage, count, _ = struct.unpack('<4Q', bytes(u.mem_read(PROGRESS, 32)))
    assert magic == VST_MAGIC and count == len(reads), (hex(magic), count)
    # The EL3 record is printed whole before any PMIC call, or as far as it goes.
    el3 = [line for line in state['messages'] if line.startswith('GPU VST el3 ')]
    if case != 'vs-time-budget':
        assert len(el3) == len(VST_EL3_WRITES) - (case == 'vs-el3-missing'), len(el3)
        assert el3[0] == 'GPU VST el3 n=1 a=0x2054004 entry=0x0 after=0xffffffff\n', el3[0]
    reached = [c for c in calls if c in VST_CALLS + ['a733_gpu_platform_begin']]
    last = VST_LAST.get(case, 'a733_gpu_platform_begin')
    if last is None:
        assert not reached, reached
    else:
        order = VST_CALLS[:VST_CALLS.index(last) + 1] if last in VST_CALLS else VST_CALLS + ['a733_gpu_platform_begin']
        assert [c for i, c in enumerate(reached) if not i or c != reached[i - 1]] == order, reached
        stages = {'a733_rail_acquire': 2, 'a733_rail_poll': 2, 'a733_pmic_submit_write': 3, 'a733_pmic_result': 3,
                  'a733_pmic_reap': 3, 'a733_rail_release': 3, 'a733_gpu_platform_begin': 5}
        assert all(state['vs_stages'][c] == stages[c] for c in order), state['vs_stages']
        before = state['messages'][:state['vs_messages']['a733_rail_acquire']]
        assert [line for line in before if line.startswith('GPU VST el3 ')] == el3, 'EL3 lines after the lease'
    if 'a733_gpu_platform_begin' in calls:
        # DCDC4 read back 960 mV, the token was released, and 1.2 ms passed.
        assert 'GPU VST dcdc4 status=2 boot_selector=0x0 selector=0x2e enable=0x1f uv=960000' in output, output
        assert state['begin_at'] - state['written_at'] > 1200, (state['begin_at'], state['written_at'])
        assert len(state['snapshot_reads']) == 2 * len(SNAPSHOT), len(state['snapshot_reads'])
    if reads:
        identity = 0 if case == 'vs-bad-id' else EXPECTED
        assert f'GPU VST identity raw={identity:#x} ' in output and f'matches={int(identity == EXPECTED)}' in output, output
        assert stage == 8, stage
    if case in ('vs-board', 'vs-core-on'):
        assert state['exit_status'] == 0 and VST_COMPLETE in output, output
    else:
        assert state['exit_status'] is None and 'COMPLETE' not in output
    if case == 'vs-clock-fault':
        assert 'GPU power phase=5 error=8 step=23' in output, output
    state['cycles'] = 0
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
    p.add_argument('--vendor-clk', action='store_true', help='check the vendor-clk variant')
    p.add_argument('--vendor-state', action='store_true', help='check the vendor-state variant')
    a = p.parse_args()
    assert a.power_only + a.wrapper_read + a.probe_read + a.vendor_clk + a.vendor_state <= 1, 'choose one variant'
    # The shared owner added delays_ready in existing padding. Reused objects
    # retain the old DWARF member list; consume the actual replacement owner's
    # layout, and verify every old member and the total size still agree.
    image = harness.Image(a.image, layout_names=[n for n in NAMES if n != 'a733_power_domain'] +
                          (VST_NAMES if a.vendor_state else []))
    # The variant is a compile option of main.c: only the identity variant
    # defines the identity read counter.
    identity = bool(image.symbols.get('id_reads', set()) - {0})
    wrapper_image = bool(image.symbols.get('wrapper_words', set()) - {0})
    probe_image = bool(image.symbols.get('probe_steps', set()) - {0})
    assert probe_image == a.probe_read, 'image variant does not match --probe-read'
    vclk_image = bool(image.symbols.get('identity_words', set()) - {0})
    assert vclk_image == a.vendor_clk, 'image variant does not match --vendor-clk'
    vst_image = bool(image.symbols.get('vendor_state_identity', set()) - {0})
    assert vst_image == a.vendor_state, 'image variant does not match --vendor-state'
    assert identity == (not a.power_only and not a.wrapper_read and not a.probe_read and not a.vendor_clk and
                        not a.vendor_state), 'image variant does not match the options'
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
    if a.probe_read:
        cases = [*PROBE_PATHS, 'pr-table-mutant', 'pr-restore-mutant']
    if a.vendor_clk:
        cases = list(VCLK_PATHS)
    if a.vendor_state:
        cases = list(VST_PATHS)
    if a.cases:
        assert set(a.cases) <= set(cases)
        cases = a.cases
    result = {'physical_access': False,
              'variant': ('power-only' if a.power_only else 'wrapper-read' if a.wrapper_read
                          else 'probe-read' if a.probe_read else 'vendor-clk' if a.vendor_clk
                          else 'vendor-state' if a.vendor_state else 'identity'),
              'scope': 'actual ELF client and ID access; modeled provider/power transitions',
              'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
              'cases': []}
    for case in cases:
        try:
            result['cases'].append(run(image, case, a.power_only, a.wrapper_read, a.probe_read, a.vendor_clk,
                                       a.vendor_state))
        except Exception as error:
            result['failed_case'] = case
            result['fixture_error'] = repr(error)
            a.output.write_text(json.dumps(result, indent=2) + '\n')
            raise
        a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(cases)} focused client cases passed; no physical access')


if __name__ == '__main__':
    main()
