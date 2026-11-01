#!/usr/bin/env python3
"""Run the IRQ-selected ISP0 owner with deferred device events in its linked ELF.

Register propagation, IRQ dispatch entry, scheduling, pixels and notification
blocking are modeled. Native source masking, arming phases, waits and completion
checks execute. This does not qualify physical IRQ delivery or transaction drain.
"""
import harness  # Refuse optimized Python before assertion-based verdicts.
import argparse
import hashlib
import importlib.util
import json
import gzip
import pickle
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('isp0_irq_owner', ROOT / 'checks/isp0-usb-copy.py')
transport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transport)
native = transport.native


class Interrupts(transport.Transport):
    def __init__(self, *args):
        super().__init__(*args)
        self.u.mem_map(0x03400000, 0x10000)
        self.regs[0x03400000] = 0x33
        self.waited = False
        self.waits = []
        self.masks = {149: 0, 153: 0, 161: 0}
        for name in ('ulTaskGenericNotifyTake', 'vTaskEnterCriticalFromISR',
                     'vTaskExitCriticalFromISR', 'vTaskGenericNotifyGiveFromISR'):
            address = self.im.address(name)
            self.hooks[address] = name
            self.u.hook_add(native.UC_HOOK_CODE, self.code, begin=address, end=address)

    def code(self, u, pc, size, unused):
        name = self.hooks.get(pc)
        if name == 'ulTaskGenericNotifyTake':
            self.waits.append(self.args()[2])
            self.waited = self.done = True
            u.emu_stop()
            return
        if name in ('vTaskEnterCriticalFromISR', 'vTaskExitCriticalFromISR',
                    'vTaskGenericNotifyGiveFromISR'):
            self.ret(0)
            return
        super().code(u, pc, size, unused)

    def write_mmio(self, u, access, address, size, value, unused):
        if 0x03400000 <= address < 0x03410000:
            offset = address - 0x03400000
            if 0x180 <= offset < 0x1a0:
                target = address - 0x80
                self.regs[target] = self.regs.get(target, 0) & ~value
                self.regs[0x03400000] |= 1 << 31
                for intid in self.masks:
                    if offset == 0x180 + 4 * (intid // 32) and value & (1 << (intid % 32)):
                        self.masks[intid] += 1
            elif 0x100 <= offset < 0x120:
                self.regs[address] = self.regs.get(address, 0) | value
            elif 0x280 <= offset < 0x2a0:
                self.regs[address - 0x80] = self.regs.get(address - 0x80, 0) & ~value
            else:
                self.regs[address] = value
            return
        super().write_mmio(u, access, address, size, value, unused)

    def effect(self):
        assert self.regs.get(0x05830254, 0) == 0
        assert self.regs.get(0x0590002c, 0) == 0
        assert self.regs.get(0x05910030, 0) == 0
        previous = self.started
        self.started = 0
        super().effect()
        self.started = previous + 1
        self.expected = bytes((value + previous) & 255 for value in self.expected)
        self.u.mem_write(self.memory(2), self.expected[:307200])
        self.u.mem_write(self.memory(3), self.expected[307200:])
        # LOAD alone never masks the later SAVE wake. Delay all other events.
        self.regs[0x0590002c] = 8
        self.regs[0x05910030] = self.regs[0x05830254] = 0
        assert self.regs[0x05900028] == 0x7f0e04
        assert self.regs[0x05910020] == 0x100
        assert self.regs[0x05830250] == 0x2fe045

    def tick(self):
        # Every disable asserts RWP; propagation settles between polls only.
        self.regs[0x03400000] &= ~(1 << 31)
        self.waited = False
        super().tick()

    def event(self, intid):
        for source, address, value in ((149, 0x05830254, 1), (153, 0x05910030, 0x100),
                                      (161, 0x0590002c, 12)):
            if source == intid:
                self.regs[address] = value
        before = tuple(self.regs[a] for a in (0x05830254, 0x05910030, 0x0590002c))
        assert self.call('a733_irq_table_dispatch', intid, 0) == 1
        assert before == tuple(self.regs[a] for a in (0x05830254, 0x05910030, 0x0590002c))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'session', 'contract', 'input', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--replay-checkpoint', type=Path, help='Resume only replay from an accepted first capture')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = native.helper.Image(args.image)
    machine = Interrupts(image, args.session.read_bytes(), args.contract.read_bytes(), args.input.read_bytes())
    machine.image_path = args.image
    machine.output = args.output
    if args.replay_checkpoint:
        completed = pickle.loads(gzip.decompress(args.replay_checkpoint.read_bytes()))
    else:
        machine.prepare()
        for _ in range(150):
            machine.tick()
            if machine.get(machine.base + machine.f['stage'], 4) == 1:
                break
            assert not machine.failed
        else:
            raise AssertionError('Provider preparation did not complete')
        initial = machine.checkpoint(args.output / 'prepared.pkl.gz')
    results = {}
    for case, events in (() if args.replay_checkpoint else (('load-save-vipp-dma', [161, 153, 149]),
                          ('dma-before-save', [149, 153, 161]),
                          ('missing-save', [149, 153]))):
        machine.restore(initial)
        machine.waits = []
        machine.masks = {149: 0, 153: 0, 161: 0}
        remaining = list(events)
        for _ in range(700):
            machine.tick()
            result = machine.get(machine.base + machine.f['result'], 4)
            if result:
                break
            if machine.waited:
                assert machine.get(machine.base + machine.f['waiting'], 4) == 1
                assert machine.waits[-1] > 1
                if remaining:
                    machine.event(remaining.pop(0))
                else:
                    machine.now = machine.deadline() + 1
        else:
            raise AssertionError(('Unbounded IRQ owner', case))
        assert not remaining and machine.started == 1
        assert result == (2 if case == 'missing-save' else 1)
        assert machine.failed == (case == 'missing-save')
        for intid in (149, 153, 161):
            assert machine.masks[intid] == (1 if case == 'missing-save' and intid == 161 else 2)
        if result == 1:
            assert bytes(machine.u.mem_read(machine.get(machine.base + machine.f['payload']), 614400)) == machine.expected
            captured = machine.checkpoint(args.output / (case + '.pkl.gz'))
            if case == 'load-save-vipp-dma':
                completed = captured
        assert all(row['state'] == 2 for row in machine.rows()['uncached_pool'])
        results[case] = dict(passed=True, native_result=result, waits=machine.waits,
                             source_masks=machine.masks, retained=True)
        (args.output / 'progress.json').write_text(json.dumps(results, indent=2) + '\n')
    # Compose USB consumer retirement, native rearm, interrupt arming and a
    # second set of staggered device completions on the same retained extents.
    machine.restore(completed)
    machine.symbols(args.image)
    machine.masks = {149: 2, 153: 2, 161: 2}
    machine.waits = []
    machine.incoming.extend(b'\0')
    machine.exchange();machine.exchange();machine.drain()
    first = bytes(machine.wire)
    assert machine.value('delivered') == 1 and len(first) == 614592
    machine.incoming.extend(b'\2' + machine.session[32:40] + struct.pack('<Q', 1) + first[96:128])
    machine.exchange()
    assert machine.get(machine.base + machine.f['result'], 4) == 3
    remaining = [149, 153, 161]
    for _ in range(700):
        machine.tick()
        if machine.get(machine.base + machine.f['result'], 4):
            break
        if machine.waited:
            assert remaining
            machine.event(remaining.pop(0))
    else:
        raise AssertionError('Unbounded second IRQ capture')
    assert not remaining and not machine.failed and machine.started == 2
    assert machine.get(machine.base + machine.f['result'], 4) == 1
    assert machine.masks == {149: 4, 153: 4, 161: 4}
    for index in range(3):
        assert machine.get(machine.base + machine.f['interrupts'] + 4 * index, 4) == 2
    machine.wire.clear();machine.incoming.extend(b'\0')
    machine.exchange();machine.exchange();machine.drain()
    second = bytes(machine.wire)
    assert len(second) == 614592 and second[192:] == machine.expected and second[192:] != first[192:]
    assert struct.unpack_from('<4I', second) == (0x30495053, 2, 1, 614400)
    assert struct.unpack_from('<I', second, 148)[0] == 2
    (args.output / 'second-capture.bin').write_bytes(second)
    results['interrupt-replay'] = dict(passed=True, captures=2, source_masks=machine.masks, waits=machine.waits)
    (args.output / 'result.json').write_text(json.dumps(dict(status='pass', cases=results,
        image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(), physical_access=False,
        numerical_qualified=False, scope=__doc__), indent=2) + '\n')
    print('PASS linked ISP0 deferred IRQ completion and missing-save deadline')


if __name__ == '__main__':
    main()
