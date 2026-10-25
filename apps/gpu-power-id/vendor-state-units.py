#!/usr/bin/env python3
"""Unit checks of the vendor-state diagnostic's own copies of shared code.

The script compiles vendor-state-units.c with the copies it links, using the
compile flags of the vendor-state image's base build, and runs each case
under Unicorn with the registers modeled here. It covers:

- the controller (vendor-state-power.c) on the native operations
  (vendor-state-native.c): the ON register writes in their order on the
  board's state, and every refusal path of the PLL, lock, update-bit,
  admission and resource checks;
- the native allowlist's refusals;
- the supply copies: the PMIC provider's writable() (vendor-state-pmic.c)
  admits DCDC4 code 0x2e alone, the consumer's correct()
  (vendor-state-consumer.c) admits DCDC4 only at 960 mV and samples DCDC4
  itself, and the resource manager (vendor-state-resources.c) admits a
  LOCAL lease only with DCDC4 at selector 0x2e and reports 960 mV;
- the approval record (vendor-state-approval.json) names the selector,
  voltage, entry value and PLL words that vendor-state.h defines.

Each copy is also compared with the file it was copied from: the unified
diff must hash to the value pinned in COPIES, so a change to either file
fails this check until the copy is reviewed again.

Nothing here contacts a device.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_LR

HERE = 'apps/gpu-power-id/'
UNITS = [HERE + 'vendor-state-units.c', HERE + 'vendor-state-power.c', HERE + 'vendor-state-native.c',
         HERE + 'vendor-state-resources.c', HERE + 'vendor-state-units-pmic.c', HERE + 'vendor-state-units-supply.c']
# vendor-state-units-pmic.c and vendor-state-units-supply.c include these copies.
INCLUDED = [HERE + 'vendor-state-pmic.c', HERE + 'vendor-state-consumer.c', HERE + 'vendor-state-units-supply.h']
# Each copy, the file it was copied from and the SHA-256 of their unified
# diff (original first, three lines of context).
COPIES = {
    HERE + 'vendor-state-power.c': (HERE + 'vendor-clk-power.c',
        '9fcaada279c64d04d5c7ffe9414f6052d09e21eb6debd2cf3e82ab1e7cb99971'),
    HERE + 'vendor-state-pmic.c': ('drivers/power/a733_pmic.c',
        'be1a1e3895d9d123286f87e6409267025d588da4bd241f4cf15c8d42bdbf1cd4'),
    HERE + 'vendor-state-consumer.c': ('drivers/power/a733_pmic_consumer.c',
        '7805ee6de8da41d6e7fea1d05361283ff3b0ad28d951bde6aadfc65effda5795'),
    HERE + 'vendor-state-resources.c': ('drivers/gpu/a733_gpu_resources.c',
        'ccf22ba67b87a7ffcda240293e83d0ee6230bed8ce153d667b35c54c459e1eef'),
    HERE + 'vendor-state-native.c': ('drivers/gpu/a733_gpu_platform_native.c',
        'e8ccf5a76cb0428c07995ac88bd1e0f2b3b12c56895deaefea869032cecee517'),
}
BASE = harness.CACHE / 'evidence/gpu-provider-ready-20261024/image-1/build.json'
OLDROOT = '/home/sunil/git/sunstoneos-worktrees/gpu-provider-ready-1024'
LLD = harness.CACHE / 'host/usr/lib/llvm-21/bin/ld.lld'
SENTINEL = 0x401ff000

BIT = lambda n: 1 << n
PLL, B20, B24 = 0x020020e0, 0x02002b20, 0x02002b24
CORE = 0x07066000
ENTRY = 0x41104500


def sha(data):
    return hashlib.sha256(data).hexdigest()


def diff_hash(copy, original):
    a = (ROOT / original).read_text().splitlines(keepends=True)
    b = (ROOT / copy).read_text().splitlines(keepends=True)
    text = ''.join(difflib.unified_diff(a, b, original, copy, n=3))
    return sha(text.encode()), text


def build(out):
    base = json.loads(BASE.read_text())
    flags = base['compile'][0][:base['compile'][0].index('-MD')]
    flags = [v.replace(OLDROOT, str(ROOT)) for v in flags] + ['-I', str(ROOT / HERE)]
    objects, commands = [], []
    for unit in UNITS:
        obj = out / (Path(unit).stem + '.o')
        command = flags + ['-c', str(ROOT / unit), '-o', str(obj)]
        subprocess.run(command, check=True, cwd=ROOT)
        objects.append(str(obj))
        commands.append(command)
    script = out / 'units.ld'
    script.write_text('SECTIONS { . = 0x40000000; .text : { KEEP(*(.text.vsu_*)) *(.text*) } .rodata : { *(.rodata*) } '
                      '.data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } /DISCARD/ : { *(.comment) *(.eh_frame*) } }\n')
    elf = out / 'units.elf'
    link = [flags[0], '--target=aarch64-none-elf', f'--ld-path={LLD}', '-nostdlib', '-static',
            '-Wl,-T,' + str(script), '-Wl,-e,vsu_power', '-Wl,--gc-sections', *objects, '-o', str(elf)]
    subprocess.run(link, check=True, cwd=ROOT)
    commands.append(link)
    return elf, commands


class Board:
    """The modeled registers of one case."""

    def __init__(self, u, case):
        self.u, self.case = u, case
        self.pll = case.get('pll_entry', ENTRY)
        self.pll_reads = 0          # reads since lock enable with the PLL enabled
        self.b20 = case.get('b20_entry', 0)
        self.b20_reads = None       # reads since a write with the update bit
        self.writes, self.reads = [], []
        self.policy = 8

    def locked(self):
        pattern = self.case.get('lock', [0, 1])
        if self.pll & (BIT(31) | BIT(29)) != BIT(31) | BIT(29):
            return False
        if self.case.get('lock_never_second') and self.pll & 0x0000ff00 == 0x5300:
            return False
        if self.case.get('lock_never'):
            return False
        i = self.pll_reads - 1
        return bool(pattern[i] if i < len(pattern) else pattern[-1])

    def put(self, address, value):
        self.u.mem_write(address, (value & 0xffffffff).to_bytes(4, 'little'))

    def read(self, uc, access, address, size, value, arg):
        assert size == 4, (hex(address), size)
        self.reads.append(address)
        if address == PLL:
            if self.pll & (BIT(31) | BIT(29)) == BIT(31) | BIT(29):
                self.pll_reads += 1
            self.put(PLL, self.pll | (BIT(28) if self.locked() else 0))
        elif address == B20:
            pending = False
            if self.b20_reads is not None:
                self.b20_reads += 1
                pending = self.case.get('update_never') or self.b20_reads <= self.case.get('update_after', 1)
            self.put(B20, self.b20 | (BIT(27) if pending else 0))
        elif address == CORE + 8:
            self.put(address, 8 if self.case.get('core_on') and self.policy == 8 else 0)

    def write(self, uc, access, address, size, value, arg):
        assert size == 4, (hex(address), size)
        self.writes.append([hex(address), hex(value)])
        if address == PLL:
            ignore = self.case.get('ignore')
            if ignore and ignore(self.pll, value):
                return
            before = self.pll
            self.pll = value & ~BIT(28)
            both = BIT(31) | BIT(29)
            # A new factor word or a lock-enable toggle starts a new lock.
            if self.pll & both != both or before & both != both or (self.pll ^ before) & 0x0070ff00:
                self.pll_reads = 0
        elif address == B20:
            self.b20 = value & ~BIT(27)
            self.b20_reads = 0 if value & BIT(27) else None
        elif address == CORE:
            self.policy = value


def load(elf):
    with elf.open('rb') as stream:
        image = ELFFile(stream)
        segments = [(s['p_vaddr'], s.data(), s['p_memsz']) for s in image.iter_segments() if s['p_type'] == 'PT_LOAD']
        symbols = {s.name: s['st_value'] for s in image.get_section_by_name('.symtab').iter_symbols() if s.name}
    return segments, symbols


def machine(segments, symbols, case):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for address, size in [(0x40000000, 0x200000), (0x02002000, 0x2000), (0x07010000, 0x1000), (CORE, 0x1000),
                          (0x07090000, 0x1000)]:
        u.mem_map(address, size)
    for address, data, _ in segments:
        u.mem_write(address, data)
    u.cpr_write(3, 3, 14, 0, 0, 24000000)
    board = Board(u, case)
    for address, value in [(PLL, board.pll), (B20, board.b20), (B24, 0), (0x020025c0, BIT(7)), (0x020025e0, BIT(16)),
                           (0x070101ac, 1), (CORE, 8), (CORE + 8, 0), (CORE + 0x14, 0), (CORE + 0x20, 0x101),
                           (CORE + 0xfb0, 0x10130101)]:
        board.put(address, value)
    clock = [100]

    def instruction(uc, address, size, arg):
        word = int.from_bytes(uc.mem_read(address, 4), 'little')
        if word & 0xffffffe0 == 0xd53be020:     # mrs Xt, cntpct_el0
            uc.reg_write(UC_ARM64_REG_X0 + (word & 31), clock[0] * 24)
            clock[0] += 1
            uc.reg_write(UC_ARM64_REG_PC, address + 4)
    u.hook_add(UC_HOOK_CODE, instruction)
    for page, size in [(0x02002000, 0x2000), (0x07010000, 0x1000), (CORE, 0x1000)]:
        u.hook_add(UC_HOOK_MEM_READ, board.read, begin=page, end=page + size - 1)
        u.hook_add(UC_HOOK_MEM_WRITE, board.write, begin=page, end=page + size - 1)
    return u, board


def call(u, symbols, name, count=4000000):
    u.reg_write(UC_ARM64_REG_SP, 0x401f0000)
    u.reg_write(UC_ARM64_REG_LR, SENTINEL)
    u.emu_start(symbols[name], SENTINEL, count=count)
    assert u.reg_read(UC_ARM64_REG_PC) == SENTINEL, f'{name} did not return'
    return u.reg_read(UC_ARM64_REG_X0)


def words(u, address, n):
    return [int.from_bytes(u.mem_read(address + 4 * i, 4), 'little') for i in range(n)]


# The ON writes on the board's state, CORE6 status 0 throughout: PWCR 0,
# policy 8, the PLL enable (each RMW write carries the lock bit the read
# returned), the reset, bus gate and core gate, the update-bit rewrite, and
# the 1008 MHz set-rate sequence. A PLL_GPU0 write is the read value with
# bits changed, so it carries the read-only lock bit 28 as read; the
# comparison leaves that bit out.
ON_WRITES = [(CORE + 0x20, 0), (CORE, 8),
             (PLL, 0x41104500), (PLL, 0x41104500), (PLL, 0xc1104500), (PLL, 0xe1104500), (PLL, 0xe9104500),
             (B24, 0x10000), (B24, 0x10001), (B20, 0x88000000), (B20, 0x88000000),
             (PLL, 0xe1104500), (PLL, 0xe1105300), (PLL, 0xc1105300), (PLL, 0xe1105300), (PLL, 0xe9105300)]
ON_STEPS = 54   # vendor-state-power.c on_steps; the last is the CORE6 status poll
POLL_STEP = ON_STEPS - 1
ERRORS = {'state': 5, 'resources': 4, 'pll': 8, 'lock': 9, 'update': 10}
# Each case: the model, then what the controller must report.
CASES = {
    'on-board': ({}, dict(result=1, phase=1, error=0, index=POLL_STEP, writes=ON_WRITES, locks=[3, 3],
                          updates=[2, 2], pll=0xf9105300, pll_steps=10)),
    'on-core-on': ({'core_on': True}, dict(result=0, phase=2, error=0, index=ON_STEPS, writes=ON_WRITES)),
    'on-lock-late': ({'lock': [0] * 9 + [1]}, dict(result=1, phase=1, index=POLL_STEP, writes=ON_WRITES,
                                                    locks=[11, 11])),
    'on-lock-flicker': ({'lock': [0, 1, 1, 0, 1, 1, 1]}, dict(result=1, phase=1, index=POLL_STEP, writes=ON_WRITES,
                                                            locks=[6, 6])),
    'refuse-supply-800': ({'supply_uv': 800000}, dict(result=3, phase=5, error=ERRORS['resources'], index=0, writes=[])),
    'refuse-parent': ({'parent_hz': 300000000}, dict(result=3, phase=5, error=ERRORS['resources'], index=0, writes=[])),
    'refuse-pll-entry': ({'pll_entry': 0xc1104500}, dict(result=3, phase=5, error=ERRORS['state'], index=5, writes=[])),
    'refuse-b20-entry': ({'b20_entry': 0x03000000}, dict(result=3, phase=5, error=ERRORS['state'], index=3, writes=[])),
    'refuse-domain': ({'row_answer': 4}, dict(result=3, phase=5, error=ERRORS['state'], index=6, writes=[])),
    'pll-enable-ignored': ({'ignore': lambda old, new: new & BIT(31) and not old & BIT(31)},
                           dict(result=3, phase=5, error=ERRORS['pll'], index=23, writes=ON_WRITES[:5])),
    'pll-rate-ignored': ({'ignore': lambda old, new: new & 0xff00 == 0x5300},
                         dict(result=3, phase=5, error=ERRORS['pll'], index=43, writes=ON_WRITES[:13])),
    'lock-never': ({'lock_never': True}, dict(result=3, phase=5, error=ERRORS['lock'], index=26,
                                              writes=ON_WRITES[:6], locks=[100, 0])),
    'lock-never-second': ({'lock_never_second': True}, dict(result=3, phase=5, error=ERRORS['lock'], index=48,
                                                           writes=ON_WRITES[:15],
                                                           locks=[3, 100])),
    'update-stuck': ({'update_never': True}, dict(result=3, phase=5, error=ERRORS['update'], index=35,
                                                  writes=ON_WRITES[:10], updates=[100, 0])),
}


def run_case(segments, symbols, name, model, expect):
    config = {'parent_hz': 400000000, 'supply_uv': 960000, 'row_answer': 2, 'polls': 6, **model}
    u, board = machine(segments, symbols, model)
    u.mem_write(symbols['vsu_config'], b''.join(int(config[k]).to_bytes(4, 'little')
                                                for k in ('parent_hz', 'supply_uv', 'row_answer', 'polls'))
                + (10 ** 7).to_bytes(8, 'little'))
    assert call(u, symbols, 'vsu_power') == 0, 'harness setup failed'
    result, phase, error, index, polls, requests, held = words(u, symbols['vsu_result'], 7)
    clock = words(u, symbols['vendor_state_clock'], 8)
    got = {'result': result, 'phase': phase, 'error': error, 'index': index, 'polls': polls,
           'row_requests': requests, 'locks_held': held, 'locks': clock[0:2], 'lock_run': clock[2:4],
           'updates': clock[4:6], 'pll': clock[6], 'pll_steps': clock[7],
           'writes': board.writes}
    want = [[hex(a), hex(v)] for a, v in expect['writes']]
    seen = [[a, hex(int(v, 16) & ~BIT(28)) if int(a, 16) == PLL else v] for a, v in board.writes]
    assert seen == want, (name, seen, want)
    for key in ('result', 'phase', 'error', 'index', 'locks', 'updates', 'pll', 'pll_steps'):
        if key in expect:
            assert got[key] == expect[key], (name, key, got[key], expect[key])
    assert held == 0, (name, 'register lock held')
    # No case reaches beyond the words of its sequence.
    assert set(board.reads) <= {PLL, B20, B24, 0x020025c0, 0x020025e0, 0x070101ac, CORE, CORE + 8, CORE + 0x14,
                                CORE + 0x20, CORE + 0xfb0}, (name, sorted(set(map(hex, board.reads))))
    if expect['result'] == 3:
        # A failure stops the sequence: the next poll faults with no access.
        before = len(board.writes) + len(board.reads)
        assert call(u, symbols, 'vsu_poll_again') == 3, name
        assert before == len(board.writes) + len(board.reads), (name, 'access after the failure')
    return {'case': name, **{k: v for k, v in got.items() if k != 'writes'}, 'writes': len(board.writes)}


DCDC2, DCDC3, DCDC4, DCDC5 = 1, 2, 3, 4
# The provider's writable(): rail, target, admitted.
WRITABLE = [(DCDC4, 0x2e, 1), (DCDC4, 0x1e, 0), (DCDC4, 0x2d, 0), (DCDC4, 0x2f, 0), (DCDC4, 0xae, 0),
            (DCDC3, 0x23, 0), (DCDC5, 0x23, 0), (DCDC2, 0x2e, 0), (0, 0x2e, 0)]
GOOD = dict(page_before=0, chip=3, revision=0, enable=0x08, selector=0x2e, page_after=0)
# The consumer's correct(): rail, snapshot changes, admitted.
CORRECT = [(DCDC4, {}, 1), (DCDC4, {'selector': 0xae}, 1), (DCDC4, {'selector': 0x1e}, 0),
           (DCDC4, {'selector': 0x2f}, 0), (DCDC4, {'enable': 0xf7}, 0), (DCDC4, {'chip': 2}, 0),
           (DCDC4, {'page_before': 1}, 0), (DCDC4, {'page_after': 1}, 0), (DCDC4, {'revision': 1}, 0),
           (DCDC2, {'selector': 0x1e, 'enable': 0x02}, 1), (DCDC2, {'enable': 0x02}, 0)]
# The resource manager's LOCAL acquisition: rail snapshot changes, result,
# error, reported supply.
RESOURCES = [({}, 0, 0, 960000), ({'voltage': 0xae}, 0, 0, 960000), ({'voltage': 0x1e}, 3, 6, 0),
             ({'voltage': 0x2f}, 3, 6, 0), ({'enable': 0xf7}, 3, 6, 0), ({'chip': 2}, 3, 6, 0),
             ({'page_before': 1}, 3, 6, 0)]
RESOURCE_WORDS = [(0x07090160, 0x10000002), (0x02002000, 0xf8636300), (0x020020a0, 0xfe106300),
                  (0x020020a8, 0), (0x020020ac, 0), (0x02003908, 0x8fff0fff), (0x02003988, 0x0fff0000),
                  (0x070101ac, 1), (0x020025c0, 0x010000ff), (0x020025e0, 0x41055800), (B20, 0), (B24, 0)]


def approval_case():
    """The approval record names the values the copies use."""
    import re
    record = json.loads((ROOT / HERE / 'vendor-state-approval.json').read_text())
    header = (ROOT / HERE / 'vendor-state.h').read_text()
    define = lambda name: int(re.search(r'#define ' + name + r' (?:UINT32_C\()?(0x[0-9a-f]+|\d+)', header)[1], 0)
    v, c = record['voltage'], record['clock']
    assert record['schema'] == 'gpu-vendor-state-approval-v1' and record['approval']['date'] == '2026-10-25'
    assert v['rail'] == 'DCDC4' and v['register'] == '0x15' and v['field_mask'] == '0x7f'
    assert int(v['selector'], 16) == define('VENDOR_STATE_DCDC4_TARGET') and v['value_uv'] == define('VENDOR_STATE_SUPPLY_UV')
    assert int(v['entry_selector'], 16) == define('VENDOR_STATE_DCDC4_ENTRY') and v['entry_uv'] == 800000
    assert 500 + 10 * int(v['selector'], 16) == v['value_uv'] // 1000 and v['settle_us'] >= 1000 + 200
    assert int(c['entry_word'], 16) == define('VENDOR_STATE_PLL_ENTRY') and int(c['word'], 16) == define('VENDOR_STATE_PLL_1008')
    word = int(c['word'], 16)
    assert 24000000 * (((word >> 8) & 255) + 1) // (((word >> 20) & 7) + 1) == c['rate_hz'] == 1008000000
    return {'case': 'approval-record', 'selector': v['selector'], 'value_uv': v['value_uv'], 'pll': c['word']}


def supply_cases(segments, symbols):
    cases = [approval_case()]
    for rail, target, admitted in WRITABLE:
        u, _ = machine(segments, symbols, {})
        u.mem_write(symbols['vsu_supply'], rail.to_bytes(4, 'little') + target.to_bytes(4, 'little'))
        got = call(u, symbols, 'vsu_writable')
        assert got == admitted, ('writable', rail, hex(target), got)
    cases.append({'case': 'pmic-writable', 'rows': [[r, hex(t), a] for r, t, a in WRITABLE]})
    for rail, change, admitted in CORRECT:
        u, _ = machine(segments, symbols, {})
        v = {**GOOD, **change}
        fields = [rail, 0] + [v[k] for k in ('page_before', 'chip', 'revision', 'enable', 'selector', 'page_after')]
        u.mem_write(symbols['vsu_supply'], b''.join(x.to_bytes(4, 'little') for x in fields))
        got = call(u, symbols, 'vsu_correct')
        assert got == admitted, ('correct', rail, change, got)
    cases.append({'case': 'consumer-correct', 'rows': [[r, c, a] for r, c, a in CORRECT]})
    u, _ = machine(segments, symbols, {})
    assert call(u, symbols, 'vsu_recorded') == 0, 'the consumer copy uses the boot record'
    cases.append({'case': 'consumer-samples-dcdc4'})
    for change, result, error, supply in RESOURCES:
        u, board = machine(segments, symbols, {})
        for address, value in RESOURCE_WORDS:
            board.put(address, value)
        v = {'page_before': 0, 'chip': 3, 'revision': 0, 'enable': 0x08, 'voltage': 0x2e, 'page_after': 0, **change}
        u.mem_write(symbols['vsu_rail'], bytes(v[k] for k in ('page_before', 'chip', 'revision', 'enable', 'voltage',
                                                              'page_after')))
        assert call(u, symbols, 'vsu_resources') == 0
        got = words(u, symbols['vsu_acquired'], 5)
        assert got[0] == result and got[1] == error and got[3] == supply, ('resources', change, got)
        assert result or got[4] == 400000000, ('resources parent', got)
        assert not board.writes, ('resources wrote', board.writes)
    cases.append({'case': 'resources-supply', 'rows': [[c, r, e, s] for c, r, e, s in RESOURCES]})
    return cases


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True, help='new directory for the build and the result')
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    copies = {}
    for copy, (original, pinned) in COPIES.items():
        digest, text = diff_hash(copy, original)
        (out / (Path(copy).stem + '.diff')).write_text(text)
        assert digest == pinned, f'{copy} differs from {original} other than reviewed: {digest}'
        copies[copy] = {'original': original, 'diff_sha256': digest, 'pinned': pinned}
    elf, commands = build(out)
    segments, symbols = load(elf)
    result = {'physical_access': False, 'units': {u: sha((ROOT / u).read_bytes()) for u in UNITS + INCLUDED},
              'copies': copies, 'elf_sha256': sha(elf.read_bytes()), 'commands': commands, 'cases': []}
    for name, (model, expect) in CASES.items():
        result['cases'].append(run_case(segments, symbols, name, model, expect))
    u, board = machine(segments, symbols, {})
    bits = call(u, symbols, 'vsu_native')
    # Refusals 0-7, the admitted read and write 8-9, and no lock left held.
    assert bits == 0x3ff, hex(bits)
    assert board.writes == [['0x20020e0', hex(ENTRY | BIT(30))]], board.writes
    result['cases'].append({'case': 'native-allowlist', 'bits': hex(bits), 'writes': board.writes})
    result['cases'] += supply_cases(segments, symbols)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(result["cases"])} vendor-state unit cases passed; no physical access')


if __name__ == '__main__':
    main()
