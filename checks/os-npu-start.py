#!/usr/bin/env python3
"""Run the linked OS image's board_application_start against exact session bytes.

The board's sealed session record is supplied as bytes (a queue entry's
run/bundle/session.bin). The image's own session decoder, admission checks and
every application start hook run from the ELF under Unicorn. FreeRTOS object
creation, the counter, cache and translation helpers, interrupt masking and
peripheral drivers outside the start hooks are modeled with results that the
real board produces; the check records each hook's result so that a refusal
names its hook. Nothing here touches a board.
"""
import argparse
import json
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR,
                                 UC_ARM64_REG_PSTATE, UC_ARM64_REG_TPIDR_EL1)
ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--image', '{elf:ret-npu-load}', '--session', '{session:ret-npu-load}', '--output', '{output}']]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
import importlib.util
_spec = importlib.util.spec_from_file_location('hw_facts', Path(__file__).resolve().parents[1] / 'tools/hw_facts.py')
hw_facts = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(hw_facts)
# The Non-secure view of a stored priority comes from docs/hardware/a733-facts.json (checks/hw-facts.py).
GIC = hw_facts.GicModel()

LAYOUTS = ('board_application', 'a733_board_session', 'os_application', 'a733_npu_startup', 'board_provider',
           'a733_native_usb', 'a733_counter_clock', 'a733_platform_locks', 'a733_power_domains', 'a733_pmic',
           'a733_pmic_config', 'a733_clock_leases')
PREFIX = 592
IMAGE_FIELD = 48  # a733_board_session.image
RET = 0x43fff000
STACK = 0x43ffe000
NOW_US = 5_000_000
# System registers read by the port and the counter helpers: mrs encodings
# (mask 0xffffffe0) mapped to the value the board returns.
MRS = {0xd53be020: NOW_US * 24, 0xd53be000: 24000000, 0xd53800a0: 0x100, 0xd5385240: 4}


