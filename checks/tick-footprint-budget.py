#!/usr/bin/env python3
"""Hold CPU0's busy-tick cache footprint to its recorded budget per segment and structure.

The eight-core latency cause model runs under QEMU with tools/qemu-tick-footprint.c,
which records CPU0's instructions and data accesses inside busy-phase ticks.
tools/tick-footprint.py resolves every 64-byte line to its structure. For each
segment kind (CPU0's own release, budgeted separately for the direct insertion
and for the rarer path through ss_owner_publish; the largest remote release;
the largest deadline scan; and the work outside them) the check takes the largest line count
of each structure over all traced ticks and compares it with
checks/tick-footprint-budget.json. A data count above its budget, or a
structure with no budget, fails; code counts above their budget are reported.
Diagnostic hooks are excluded.

On the board, under streaming load, each such line costs one refill (about 1,268
cycles; evidence entry 1794504237145636168-9aa4ebd9). QEMU timing is not evidence;
the line counts are.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
PLUGIN_HEADER = CACHE / 'host/qemu-plugin-10.2.1/qemu-plugin.h'
PLUGIN_HEADER_SHA256 = 'aebc639d01685893a9d71b9ef3afa7ea67e0be6a8959781c4057bd4974d05d4d'
BUDGET = ROOT / 'checks/tick-footprint-budget.json'
CHECK_INPUTS = ['tools/qemu-tick-footprint.c', 'tools/tick-footprint.py', 'tools/qemu-plugin-glib',
                'checks/tick-footprint-budget.json']
GATE_RUNS = [['--build', '{build:latency-cause-model}', '--output', '{output}']]
WINDOWS = 240
# The busy phase lasts a fixed number of wake samples, not of ticks; under
# multi-threaded TCG it spanned 234 to 370 ticks depending on host load. Fewer
# than this many traced ticks means the phase did not run.
MINIMUM = 200
BUSY = 2  # enum phase in apps/latency/workload.c: QUIET, IDLE, BUSY, STOP


def symbols(elf):
    with open(elf, 'rb') as f:
        table = ELFFile(f).get_section_by_name('.symtab')
        found = {}
        for s in table.iter_symbols():
            if s['st_shndx'] != 'SHN_UNDEF' and s.name:
                found.setdefault(s.name, []).append((s['st_value'], s['st_size']))
        return found


def one(found, name):
    rows = found.get(name, [])
    if len(rows) != 1:
        raise ValueError(f'{name}: expected one symbol, found {len(rows)}')
    return rows[0]


def publish_split_case(tool, elf, found, hooks, out):
    """A forced CPU0 release window that touches five ssKernelCPUs lines.

    Through ss_owner_publish it must count as 'local publish' and fit that
    budget; without the split, or on the direct path, the same lines must count
    as 'local' and exceed its budget of 3, so the split cannot loosen the
    direct path."""
    budget = json.loads(BUDGET.read_text())
    base, size = one(found, 'ssKernelCPUs')
    lines = 5
    assert size // 8 >= lines * 64, 'ssKernelCPUs[0] spans fewer than five lines'
    publish = one(found, 'ss_owner_publish')[0]
    enqueue = one(found, 'ss_owner_enqueue')[0]
    marks = {n: hooks[n] for n in ('release_begin', 'release_end', 'state_begin', 'state_end')}
    kinds = {}
    for name, pc in (('publish', publish), ('direct', enqueue)):
        trace = out / f'split-case-{name}.txt'
        records = [f'M {hooks["release_begin"]:x} 0', f'I {pc:x}']
        records += [f'W {pc:x} {base + 64 * k:x} 8' for k in range(lines)]
        trace.write_text('\n'.join(['W 0'] + records + [f'M {hooks["release_end"]:x} 0', 'E']) + '\n')
        (segs, _, _), = tool.analyze(elf, trace, marks, complete=False)
        kinds[name] = (dict(tool.segment_kinds(segs)), dict(tool.segment_kinds(segs, split=False)))
    key = 'data os ssKernelCPUs'
    split, joined = kinds['publish']
    assert split.get(('local publish', key)) == lines and ('local', key) not in split, split
    assert lines <= budget['local publish'][key], 'the publish path budget does not hold the forced window'
    assert joined.get(('local', key)) == lines > budget['local'][key], 'without the split the window must fail'
    direct, _ = kinds['direct']
    assert direct.get(('local', key)) == lines > budget['local'][key], 'the direct path must keep its budget'
    print(f"PASS publish split case: {lines} ssKernelCPUs lines fit 'local publish' "
          f"({budget['local publish'][key]}) and exceed 'local' ({budget['local'][key]}) without it")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    parser.add_argument('--record', action='store_true',
                        help='write the measured counts as the new budget instead of checking them')
    parser.add_argument('--merge', action='store_true',
                        help='with --record, keep the larger of the recorded and measured counts')
    parser.add_argument('--lower', action='append', default=[], metavar='STRUCTURE',
                        help='with --record, lower only this structure (for example "data os ssKernelControl") '
                             'to its measured count in every segment, and each data total by the same amount; '
                             'a structure with no budget in a segment is added at its measured count; '
                             'SEGMENT:STRUCTURE limits it to one segment')
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    elf = args.build / 'latency-partitioned-pmu-model.elf'
    if hashlib.sha256(PLUGIN_HEADER.read_bytes()).hexdigest() != PLUGIN_HEADER_SHA256:
        raise SystemExit(f'{PLUGIN_HEADER}: not the QEMU 10.2.1 plugin header')
    plugin = out / 'tick-footprint.so'
    subprocess.run(['clang-21', '-shared', '-fPIC', '-O2', '-fvisibility=hidden', '-Wall', '-Werror',
                    '-I', str(PLUGIN_HEADER.parent), '-I', str(ROOT / 'tools/qemu-plugin-glib'),
                    str(ROOT / 'tools/qemu-tick-footprint.c'), '-o', str(plugin)], check=True)
    found = symbols(elf)
    hooks = {name: one(found, 'ss_latency_cause_' + name)[0]
             for name in ('tick_begin', 'tick_end', 'release_begin', 'release_end', 'state_begin', 'state_end')}
    phase = one(found, 'phase')[0]
    worker, worker_size = one(found, 'load_worker')
    spec = importlib.util.spec_from_file_location('tick_footprint', ROOT / 'tools/tick-footprint.py')
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    if not args.record:
        publish_split_case(tool, elf, found, hooks, out)
    trace = out / 'busy-trace.txt'
    plugin_args = [f'begin={hooks["tick_begin"]:x}', f'end={hooks["tick_end"]:x}', f'phase={phase:x}',
                   f'value={BUSY}', f'windows={WINDOWS}', f'skip={worker:x}-{worker + worker_size:x}',
                   f'out={trace}'] + [f'mark={hooks[n]:x}' for n in ('release_begin', 'release_end',
                                                                    'state_begin', 'state_end')]
    command = [str(args.qemu), '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
               '-accel', 'tcg,thread=multi', '-cpu', 'cortex-a55', '-smp', '8', '-m', '128M',
               '-nographic', '-monitor', 'none', '-semihosting-config', 'enable=on,target=native',
               '-device', f'loader,file={elf},cpu-num=0', '-plugin', ','.join([str(plugin)] + plugin_args)]
    run = subprocess.run(command, capture_output=True, text=True, timeout=600)
    (out / 'qemu.log').write_text(run.stdout + run.stderr)
    # The footprint needs only the traced busy ticks; the model's later functional
    # assertions are covered by its own checks and may see the plugin's slowdown.
    kept = [int(line.split()[2]) for line in trace.read_text().splitlines() if line.startswith('# kept ')]
    if not kept or kept[-1] < MINIMUM:
        raise SystemExit(f'the trace holds fewer than {MINIMUM} busy ticks; see qemu.log')

    marks = {'release_begin': hooks['release_begin'], 'release_end': hooks['release_end'],
             'state_begin': hooks['state_begin'], 'state_end': hooks['state_end']}
    windows = tool.analyze(elf, trace, marks, complete=False)
    measured = {}
    for segs, _, _ in windows:
        for (kind, key), value in tool.segment_kinds(segs).items():
            measured.setdefault(kind, {})
            measured[kind][key] = max(measured[kind].get(key, 0), value)
    for kind in measured:
        data = sum(v for k, v in measured[kind].items() if k.startswith('data '))
        measured[kind]['data total'] = data
    result = {'windows': len(windows), 'measured': measured}
    if args.record and args.lower:
        # A cut lowers its own structure; every other budget keeps the maximum
        # recorded over earlier runs, which covers their path variation.
        budget = json.loads(BUDGET.read_text())
        for kind, rows in budget.items():
            for named in args.lower:
                segment, _, key = named.rpartition(':')
                if segment and segment != kind:
                    continue
                value = measured.get(kind, {}).get(key, 0)
                if key in rows and value < rows[key]:
                    saved = rows[key] - value
                    rows[key] -= saved
                    if key.startswith('data '):
                        rows['data total'] -= saved
                elif key not in rows and value:
                    # A line the cut moved to a structure of its own.
                    rows[key] = value
                    if key.startswith('data '):
                        rows['data total'] += value
            for key in [k for k, v in rows.items() if not v]:
                del rows[key]
        BUDGET.write_text(json.dumps(budget, indent=1, sort_keys=True) + '\n')
        result['recorded'] = str(BUDGET)
    elif args.record:
        # Recording raises the budget to the largest count seen in this and earlier
        # recordings; run it several times so that path variation is covered.
        previous = json.loads(BUDGET.read_text()) if BUDGET.exists() and args.merge else {}
        for kind, rows in previous.items():
            for key, value in rows.items():
                measured.setdefault(kind, {})[key] = max(measured.get(kind, {}).get(key, 0), value)
        BUDGET.write_text(json.dumps(measured, indent=1, sort_keys=True) + '\n')
        result['recorded'] = str(BUDGET)
    else:
        budget = json.loads(BUDGET.read_text())
        over, code_over = [], []
        for kind, rows in measured.items():
            for key, value in rows.items():
                limit = budget.get(kind, {}).get(key)
                if limit is None or value > limit:
                    # Code counts follow the path each wake takes, which varies
                    # with host load under multi-threaded TCG (a remote release
                    # reached 59 lines against 56 with four checks running at
                    # once). They are reported; data counts decide.
                    (code_over if key.startswith('code ') else over).append(
                        {'segment': kind, 'structure': key, 'lines': value, 'budget': limit})
        result['over_budget'] = over
        result['code_over_budget'] = code_over
        result['under_budget'] = [{'segment': kind, 'structure': key,
                                   'lines': measured.get(kind, {}).get(key, 0), 'budget': limit}
                                  for kind, limits in budget.items() for key, limit in limits.items()
                                  if measured.get(kind, {}).get(key, 0) < limit]
        result['passed'] = not over
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    if not args.record:
        for row in code_over:
            print(f"NOTE {row['segment']} {row['structure']}: {row['lines']} lines, budget {row['budget']}")
    if not args.record and over:
        for row in over:
            print(f"FAIL {row['segment']} {row['structure']}: {row['lines']} lines, budget {row['budget']}")
        sys.exit(1)
    totals = ', '.join(f"{kind} {rows['data total']}" for kind, rows in sorted(measured.items()))
    print(f"{'RECORDED' if args.record else 'PASS'} tick footprint: {len(windows)} busy ticks; data lines {totals}")


if __name__ == '__main__':
    main()
