#!/usr/bin/env python3
"""Check the performance-mode table against pinned sources and run its host models.

The table (drivers/power/a733_perf_mode.c) is compared with the pinned vendor
device trees: every cited row and voltage line, every point, and the rule
that chooses each point (high: the column's top row; low: the fastest row at
the column's lowest voltage). The engine model (checks/perf-mode-model.c)
runs with the CPU clock driver's tables, the OS owner model
(checks/perf-os-model.c) runs apps/os/perf.c with and without the NPU, and the
NPU startup model runs the retune of a stopped NPU."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
from harness import CACHE, CLANG, SANITIZE, environment  # noqa: E402

SOURCES = CACHE / 'sources'
ENGINE = ['drivers/power/a733_perf_mode.c', 'drivers/clock/a733_cpu_clock.c', 'drivers/power/a733_pmic.c',
          'drivers/power/a733_rail_leases.c', 'drivers/clock/a733_clock_leases.c',
          'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
          'drivers/thermal/a733_thermal.c']
STARTUP = ['checks/npu-startup-host.c', 'drivers/npu/a733_npu_startup.c', 'drivers/npu/a733_npu_platform.c',
           'drivers/power/a733_pmic.c', 'drivers/power/a733_rail_leases.c', 'drivers/clock/a733_clock_leases.c',
           'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
           'drivers/power/a733_power_domains.c', 'drivers/power/a733_pmic_consumer.c', 'drivers/gpu/a733_gpu_power.c']
BLOCKS = ('cpu-l', 'cpu-b', 'dsu', 'gpu', 'npu')


def node(text, label):
    """Body of the device-tree node that starts with label, and its first line."""
    start = text.index(label)
    depth, i = 0, text.index('{', start)
    for j in range(i, len(text)):
        depth += (text[j] == '{') - (text[j] == '}')
        if not depth:
            return text[i:j], text.count('\n', 0, start) + 1
    raise AssertionError(label)


def rows(text, label, column):
    """{MHz: (mV, row line, voltage line)} of the table's uncommented rows with
    a nonzero voltage in the column."""
    body, first = node(text, label)
    out = {}
    for m in re.finditer(r'^\s*(?:[\w-]+:\s*)?(opp[@-][\w-]+)\s*\{(.*?)\};', body, re.S | re.M):
        line = first + body.count('\n', 0, m.start(1))
        head = body[body.rfind('\n', 0, m.start(1)) + 1:m.start(1)]
        if '/*' in head:
            continue
        hz = int(re.search(r'opp-hz = /bits/ 64 <(\d+)>', m[2])[1])
        v = re.search(r'(opp-microvolt-' + re.escape(column) + r') = <(\d+)>', m[2])
        if v and int(v[2]):
            out[hz // 1000000] = (int(v[2]) // 1000, line, line + m[2].count('\n', 0, v.start(1)))
    return out


def run(out, name, argv, **kw):
    r = subprocess.run([str(a) for a in argv], cwd=ROOT, env=environment(), text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)
    (out / (name + '.log')).write_text(' '.join(map(str, argv)) + '\n' + r.stdout)
    assert r.returncode == 0, '%s failed:\n%s' % (name, r.stdout[-3000:])
    return r.stdout


def table_check(inputs):
    """The C table, its citations and its choice rule against the pinned trees."""
    lock = json.loads((ROOT / 'sources.lock.json').read_text())
    pinned = {}
    for key in ('npu-platform-sun60iw2p1-cpu-vf-dtsi', 'board-gpu-power-sun60iw2p1-dtsi'):
        path = SOURCES / lock[key]['archive']
        data = path.read_bytes()
        assert hashlib.sha256(data).hexdigest() == lock[key]['sha256'], key
        inputs[str(path)] = lock[key]['sha256']
        pinned[key] = data.decode()
    vf, soc = pinned['npu-platform-sun60iw2p1-cpu-vf-dtsi'], pinned['board-gpu-power-sun60iw2p1-dtsi']
    assert 'vf-version = "V0.9"' in vf
    source = (ROOT / 'drivers/power/a733_perf_mode.c').read_text()
    body = re.search(r'static const struct a733_perf_entry a733_perf_modes\[\]=\{(.*?)\n\};', source, re.S)[1]
    assert body.count('{4,0x0400,') == 1 and '0x' not in body.replace('0x0400', ''), 'one entry, vf0400'
    pairs = [tuple(map(int, p)) for p in re.findall(r'\{(\d+),(\d+)\}', body)]
    assert len(pairs) == 10, pairs
    points = [pairs[:5], pairs[5:]]
    tables = {'cpu-l': (vf, 'cluster0_opp_table:', '26m-vf0400', 'vf'),
              'cpu-b': (vf, 'cluster1_opp_table:', '26m-vf0400', 'vf'),
              'dsu': (vf, 'dsu_opp_table:', '26m-vf0400', 'vf'),
              'gpu': (soc, 'gpu_opp_table:', 'vfdefault', 'soc'),
              'npu': (vf, 'npu_opp_table:', '26m-vf0400', 'vf')}
    result = {}
    for i, block in enumerate(BLOCKS):
        text, label, column, short = tables[block]
        table = rows(text, label, column)
        high, low = points[0][i], points[1][i]
        for mode, (mhz, mv) in (('high', high), ('low', low)):
            assert mhz in table and table[mhz][0] == mv, (block, mode, mhz, mv, table.get(mhz))
            # The comment cites the row line and the voltage line of each point.
            cite = r'%s:%d, %s:%d' % (short, table[mhz][1], short, table[mhz][2])
            assert re.search(re.escape(cite), source), (block, mode, cite)
        lowest = min(v[0] for v in table.values())
        if block != 'dsu':
            # High: the column's top row; low: the fastest row below it at the
            # column's lowest voltage (for the NPU, both rows run at 800 mV).
            assert high[0] == max(table), (block, high, max(table))
            assert low == (max(m for m, v in table.items() if v[0] == lowest and m < high[0]), lowest), (block, low)
        result[block] = {'high': high, 'low': low, 'rows': sorted(table)}
    # The DSU entries follow the rule, which the engine model recomputes.
    assert points[0][2] == (1274, 1000) and points[1][2] == (910, 800)
    # The GPU's vendor column is vfdefault (no speed-bin map); its vf0400
    # column lists 900 mV for 1008 MHz, which the vendor kernel never applies.
    assert rows(soc, 'gpu_opp_table:', 'vf0400')[1008][0] == 900
    return result


def build(out, name, sources, extra=()):
    exe = out / name
    run(out, 'compile-' + name, [CLANG, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
                                 '-Iinclude', '-Ichecks', *extra, *sources, '-o', exe])
    return exe


def os_model(out, npu):
    """apps/os/perf.c in one unit with checks/perf-os-model.c, over the shims
    of checks/perf-os-host, laid out as the OS tree so its includes resolve."""
    host = out / ('os-host-npu' if npu else 'os-host')
    (host / 'os').mkdir(parents=True)
    (host / 'board').mkdir()
    for path in ('apps/os/perf.c', 'apps/os/perf.h', 'checks/perf-os-host/os.h', 'checks/perf-os-host/npu.h',
                 'checks/perf-os-host/a733_mmio.h', 'checks/perf-os-model.c'):
        (host / 'os' / Path(path).name).write_bytes((ROOT / path).read_bytes())
    (host / 'board/application.h').write_bytes((ROOT / 'checks/perf-os-host/board/application.h').read_bytes())
    name = 'perf-os-model' + ('-npu' if npu else '')
    exe = build(out, name, [host / 'os/perf-os-model.c', *ENGINE], ['-DPERF_MODEL_NPU=1'] if npu else [])
    lines = run(out, name, [exe]).splitlines()
    assert lines[-1].startswith('PASS'), lines
    return json.loads(lines[-2])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path, help='new directory for logs and results')
    out = ap.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = {}
    result = {'table': table_check(inputs)}
    model = build(out, 'perf-mode-model', ['checks/perf-mode-model.c', *ENGINE])
    lines = run(out, 'perf-mode-model', [model]).splitlines()
    assert lines[-1].startswith('PASS'), lines
    result['engine'] = json.loads(lines[-2])
    startup = build(out, 'npu-startup-host', STARTUP, ['-Ichecks/npu-startup-host'])
    lines = run(out, 'npu-startup-retune', [startup, '--retune']).splitlines()
    assert lines[-1].startswith('PASS'), lines
    result['npu_retune'] = json.loads(lines[-2])
    for mode in ('', '--rate-852', '--second-start'):
        assert run(out, 'npu-startup' + mode, [startup] + ([mode] if mode else [])).splitlines()[-1].startswith('PASS')
    result['os_owner'] = {'cpu': os_model(out, False), 'cpu_npu': os_model(out, True)}
    for path in sorted({*ENGINE, *STARTUP, 'checks/perf-mode-model.c', 'include/a733_perf_mode.h',
                        'apps/os/perf.h', 'apps/os/perf.c', 'checks/perf-os-model.c', 'checks/perf-os-host/os.h',
                        'checks/perf-os-host/npu.h', 'checks/perf-os-host/a733_mmio.h',
                        'checks/perf-os-host/board/application.h', 'checks/perf-mode.py'}):
        inputs[path] = hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
    result['inputs'] = inputs
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['engine']))
    print('PASS performance modes: vf0400 table and citations against the pinned trees, the engine model '
          '(%d cases), the OS owner model with and without the NPU (%d CPU and %d NPU cases) and the stopped-NPU retune'
          % (result['engine']['cases'], result['os_owner']['cpu']['cases'], result['os_owner']['cpu_npu']['cases']))


if __name__ == '__main__':
    main()
