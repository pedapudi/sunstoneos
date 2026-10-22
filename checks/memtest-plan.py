#!/usr/bin/env python3
"""Planner model of the memory qualification image (docs/memory-qualification.md).

Compiles apps/memtest/plan.c for the host and drives the image's own
admission function through apps/memtest/check.py: every refusal, the board
plan over the 12,224 MiB tested map, resume chaining, excluded ranges, the
seed rule against the beacon constant K and the run-count model of the time
budget. No board access.
"""
import argparse
import ctypes
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402  (assertion policy)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


host = module('memtest_host', ROOT / 'apps/memtest/check.py')
MIB = 1 << 20
BLOCK = 1 << 21


def blocks_mib(blocks):
    return blocks * 2


def layout_matches(out):
    """The ctypes mirrors match the C layouts the image and planner use."""
    checks = [('struct memtest_contract', host.Contract), ('struct memtest_geometry', host.Geometry),
              ('struct memtest_plan', host.Plan), ('struct memtest_facts', host.Facts),
              ('struct memtest_record', host.Record)]
    lines = ['#include "memtest.h"']
    for c, py in checks:
        lines.append(f'_Static_assert(sizeof({c}) == {ctypes.sizeof(py)}, "{c} size");')
        for name, _ in py._fields_:
            lines.append(f'_Static_assert(offsetof({c}, {name}) == {getattr(py, name).offset}, "{c}.{name}");')
    source = out / 'layout.c'
    source.write_text('\n'.join(lines) + '\n')
    subprocess.run(['clang-21', '-fsyntax-only', '-Wall', '-Werror', f'-I{ROOT}/apps/memtest', str(source)], check=True)


def good(**changes):
    values = dict(sequence=1, mask=0x1f, seed=0x0123456789abcdef, budget_ticks=host.BUDGET_LIMIT,
                  slice_blocks=(32, 6144), excluded=())
    values.update(changes)
    return host.make_contract(**values)


def refusals(lib):
    """Each refusal the contract alone can cause, and the CPU and monitor facts."""
    cases = {
        'contract': host.Contract(),
        'mask': good(mask=0x80),
        'mask-unbuilt': good(mask=0x1f | host.MASK['dma-cache']),
        'slice-empty': good(slice_blocks=(100, 100)),
        'slice-outside': good(slice_blocks=(31, 543)),
        'slice-excluded': good(slice_blocks=(32, 6144), excluded=[(1000, 1001)]),
        'excluded-count': good(slice_blocks=(32, 544), excluded=[(1000 + 2 * i, 1001 + 2 * i) for i in range(10)]),
        'excluded-empty': good(slice_blocks=(32, 544), excluded=[(1000, 1000)]),
        'excluded-order': good(slice_blocks=(32, 544), excluded=[(1000, 1010), (1005, 1020)]),
        'excluded-outside': good(slice_blocks=(32, 544), excluded=[(6100, 6145)]),
        'fixed-excluded': good(slice_blocks=(1000, 1512), excluded=[(32, 33)]),
        'grouping': good(slice_blocks=(32, 232)),
        'seed': good(seed=host.K),
        'budget': good(budget_ticks=0),
    }
    count = cases['excluded-count']
    count.excluded_count = 11
    seen = {}
    for reason, contract in cases.items():
        got, _ = host.admit(lib, contract)
        assert got == reason, (reason, got)
        seen[reason] = True
    outside = host.board_geometry()
    outside.fixed[2] = host.Extent(0x340000000 - (64 << 20), 128 << 20)
    assert host.admit(lib, good(), geometry=outside)[0] == 'fixed-outside'
    seen['fixed-outside'] = True
    facts = {'dczid': host.Facts(0x14, 1, host.WRITERS_MAGIC << 32),
             'dcache': host.Facts(4, (8 << 20) + 64, host.WRITERS_MAGIC << 32),
             'writers-unsampled': host.Facts(4, 1, 0x12),
             'writers': host.Facts(4, 1, (host.WRITERS_MAGIC << 32) | (1 << 2))}
    for reason, f in facts.items():
        assert host.admit(lib, good(), f)[0] == reason, reason
        seen[reason] = True
    assert host.admit(lib, good(), host.Facts(3, 1, host.WRITERS_MAGIC << 32))[0] == 'dczid'
    assert host.admit(lib, good(), host.Facts(4, 0, host.WRITERS_MAGIC << 32))[0] == 'dcache'
    # OR100, CE and USB0 together are the accepted remote writers.
    accepted = (host.WRITERS_MAGIC << 32) | (1 << 1) | (1 << 4) | (1 << 12)
    assert host.admit(lib, good(), host.Facts(4, 8 << 20, accepted))[0] == 'admitted'
    # 'mapping' and 'record' come from live translation and are image-only.
    assert set(seen) | {'admitted', 'mapping', 'record'} == set(host.REFUSALS), set(host.REFUSALS) - set(seen)
    return sorted(seen)


