#!/usr/bin/env python3
"""Exercise repeated captures and consumer retirement in the actual ISP0 ELF.

ISP pixels, interrupts, register side effects and USB transport are modeled.
This establishes software ownership and fresh completion checks, not physical
DMA retirement, numerical correctness, interrupt delivery or safe ROM return.
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
import pickle
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('isp0_transport', ROOT / 'checks/isp0-usb-copy.py')
transport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transport)


class Replay(transport.Transport):
    def effect(self):
        previous = self.started
        assert self.regs.get(0x05830254, 0) == 0, 'stale DMA completion'
        assert self.regs.get(0x0590002c, 0) == 0, 'stale ISP completion'
        assert self.regs.get(0x05910030, 0) == 0, 'stale VIPP completion'
        self.started = 0
        super().effect()
        self.started = previous + 1
        # Different modeled output on each submission detects stale copy reuse.
        self.expected = bytes((v + previous) & 255 for v in self.expected)
        self.u.mem_write(self.memory(2), self.expected[:307200])
        self.u.mem_write(self.memory(3), self.expected[307200:])

    def write_mmio(self, u, access, address, size, value, unused):
        if self.variant == 'stuck-isp' and address == 0x0590002c:
            self.inject('stuck-isp')
            return
        super().write_mmio(u, access, address, size, value, unused)

    def query(self):
        self.wire.clear()
        self.incoming.extend(b'\0')
        self.exchange()
        self.exchange()
        self.drain()
        return bytes(self.wire)

    def acknowledge(self, command=2, sequence=None, digest=None):
        if sequence is None:
            sequence = self.get(self.base + self.f['sequence'], 4)
        if digest is None:
            digest = bytes(self.u.mem_read(self.base + self.f['digest'], 32))
        pending = self.session[32:40] + struct.pack('<Q', sequence) + digest
        self.incoming.extend(bytes([command]))
        self.exchange()
        # Split the acknowledgment across callbacks, including its digest.
        self.incoming = bytearray(pending[:13])
        self.exchange()
        assert not self.failed and self.get(self.base + self.f['result'], 4) == 1
        self.incoming.extend(pending[13:])
        self.exchange()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'session', 'contract', 'input', 'checkpoint', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--additional-only', action='store_true', help='Run only subsequent-capture ACK refusal cases')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = transport.native.helper.Image(args.image)
    state = pickle.loads(gzip.decompress(args.checkpoint.read_bytes()))
    results = {}

    def fresh(speed=1):
        machine = Replay(image, args.session.read_bytes(), args.contract.read_bytes(), args.input.read_bytes())
        machine.image_path = args.image
        machine.output = args.output
        machine.restore(state)
        machine.symbols(args.image)
        machine.speed = speed
        assert machine.started == 1 and machine.get(machine.base + machine.f['sequence'], 4) == 1
        return machine

    for label, speed in (() if args.additional_only else (('full-speed', 1), ('high-speed', 3))):
        machine = fresh(speed)
        allocations = machine.rows()
        for sequence in range(1, 5):
            wire = machine.query()
            assert len(wire) == 614592 and machine.value('delivered') == 1
            assert struct.unpack_from('<4I', wire) == (0x30495053, 1 if sequence == 1 else 2, 1, 614400)
            assert struct.unpack_from('<I', wire, 148)[0] == (0 if sequence == 1 else sequence)
            assert wire[192:] == machine.expected
            assert wire[96:128] == hashlib.sha256(wire[192:]).digest()
            (args.output / f'{label}-{sequence}.bin').write_bytes(wire)
            # The producer cannot change published data while the consumer holds it.
            before = machine.started
            for _ in range(3):
                machine.tick()
            assert machine.started == before and machine.rows() == allocations
            machine.acknowledge(command=3 if sequence == 4 else 2)
            if sequence == 4:
                assert machine.value('acked') == 1
                receipt = machine.query()
                assert len(receipt) == 192 and struct.unpack_from('<2I', receipt, 8) == (3, 0)
                assert receipt[16:] == wire[16:192]
                break
            assert machine.get(machine.base + machine.f['result'], 4) == 3
            pending = machine.query()
            assert len(pending) == 192 and struct.unpack_from('<2I', pending, 8) == (0, 0)
            machine.run()
            assert machine.started == sequence + 1 and not machine.failed
            assert machine.rows() == allocations
        results[label] = dict(passed=True, captures=machine.started, device_backing_retained=True)

    for case in (() if args.additional_only else ('old-sequence', 'wrong-digest', 'stuck-isp', 'late-isp-error',
                 'late-dma-error', 'late-iommu-error', 'expired', 'sequence-overflow')):
        machine = fresh()
        machine.query()
        before = machine.started
        if case == 'sequence-overflow':
            machine.put(machine.base + machine.f['sequence'], 0xffffffff)
        machine.acknowledge(sequence=0 if case == 'old-sequence' else None,
                            digest=bytes(32) if case == 'wrong-digest' else None)
        if not machine.failed:
            assert machine.get(machine.base + machine.f['result'], 4) == 3
            if case == 'stuck-isp':
                machine.variant = case
            elif case == 'late-isp-error':
                machine.regs[0x0590002c] |= 1 << 16
            elif case == 'late-dma-error':
                machine.regs[0x05830254] |= 1 << 8
            elif case == 'late-iommu-error':
                machine.regs[0x03900838] = 1
            elif case == 'expired':
                machine.now = machine.deadline() + 1
            machine.run()
        assert machine.failed and machine.started == before, case
        assert all(row['state'] == 2 for row in machine.rows()['uncached_pool'])
        machine.checkpoint(args.output / (case + '.pkl.gz'))
        results[case] = dict(passed=True, no_new_submission=True, device_backing_retained=True)

    # Queued next-frame requests cannot pass an outstanding final ZLP.
    machine = fresh()
    machine.incoming.extend(b'\0')
    machine.exchange()
    machine.exchange()
    while machine.fifo:
        count = min(len(machine.fifo), 512)
        machine.wire.extend(machine.fifo[:count])
        del machine.fifo[:count]
        machine.call('tud_vendor_tx_cb', 0, count)
    assert machine.value('terminal_zlp') == 1 and machine.value('delivered') == 0
    machine.incoming.extend(b'\2')
    machine.exchange()
    for _ in range(3):
        machine.tick()
    assert machine.started == 1 and machine.get(machine.base + machine.f['result'], 4) == 1
    assert machine.incoming == b'\2'
    results['terminal-zlp'] = dict(passed=True, no_early_handover=True)
    for case in ('previous-capture', 'legacy-ack-after-replay', 'next-after-final'):
        machine = fresh()
        machine.query()
        machine.acknowledge()
        machine.run()
        machine.query()
        assert machine.started == 2 and machine.get(machine.base + machine.f['sequence'], 4) == 2
        if case == 'previous-capture':
            machine.acknowledge(sequence=1)
        elif case == 'legacy-ack-after-replay':
            machine.incoming.extend(b'\1')
            machine.exchange()
        else:
            machine.acknowledge(command=3)
            machine.incoming.extend(b'\2')
            machine.exchange()
        assert machine.failed and machine.started == 2, case
        results[case] = dict(passed=True, no_new_submission=True)
    report = dict(status='pass', cases=results, image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),
                  physical_access=False, numerical_qualified=False, scope=__doc__)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS repeated ISP0 capture, sequence-bound ACK and retained failures')


if __name__ == '__main__':
    main()
