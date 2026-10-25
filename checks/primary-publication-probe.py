#!/usr/bin/env python3
"""Check the new returning overlay on actual boot; no physical cache claims."""
import argparse
import copy
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


publication = module('publication', ROOT/'checks/a733-supervised-publication.py')
probe = module('primary_probe', ROOT/'tools/prepare-primary-publication-probe.py')


class Probe(publication.Publication):
    def __init__(self, image, bundle):
        super().__init__(image, 'boot')
        self.manifest = json.loads((bundle/'manifest.json').read_text())
        assert self.manifest['native_sha256'] == probe.digest(image) == probe.NATIVE
        assert all(probe.digest(ROOT/name) == sha for name, sha in self.manifest['sources'].items())
        self.hooks = self.manifest['hooks']
        for payload in self.manifest['payloads']:
            data = (bundle/payload['name']).read_bytes()
            assert hashlib.sha256(data).hexdigest() == payload['sha256']
            ctypes.memmove(self.ram_pointer+payload['address']-0x41000000, data, len(data))
        data = (bundle/'overlay.bin').read_bytes()
        assert hashlib.sha256(data).hexdigest() == self.manifest['overlay']['sha256']
        ctypes.memmove(self.ram_pointer+probe.OVERLAY[0]-0x41000000, data, len(data))
        self.mmio.update({0x02050014: 0, 0x02050010: 0, 0x070501c0: 0, 0x070501d4: 0})
        self.regs[0].update(sctlr_el1=0xc50838, tcr_el1=0, s3_0_c15_c3_1=0x41,
                            s3_0_c15_c3_0=0x073fc237)
        build_id = self.read(self.symbols['a733_smp_build_id'], 32)
        data = struct.pack('<6Q', 0x4153373353554333, 3, 224, 5, 7, publication.model.MASK-7)+build_id
        data += struct.pack('<18Q', 0, 24000000, 480000000, 480000000, 9, *([0]*7), 1, 0, 1, 2, 3, 4)
        ctypes.memmove(self.ram_pointer+0x1004000, data, len(data))
        # Fixed offline nonce, not physical authority.
        self.nonce = bytes(range(16))
        ctypes.memmove(self.ram_pointer+probe.REPORT[0]-0x41000000+32, self.nonce, 16)
        self.returned = False
        self.mmio_writes = []
        self.post_prepare = None
        self.inject_restore_fault = False

    def memory(self, u, access, address, size, value, core):
        if address < 0x41000000 and access == UC_MEM_WRITE:
            assert (address, size, value) in ((0x08001004, 4, 0x20), (0x08001008, 4, 0)), (
                'unexpected device write before returning', hex(address), size, value)
            self.mmio_writes.append((address, size, value))
        super().memory(u, access, address, size, value, core)

    def instruction(self, u, address, size, core):
        if address == self.hooks['primary_publication_after_prepare']:
            assert self.maintained == self.expected and not any(self.active[1:])
            self.post_prepare = (bytes(self.ram), copy.deepcopy(self.regs[0]), u.context_save())
        if address == self.hooks['primary_publication_park']:
            self.retained = True
            u.emu_stop()
            return
        assert address != self.symbols['a733_smp_stop'], ('native refusal', self.reg(0, 'x0'), self.reg(0, 'x1'))
        ins = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        if ins.mnemonic == 'msr':
            name, source = ins.op_str.split(', ')
            assert name not in ('s3_0_c15_c1_0', 's3_0_c15_c1_1', 's3_0_c15_c1_2',
                                's3_0_c15_c1_4', 's3_0_c15_c3_3'), 'Unexpected workaround write'
            if name == 'rmr_el3':
                assert self.reg(core, source) == 2
                self.returned = True
                u.emu_stop()
                return
            if name == 'sctlr_el1' and self.inject_restore_fault:
                super().instruction(u, address, size, core)
                self.regs[0][name] ^= 1
                return
        super().instruction(u, address, size, core)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'bundle', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    machine = Probe(args.image, args.bundle)
    machine.cpus[0].emu_start(machine.symbols['a733_smp_entry'], 0, count=3000000)
    assert machine.returned and not machine.retained
    assert machine.mmio_writes == [(0x08001004, 4, 0x20), (0x08001008, 4, 0)]
    result = probe.decode(machine.read(probe.REPORT[0], probe.REPORT[1]), machine.nonce)
    assert machine.maintained == machine.expected and machine.invalidated_instructions
    cases = [{'case': 'actual-boot-primary-prepare-restore-return', 'pass': True,
              'maintained_lines': len(machine.maintained), 'report': result}]
    # Reuse the already executed prefix. Only the changed restoration suffix runs.
    ram, regs, context = machine.post_prepare
    ctypes.memmove(machine.ram_pointer, ram, len(ram))
    machine.regs[0] = regs
    machine.cpus[0].context_restore(context)
    machine.inject_restore_fault = True
    machine.returned = False
    machine.mmio_writes = []
    machine.cpus[0].emu_start(machine.hooks['primary_publication_after_prepare'], 0, count=1000)
    assert machine.retained and not machine.returned and not machine.mmio_writes
    try:
        probe.decode(machine.read(probe.REPORT[0], probe.REPORT[1]), machine.nonce)
    except ValueError:
        pass
    else:
        raise AssertionError('Host accepted incomplete restoration')
    cases.append({'case': 'restoration-readback-failure-retains-without-reset', 'pass': True,
                  'prefix_reused': True})
    # Reset and earlier ROM returns do not establish the values of inactive
    # translation controls. Exercise the same actual boot from distinct values;
    # these are model inputs, not observations of either board's failing run.
    inherited = []
    for name, controls in (
        ('inactive-el1-controls', {'sctlr_el1': 0x30d00800, 'tcr_el1': 0x200803519}),
        ('inactive-el3-controls', {'tcr_el3': 0x80823519,
                                  'ttbr0_el3': 0x421e0000, 'mair_el3': 0xff4400}),
    ):
        variant = Probe(args.image, args.bundle)
        variant.regs[0].update(controls)
        variant.cpus[0].emu_start(variant.symbols['a733_smp_entry'], 0, count=3000000)
        assert variant.returned and not variant.retained, name
        assert not any(variant.active[1:]), name
        assert variant.mmio_writes == [(0x08001004, 4, 0x20), (0x08001008, 4, 0)], name
        assert all(variant.regs[0][key] == value for key, value in controls.items()), name
        report = probe.decode(variant.read(probe.REPORT[0], probe.REPORT[1]), variant.nonce)
        assert variant.maintained == variant.expected and variant.invalidated_instructions, name
        inherited.append({'case': name, 'pass': True, 'initial_controls': controls,
                          'report': report, 'maintained_lines': len(variant.maintained),
                          'device_writes': variant.mmio_writes})
    args.output.write_text(json.dumps({'native_sha256': probe.digest(args.image),
        'manifest_sha256': probe.digest(args.bundle/'manifest.json'), 'physical_access': False,
        'sources': {name: probe.digest(ROOT/name) for name in (
            'checks/primary-publication-probe.py', 'checks/a733-supervised-publication.py',
            'checks/a733-smp-cached.py')}, 'cases': cases, 'inherited_controls': inherited,
        'limits': 'Instruction execution and modeled controls only; no physical cache visibility, reset, or USB qualification.'}, indent=2)+'\n')
    print('PASS primary setup/return; baseline checkpoint reused for restoration failure; two inherited-control variants')


if __name__ == '__main__':
    main()
