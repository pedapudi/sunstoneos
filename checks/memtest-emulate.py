#!/usr/bin/env python3
"""Unicorn checks of the linked memory qualification image.

The linked kernels and the image's own task function run in
checks/memtest-machine.py over a reduced tested map (blocks 32-63, 64 MiB)
with a modeled counter, a modeled monitor remote-writer sample and, unless a
case says otherwise, the memtest platform's tested-map translation entries
written into the platform's tables. Cases:

- kernel instruction counts per page or line, the A55 cost model, the work
  per 2 MiB block outside the inner loop, PRFM lookahead and load-to-use
  distances, against the budgets of docs/memory-qualification.md;
- complete runs: pass, the current platform's mapping and an absent writer
  sample refused, a non-empty excluded-range list, the time budget with
  renewal gaps, the time budget with a fault in every march block, and a
  damaged recovery page;
- injected faults: address bits 6-25 stuck and open; a single-bit wrap at each
  of bits 26-33, a multi-bit fold, a line-delta alias for each delta 0-63 and
  stuck line-index bits 6-11 with page aliases onto each recovery page, a
  two-bit alias, a page alias at a distance that is not a multiple of 64
  pages; a dropped write, a coupled word, a transient read error and an
  address decode fault in the march;
- the monitor's renewal SMC handler.
"""
import argparse
import collections
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # noqa: E402
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_WRITE  # noqa: E402
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP  # noqa: E402


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


machine = module('memtest_machine', ROOT / 'checks/memtest-machine.py')
host = module('memtest_host', ROOT / 'apps/memtest/check.py')
BLOCK = 1 << 21
MAP_START, MAP_END = 0x44000000, 0x48000000
RECORD = 0x42003000
RECOVERY = 0x41001000
TICK = 24000000
SEED = 0x0123456789abcdef


def geometry():
    return host.Geometry(32, 64, 2, 2, (host.Extent * 4)(
        host.Extent(0x44000000, 64), host.Extent(0x44000000, (1 << 25) + 64),
        host.Extent(0x46000000, 3 << 20), host.Extent(0x47000000, 1 << 20)))


def record_word(m, name):
    return m.read(RECORD + getattr(host.Record, name).offset)


class Run:
    """One complete run of the image's task over the reduced map."""

    def __init__(self, elf, mask=0x1f, slice_blocks=(32, 40), excluded=(), golden=True, writers=True,
                 step=2000, budget=20 * TICK, seed=SEED, extra_map=()):
        self.m = m = machine.Machine(elf, step=step)
        m.map(MAP_START, MAP_END - MAP_START)
        for address, size, alias in extra_map:
            m.map(address, size, alias)
        m.platform_tables(golden)
        m.counts.clear()
        m.uc.mem_write(m.address('memtest_geometry'), bytes(geometry()))
        self.contract = host.make_contract(7, mask, seed, budget, slice_blocks, excluded)
        m.uc.mem_write(m.address('memtest_contract'), bytes(self.contract))
        if writers:
            m.write(RECORD + 16, (host.WRITERS_MAGIC << 32) | (1 << 1) | (1 << 4) | (1 << 12))

    def go(self):
        m = self.m
        m.call('run', 0)
        assert m.exit_status is not None, f'run did not reach platform_exit; pc {m.pc():#x}'
        raw = bytes(m.uc.mem_read(RECORD, 4096))
        self.record = host.decode(raw, self.contract)
        return self.record