class Start:
    def __init__(self, image, session):
        self.image = image
        self.e = e = harness.Image(image, layout_names=LAYOUTS)
        self.uc = u = harness.machine()
        for base, size in ((0x02000000, 0x08000000), (0x40000000, 0x40000000)):
            u.mem_map(base, size)
        for at, data in e.parts:
            u.mem_write(at, data)
        u.reg_write(UC_ARM64_REG_PSTATE, 5)
        u.reg_write(UC_ARM64_REG_TPIDR_EL1, 0)
        self.session = session
        self.sealed = 0x44f00000
        u.mem_write(self.sealed, session)
        self.calls = []
        self.names = {}
        for name, addrs in e.symbols.items():
            for a in addrs:
                if a and not name.startswith('$') and not name.startswith('.'):
                    self.names.setdefault(a, name)
        self.hooks = {}
        self.refusals = []
        self.notes = []
        self.stub()
        sites = set(self.hooks)
        for at, data in e.parts:
            for off in range(0, len(data) - 3, 4):
                if int.from_bytes(data[off:off + 4], 'little') & 0xffffffe0 in MRS:
                    sites.add(at + off)
        self.sites = sites
        for a in sites:
            u.hook_add(UC_HOOK_CODE, self.code, begin=a, end=a)
        self.entries = {}
        for a, n in self.names.items():
            if a not in sites and e.parts[0][0] <= a < e.parts[0][0] + len(e.parts[0][1]):
                self.entries[a] = n
                u.hook_add(UC_HOOK_CODE, self.enter, begin=a, end=a)
        u.hook_add(UC_HOOK_MEM_UNMAPPED, self.unmapped)
        self.trace = []
        self.flow = []

    def get(self, a, n=8):
        return int.from_bytes(self.uc.mem_read(a, n), 'little')

    def put(self, a, v, n=8):
        self.uc.mem_write(a, int(v).to_bytes(n, 'little'))

    def addr(self, name, unit=None):
        return self.e.address(name, unit)

    def arg(self, i):
        return self.uc.reg_read(UC_ARM64_REG_X0 + i)

    def returned(self, value=0):
        self.uc.reg_write(UC_ARM64_REG_X0, value & ((1 << 64) - 1))
        self.uc.reg_write(UC_ARM64_REG_PC, self.uc.reg_read(UC_ARM64_REG_LR))

    def stub(self):
        e = self.e
        def reg(name, fn, unit=None):
            if name in e.symbols or unit:
                try:
                    self.hooks[e.address(name, unit)] = (name, fn)
                except AssertionError:
                    pass
        ok = lambda: self.returned(1)
        zero = lambda: self.returned(0)
        # The port's interrupt-mask calls read TPIDRRO_EL0 and trap to the kernel when the task is not privileged; the model
        # has no kernel, so they return as at EL1 with nothing masked, and the core is the application's.
        for n in ('vTaskEnterCritical', 'vTaskExitCritical', 'vTaskDelay', 'vTaskCoreAffinitySet', 'uxPortSetInterruptMask',
                  'vPortClearInterruptMask'):
            reg(n, zero)
        reg('ucPortGetCoreID', lambda: self.returned(6))
        for n in ('aarch64_device_cache', 'board_live', 'a733_cached_devices_prepare'):
            reg(n, ok)
        reg('aarch64_device_cache_stride', lambda: self.returned(64))
        reg('a733_smp_counter', lambda: self.returned(NOW_US * 24))
        reg('xTaskGetCurrentTaskHandle', zero)
        reg('xTaskGetSchedulerState', ok)  # taskSCHEDULER_NOT_STARTED is 1
        reg('xQueueCreateMutexStatic', lambda: self.returned(self.arg(1)))
        reg('xQueueGenericCreateStatic', lambda: self.returned(self.arg(3)))
        reg('xTaskCreateStatic', lambda: self.returned(self.arg(6)))
        reg('xTaskCreateStaticAffinitySet', lambda: self.created())
        reg('a733_counter_clock_now', lambda: (self.put(self.arg(1), NOW_US), self.returned(1)))
        reg('aarch64_device_translate', self.translate)
        reg('board_session_sealed', lambda: self.returned(self.sealed))
        reg('board_session_snapshot', lambda: self.returned(self.sealed))
        reg('platform_panic', self.panic)
        reg('board_fail_note', self.note)
        # The thermal owner is common to the control image, which the board accepts.
        reg('os_thermal_start', ok)
        # Board provider state (counter, clock registry, locks, power domains)
        # is the control image's, which the board accepts; the NPU hooks past
        # these two calls run from the ELF.
        # A pure checksum over the multi-megabyte model, verified by the model checks.
        reg('nbg_checksum_verify', ok)

    def startup_init(self):
        self.put(self.arg(0) + self.e.layouts['a733_npu_startup']['initialized'], 1, 4)
        self.returned(1)

    def startup_bind(self):
        self.put(self.arg(0) + self.e.layouts['a733_npu_startup']['task'], self.arg(1))
        self.returned(1)

    def note(self):
        # board_fail_note(site, value, caller): the refusal record a board run returns.
        caller = self.arg(2)
        self.notes.append({'site': hex(self.arg(0)), 'group': (self.arg(0) >> 12) & 15, 'line': self.arg(0) & 0xfff,
                           'value': hex(self.arg(1)), 'caller': self.names.get(caller, hex(caller))})
        self.returned(0)

    def created(self):
        name = bytes(self.uc.mem_read(self.arg(1), 16)).split(b'\0')[0].decode()
        self.trace.append(('task', name))
        self.returned(self.arg(6))

    def translate(self):
        address, out = self.arg(0), self.arg(2)
        attr, share = (0xff, 3) if address >= 0x40000000 else (0, 2)
        self.uc.mem_write(out, struct.pack('<QII', address, attr, share))
        self.returned(1)

    def panic(self):
        text = bytes(self.uc.mem_read(self.arg(0), 64)).split(b'\0')[0].decode()
        self.refusals.append(('panic', text, self.arg(1)))
        self.uc.emu_stop()

    def code(self, uc, at, size, user):
        if at in self.hooks:
            name, fn = self.hooks[at]
            self.trace.append(('stub', name))
            fn()
            return
        w = int.from_bytes(uc.mem_read(at, 4), 'little')
        reg = UC_ARM64_REG_X0 + (w & 31)
        uc.reg_write(reg, MRS[w & 0xffffffe0])
        uc.reg_write(UC_ARM64_REG_PC, at + 4)

    def enter(self, uc, at, size, user):
        self.flow.append((self.entries[at], uc.reg_read(UC_ARM64_REG_LR)))

    def unmapped(self, uc, access, address, size, value, user):
        self.refusals.append(('unmapped', hex(address), hex(uc.reg_read(UC_ARM64_REG_PC))))
        return False

    def call(self, name, *args):
        u = self.uc
        self.done = False
        u.hook_add(UC_HOOK_CODE, lambda c, at, s, x: (setattr(self, 'done', True), c.emu_stop()), begin=RET, end=RET)
        u.reg_write(UC_ARM64_REG_SP, STACK)
        u.reg_write(UC_ARM64_REG_LR, RET)
        for i, v in enumerate(args):
            u.reg_write(UC_ARM64_REG_X0 + i, v)
        try:
            u.emu_start(self.addr(name), 0, count=3_000_000_000)
        except Exception as error:
            pc = u.reg_read(UC_ARM64_REG_PC)
            self.refusals.append(('exception', str(error), hex(pc), self.names.get(pc), bytes(u.mem_read(pc, 4)).hex()))
        return u.reg_read(UC_ARM64_REG_X0) & 0xffffffff if self.done else None

    def start_hooks(self):
        """Names of os_applications[i].start in list order."""
        table, count = self.addr('os_applications'), self.get(self.addr('os_application_count'), 4)
        size = 8 * 4
        out = []
        for i in range(count):
            fields = [self.get(table + i * size + 8 * k) for k in range(4)]
            out.append([self.names.get(f, hex(f)) for f in fields])
        return out


