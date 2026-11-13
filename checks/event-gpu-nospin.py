#!/usr/bin/env python3
"""Execute OS GPU controller waits with modeled register values and time.

Wait intervals and read limits follow the pinned vendor contracts cited by
GPU startup, shutdown and power documentation. No GPU or USB device is used.
"""
import argparse
import hashlib
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import Image, machine, save, skip_crumbs

GATE_RUNS = [['--image', '{elf:ret-gpu}', '--output', '{output}']]
POWER = '/drivers/gpu/a733_gpu_power.c'
START = '/drivers/gpu/pvr_gpu_start.c'
STOP = '/drivers/gpu/pvr_gpu_stop.c'
TYPES = {'a733_gpu_power', 'a733_gpu_power_ops', 'a733_gpu_wait', 'pvr_gpu_start',
         'pvr_gpu_start_ops', 'pvr_gpu_start_step', 'pvr_gpu_stop', 'pvr_gpu_stop_ops',
         'pvr_gpu_stop_step', 'pvr_gpu_control', 'pvr_ccb', 'a733_power_domain_row'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    image = Image(args.image, TYPES)
    enums, table = {}, None
    with args.image.open('rb') as stream:
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            unit = cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
            local = enums.setdefault(unit, {})
            for die in cu.iter_DIEs():
                if die.tag == 'DW_TAG_enumerator':
                    local[die.attributes['DW_AT_name'].value.decode()] = die.attributes['DW_AT_const_value'].value
                if unit.endswith(POWER) and die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value == b'on_steps':
                    typ = die.get_DIE_from_attribute('DW_AT_type').get_DIE_from_attribute('DW_AT_type')
                    while typ.tag in ('DW_TAG_const_type', 'DW_TAG_typedef'):
                        typ = typ.get_DIE_from_attribute('DW_AT_type')
                    table = (typ.attributes['DW_AT_byte_size'].value,
                             {m.attributes['DW_AT_name'].value.decode(): m.attributes['DW_AT_data_member_location'].value for m in typ.iter_children() if m.tag == 'DW_TAG_member'})
    def values(unit):
        return next(v for u, v in enums.items() if u.endswith(unit))
    pe, se, te = values(POWER), values(START), values(STOP)
    all_enums = {k: v for v in enums.values() for k, v in v.items() if k.startswith(('A733_', 'PVR_'))}
    rows = []

    class Model:
        def __init__(self):
            self.u = machine()
            self.u.mem_map(0x41000000, 0x2000000)
            self.u.mem_map(0x80000000, 0x40000)
            for address, data in image.parts:
                if data:
                    self.u.mem_write(address, data)
            skip_crumbs(self.u, image)
            self.now, self.reads, self.writes, self.clocks = 1000, [], [], 0
            self.value, self.started = 0, 0
            self.nominal, self.released, self.actions, self.domain = False, 0, [], 1
            self.complete_row = True
            self.regs = {}
            self.hooks = {0x80000100: 'now', 0x80000110: 'read', 0x80000120: 'started',
                          0x80000130: 'sync', 0x80000140: 'update', 0x80000150: 'release', 0x80000170: 'row_request'}
            for name in ('pvr_gpu_control_poll', 'pvr_gpu_crumb_fail'):
                if name in image.symbols:
                    self.hooks[image.address(name)] = name
            self.u.hook_add(UC_HOOK_CODE, self.hook)

        def get(self, address, width=8):
            return int.from_bytes(self.u.mem_read(address, width), 'little')

        def put(self, address, value, width=8):
            self.u.mem_write(address, int(value).to_bytes(width, 'little'))

        def field(self, base, typ, name):
            return base + image.layouts[typ][name]

        def set(self, base, typ, name, value, width=8):
            self.put(self.field(base, typ, name), value, width)

        def at(self, base, typ, name, width=8):
            return self.get(self.field(base, typ, name), width)

        def ret(self, value=0):
            self.u.reg_write(UC_ARM64_REG_X0, value)
            self.u.reg_write(UC_ARM64_REG_PC, self.u.reg_read(UC_ARM64_REG_LR))

        def hook(self, uc, address, size, unused):
            name = self.hooks.get(address)
            if name is None:
                return
            a = [uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(5)]
            if name == 'now':
                self.clocks += 1
                assert self.clocks < 128, 'controller spins on a clock instead of returning AGAIN'
                self.put(a[1], self.now)
                self.ret(1)
            elif name == 'read':
                self.reads.append((self.now, a[2]))
                # Power reads use bank,offset,out; stop reads use offset,width,out.
                value = self.value
                if self.nominal:
                    bank, off = a[1], a[2]
                    value = self.regs.get((bank, off), 0)
                    if bank == pe['A733_GPU_CCU'] and off == 0xe0:
                        value = value | (1 << 28) if value & 0xe0000000 == 0xe0000000 else value & ~(1 << 28)
                        self.regs[(bank, off)] = value
                    elif bank == pe['A733_GPU_CORE'] and off == 8:
                        if self.regs.get((pe['A733_GPU_CCU'], 0xb20), 0) & 0x80000000:
                            value = self.regs[(pe['A733_GPU_CORE'], 0)]
                            self.regs[(bank, off)] = value
                    self.actions.append((self.now, 'read', bank, off, value))
                self.put(a[3], value, 4 if a[1] <= 2 else a[2])
                self.ret(1)
            elif name in ('started', 'sync'):
                self.reads.append((self.now, name))
                self.put(a[1], self.started if name == 'started' else self.value, 4)
                self.ret(1)
            elif name == 'update':
                self.writes.append(tuple(a[1:]))
                if self.nominal:
                    bank, off, clear, bits = a[1:]
                    before = self.regs.get((bank, off), 0)
                    after = before & ~clear | bits
                    self.regs[(bank, off)] = after
                    self.actions.append((self.now, 'write', bank, off, before, after))
                    if bank == pe['A733_GPU_CCU'] and off == 0xb20:
                        self.regs[(bank, off)] &= ~(1 << 27)
                    if bank == pe['A733_GPU_CORE'] and off == 0:
                        self.regs[(bank, 8)] = after
                self.ret(1)
            elif name == 'row_request':
                request = self.at(a[0], 'a733_power_domain_row', 'request', 4)
                self.domain = request & 1
                self.actions.append((self.now, 'domain', self.domain))
                if self.complete_row:
                    state = all_enums['A733_POWER_ROW_ON' if self.domain else 'A733_POWER_ROW_OFF']
                    self.set(a[0], 'a733_power_domain_row', 'state', (request >> 1) << 3 | state, 4)
                self.ret()
            elif name == 'release':
                self.released += 1
                self.ret(1)
            elif name in ('pvr_gpu_control_poll', 'pvr_gpu_crumb_fail'):
                self.ret(0)
            else:
                raise AssertionError(name)

        def call(self, name, state):
            self.clocks = 0
            self.u.reg_write(UC_ARM64_REG_SP, 0x8003f000)
            self.u.reg_write(UC_ARM64_REG_LR, 0x80000000)
            self.u.reg_write(UC_ARM64_REG_X0, state)
            self.u.emu_start(image.address(name), 0x80000000, count=100000)
            assert self.u.reg_read(UC_ARM64_REG_PC) == 0x80000000, (name, 'controller did not return one bounded step')
            return self.u.reg_read(UC_ARM64_REG_X0)

        def wait(self, state, typ):
            return self.at(self.field(state, typ, 'wait'), 'a733_gpu_wait', 'next_wait_us')

    def power(kind, deadline=1000000):
        m, state = Model(), 0x80001000
        ops = m.field(state, 'a733_gpu_power', 'ops')
        for name, address in (('now', 0x80000100), ('read', 0x80000110), ('update', 0x80000140)):
            m.set(ops, 'a733_gpu_power_ops', name, address)
        m.set(state, 'a733_gpu_power', 'phase', all_enums['A733_GPU_POWER_STARTING'], 4)
        m.set(state, 'a733_gpu_power', 'lease_attempted', 1, 4)
        m.set(state, 'a733_gpu_power', 'lease_held', 1, 4)
        m.set(state, 'a733_gpu_power', 'deadline', deadline)
        stride, fields = table
        base = image.address('on_steps', POWER)
        index = next(i for i in range(64) if m.get(base + i * stride + fields['kind'], 4) == pe[kind])
        m.set(state, 'a733_gpu_power', 'index', index, 4)
        return m, state, index

    m, s, index = power('LOCK')
    m.nominal = True
    m.set(s, 'a733_gpu_power', 'index', 0, 4)
    row = m.field(s, 'a733_gpu_power', 'row')
    m.set(row, 'a733_power_domain_row', 'domains', 0x80020000)
    m.set(row, 'a733_power_domain_row', 'domain', 5, 4)
    m.set(row, 'a733_power_domain_row', 'notify', 0x80000170)
    m.set(row, 'a733_power_domain_row', 'notify_context', row)
    m.set(m.field(s, 'a733_gpu_power', 'ops'), 'a733_gpu_power_ops', 'release', 0x80000150)
    ccu, rccu, core = pe['A733_GPU_CCU'], pe['A733_GPU_R_CCU'], pe['A733_GPU_CORE']
    m.regs = {(ccu, 0x5c0): 1 << 7, (ccu, 0x5e0): 1 << 16, (rccu, 0x1ac): 1,
              (ccu, 0xe0): 0x01105300, (core, 0): 8, (core, 8): 0,
              (core, 0x20): 0x101, (core, 0xfb0): 0x00031001}
    for on in (True, False):
        if not on:
            # The exact same admitted controller begins the ordinary OFF.
            m.u.reg_write(UC_ARM64_REG_SP, 0x8003f000)
            m.u.reg_write(UC_ARM64_REG_LR, 0x80000000)
            m.u.reg_write(UC_ARM64_REG_X0, s)
            m.u.reg_write(UC_ARM64_REG_X0 + 1, 1000000)
            m.u.emu_start(image.address('a733_gpu_power_off'), 0x80000000, count=100000)
            assert m.u.reg_read(UC_ARM64_REG_X0) == all_enums['A733_GPU_POWER_AGAIN']
        calls = 0
        while True:
            result = m.call('a733_gpu_power_poll', s)
            calls += 1
            assert calls <= 128, 'nominal sequence did not finish within its table and waits'
            if result != all_enums['A733_GPU_POWER_AGAIN']:
                break
            until = m.wait(s, 'a733_gpu_power')
            assert until > m.now, 'pending native power state returned without a scheduling deadline'
            m.now = until
        assert result == all_enums['A733_GPU_POWER_OK'],(on,result,m.at(s,'a733_gpu_power','error',4),m.at(s,'a733_gpu_power','index',4),m.actions[-8:])
        assert m.regs[(ccu, 0xe0)] == (0xf9105300 if on else 0x01105300)
        assert m.released == (0 if on else 1)
    writes = [a for a in m.actions if a[1] == 'write']
    domain_on = next(i for i, a in enumerate(m.actions) if a[1:] == ('domain', 1))
    domain_off = next(i for i, a in enumerate(m.actions) if a[1:] == ('domain', 0))
    core_on = next(i for i, a in enumerate(m.actions) if a[1:4] == ('write', core, 0) and a[5] == 8)
    core_off = next(i for i, a in enumerate(m.actions) if a[1:4] == ('write', core, 0) and a[5] == 0)
    assert domain_on < core_on < core_off < domain_off
    bus_reset = next(i for i, a in enumerate(writes) if a[2:4] == (ccu, 0xb24) and a[5] & (1 << 16))
    bus_gate = next(i for i, a in enumerate(writes) if a[2:4] == (ccu, 0xb24) and a[5] & 1)
    core_gate = next(i for i, a in enumerate(writes) if a[2:4] == (ccu, 0xb20) and a[5] & (1 << 31))
    assert bus_reset < bus_gate < core_gate
    for index, action in enumerate(m.actions):
        if action[1:4] == ('write', ccu, 0xe0) and not action[4] & (1 << 27) and action[5] & (1 << 27):
            last_lock = max(a[0] for a in m.actions[:index] if a[1:4] == ('read', ccu, 0xe0) and a[4] & (1 << 28))
            assert action[0] - last_lock >= 25, 'PLL output opened before final4us and20us holds'
    rows.append({'case': 'native-on-off-vendor-order', 'writes': len(writes), 'elapsed_us': m.now - 1000})

    m, s, index = power('LOCK')
    m.value = 1 << 28
    for expected in (1, 2, 3):
        assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_AGAIN']
        assert len(m.reads) == expected and m.at(s, 'a733_gpu_power', 'sample_reads', 4) == expected
        until = m.wait(s, 'a733_gpu_power')
        assert until - m.now == (4 if expected == 3 else 50)
        m.now = until - 1
        assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_AGAIN']
        assert len(m.reads) == expected and m.at(s, 'a733_gpu_power', 'index', 4) == index
        m.now = until
    assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_AGAIN']
    assert m.at(s, 'a733_gpu_power', 'index', 4) == index + 1
    assert m.wait(s, 'a733_gpu_power') == m.now + 21
    assert len(m.reads) == 3, 'third stable sample lost its final vendor hold'
    rows.append({'case': 'three-lock-samples-and-final-hold', 'reads': m.reads})

    for kind, error in (('LOCK', 'A733_GPU_POWER_LOCK'), ('UPDATED', 'A733_GPU_POWER_UPDATE')):
        m, s, index = power(kind)
        m.value = 0 if kind == 'LOCK' else 1 << 27
        for expected in range(1, 101):
            result = m.call('a733_gpu_power_poll', s)
            assert len(m.reads) == expected
            if result == all_enums['A733_GPU_POWER_FAULT']:
                assert kind == 'UPDATED' and expected == 100
                break
            until = m.wait(s, 'a733_gpu_power')
            assert until == m.now + 50
            m.now = until - 1
            assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_AGAIN']
            assert len(m.reads) == expected, 'early hint consumed another vendor sample'
            m.now = until
        if kind == 'LOCK':
            assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_FAULT']
        assert m.at(s, 'a733_gpu_power', 'error', 4) == all_enums[error]
        assert len(m.reads) == 100 and m.at(s, 'a733_gpu_power', 'lease_held', 4) == 1
        assert not m.writes
        assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_FAULT']
        assert len(m.reads) == 100
        rows.append({'case': kind.lower() + '-read-bound-retains', 'reads': 100, 'elapsed_us': m.now - 1000})

    for timing in ('before-arm', 'early-notification', 'missing'):
        m, s, index = power('DOMAIN', 1100)
        m.nominal, m.complete_row = True, timing == 'before-arm'
        ccu, core = pe['A733_GPU_CCU'], pe['A733_GPU_CORE']
        m.regs = {(ccu, 0x5c0): 1 << 7, (ccu, 0x5e0): 1 << 16,
                  (ccu, 0xb20): 1 << 31, (ccu, 0xb24): 0x10001,
                  (core, 0): 8, (core, 8): 8, (core, 0x20): 0}
        row = m.field(s, 'a733_gpu_power', 'row')
        for name, value in (('domains', 0x80020000), ('notify', 0x80000170), ('notify_context', row)):
            m.set(row, 'a733_power_domain_row', name, value)
        m.set(row, 'a733_power_domain_row', 'domain', 5, 4)
        result = m.call('a733_gpu_power_poll', s)
        if timing == 'before-arm':
            assert result == all_enums['A733_GPU_POWER_OK'] and not m.wait(s, 'a733_gpu_power')
        else:
            assert result == all_enums['A733_GPU_POWER_AGAIN'] and m.wait(s, 'a733_gpu_power') == 1100
            m.now = 1001 if timing == 'early-notification' else 1100
            if timing == 'early-notification':
                request = m.at(row, 'a733_power_domain_row', 'request', 4)
                m.set(row, 'a733_power_domain_row', 'state', (request >> 1) << 3 | all_enums['A733_POWER_ROW_ON'], 4)
            result = m.call('a733_gpu_power_poll', s)
            assert result == all_enums['A733_GPU_POWER_FAULT' if timing == 'missing' else 'A733_GPU_POWER_OK']
            assert m.at(s, 'a733_gpu_power', 'lease_held', 4) == 1 and not m.released
        assert len([a for a in m.actions if a[1] == 'domain']) == 1
        rows.append({'case': 'domain-' + timing, 'result': result})

    m, s, index = power('DELAY', 1010)
    assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_AGAIN']
    assert m.wait(s, 'a733_gpu_power') == 1010
    m.now = 1009
    assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_AGAIN']
    m.now = 1010
    assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_FAULT']
    assert m.at(s, 'a733_gpu_power', 'index', 4) == index and not m.reads and not m.writes
    assert m.at(s, 'a733_gpu_power', 'lease_held', 4) == 1
    rows.append({'case': 'short-deadline-refuses-dependent-write', 'writes': 0})

    m, s, index = power('LOCK', 1080)
    for _ in range(2):
        assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_AGAIN']
        m.now = m.wait(s, 'a733_gpu_power')
    assert m.now == 1080
    assert m.call('a733_gpu_power_poll', s) == all_enums['A733_GPU_POWER_FAULT']
    assert len(m.reads) == 2 and m.at(s, 'a733_gpu_power', 'lease_held', 4) == 1
    assert m.at(s, 'a733_gpu_power', 'error', 4) == all_enums['A733_GPU_POWER_TIMEOUT']
    rows.append({'case': 'operation-deadline-precedes-read-bound', 'reads': 2})

    m, s = Model(), 0x80001000
    ops = m.field(s, 'pvr_gpu_start', 'ops')
    m.set(ops, 'pvr_gpu_start_ops', 'now', 0x80000100)
    m.set(ops, 'pvr_gpu_start_ops', 'started', 0x80000120)
    m.set(s, 'pvr_gpu_start', 'phase', all_enums['PVR_GPU_START_RUNNING'], 4)
    m.set(s, 'pvr_gpu_start', 'deadline', 2000)
    step = m.field(s, 'pvr_gpu_start', 'steps')
    m.set(step, 'pvr_gpu_start_step', 'kind', se['DELAY'], 4)
    m.set(step, 'pvr_gpu_start_step', 'value', 3)
    assert m.call('pvr_gpu_start_poll', s) == all_enums['PVR_GPU_START_AGAIN']
    assert m.wait(s, 'pvr_gpu_start') == 1004
    m.now = 1003
    assert m.call('pvr_gpu_start_poll', s) == all_enums['PVR_GPU_START_AGAIN']
    assert m.at(s, 'pvr_gpu_start', 'index', 4) == 0
    m.now = 1004
    assert m.call('pvr_gpu_start_poll', s) == all_enums['PVR_GPU_START_AGAIN']
    assert m.at(s, 'pvr_gpu_start', 'index', 4) == 1
    m.set(s, 'pvr_gpu_start', 'index', 0, 4)
    m.set(step, 'pvr_gpu_start_step', 'kind', se['WAIT_BOOT'], 4)
    assert m.call('pvr_gpu_start_poll', s) == all_enums['PVR_GPU_START_AGAIN']
    assert len(m.reads) == 1 and m.wait(s, 'pvr_gpu_start') == 1054
    m.now = 1053
    assert m.call('pvr_gpu_start_poll', s) == all_enums['PVR_GPU_START_AGAIN']
    assert len(m.reads) == 1
    m.now, m.started = 1054, 1
    assert m.call('pvr_gpu_start_poll', s) == all_enums['PVR_GPU_START_OK']
    assert len(m.reads) == 2
    rows.append({'case': 'firmware-delay-and-started-wait', 'reads': m.reads})

    for register in (False, True):
        m, s, control, queue = Model(), 0x80001000, 0x80010000, 0x80012000
        ops = m.field(s, 'pvr_gpu_stop', 'ops')
        m.set(ops, 'pvr_gpu_stop_ops', 'now', 0x80000100)
        m.set(ops, 'pvr_gpu_stop_ops', 'sync_read', 0x80000130)
        m.set(ops, 'pvr_gpu_stop_ops', 'read', 0x80000110)
        m.set(s, 'pvr_gpu_stop', 'control', control)
        m.set(s, 'pvr_gpu_stop', 'deadline', 1050)
        m.set(control, 'pvr_gpu_control', 'queue', queue)
        m.set(s, 'pvr_gpu_stop', 'phase', all_enums['PVR_GPU_STOP_REGISTERS' if register else 'PVR_GPU_STOP_WAIT_IDLE'], 4)
        m.set(control, 'pvr_gpu_control', 'phase', all_enums['PVR_GPU_CONTROL_RETIRED' if register else 'PVR_GPU_CONTROL_READY'], 4)
        m.set(queue, 'pvr_ccb', 'phase', all_enums['PVR_CCB_RETIRED' if register else 'PVR_CCB_ACTIVE'], 4)
        if register:
            step = m.field(s, 'pvr_gpu_stop', 'steps')
            for name, value in (('kind', te['POLL']), ('offset', 0x3c8), ('width', 4), ('mask', 0x78), ('value', 0x78)):
                m.set(step, 'pvr_gpu_stop_step', name, value, 8 if name in ('mask', 'value') else 4)
        assert m.call('pvr_gpu_stop_poll', s) == all_enums['PVR_GPU_STOP_AGAIN']
        assert len(m.reads) == 1 and m.wait(s, 'pvr_gpu_stop') == 1050
        m.now = 1049
        assert m.call('pvr_gpu_stop_poll', s) == all_enums['PVR_GPU_STOP_AGAIN']
        assert len(m.reads) == 1
        m.now = 1050
        assert m.call('pvr_gpu_stop_poll', s) == all_enums['PVR_GPU_STOP_FAULT']
        assert len(m.reads) == 1 and not m.at(s, 'pvr_gpu_stop', 'isolation_started', 4)
        rows.append({'case': 'stop-' + ('register' if register else 'ack') + '-single-sample-timeout', 'reads': 1})

    args.output.mkdir(parents=True, exist_ok=False)
    save(args.output / 'results.json', {'physical_access': False, 'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(), 'cases': rows})
    print('PASS native GPU nonspinning controllers:', len(rows), 'cases')


if __name__ == '__main__':
    main()
