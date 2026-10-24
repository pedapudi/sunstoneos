#!/usr/bin/env python3
"""Check the actual checkpoint and return code with existing architectural models."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
from unicorn.arm64_const import *

if not __debug__: raise SystemExit('Checkpoint checks require assertions enabled.')
ROOT = Path(__file__).resolve().parents[2]

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value

prior = module('checkpoint_prior', ROOT / 'apps/gpu-finite/return-check.py')
# The image intentionally preserves its frozen noncacheable table-walk value.
prior.entry.shared.TCR = 0x200803019
prior.entry.shared.shared.TCR = 0x200803019
report = module('checkpoint_report', ROOT / 'apps/gpu-entry-checkpoint/report.py')
normal_report = module('checkpoint_normal_report', ROOT / 'apps/gpu-finite/report.py')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=False)
path = args.build / 'build/checkpoint.elf'; image = prior.layout.Image(path); image.path = path; image.ops = {}
for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump', '-d', '--no-show-raw-insn', str(path)], text=True).splitlines():
    match = re.match(r'^\s*([0-9a-f]+):\s+(.+)$', line)
    if match: image.ops[int(match[1], 16)] = match[2].strip()
raw = bytearray((Path.home() / '.cache/sunstoneos/evidence/gpu-single-core-20261018/provider-valid-fixture/fixtures/session-template.bin').read_bytes())
raw[48:80] = bytes.fromhex(json.loads((args.build / 'manifest.json').read_text())['build_id'])

class Machine(prior.Machine):
    def __init__(self, variant):
        super().__init__(image, bytes(raw), variant)
        self.admission_model = False; self.provider_calls = []
        self.admission = {image.address(n) for n in ('a733_smp_device_preparation_admitted', 'a733_device_memory_ready')}
        self.providers = {}
        for name in ('a733_native_usb_init', 'a733_pmic_poll', 'pvr_gpu_service_init'):
            try: self.providers[image.address(name)] = name
            except AssertionError: pass  # Link-time garbage collection removed unreachable providers.
        self.events.update(self.admission); self.events.update(self.providers); self.events.add(image.address('platform_exit'))
    def hook(self, cpu, address, size, unused):
        if address in self.providers:
            self.provider_calls.append(self.providers[address]); raise AssertionError('Provider reached')
        if self.admission_model and address in self.admission:
            self.ret(1); return
        if address == image.address('platform_exit') and not self.cleanup:
            self.cleanup = True; self.stop_at = None
            self.sysregs.update(spsr_el3=0x84 if self.variant == 'wrong-caller' else 0x85,
                                sctlr_el1=0x30d01805, sctlr_el3=0, scr_el3=0x505, esr_el3=0x17 << 26)
            for line in range(image.address('__gpu_private_start'), (image.address('__gpu_private_end')+63)&~63, 64):
                self.dram[line] = bytes(64); self.dirty.add(line)
            if self.variant == 'expired-publication':
                self.time = self.read(image.address('gpu_runtime_contract') + 96)
        return super().hook(cpu, address, size, unused)
    def prepare(self):
        self.cpu.emu_start(image.address('sd_single_core_entry'), 0, count=10000000)
        assert self.finished and not self.parked
        self.call('platform_init')
        # Reuse the accepted mapping-scan evidence. Here the memory admission
        # predicates are modeled; the two actual pool initializers still run.
        c = bytes(self.cpu.mem_read(image.address('gpu_runtime_contract'), 224))
        epoch = struct.unpack_from('<Q', c, 24)[0]
        startup, diagnostic = struct.unpack_from('<2Q', c, 88)
        self.cpu.mem_write(image.address('a733_cached_runtime'), struct.pack('<4Q', epoch, startup, diagnostic, 0x03460000))
        self.admission_model = True
        for name, start, size in [('boot_pool', 0x48000000, 0x30000000), ('uncached_pool', 0x78000000, 0x01000000)]:
            assert self.call('device_memory_pool_init', image.address(name), start, start, size) == 1
        for address in (0x03400108, 0x03400208, 0x03400308, 0x02002b20, 0x02002b24, 0x07065000, 0x07065008):
            self.write(address, 0)
    def run_checkpoint(self):
        self.finished = False; self.parked = False; self.stop_at = None
        self.cpu.reg_write(UC_ARM64_REG_PSTATE, 0x3c5)
        self.cpu.reg_write(UC_ARM64_REG_SP, image.address('__exception_stack_top'))
        self.cpu.emu_start(image.address('gpu_entry_checkpoint'), 0, count=50000000)
        assert self.reset or self.parked, hex(self.cpu.reg_read(UC_ARM64_REG_PC))

results = {}
m = Machine('state-refusals'); m.prepare()
cp = image.address('gpu_checkpoint'); wb = image.address('boot_pool'); nc = image.address('uncached_pool')
session = bytes(m.cpu.mem_read(image.address('sealed_session'), 592))
epoch, generation, lease = struct.unpack_from('<3Q', session, 24)
m.cpu.mem_write(cp, struct.pack('<5QI4x', wb, nc, epoch, generation, lease, 1))
assert m.call('gpu_checkpoint_ready') == 1
mutations = [('nonzero-application', image.address('app') + 591, 1, 1),
             ('free-ledger-with-owner', wb + image.layouts['device_memory_pool']['allocations'], 7, 8),
             ('used-generation', wb + image.layouts['device_memory_pool']['next_generation'], 2, 8),
             ('changed-binding', cp + image.layouts['gpu_entry_checkpoint']['epoch'], epoch + 1, 8),
             ('outside-private-pool', cp, 0x01800000, 8)]
for name, address, value, size in mutations:
    before = bytes(m.cpu.mem_read(address, size)); m.write(address, value, size)
    assert m.call('gpu_checkpoint_ready') == 0, name
    m.cpu.mem_write(address, before)
    results[name] = dict(passed=True, reset=False)
    print('PASS checkpoint:', name, flush=True)
assert m.call('gpu_checkpoint_ready') == 1 and not m.provider_calls
for variant in ('normal', 'wrong-caller', 'expired-publication'):
    m = Machine(variant); m.prepare()
    try: m.run_checkpoint()
    except Exception:
        debug = {name: dict(address=image.address(name), data=bytes(m.cpu.mem_read(image.address(name), size)).hex())
                 for name, size in [('gpu_checkpoint',48),('boot_pool',64),('uncached_pool',64),
                                    ('gpu_runtime_contract',112),('a733_cached_runtime',48),('sealed_session',48)]}
        debug['private'] = [image.address('__gpu_private_start'),image.address('__gpu_private_end')]
        app_size = 638048
        debug['app_nonzero'] = [(i,x) for i,x in enumerate(m.cpu.mem_read(image.address('app'),app_size)) if x][:10]
        (args.output / 'failure-state.json').write_text(json.dumps(debug,indent=2)+'\n')
        raise
    assert not m.provider_calls
    if variant == 'normal':
        assert m.reset and m.setway and not m.parked
        assert m.cleans == list(range(image.address('__gpu_private_start'), (image.address('__gpu_private_end')+63)&~63, 64))
        assert all(x == 'NS' for x in m.publication_security)
        packet = bytes(m.cpu.mem_read(0x42000100, 320))
        monitor = bytes(m.cpu.mem_read(0x42000000, 128))
        # The architectural model retains the image-private platform log.
        platform = bytes(m.cpu.mem_read(image.address('__gpu_platform_log'), 16384))
        receipt = bytes(m.cpu.mem_read(image.address('gpu_return_receipt'), 104))
        r = struct.unpack('<13Q', receipt)
        prepared = dict(execution_profile='gpu-entry-memory-checkpoint-v1', epoch=r[1], generation=r[2], lease=r[3],
                        geometry=dict(clean_start=r[6], clean_end=r[7], wb_start=0, wb_end=0))
        for name, data in [('monitor',monitor),('platform',platform),('receipt',receipt),('packet',packet)]:
            (args.output / (name+'.bin')).write_bytes(data)
        result = report.validate_reports(monitor, platform, receipt, packet, prepared)
        assert result['gpu_jobs'] == 0 and not result['gpu_functional_success']
        try: normal_report.validate_reports(monitor, platform, receipt, packet, prepared)
        except ValueError: pass
        else: raise AssertionError('Normal GPU parser accepted checkpoint')
        for name, offset, value in [('normal-packet', 0, normal_report.PACKET_MAGIC), ('nonzero-jobs', 40, 1)]:
            changed = bytearray(packet); struct.pack_into('<Q' if offset == 0 else '<I', changed, offset, value)
            try: report.validate_reports(monitor, platform, receipt, bytes(changed), prepared)
            except ValueError: results[name] = dict(passed=True)
            else: raise AssertionError(name)
    else:
        assert m.parked and not m.reset
    results[variant] = dict(passed=True, reset=m.reset, retained_status=m.stop_reason,
                            clean_lines=len(m.cleans), provider_calls=m.provider_calls)
    (args.output / 'results.json').write_text(json.dumps(dict(elf_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        physical_access=False, cases=results), indent=2)+'\n')
    print('PASS checkpoint:', variant, flush=True)
