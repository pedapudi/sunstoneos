#!/usr/bin/env python3
"""Exercise CPU0 production cache adoption and restoration; no physical claims."""
import argparse
import copy
import ctypes
import importlib.util
import json
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


parent = module('primary_cache_parent_check', ROOT/'checks/primary-publication-probe.py')
probe = module('primary_cache_prepare', ROOT/'tools/prepare-primary-cache-probe.py')


class Probe(parent.Probe):
    def __init__(self, image, primary, bundle):
        super().__init__(image, primary)
        self.cache_manifest = json.loads((bundle/'manifest.json').read_text())
        assert self.cache_manifest['primary_manifest']['sha256'] == probe.digest(primary/'manifest.json')
        assert self.cache_manifest['sources'] == {name: probe.digest(ROOT/name) for name in probe.SOURCES}
        data = (bundle/'overlay.bin').read_bytes()
        assert probe.digest(bundle/'overlay.bin') == self.cache_manifest['overlay']['sha256']
        ctypes.memmove(self.ram_pointer+parent.probe.OVERLAY[0]-0x41000000, data, len(data))
        self.suffix = False
        self.inject_cache_restore_fault = False
        self.cache_writes = []

    def memory(self, u, access, address, size, value, core):
        if self.suffix and self.regs[0]['sctlr_el3'] & 0x1005 == 0x1005 and access == UC_MEM_WRITE:
            # The interval must not require cleaning dirty Normal-memory data.
            assert 0x42000000 <= address and address+size <= 0x421f0000
            self.cache_writes.append((address, size))
        super().memory(u, access, address, size, value, core)

    def instruction(self, u, address, size, core):
        if not self.suffix:
            super().instruction(u, address, size, core)
            if address == self.hooks['primary_publication_after_prepare']:
                self.suffix = True
            return
        if address in (self.cache_manifest['hooks']['primary_cache_park'],
                       self.hooks['primary_publication_park']):
            self.retained = True
            u.emu_stop()
            return
        ins = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        op, args = ins.mnemonic, ins.op_str.split(', ')
        if op == 'msr' and args[0] == 'rmr_el3':
            assert self.reg(core, args[1]) == 2
            self.returned = True
            u.emu_stop()
            return
        parent.publication.model.Machine.instruction(self, u, address, size, core)
        if self.inject_cache_restore_fault and op == 'msr' and args[0] == 'sctlr_el3' and not self.reg(core, args[1]) & 1:
            self.regs[0]['sctlr_el3'] ^= 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'primary', 'bundle', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    machine = Probe(args.image, args.primary, args.bundle)
    machine.cpus[0].emu_start(machine.symbols['a733_smp_entry'], 0, count=3000000)
    assert machine.returned and not machine.retained and machine.el3_enabled == {0}
    assert not any(machine.active[1:]) and machine.cache_writes
    assert machine.mmio_writes == [(0x08001004, 4, 0x20), (0x08001008, 4, 0)]
    raw = machine.read(probe.REPORT[0], probe.REPORT[1])
    result = probe.decode(raw, machine.nonce)
    assert result['epoch'] == 5 and result['startup_deadline'] == 24000000
    cases = [{'case': 'primary-production-cache-adoption-restored-return', 'pass': True,
              'report': result, 'cached_device_stores': len(machine.cache_writes)}]
    # Reuse the executed preparation prefix; inject only the new restore fault.
    ram, regs, context = machine.post_prepare
    ctypes.memmove(machine.ram_pointer, ram, len(ram))
    machine.regs[0] = copy.deepcopy(regs)
    machine.cpus[0].context_restore(context)
    machine.el3_enabled.clear()
    machine.suffix = True
    machine.inject_cache_restore_fault = True
    machine.returned = machine.retained = False
    machine.mmio_writes = []
    machine.cpus[0].emu_start(machine.hooks['primary_publication_after_prepare'], 0, count=20000)
    assert machine.retained and not machine.returned and not machine.mmio_writes
    try:
        probe.decode(machine.read(probe.REPORT[0], probe.REPORT[1]), machine.nonce)
    except ValueError:
        pass
    else:
        raise AssertionError('Host accepted failed cache restoration')
    cases.append({'case': 'cache-restoration-failure-retains-without-reset', 'pass': True,
                  'prefix_reused': True})
    args.output.write_text(json.dumps({'physical_access': False,
        'native_sha256': probe.digest(args.image), 'manifest_sha256': probe.digest(args.bundle/'manifest.json'),
        'sources': {n: probe.digest(ROOT/n) for n in ('checks/primary-cache-probe.py',
                    'checks/primary-publication-probe.py', 'checks/a733-supervised-publication.py',
                    'checks/a733-smp-cached.py')}, 'cases': cases,
        'limits': 'Actual instructions with modeled translation controls; physical adoption, cache visibility and ROM return remain untested.'}, indent=2)+'\n')
    print('PASS CPU0 EL3 cache adoption/restoration and retained restoration failure; prefix reused')


if __name__ == '__main__':
    main()
