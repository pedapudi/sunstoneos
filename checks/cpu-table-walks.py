#!/usr/bin/env python3
"""Check the cacheable EL1 table walk profile: one TCR source, table flush and checksum order."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X29, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402  Refuses to run with assertions disabled.


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


TCR = load('a733_tcr', ROOT / 'tools/a733_tcr.py').TCR
GPR = [UC_ARM64_REG_X0 + i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_LR]
SENTINEL = 0x47fff000
STACK = 0x47f00000

# Files that may spell an EL1 TCR value, by directory, and why. Every other
# tracked source must name A733_CPU_COHERENCE_TCR or read it with
# tools/a733_tcr.py. The check only reads these files as text.
FROZEN = 'records from the digest-pinned native image'
ALLOWED_BY_DIRECTORY = {
    'include': {'a733_cpu_coherence.h': 'the definition'},
    'platform/a733': {'cpu_continue.c': 'non-cacheable continuation profile (MAIR 0x4400)',
                      'cpu_continue.S': 'non-cacheable continuation profile (MAIR 0x4400)'},
    'checks': {'a733-cpu-continue.py': 'non-cacheable continuation profile (MAIR 0x4400)',
               'native-startup-probe.py': FROZEN},
    'apps/gpu-finite': {'return-check.py': 'frozen returning image'},
    'boot/a733': {'native-el1-cache-probe.S': FROZEN},
    'tools': {'run-native-startup-probe.py': FROZEN, 'prepare-primary-cache-probe.py': FROZEN,
              'secondary-cache-host.py': FROZEN, 'all-core-pre-scheduler-host.py': FROZEN},
}
ALLOWED = {f'{d}/{n}': why for d, names in ALLOWED_BY_DIRECTORY.items() for n, why in names.items()}
# The previous and the current EL1 value, as a literal or as a mov/movk triple.
LITERAL = re.compile(r'0x200803[05]19', re.I)
TRIPLE = re.compile(r'mov\s+x\d+,\s*#0x3[05]19\s*\n[+ ]?\s*movk\s+x\d+,\s*#0x0*80,\s*lsl\s*#16', re.I)


def literal_sites():
    files = subprocess.run(['git', '-C', str(ROOT), 'ls-files'], check=True, capture_output=True, text=True).stdout.split()
    found = {}
    for name in files:
        if name.startswith('docs/') or name.endswith('.md') or name == 'checks/cpu-table-walks.py':
            continue
        path = ROOT / name
        if not path.is_file():
            continue
        text = path.read_text(errors='replace')
        count = len(LITERAL.findall(text)) + len(TRIPLE.findall(text))
        if count:
            found[name] = count
    return found


def disassemble(image):
    text = subprocess.run(['/usr/lib/llvm-21/bin/llvm-objdump', '-d', '--no-show-raw-insn', str(image)],
                          check=True, capture_output=True, text=True).stdout
    functions, current = {}, None
    for line in text.splitlines():
        m = re.match(r'^([0-9a-f]+) <([^>]+)>:$', line)
        if m:
            current = functions.setdefault(m[2], [])
            continue
        m = re.match(r'^\s*([0-9a-f]+):\s+(.+)$', line)
        if m and current is not None:
            current.append(m[2].strip())
    return functions


def calls(body):
    return [m[1] for op in body for m in [re.match(r'b(?:l)?\s+0x[0-9a-f]+ <([^>+]+)>$', op)] if m]


class Machine:
    """The linked image with modeled system registers, cache maintenance and counter."""

    def __init__(self, image):
        self.image = image
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.mem_map(0x40000000, 0x08000000)
        for address, data in image.parts:
            self.uc.mem_write(address, data)
        self.model = {'currentel': 12, 'daif': 0x3c0, 'sctlr_el3': 0x30c50830, 'sctlr_el1': 0x30d00800,
                      'mpidr_el1': 0x81000000, 'ctr_el0': 4 << 16, 'cntfrq_el0': 24000000}
        self.clock = 24
        self.trace = []
        self.decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.decoded = {}
        self.counting = False
        self.executed = []
        self.uc.hook_add(UC_HOOK_CODE, self.hook)
        start, end = image.address('__tables_start'), image.address('__tables_end')
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.wrote, begin=start, end=end - 1)

    def gpr(self, name):
        return 0 if name in ('xzr', 'wzr') else self.uc.reg_read(GPR[int(name[1:])])

    def wrote(self, uc, access, address, size, value, unused):
        self.trace.append(('write', address, value))

    def hook(self, uc, address, size, unused):
        if address not in self.decoded:
            self.decoded[address] = next(self.decoder.disasm(bytes(uc.mem_read(address, 4)), address))
        ins = self.decoded[address]
        if self.counting:
            self.executed.append(ins.mnemonic)
        args = ins.op_str.split(', ')
        if ins.mnemonic == 'mrs':
            if args[1] == 'cntpct_el0':
                value = self.clock
                self.clock += 24
            else:
                assert args[1] in self.model, (hex(address), ins.op_str)
                value = self.model[args[1]]
            uc.reg_write(GPR[int(args[0][1:])], value)
        elif ins.mnemonic == 'msr' and not args[0].startswith('daif'):
            self.model[args[0]] = self.gpr(args[1])
            self.trace.append(('msr', args[0], self.model[args[0]]))
        elif ins.mnemonic in ('dc', 'ic', 'tlbi'):
            self.trace.append((ins.mnemonic, args[0], self.gpr(args[1]) if len(args) > 1 else 0))
        elif ins.mnemonic in ('dsb', 'isb'):
            self.trace.append((ins.mnemonic, args[0] if args and args[0] else '', 0))
            return
        else:
            return
        uc.reg_write(UC_ARM64_REG_PC, address + 4)

    def call(self, name, *args, count=False):
        for i, v in enumerate(args):
            self.uc.reg_write(GPR[i], v)
        self.uc.reg_write(UC_ARM64_REG_SP, STACK)
        self.uc.reg_write(UC_ARM64_REG_LR, SENTINEL)
        self.counting, self.executed = count, []
        self.uc.emu_start(self.image.address(name), SENTINEL, count=2000000)
        self.counting = False
        assert self.uc.reg_read(UC_ARM64_REG_PC) == SENTINEL, name
        return self.uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff

    def read(self, address):
        return int.from_bytes(self.uc.mem_read(address, 8), 'little')

    def write(self, address, value):
        self.uc.mem_write(address, value.to_bytes(8, 'little'))

    def el1(self):
        self.model.update(currentel=4, sctlr_el1=0x30d00800)
        self.trace = []


def prepared(image):
    m = Machine(image)
    assert m.call('a733_cached_memory_prepare') == 1
    return m


def measure(image_path, baseline):
    image = harness.Image(image_path)
    functions = disassemble(image_path)
    start, end = image.address('__tables_start'), image.address('__tables_end')
    pages = {n: image.address(n) for n in ('a733_cached_el3_l1', 'a733_cached_el3_l2', 'a733_cached_el3_l3',
                                            'a733_cached_el1_l1', 'a733_cached_el1_l2')}
    result = {'image': str(image_path), 'table_bytes': end - start,
              'table_addresses': {n: hex(a) for n, a in pages.items()},
              'publication_lines': (end - start + 63) // 64}
    m = prepared(image)
    el3_flush = [x for x in m.trace if x[0] == 'dc']
    result['prepare_dc_civac'] = len([x for x in el3_flush if x[1] == 'civac'])
    root, level2 = pages['a733_cached_el1_l1'], pages['a733_cached_el1_l2']
    result['el1_root_entry0'] = hex(m.read(root))
    result['el1_mapped_l1_blocks'] = sum(1 for i in range(512) if m.read(root + 8 * i) & 3 == 1)
    m.el1()
    assert m.call('a733_cpu_coherence_enable_el1', root, m.clock + 10**7, count=True) == 1
    result['enable_el1_instructions'] = len(m.executed)
    result['enable_el1_ldp'] = m.executed.count('ldp')
    result['enable_el1_dc_civac'] = len([x for x in m.trace if x[:2] == ('dc', 'civac')])
    result['tcr_el1'] = hex(m.model['tcr_el1'])
    if baseline:
        return result, None

    # Head-only cases and budgets.
    cases = {}
    names = calls(functions['a733_cached_memory_prepare'])
    assert 'a733_cpu_coherence_flush' in names and 'a733_cpu_coherence_tables' in names, names
    assert names.index('a733_cpu_coherence_flush') < names.index('a733_cpu_coherence_tables'), names
    cases['prepare-calls-flush-before-builder'] = True

    # Layout: five pages first, then the checksum line, all inside the section.
    total = image.address('a733_cached_el1_table_sum')
    assert len({a >> 12 for a in pages.values()}) == 5 and all(start <= a and a + 4096 <= end for a in pages.values())
    assert total % 64 == 0 and total >= max(pages.values()) + 4096 and total + 64 <= end
    cases['checksum-line-follows-the-five-pages'] = True

    # EL3: one DC CIVAC per section line, all before the first table write.
    m = prepared(image)
    flush = [i for i, x in enumerate(m.trace) if x[:2] == ('dc', 'civac')]
    writes = [i for i, x in enumerate(m.trace) if x[0] == 'write']
    assert [m.trace[i][2] for i in flush] == list(range(start, end, 64))
    assert flush[-1] < writes[0] and ('dsb', 'sy', 0) in m.trace[flush[-1]:writes[0]]
    words = [m.read(a) for p in (root, level2) for a in range(p, p + 4096, 8)]
    assert m.read(total) == sum(words) % (1 << 64)
    assert m.trace[writes[-1]][1] == total, 'the checksum is the last table-section write'
    assert m.read(root) & (3 << 8) == 2 << 8 and m.read(pages['a733_cached_el3_l1']) & (3 << 8) == 2 << 8
    cases['el3-flush-precedes-descriptors-and-checksum-is-last'] = True

    # EL1: flush, readback and compare precede every translation register write.
    m.el1()
    assert m.call('a733_cpu_coherence_enable_el1', root, m.clock + 10**7) == 1
    ops = m.trace
    civac = [x[2] for x in ops if x[:2] == ('dc', 'civac')]
    assert civac == list(range(root, root + 4096, 64)) + list(range(level2, level2 + 4096, 64))
    first_msr = next(i for i, x in enumerate(ops) if x[0] == 'msr')
    last_dc = max(i for i, x in enumerate(ops) if x[0] == 'dc')
    assert last_dc < first_msr and ('dsb', 'sy', 0) in ops[last_dc:first_msr]
    assert m.model['tcr_el1'] == TCR and m.model['ttbr0_el1'] == root and m.model['sctlr_el1'] & 0x1005 == 0x1005
    cases['enable-el1-checks-before-translation-registers'] = True

    def refused(change, function='a733_cpu_coherence_enable_el1'):
        m = prepared(image)
        change(m)
        m.el1()
        assert m.call(function, root, m.clock + 10**7) == 0xffffffff, function
        assert not [x for x in m.trace if x[0] == 'msr'], 'no translation register changes after a refusal'
        assert not m.model['sctlr_el1'] & 1
        return m

    refused(lambda m: m.write(level2 + 8 * 300, m.read(level2 + 8 * 300) ^ (1 << 12)))
    refused(lambda m: m.write(root + 8 * 5, 1))
    refused(lambda m: m.write(total, m.read(total) + 1))
    refused(lambda m: m.write(root + 8, 0x48000003))
    refused(lambda m: m.write(root + 8, root | 3))
    refused(lambda m: m.write(level2 + 8 * 300, m.read(level2 + 8 * 300) ^ (1 << 12)), 'a733_cpu_coherence_check_el1')
    cases['corrupted-table-checksum-or-root-leaves-translation-off'] = True

    m = prepared(image)
    m.el1()
    assert m.call('a733_cpu_coherence_check_el1', root, m.clock + 10**7, count=True) == 1
    result['check_el1_instructions'] = len(m.executed)
    assert not [x for x in m.trace if x[0] == 'msr']
    cases['check-el1-alone-changes-no-register'] = True

    for label, el, args in [('below-dram', 12, (0x3fffffc0, 128)), ('beyond-dram', 12, (0x47ffffc0, 128)),
                            ('empty', 12, (0x42000000, 0)), ('el2', 8, (0x42000000, 64)),
                            ('expired', 12, (0x42000000, 64))]:
        m = Machine(image)
        m.model['currentel'] = el
        deadline = 1 if label == 'expired' else 10**7
        assert m.call('a733_cpu_coherence_flush', *args, deadline) == 0xffffffff, label
        assert not [x for x in m.trace if x[0] == 'dc'], label
    for reg in ('sctlr_el3', 'sctlr_el1'):
        m = Machine(image)
        m.model['currentel'] = 12 if reg == 'sctlr_el3' else 4
        m.model[reg] |= 4
        assert m.call('a733_cpu_coherence_flush', 0x42000000, 64, 10**7) == 0xffffffff
    m = Machine(image)
    m.model['daif'] = 0
    assert m.call('a733_cpu_coherence_flush', 0x42000000, 64, 10**7) == 0xffffffff
    m = Machine(image)
    m.model['currentel'] = 4
    assert m.call('a733_cpu_coherence_flush', 0x42000010, 100, 10**7) == 1
    last = max(i for i, x in enumerate(m.trace) if x[0] == 'dc')
    assert [x[2] for x in m.trace if x[0] == 'dc'] == [0x42000000, 0x42000040] and ('dsb', 'sy', 0) in m.trace[last:]
    cases['flush-refusals-and-partial-lines'] = True

    budgets = {'enable_el1_instructions': 2500}
    assert result['enable_el1_instructions'] <= budgets['enable_el1_instructions'], result
    assert result['enable_el1_ldp'] == 512 and result['enable_el1_dc_civac'] == 128, result
    assert result['tcr_el1'] == hex(TCR)
    return result, cases


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--image', type=Path, required=True, help='linked cached eight-core image without the extended device-memory profile')
    p.add_argument('--single-core-image', type=Path, action='append', default=[],
                   help='image whose patched platform_init() adopts the cached EL1 tables')
    p.add_argument('--baseline', action='store_true',
                   help='record the metrics of an image built before this profile; no budgets')
    a = p.parse_args()
    out = a.output.resolve()
    assert ROOT not in [out, *out.parents]
    out.mkdir(parents=True, exist_ok=False)
    result, cases = measure(a.image.resolve(), a.baseline)
    if not a.baseline:
        fields = {'T0SZ': (TCR & 63, 25), 'IRGN0': ((TCR >> 8) & 3, 1), 'ORGN0': ((TCR >> 10) & 3, 1),
                  'SH0': ((TCR >> 12) & 3, 3), 'TG0': ((TCR >> 14) & 3, 0), 'EPD1': ((TCR >> 23) & 1, 1),
                  'IPS': ((TCR >> 32) & 7, 2)}
        assert all(v == want for v, want in fields.values()) and TCR >> 35 == 0, fields
        cases['header-tcr-selects-write-back-inner-shareable-walks'] = True
        sites = literal_sites()
        result['tcr_literal_files'] = sorted(sites)
        unexpected = sorted(set(sites) - set(ALLOWED))
        stale = sorted(set(ALLOWED) - set(sites))
        assert not unexpected and not stale, ('unexpected', unexpected, 'stale allowance', stale)
        cases['tcr-literal-only-in-header-and-frozen-sites'] = True
        capacity = load('device_memory_capacity', ROOT / 'checks/device-memory-capacity.py')
        writers = capacity.later_table_writers()
        result['later_table_writers'] = writers
        assert not writers, writers
        cases['no-table-writes-after-preparation'] = True
        for path in a.single_core_image:
            body = disassemble(path.resolve())['platform_init']
            names = calls(body)
            check = next(i for i, op in enumerate(body) if re.match(r'bl\s+0x[0-9a-f]+ <a733_cpu_coherence_check_el1>$', op))
            tcr = next(i for i, op in enumerate(body) if re.match(r'msr\s+TCR_EL1,', op, re.I))
            assert 'a733_cpu_coherence_check_el1' in names and check < tcr, path
            cases[f'single-core-checks-tables-before-tcr:{path.name}'] = True
    report = {'passed': True, 'baseline': a.baseline, 'metrics': result, 'cases': cases,
              'scope': 'Linked-image instruction model: system registers, cache maintenance and the counter '
                       'are modeled; no physical cache, walk or coherence behaviour is established.'}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['metrics'], indent=2))
    print(f'PASS {len(cases or {})} cases' if not a.baseline else 'RECORDED baseline metrics')


if __name__ == '__main__':
    main()