def seed_rule(lib):
    """Seeds whose march words or signatures could equal a beacon are refused."""
    k_high = host.K >> 34
    refused = 0
    for delta in (-2, -1, 0, 1, 2):
        for complement in (0, 1):
            high = (k_high + delta) & ((1 << 30) - 1)
            seed = (high << 34) | 0x123456789
            if complement:
                seed ^= 2**64 - 1
            got = host.admit(lib, good(seed=seed))[0]
            assert (got == 'seed') == (abs(delta) <= 1), (delta, complement, got)
            refused += got == 'seed'
    # For an admitted seed, no march word P(a) = a + seed or ~P(a) at a DRAM
    # address, and no signature a ^ seed, has K's bits 34-63.
    seed = (((k_high + 2) & ((1 << 30) - 1)) << 34) | ((1 << 34) - 1)
    assert host.admit(lib, good(seed=seed))[0] == 'admitted'
    for a in (0x40000000, 0x340000000 - 8, (1 << 34) - 8):
        for value in ((a + seed) % 2**64, ~(a + seed) % 2**64, a ^ seed):
            assert (value >> 34) != k_high
    return refused


def board_plan(lib):
    refusal, plan = host.admit(lib, good())
    assert refusal == 'admitted'
    pieces = [plan.piece[i] for i in range(plan.pieces)]
    groups = [plan.group[i] for i in range(plan.groups)]
    assert pieces[0].first == 32 and sum(p.blocks for p in pieces) == 6112
    assert all(pieces[i].first + pieces[i].blocks == pieces[i + 1].first for i in range(len(pieces) - 1))
    assert all(p.blocks <= 128 for p in pieces)
    covered = 0
    for g in groups:
        members = pieces[g.first_piece:g.first_piece + g.pieces]
        assert g.first_piece == covered and g.pieces in (2, 3)
        total = sum(m.blocks for m in members)
        assert all(total - m.blocks >= 128 for m in members), 'eviction rule'
        covered += g.pieces
    assert covered == len(pieces)
    # Each group boundary is a resume address; the remaining slice must plan.
    for g in groups[1:]:
        start = pieces[g.first_piece].first
        assert host.admit(lib, good(slice_blocks=(start, 6144)))[0] == 'admitted', start
    tested = (6144 - 32) * 2
    pages = tested * 256
    # Per-page and per-block operation counts the emulation measures on one
    # block, scaled to the board map: two scan loads per page, one DC ZVA,
    # store, DC CIVAC and load per page of the signature, one DSB per block.
    counts = {'scan_loads': 2 * pages, 'signature_operations_each': pages, 'signature_dsb': 6144 - 32}
    assert counts['scan_loads'] == 6258688 and pages == 3129344
    return {'tested_mib': tested, **counts, 'sub_slices': len(pieces), 'sub_slices_of_256_mib': tested / 256,
            'groups': len(groups), 'group_sizes': [g.pieces for g in groups]}