def run(image, session_path, case='accept'):
    data = bytearray(Path(session_path).read_bytes())
    if case == 'wrong-client-model':
        data[1044] ^= 1                       # os_npu_session.client_model (the NPU record starts at 992 in the template, the field at 52)
    # The runner binds a bundle's session to the ELF it launches by writing the
    # image's build identifier into the record; do the same for this ELF.
    s = Start(image, bytes(data))
    build_id = bytes(s.uc.mem_read(s.addr('a733_smp_build_id'), 32))
    bound = bytes(data[IMAGE_FIELD:IMAGE_FIELD + 32]) == build_id
    data[IMAGE_FIELD:IMAGE_FIELD + 32] = build_id
    s.session = bytes(data)
    s.uc.mem_write(s.sealed, s.session)
    e = s.e
    L = e.layouts
    board = e.address('board', 'return-main-pinned.c') if ('board', ) else None
    fields = L['board_application']
    sf = L['a733_board_session']
    # What a733_smp_diagnostic_start leaves: the admitted copy of the snapshot,
    # a nonzero deadline and live flags.
    s.uc.mem_write(board + fields['admitted_session'], s.session[:ctypes_size(sf, s)])
    s.uc.mem_write(board + fields['session'], s.session[:PREFIX])
    s.put(board + fields['session'] + sf['deadline_us'], 60_000_000)
    s.put(board + fields['live_until'], 60_000_000)
    # What common boot and the image's own interrupt table leave in the GIC
    # distributor (ARE set, 1024 lines); the real table routine writes the routes.
    s.put(0x03400000, 0x30, 4)
    s.put(0x03400004, 31, 4)
    routed = s.call('a733_irq_table_route', 1 << 62, s.addr('a733_smp_counter')) if case != 'no-route' else None
    # The routine writes the Secure-view priority of INTID 97. The OS reads it from
    # Nonsecure EL1 with the distributor's DS bit clear, where GICD_IPRIORITYR
    # returns the stored byte shifted left by one (IHI0069D; the board read 0xb0
    # for the stored 0xd8 of PPI 27). Present that view; a RAM-backed distributor
    # would return the Secure value and hide a comparison of the wrong view.
    stored = s.get(0x03400000 + 0x400 + 97, 1)
    s.put(0x03400000 + 0x400 + 97, GIC.ns_read(stored), 1)
    # The shared providers as the board's main leaves them before the hooks run
    # (apps/board/main.c): counter, locks and the power-domain provider ready.
    L = e.layouts
    providers = e.address('providers')
    shared = providers + L['board_provider']['shared']
    domains = providers + L['board_provider']['domains']
    nu = L['a733_native_usb']
    s.put(shared + nu['counter'] + L['a733_counter_clock']['initialized'], 1, 4)
    s.put(shared + nu['locks'] + L['a733_platform_locks']['initialized'], 1, 4)
    s.put(domains + L['a733_power_domains']['self'], domains)
    s.put(domains + L['a733_power_domains']['pmic'], shared + nu['pmic'])
    clocks = s.call('a733_clock_leases_init', shared + nu['clocks'], s.addr('a733_counter_clock_now'),
                    shared + nu['counter'])
    result = {'hooks': s.start_hooks(), 'irq_table_route': routed, 'clock_leases_init': clocks}
    s.refusals.clear()
    value = s.call('board_application_start')
    result['returned'] = value
    result['refusals'] = s.refusals
    result['case'] = case
    result['session_already_bound_to_image'] = bound
    result['notes'] = s.notes
    result['trace'] = s.trace[-40:]
    compact = []
    for n, lr in s.flow:
        if not compact or compact[-1][0] != n:
            compact.append([n, 1])
        else:
            compact[-1][1] += 1
    result['flow'] = [f'{n} x{c}' if c > 1 else n for n, c in compact][-70:]
    return result


def ctypes_size(layout, s):
    # The sealed record begins with the a733_board_session prefix.
    return PREFIX


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--session', type=Path, required=True)
    p.add_argument('--output', type=Path)
    p.add_argument('--case', default='accept', choices=('accept', 'no-route', 'wrong-client-model'))
    a = p.parse_args()
    r = run(a.image, a.session, a.case)
    print(json.dumps(r, indent=1, default=str))
    if a.output:
        a.output.write_text(json.dumps(r, indent=1, default=str))


if __name__ == '__main__':
    main()
