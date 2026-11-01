#!/usr/bin/env python3
"""Exercise fixed CPU transitions and regress stale readback and N-stage timing.

Temporary driver mutations retain the full merged sampling/deadline logic.
The source driver is unchanged. Each mutant must fail at its originating
observation, while the fixed driver repeatedly raises and lowers the clock.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from harness import CLANG, SANITIZE, environment
from register_latency import RegisterLatency

ROOT = Path(__file__).resolve().parents[1]
UNITS = ['drivers/power/a733_pmic.c', 'drivers/power/a733_rail_leases.c',
         'drivers/clock/a733_clock_leases.c', 'drivers/gpu/a733_gpu_resources.c',
         'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
         'drivers/thermal/a733_thermal.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    start = time.monotonic()
    # Busy and N are both stale until completion, even after repeated reads.
    old, new, busy = 38 << 8, 46 << 8, 1 << 26
    event = RegisterLatency(100, 10, 0xff00 | busy, new)
    visible = old | busy
    assert all(event.apply(109, visible) == visible for _ in range(20))
    assert event.apply(110, visible) == new and not event.pending
    stuck = RegisterLatency(100, 10, busy, 0, stuck=True)
    assert stuck.apply(100000, visible) == visible and stuck.pending

    wrap = RegisterLatency((1 << 64) - 4, 10, busy, 0)
    assert wrap.apply(5, visible) == visible
    assert wrap.apply(6, visible) == old

    path = ROOT / 'drivers/clock/a733_cpu_clock.c'
    source = path.read_text()
    mismatch = ('        if(((pll>>8)&255)!=c->n){\n'
                '            a733_cpu_clock_pll_mismatch_note(c,__LINE__,SUNSTONE_CRUMB_HERE(),pll);\n'
                '            return fault(c);\n'
                '        }\n')
    assert source.count(mismatch) == 1, 'review changed CPU mismatch sequence'
    wait = source.index('        /* N is manually updated:')
    assert wait < source.index(mismatch)
    premature = source[:wait] + mismatch + source[wait:].replace(mismatch, '', 1)
    stage = '!put(c,pll_word(c->cluster),pll) || !settle(c,SETTLE_US) ||'
    assert source.count(stage) == 1, 'review changed CPU N-stage sequence'
    no_stage_delay = source.replace(stage, '!put(c,pll_word(c->cluster),pll) ||', 1)
    results = {}
    inputs = [path, ROOT / 'checks/register_latency.h', ROOT / 'checks/register_latency.py',
              ROOT / 'checks/cpu-clock-model.c', ROOT / 'checks/cpu-update-latency.c',
              ROOT / 'checks/a733-pmic.c', ROOT / 'checks/register-latency.py',
              *(ROOT / p for p in UNITS)]
    for name, variant in [('fixed', source), ('premature', premature), ('no-stage-delay', no_stage_delay)]:
        driver = out / (name + '.c')
        driver.write_text(variant)
        exe = out / name
        command = [str(CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Iinclude', *SANITIZE, '-Wl,--wrap=a733_rail_floor',
                   'checks/cpu-update-latency.c', str(driver), *UNITS, '-o', str(exe)]
        if name != 'fixed':
            command.insert(1, '-D' + ('EXPECT_PREMATURE_VERIFY' if name == 'premature' else 'EXPECT_STAGE_FAILURE') + '=1')
        for phase, argv in [('build', command), ('run', [str(exe)])]:
            result = subprocess.run(argv, cwd=ROOT, env=environment(), text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (out / f'{name}-{phase}.log').write_text(result.stdout)
            assert result.returncode == 0, (name, phase, result.stdout)
        if name == 'premature':
            assert result.stdout.count('origin=multiplier-before-update') == 3
        if name == 'no-stage-delay':
            assert result.stdout.count('origin=update-captured-unstaged-n') == 3
        results[name] = result.stdout
    (out / 'results.json').write_text(json.dumps({
        'status': 'PASS', 'seconds': round(time.monotonic() - start, 3),
        'source_driver': 'fixed verification and vendor N-stage interval',
        'inputs': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in inputs}, 'cases': results,
        'limits': 'Host models only; temporary mutations isolate verification order and the vendor N-stage interval. Native posted-write completion is checked by os-cpu-clock-native.py.'
    }, indent=2) + '\n')
    print('PASS: fixed repeated CPU transitions; stale-readback and N-stage-delay mutations caught')


if __name__ == '__main__':
    main()
