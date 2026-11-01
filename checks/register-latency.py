#!/usr/bin/env python3
"""Check deferred register effects and reproduce premature CPU PLL verification.

The CPU driver belongs to another stream. Two copies in the output directory
isolate its known faulty verification order and the corresponding waited order.
Both execute the real transition fixture. The source driver is never modified.
This regression passes only when the old order fails while UPDATE is pending
and the waited order completes repeated raises and lowers at three latencies.
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
    prefix = '    case S_UPDATE:\n    case S_LOCK: {\n        uint32_t pll;\n'
    begin = source.index(prefix) + len(prefix)
    end = source.index('        if(c->state==S_UPDATE ?', begin)
    old_check = ('        if(!rd(c,pll_word(c->cluster),&pll) || '
                 '((pll>>8)&255)!=c->n)return fault(c);\n')
    waited_check = ('        if(!rd(c,pll_word(c->cluster),&pll))return fault(c);\n'
                    '        /* UPDATE clear makes the multiplier readback valid. */\n'
                    '        if(c->state==S_UPDATE && (pll&UPDATE))\n'
                    '            return c->last_now<c->timer ? A733_CPU_CLOCK_PENDING : fault(c);\n'
                    '        if(((pll>>8)&255)!=c->n)return fault(c);\n')
    observed = source[begin:end]
    assert observed in (old_check, waited_check), 'review changed CPU verification sequence'
    results = {}
    inputs = [path, ROOT / 'checks/register_latency.h', ROOT / 'checks/register_latency.py',
              ROOT / 'checks/cpu-clock-model.c', ROOT / 'checks/cpu-update-latency.c',
              ROOT / 'checks/a733-pmic.c', ROOT / 'checks/register-latency.py',
              *(ROOT / p for p in UNITS)]
    for name, check in [('premature', old_check), ('waited', waited_check)]:
        driver = out / (name + '.c')
        driver.write_text(source[:begin] + check + source[end:])
        exe = out / name
        command = [str(CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Iinclude', *SANITIZE, '-Wl,--wrap=a733_rail_floor',
                   'checks/cpu-update-latency.c', str(driver), *UNITS, '-o', str(exe)]
        if name == 'premature':
            command.insert(1, '-DEXPECT_PREMATURE_VERIFY=1')
        for phase, argv in [('build', command), ('run', [str(exe)])]:
            result = subprocess.run(argv, cwd=ROOT, env=environment(), text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (out / f'{name}-{phase}.log').write_text(result.stdout)
            assert result.returncode == 0, (name, phase, result.stdout)
        if name == 'premature':
            assert result.stdout.count('origin=multiplier-before-update') == 3
        results[name] = result.stdout
    (out / 'results.json').write_text(json.dumps({
        'status': 'PASS', 'seconds': round(time.monotonic() - start, 3),
        'source_has_known_bug': observed == old_check,
        'inputs': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in inputs}, 'cases': results,
        'limits': 'Host models only; temporary driver copies isolate verification order.'
    }, indent=2) + '\n')
    print('PASS: stale readback catches premature CPU verification; waited raises/lowers complete')


if __name__ == '__main__':
    main()