def kernel_profile(elf):
    """Instruction mix of each kernel over one 2 MiB block in each direction."""
    m = machine.Machine(elf)
    m.map(MAP_START, 4 * BLOCK)
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    text = {}
    for address, data in m.e.parts:
        for insn in disassembler.disasm(data, address):
            text[insn.address] = (insn.mnemonic, insn.op_str)
    walk_at = 0x43f00000
    results = {}

    def walk(cursor, end, block_step, line_step, complement=0, deadline=1 << 62):
        w = struct.pack('<QQqqQQQQQQQQQ', cursor, end, block_step, line_step, SEED, deadline, 1 << 62,
                        complement, RECORD + 72, 0, 0, host.K | RECOVERY, 0x4000)
        m.uc.mem_write(walk_at, w)

    def traced(name, *args):
        start = m.address(name)
        trace = []
        hook = m.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: trace.append(a), begin=start, end=start + 0x1800)
        m.call(name, *args)
        m.uc.hook_del(hook)
        return trace

    def mix(trace):
        return collections.Counter(text[a][0] for a in trace)

    def first_use(trace, index, registers):
        for j in range(index + 1, len(trace)):
            mnemonic, operands = text[trace[j]]
            sources = operands if mnemonic in ('st1', 'cmp', 'ccmp', 'str') else operands.split(',', 1)[-1]
            if mnemonic in ('ld1', 'ldr') and not any(r in operands.split('[')[1] for r in registers):
                continue
            if any(r in sources for r in registers):
                return j - index - 1
        return None

    def load_to_use(trace, mnemonic, base):
        """Fewest instructions between an inner-loop load from base and the
        first instruction that reads a register it loaded."""
        distances = []
        for i, a in enumerate(trace):
            if text[a][0] != mnemonic or '[' + base not in text[a][1]:
                continue
            operands = text[a][1]
            if mnemonic == 'ld1':
                registers = [r.split('.')[0] for r in operands.split('}')[0].strip('{ ').split(', ')]
            else:
                registers = [operands.split(',')[0]]
            registers = [r + ('.' if r.startswith('v') else ',') for r in registers]
            d = first_use(trace, i, registers)
            if d is not None:
                distances.append(d)
        return min(distances)

    lines = BLOCK // 64
    pages = BLOCK // 4096
    base = MAP_START
    # Scan: two loads per page.
    walk(base, base + BLOCK, BLOCK, 64)
    trace = traced('memtest_scan', walk_at)
    c = mix(trace)
    loads = sum(1 for a in trace if text[a][0] == 'ldr' and '[x5' in text[a][1])
    assert loads == 2 * pages and 'dc' not in c and 'dsb' not in c
    results['scan'] = {'instructions_per_page': round(len(trace) / pages, 3),
                       'load_to_use_min': load_to_use(trace, 'ldr', 'x5'), 'counter_reads': m.counts.get('cntpct_el0', 0)}
    assert results['scan']['instructions_per_page'] <= 9 and results['scan']['load_to_use_min'] >= 15
    # Signature write: no load from the block, one DC ZVA, store and DC CIVAC per page.
    reads = []
    hook = m.uc.hook_add(UC_HOOK_MEM_READ, lambda uc, acc, a, s, v, d: reads.append(a), begin=base, end=base + BLOCK - 1)
    m.counts.clear()
    walk(base, base + BLOCK, BLOCK, 64)
    trace = traced('memtest_sig_write', walk_at)
    m.uc.hook_del(hook)
    c = mix(trace)
    assert not reads and m.counts['dc zva'] == pages and m.counts['dc civac'] == pages and c['dsb'] == 1
    assert all(m.read(base + 4096 * i) == (base + 4096 * i) ^ SEED for i in range(pages))
    results['signature_write'] = {'instructions_per_page': round(len(trace) / pages, 3), 'dsb_per_block': c['dsb'],
                                  'linefill_reads': len(reads)}
    assert results['signature_write']['instructions_per_page'] <= 7
    walk(base, base + BLOCK, BLOCK, 64)
    trace = traced('memtest_sig_read', walk_at)
    assert m.read(walk_at) == base + BLOCK
    results['signature_read'] = {'instructions_per_page': round(len(trace) / pages, 3),
                                 'load_to_use_min': load_to_use(trace, 'ldr', 'x5')}
    assert results['signature_read']['instructions_per_page'] <= 7
    # March kernels in both directions over the second and third blocks.
    cost = {'ld1': 8, 'st1': 8}
    budgets = {'memtest_march_w': 12, 'memtest_march_r': 21, 'memtest_march_rw': 33.5}
    for name, complement in (('memtest_march_w', 0), ('memtest_march_rw', 0), ('memtest_march_rw', 1),
                             ('memtest_march_r', 0)):
        for direction in ('up', 'down'):
            low = base + BLOCK
            if direction == 'up':
                walk(low, low + BLOCK, BLOCK, 64, complement)
            else:
                walk(low, low - BLOCK, -BLOCK, -64, complement)
            if name != 'memtest_march_w':
                # Store the words the element expects, so the accumulator stays zero.
                data = b''.join(struct.pack('<Q', ((a + SEED) ^ (2**64 - 1 if complement else 0)) % 2**64)
                                for a in range(low, low + BLOCK, 8))
                m.uc.mem_write(low, data)
            prefetch = []
            sites = [a for a, (mn, _) in text.items() if mn == 'prfm' and m.address(name) <= a < m.address(name) + 0x2000]
            hooks = [m.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: prefetch.append(
                (uc.reg_read(UC_ARM64_REG_X0 + 6) + uc.reg_read(UC_ARM64_REG_X0 + 8)) % 2**64), begin=a, end=a)
                for a in sites]
            m.counts.clear()
            status_trace = traced(name, walk_at)
            for h in hooks:
                m.uc.hook_del(h)
            assert m.read(walk_at + 72) == 0 and m.uc.reg_read(UC_ARM64_REG_X0) == 0, 'no hit expected'
            c = mix(status_trace)
            loop = sum(c[k] for k in c)
            per_line = loop / lines
            cycles = sum(cost.get(k, 1) * v for k, v in c.items() if k not in ('b.ne', 'b')) / lines
            branch = (c['b.ne'] / 2) / lines
            key = f'{name[8:]}{"_complement" if complement else ""}_{direction}'
            entry = {'instructions_per_line': round(per_line, 3), 'cycles_per_line': round(cycles + branch, 3)}
            outside = {k: c[k] for k in ('isb', 'umaxv', 'dsb', 'dmb', 'dc', 'bl', 'blr', 'smc')}
            outside['cntpct_reads'] = m.counts.get('cntpct_el0', 0)
            outside['dc'] = sum(v for k, v in m.counts.items() if isinstance(k, str) and k.startswith('dc '))
            assert outside['isb'] == 1 and outside['cntpct_reads'] == 1 and not any(
                outside[k] for k in ('dsb', 'dmb', 'dc', 'bl', 'blr', 'smc'))
            assert outside['umaxv'] == (0 if name == 'memtest_march_w' else 1)
            entry['per_block'] = outside
            assert m.read(RECORD + 72) == low, 'one progress store naming the block'
            if name != 'memtest_march_w':
                assert c['ld1'] == lines and c['prfm'] == lines - 1
                step = 64 if direction == 'up' else -64
                ahead = collections.Counter()
                for i, target in enumerate(prefetch):
                    line = low + (i * step if direction == 'up' else BLOCK - 64 + i * step)
                    ahead[(target - line) // step] += 1
                # A one-block walk is the member's last block: its last 15
                # prefetches name the next line instead of reaching past it.
                assert ahead == {16: lines - 16, 1: 15}, ahead
                entry['prefetch_lines_ahead'] = dict(ahead)
                # The block's first line has no earlier line to overlap with.
                first = next(i for i, a in enumerate(status_trace) if text[a][0] == 'ld1')
                entry['load_to_use_instructions_min'] = load_to_use(status_trace[first + 1:], 'ld1', 'x6')
                # A full line of work (17 or 12 instructions after its load) lies between.
                assert entry['load_to_use_instructions_min'] >= (12 if name == 'memtest_march_r' else 17), (name, entry)
            results[key] = entry
            assert entry['cycles_per_line'] <= budgets[name] + 0.25, (key, entry)
    # A member's last block: the last 16 lines prefetch the line the next load reads.
    walk(base + BLOCK, base + 2 * BLOCK, BLOCK, 64)
    data = b''.join(struct.pack('<Q', (a + SEED) % 2**64) for a in range(base + BLOCK, base + 2 * BLOCK, 8))
    m.uc.mem_write(base + BLOCK, data)
    targets = []
    name = 'memtest_march_r'
    sites = [a for a, (mn, _) in text.items() if mn == 'prfm' and m.address(name) <= a < m.address(name) + 0x2000]
    hooks = [m.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: targets.append(
        (uc.reg_read(UC_ARM64_REG_X0 + 6) + uc.reg_read(UC_ARM64_REG_X0 + 8)) % 2**64), begin=a, end=a)
        for a in sites]
    m.call(name, walk_at)
    for h in hooks:
        m.uc.hook_del(h)
    assert max(targets) < base + 2 * BLOCK, 'prefetch past the member'
    results['last_block_prefetch_max'] = hex(max(targets))
    # Harness kernels over 1 MiB and the chase.
    mib = 1 << 20
    for name, args in (('memtest_read_up', (base, mib)), ('memtest_read_down', (base, mib)),
                       ('memtest_write', (base, mib)), ('memtest_copy', (base + mib, base, mib))):
        trace = traced(name, *args)
        results[name[8:]] = round(len(trace) / (mib // 64), 3)
    assert results['read_up'] <= 2 and results['read_down'] <= 2 and results['write'] <= 3 and results['copy'] <= 5
    for i in range(256):
        m.write(base + 64 * i, base + 64 * ((i * 5 + 1) % 256))
    trace = traced('memtest_chase', base, 4096)
    results['chase_per_load'] = round((len(trace) - 1) / 4096, 4)  # less the return
    assert results['chase_per_load'] <= 1.25
    return results


def full_runs(elf):
    out = {}
    # Pass, with every built test.
    r = Run(elf)
    renewals = r.m.renewals
    rec = r.go()
    assert rec['status'] == 'pass' and r.m.exit_status == 0, rec
    assert all(rec['tests'][t]['status'] == 'pass' for t in ('scan', 'data', 'address', 'signature', 'march',
                                                             'harness')), rec['tests']
    assert rec['resume'] == 0x40000000 + 40 * BLOCK and rec['groups'] == 2
    assert rec['address_tested'] == ((1 << 34) - 1) & ~63 and rec['address_failed'] == 0
    assert rec['tests']['scan']['words'] == 2 * 32 * 512 and rec['digest_admission'] == rec['digest_last']
    assert r.m.counts.get('at s1e1w', 0) == 3, r.m.counts
    assert rec['remote_writers'] >> 32 == host.WRITERS_MAGIC and rec['daif'] & 0x100 == 0
    assert rec['dczid'] == 4 and rec['dcache_bytes'] == (32 + 256 + 2048) * 1024
    assert len(rec['chase']) == 4 and all(s['ticks'][2] for s in rec['samples'])
    out['pass'] = {'translations': r.m.counts.get('at s1e1w', 0), 'renewals': len(renewals),
                   'march_words': rec['tests']['march']['words'], 'signature_pages': rec['tests']['signature']['words']}
    # The current platform maps 128 MiB, so admission refuses the run.
    r = Run(elf, golden=False)
    rec = r.go()
    assert rec['status'] == 'refused' and rec['refusal'] == 'mapping' and r.m.exit_status == 1
    out['current_platform'] = rec['refusal']
    rec = Run(elf, writers=False).go()
    assert rec['refusal'] == 'writers-unsampled'
    rec = Run(elf, mask=0x1f | host.MASK['dma-reach']).go()
    assert rec['refusal'] == 'mask-unbuilt'
    rec = Run(elf, seed=host.K).go()
    assert rec['refusal'] == 'seed'
    out['refusals'] = ['mapping', 'writers-unsampled', 'mask-unbuilt', 'seed']
    # A non-empty excluded-range list: no access to the excluded block.
    r = Run(elf, slice_blocks=(56, 64), excluded=[(52, 53)])
    touched = []
    excluded = 0x40000000 + 52 * BLOCK
    for kind in (UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE):
        r.m.uc.hook_add(kind, lambda uc, acc, a, s, v, d: touched.append(a), begin=excluded, end=excluded + BLOCK - 1)
    rec = r.go()
    assert rec['status'] == 'pass' and not touched and rec['tests']['scan']['words'] == 2 * 31 * 512
    out['excluded_range'] = {'scan_pages': rec['tests']['scan']['words'] // 2, 'accesses_to_excluded_block': 0}
    # Time budget: 11 ms of counter per read (one 2 MiB march block at the
    # slow rate). The march stops at a group boundary and records it.
    step = 11 * TICK // 1000
    r = Run(elf, mask=0x08, slice_blocks=(32, 64), step=step, budget=TICK)
    rec = r.go()
    gaps = [b - a for a, b in zip(r.m.renewals, r.m.renewals[1:])]
    assert rec['status'] == 'pass' and 0 < rec['groups'] < 8
    assert rec['resume'] == 0x40000000 + (32 + 4 * rec['groups']) * BLOCK
    assert rec['end_ticks'] - rec['start_ticks'] <= TICK + step
    assert max(gaps) <= TICK // 5 + 2 * step, gaps
    out['budget'] = {'groups': rec['groups'], 'resume_block': (rec['resume'] - 0x40000000) // BLOCK,
                     'wall_ticks': rec['end_ticks'] - rec['start_ticks'], 'max_renewal_gap_ticks': max(gaps),
                     'counter_step_ticks': step}
    # A fault in every block: one word per block reads as a value that is not
    # another address's pattern, so every block hits and is rescanned. Once
    # the march starts the counter moves 0.1 s per read. The run still ends
    # within the budget plus one block, as a clean run does.
    def every_block(faulted):
        r = Run(elf, mask=0x08, slice_blocks=(32, 40), budget=TICK)
        m = r.m
        def slow(uc, a, s, d):
            m.step = TICK // 10
        m.uc.hook_add(UC_HOOK_CODE, slow, begin=m.address('memtest_march_w'), end=m.address('memtest_march_w'))
        if faulted:
            for block in range(32, 40):
                word = 0x40000000 + block * BLOCK + 0x108
                m.uc.hook_add(UC_HOOK_MEM_READ, lambda uc, acc, a, s, v, d, word=word: uc.mem_write(
                    word, (0x5555555555555555).to_bytes(8, 'little')), begin=word, end=word + 7)
        return r.go()
    clean, faulted = every_block(False), every_block(True)
    step = TICK // 10
    assert clean['status'] == 'pass' and faulted['status'] == 'fail' and faulted['mismatch_count'] >= 1
    assert faulted['tests']['march']['status'] == 'fail'
    wall = {name: rec['end_ticks'] - rec['start_ticks'] for name, rec in (('clean', clean), ('faulted', faulted))}
    # The model adds a step at every counter read: one for the block in which
    # the deadline passes and one each for the closing result and record.
    assert wall['faulted'] <= TICK + 3 * step and wall['faulted'] <= wall['clean'], wall
    out['fault_in_every_block'] = {'budget_ticks': TICK, 'counter_step_ticks': step, 'wall_ticks': wall,
                                   'mismatches': faulted['mismatch_count'], 'groups': faulted['groups']}
    # A damaged recovery page after admission stops the run at the next digest.
    r = Run(elf, mask=0x08)
    damaged = []
    def damage(uc, a, s, d):
        if not damaged and record_word(r.m, 'digest_admission'):
            r.m.write(0x41002800, 0)
            damaged.append(a)
    r.m.uc.hook_add(UC_HOOK_CODE, damage, begin=r.m.address('memtest_march_w'), end=r.m.address('memtest_march_w'))
    rec = r.go()
    assert rec['status'] == 'stopped' and rec['stop'] == 'digest' and rec['groups'] == 0
    out['digest'] = rec['stop']
    return out


def alias_hooks(m, page, target_of):
    """Redirect every access to the tested page through target_of(offset)."""
    def read(uc, access, address, size, value, data):
        for a in range(address & ~7, address + size, 8):
            uc.mem_write(a, bytes(uc.mem_read(target_of(a - page), 8)))
    def write(uc, access, address, size, value, data):
        uc.mem_write(target_of(address - page), (value % 2**64).to_bytes(8, 'little')[:size])
    m.uc.hook_add(UC_HOOK_MEM_READ, read, begin=page, end=page + 4095)
    m.uc.hook_add(UC_HOOK_MEM_WRITE, write, begin=page, end=page + 4095)


def scan_case(elf, page, recovery=RECOVERY, target_of=None):
    """Scan only the block holding page, with page aliased onto a recovery
    page: through a shared host mapping when the block lies outside the
    reduced map, or through access hooks (target_of) inside it."""
    block = page & ~(BLOCK - 1)
    inside = MAP_START <= block < MAP_END
    extra = []
    if not inside and target_of is None:
        extra = [(block, page - block, None), (page, 4096, recovery), (page + 4096, block + BLOCK - page - 4096, None)]
        extra = [x for x in extra if x[1]]
    elif not inside:
        extra = [(block, BLOCK, None)]
    r = Run(elf, mask=0, extra_map=extra)
    g = geometry()
    g.first_block = (block - 0x40000000) // BLOCK
    g.end_block = g.first_block + 1
    g.fixed = (host.Extent * 4)(*[host.Extent(block, 64)] * 4)
    r.m.uc.mem_write(r.m.address('memtest_geometry'), bytes(g))
    if inside or target_of is not None:
        alias_hooks(r.m, page, target_of or (lambda o: recovery + o))
    rec = r.go()
    assert rec['status'] == 'stopped' and rec['stop'] == 'scan', rec
    assert rec['scan_page'] & ~4095 == page, (hex(rec['scan_page']), hex(page))
    return rec


def scan_faults(elf):
    out = {}
    # Single-bit wraps at offset bits 26-33 onto the vector page: the hit
    # names the bit. An offset is the physical address minus the DRAM base.
    for bit in range(26, 34):
        page = 0x40000000 + ((RECOVERY - 0x40000000) ^ (1 << bit))
        rec = scan_case(elf, page)
        assert rec['address_failed'] == 1 << bit, (bit, hex(rec['address_failed']))
    out['single_bit_wraps'] = list(range(26, 34))
    rec = scan_case(elf, RECOVERY ^ (1 << 27) ^ (1 << 31))  # bit 30 is common, so offsets differ in 27 and 31
    assert rec['address_failed'] == (1 << 27) | (1 << 31)
    rec = scan_case(elf, 0x41003000 ^ (1 << 14) ^ (1 << 28), recovery=0x41003000)
    assert rec['address_failed'] == (1 << 14) | (1 << 28)
    out['multi_bit'] = ['bits 27 and 31 onto the vector page', 'bits 14 and 28 onto the state page']
    # Page alias at a distance that is not a multiple of 64 pages.
    page = 0x44025000
    assert ((page - RECOVERY) // 4096) % 64
    scan_case(elf, page)
    out['page_distance_pages'] = (page - RECOVERY) // 4096
    # Line deltas 0-63 onto each recovery page: line i reaches line i ^ d.
    page = 0x44031000
    deltas = 0
    for recovery in range(RECOVERY, RECOVERY + 0x4000, 0x1000):
        for d in range(64):
            scan_case(elf, page, target_of=lambda o, d=d, recovery=recovery: recovery + (o ^ (d << 6)))
            deltas += 1
    out['line_delta_cases'] = deltas
    # Stuck line-index bits 6-11 at 0 and at 1, with a page alias onto each
    # recovery page.
    stuck = 0
    for recovery in range(RECOVERY, RECOVERY + 0x4000, 0x1000):
        for bit in range(6, 12):
            for value in (0, 1):
                def target(o, bit=bit, value=value, recovery=recovery):
                    return recovery + ((o | (1 << bit)) if value else (o & ~(1 << bit)))
                scan_case(elf, page, target_of=target)
                stuck += 1
    out['stuck_line_bit_cases'] = stuck
    return out


def word_fault(elf, fault, **run):
    r = Run(elf, **run)
    m = r.m
    fault(m)
    return r, r.go()


def line_faults(elf):
    out = {}
    base = 0x44000000
    # Address lines 6-25: stuck at 0 sends base ^ 2^b to the base; open (here
    # stuck at 1) sends the base to base ^ 2^b.
    for bit in range(6, 26):
        for kind in ('stuck', 'open'):
            def fault(m, bit=bit, kind=kind):
                partner = base ^ (1 << bit)
                source, target = (partner, base) if kind == 'stuck' else (base, partner)
                def read(uc, acc, a, s, v, d):
                    if a & ~7 == source:
                        uc.mem_write(source, bytes(uc.mem_read(target, 8)))
                def write(uc, acc, a, s, v, d):
                    if a & ~7 == source:
                        uc.mem_write(target, (v % 2**64).to_bytes(8, 'little'))
                m.uc.hook_add(UC_HOOK_MEM_READ, read, begin=source, end=source + 7)
                m.uc.hook_add(UC_HOOK_MEM_WRITE, write, begin=source, end=source + 7)
            _, rec = word_fault(elf, fault, mask=0x02)
            assert rec['tests']['address']['status'] == 'fail' and rec['address_failed'] >> bit & 1, (bit, kind, rec)
            assert rec['status'] == 'fail'
    out['address_bits'] = '6-25 stuck and open'
    return out


def march_faults(elf):
    out = {}
    victim = 0x44100108
    # Dropped write: the word keeps its power-on value.
    def dropped(m):
        def read(uc, acc, a, s, v, d):
            uc.mem_write(victim, bytes(8))
        m.uc.hook_add(UC_HOOK_MEM_READ, read, begin=victim, end=victim + 7)
    _, rec = word_fault(elf, dropped, mask=0x08)
    assert rec['status'] == 'fail' and rec['mismatches'][0]['address'] == victim, rec['mismatches'][:2]
    assert rec['tests']['march']['status'] == 'fail' and rec['groups'] == 1
    out['dropped_write'] = {'first': hex(victim), 'mismatches': rec['mismatch_count']}
    # Coupled word: each write to the aggressor inverts bit 0 of the victim.
    aggressor = 0x44100200
    def coupled(m):
        def write(uc, acc, a, s, v, d):
            old = int.from_bytes(uc.mem_read(victim, 8), 'little')
            uc.mem_write(victim, (old ^ 1).to_bytes(8, 'little'))
        m.uc.hook_add(UC_HOOK_MEM_WRITE, write, begin=aggressor, end=aggressor + 7)
    _, rec = word_fault(elf, coupled, mask=0x08)
    assert rec['status'] == 'fail' and {x['address'] for x in rec['mismatches']} == {victim}, rec['mismatches']
    # An up element rewrites the aggressor after the victim inside the same
    # block, which restores the victim before the block's rescan: those hits
    # count as unconfirmed, which also fails the run.
    out['coupled_word'] = {'victim': hex(victim), 'elements': sorted({x['element'] for x in rec['mismatches']}),
                           'unconfirmed': rec['unconfirmed']}
    # Transient read error in the last (read-only) element: the rescan finds
    # nothing, so the hit counts as unconfirmed and the run fails.
    def transient(m):
        state = {'done': False}
        def read(uc, acc, a, s, v, d):
            if not state['done'] and record_word(m, 'phase') == (4 << 4) | 5:
                state['done'] = True
                true = bytes(uc.mem_read(victim, 8))
                uc.mem_write(victim, bytes(b ^ 0xff for b in true))
                state['restore'] = true
            elif 'restore' in state:
                uc.mem_write(victim, state.pop('restore'))
        m.uc.hook_add(UC_HOOK_MEM_READ, read, begin=victim, end=victim + 7)
    _, rec = word_fault(elf, transient, mask=0x08)
    assert rec['status'] == 'fail' and rec['unconfirmed'] == 1 and rec['mismatch_count'] == 0, rec
    out['transient_read'] = {'unconfirmed': rec['unconfirmed']}
    # Address decode fault: a write to one word also lands on another, which
    # then reads the first word's pattern. The run stops before its next write.
    other = 0x44180040
    writes_after = []
    def decode_fault(m):
        def write(uc, acc, a, s, v, d):
            uc.mem_write(other, (v % 2**64).to_bytes(8, 'little'))
        m.uc.hook_add(UC_HOOK_MEM_WRITE, write, begin=victim, end=victim + 7)
    r, rec = word_fault(elf, decode_fault, mask=0x08)
    assert rec['status'] == 'stopped' and rec['stop'] == 'decode' and rec['mismatches'][-1]['address'] == other, rec
    assert rec['groups'] == 0
    out['decode_fault'] = {'word': hex(other), 'element': rec['mismatches'][-1]['element']}
    # Signature: a page that holds another page's signature stops the run
    # before the march.
    def signature_alias(m):
        page, source = 0x44200000, 0x44201000
        def write(uc, acc, a, s, v, d):
            uc.mem_write(page, (v % 2**64).to_bytes(8, 'little'))
        m.uc.hook_add(UC_HOOK_MEM_WRITE, write, begin=source, end=source + 7)
    _, rec = word_fault(elf, signature_alias, mask=0x0c)
    assert rec['stop'] == 'decode' and rec['tests']['march']['status'] == 'not-run', rec
    out['signature_decode'] = rec['stop']
    return out


def fixed_tests(elf):
    """Accesses, cache maintenance and barriers of the address-line test and
    of the recovery digest, counted during a run by the record's phase word."""
    r = Run(elf, mask=0x0a)
    m = r.m
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    sites = {insn.address: insn.mnemonic + (' ' + insn.op_str.split(',')[0] if insn.mnemonic == 'dc' else '')
             for address, data in m.e.parts for insn in disassembler.disasm(data, address)
             if insn.mnemonic in ('dsb', 'dmb', 'isb', 'dc')}
    digest = m.address('recovery_digest')
    state = {'digest': False}
    counts = collections.defaultdict(collections.Counter)

    def phase():
        if state['digest']:
            return 'recovery_digest'
        return {2 << 4: 'address_lines'}.get(record_word(m, 'phase'))

    def access(uc, kind, a, size, v, d):
        p = phase()
        if p == 'address_lines' and MAP_START <= a < MAP_END or p == 'recovery_digest' and RECOVERY <= a < RECOVERY + 0x4000:
            counts[p]['write_bytes' if kind == UC_MEM_WRITE else 'read_bytes'] += size
    for kind in (UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE):
        m.uc.hook_add(kind, access)
    for a, name in sites.items():
        m.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d, name=name: phase() and counts[phase()].update([name]),
                      begin=a, end=a)
    # The digest runs at admission and after each march group; count the first.
    def enter(uc, a, s, d):
        state['digest'] = not counts['recovery_digest']
    def leave(uc, a, s, d):
        state['digest'] = False
    m.uc.hook_add(UC_HOOK_CODE, enter, begin=digest, end=digest)
    disasm = list(disassembler.disasm(bytes(m.uc.mem_read(digest, 0x200)), digest))
    ret = next(i.address for i in disasm if i.mnemonic == 'ret')
    m.uc.hook_add(UC_HOOK_CODE, leave, begin=ret, end=ret)
    rec = r.go()
    assert rec['status'] == 'pass'
    out = {name: dict(c) for name, c in counts.items()}
    a = out['address_lines']
    assert a['read_bytes'] == 21 * 8 and a['write_bytes'] == 21 * 8 and a['dc civac'] == 21 and a['dsb'] == 1, a
    d = out['recovery_digest']
    assert d == {'read_bytes': 12 * 1024, 'dc civac': 192, 'dsb': 1}, d
    return out


def host_round_trip(elf, binary, out):
    """apps/memtest/check.py prepares a bundle from a run log; its execute()
    uploads it through a modeled FEL transaction to the emulated image on the
    current platform, which refuses at the mapping check, and decodes the
    returned record against the uploaded contract."""
    import argparse as ap
    import contextlib
    import io
    bundle = out / 'host-bundle'
    shutil.rmtree(bundle, ignore_errors=True)
    log = out / 'run-log.json'
    accepted = {'by': 'model user', 'date': '2026-10-22', 'residuals': list(host.RESIDUALS)}
    log.write_text(json.dumps({'session': 1, 'runs': [], 'acceptance': accepted}))
    bundle.mkdir(parents=True)
    with contextlib.redirect_stdout(io.StringIO()):
        host.prepare(ap.Namespace(elf=elf, image=binary, run_log=log, output=bundle, mask=[], budget=30.0))
    p = json.loads((bundle / 'prepared.json').read_text())
    assert p['entry'] == 0x41000000 and p['contract_address'] == 0x41005000 and p['budget_ticks'] == 20 * TICK
    assert p['mask'] == 0x1f and p['slice'] == [32, 6144] and p['groups'] == 23 and p['acceptance'] == accepted
    m = machine.Machine(elf)
    m.platform_tables(golden=False)
    requests = []

    def transaction(*arguments, pass_fds=()):
        requests.append(arguments[0])
        args = list(arguments)
        while args:
            verb = args.pop(0)
            if verb == 'write':
                address, path = int(args.pop(0), 16), Path(args.pop(0))
                m.uc.mem_write(address, path.read_bytes())
            elif verb == 'reset64':
                assert int(args.pop(0), 16) == m.address('monitor_entry')
                m.write(RECORD + 16, (host.WRITERS_MAGIC << 32) | (1 << 1))
                m.call('run', 0)
            elif verb == 'read':
                address, size, path = int(args.pop(0), 16), args.pop(0), Path(args.pop(0))
                path.write_bytes(bytes(m.uc.mem_read(address, size)))
        return ''
    record = {}
    decoded = host.execute(transaction, lambda: None, lambda: None, p, record, lambda: None, bundle, out)
    assert decoded['refusal'] == 'mapping' and decoded['sequence'] == 1 and record['complete']
    assert record['launch_attempted'] and requests == ['write', 'reset64', 'read']
    return {'refusal': decoded['refusal'], 'requests': requests}


def renewal(elf):
    """The monitor's 0x734 handler returns zero and moves CNTPS_CVAL_EL1 2 s on."""
    m = machine.Machine(elf, step=0)
    start = m.address('monitor_sync')
    eret = start + 0x38
    assert int.from_bytes(m.uc.mem_read(eret, 4), 'little') == 0xd69f03e0
    m.sys[(3, 6, 5, 2, 0)] = 0x17 << 26
    m.now = 5 * TICK
    m.uc.reg_write(UC_ARM64_REG_SP, 0x43ff0000)
    for i, value in enumerate((0x734, 11, 22, 33)):
        m.uc.reg_write(UC_ARM64_REG_X0 + i, value)
    m.uc.emu_start(start, eret)
    regs = [m.uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(4)]
    assert regs == [0, 11, 22, 33] and m.uc.reg_read(UC_ARM64_REG_SP) == 0x43ff0000
    assert m.sys[(3, 7, 14, 2, 2)] == m.now + 2 * TICK
    # Any other SMC ends in the monitor's exception path.
    m.uc.reg_write(UC_ARM64_REG_X0, 0x735)
    m.uc.emu_start(start, m.address('monitor_exception'))
    assert m.pc() == m.address('monitor_exception')
    return {'cntps_cval_advance_ticks': 2 * TICK}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--only', choices=('kernels', 'fixed', 'runs', 'scan', 'lines', 'march', 'host', 'renewal'),
                        action='append')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    cases = {'kernels': kernel_profile, 'fixed': fixed_tests, 'runs': full_runs, 'scan': scan_faults, 'lines': line_faults,
             'march': march_faults, 'host': lambda elf: host_round_trip(elf, args.binary, args.out),
             'renewal': renewal}
    result = {}
    for name, case in cases.items():
        if not args.only or name in args.only:
            result[name] = case(args.elf)
            print(name, json.dumps(result[name]), flush=True)
    harness.save(args.out / ('memtest-emulate.json' if not args.only else f'memtest-emulate-{"-".join(args.only)}.json'),
                 result)


if __name__ == '__main__':
    main()