def excluded_ranges(lib):
    """A non-empty excluded list: the scan and signature skip it, slices stop at it."""
    excluded = [(1000, 1004), (3000, 3001)]
    assert host.admit(lib, good(slice_blocks=(32, 1000), excluded=excluded))[0] == 'admitted'
    assert host.admit(lib, good(slice_blocks=(1004, 3000), excluded=excluded))[0] == 'admitted'
    assert host.admit(lib, good(slice_blocks=(3001, 6144), excluded=excluded))[0] == 'admitted'
    assert host.admit(lib, good(slice_blocks=(999, 1600), excluded=excluded))[0] == 'slice-excluded'
    log = {'session': 1, 'excluded': excluded, 'runs': [
        {'session': 1, 'sequence': 1, 'status': 'pass', 'resume': host.DRAM_BASE + 1000 * BLOCK}]}
    contract, _ = host.next_contract(log, lib, [], 20)
    assert (contract.slice_start, contract.slice_end) == (1004, 3000) and contract.excluded_count == 2
    assert not contract.mask & (host.MASK['signature'] | host.MASK['harness'])
    first, _ = host.next_contract({'session': 2, 'excluded': excluded, 'runs': log['runs']}, lib, [], 20)
    assert first.mask & host.MASK['signature'] and (first.slice_start, first.slice_end) == (32, 1000)
    assert first.sequence == 2 and not host.near_k(first.seed)
    return {'spans': [[32, 1000], [1004, 3000], [3001, 6144]]}


def budget_clamp(lib):
    _, plan = host.admit(lib, good(budget_ticks=40 * host.COUNTER_HZ))
    assert plan.budget_ticks == host.BUDGET_LIMIT
    _, plan = host.admit(lib, good(budget_ticks=5 * host.COUNTER_HZ))
    assert plan.budget_ticks == 5 * host.COUNTER_HZ


def dcache_model(lib):
    """CLIDR and CCSIDR of a Cortex-A55 with 32 KiB L1D, 256 KiB L2 and a 2 MiB
    DSU L3 sum to 2.28 MiB, below the 8 MiB the eviction rule allows."""
    clidr = (3 << 0) | (4 << 3) | (4 << 6)
    def ccsidr(line, ways, sets):
        return (line.bit_length() - 5) | ((ways - 1) << 3) | ((sets - 1) << 13)
    values = (ctypes.c_uint64 * 7)(ccsidr(64, 4, 128), ccsidr(64, 4, 1024), ccsidr(64, 16, 2048))
    total = lib.memtest_dcache_bytes(clidr, values, 0)
    assert total == (32 + 256 + 2048) * 1024, total
    extended = (ctypes.c_uint64 * 7)(2 | (3 << 3) | (127 << 32))
    assert lib.memtest_dcache_bytes(3, extended, 1) == 64 * 4 * 128
    return total


def runs_model(lib):
    """Runs the march needs under the image's group-start rule, at the fast and
    slow ends of the cost model (0.91 and 1.45 s per 256 MiB)."""
    _, plan = host.admit(lib, good())
    groups = [sum(plan.piece[plan.group[g].first_piece + i].blocks for i in range(plan.group[g].pieces)) * 2
              for g in range(plan.groups)]
    first_estimate = 136000  # ticks per MiB before any group is timed
    results = {}
    for name, per_256, scan in (('fast', 0.91, 0.7), ('slow', 1.45, 1.25)):
        per_mib = per_256 / 256 * host.COUNTER_HZ
        runs, index = 0, 0
        while index < len(groups):
            runs += 1
            first = runs == 1
            now = scan + (1.5 if first else 0)
            limit = 20 - (2.5 if first else 0)
            slowest = first_estimate
            started = 0
            while index < len(groups):
                need = slowest * groups[index] / host.COUNTER_HZ
                if now + need > limit:
                    break
                now += per_mib * groups[index] / host.COUNTER_HZ
                slowest = per_mib
                index += 1
                started += 1
            assert started, 'a run made no progress'
        results[name] = runs
    assert 3 <= results['fast'] <= results['slow'] <= 5, results
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    layout_matches(args.out)
    lib = host.library(args.out)
    result = {'refusals': refusals(lib), 'seeds_refused_near_k': seed_rule(lib), 'board_plan': board_plan(lib),
              'excluded_ranges': excluded_ranges(lib), 'dcache_bytes_a55_model': dcache_model(lib),
              'runs_for_march': runs_model(lib)}
    budget_clamp(lib)
    harness.save(args.out / 'memtest-plan.json', result)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
