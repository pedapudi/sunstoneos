#!/usr/bin/env python3
"""Exercise partial radio stop against actual platform, PMIC and SDIO workers."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['checks/a733-radio-stop.c', 'checks/a733-radio-platform.c',
                'checks/a733-pmic.c', 'drivers/net/a733_radio_platform.c',
                'include/a733_radio_platform.h', 'drivers/net/a733_radio_session.c',
                'include/a733_radio_session.h']
GATE_RUNS = [['--evidence', '{output}']]
SOURCES = ['drivers/net/a733_radio_platform.c', 'drivers/net/a733_radio_result.c',
           'drivers/storage/sunxi_sdio.c', 'drivers/storage/sdio_card.c',
           'drivers/clock/a733_clock_leases.c', 'drivers/power/a733_rail_leases.c',
           'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c',
           'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
           'checks/a733-radio-stop.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    out = args.evidence.resolve()
    out.mkdir(parents=True, exist_ok=False)
    compiler = shutil.which('clang-21') or shutil.which('clang')
    assert compiler
    binary = out / 'radio-stop'
    command = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-Iinclude', '-Ichecks', *SOURCES, '-o', str(binary)]
    results = []
    for cmd in (command, [str(binary)]):
        run = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
        results.append({'command': cmd, 'returncode': run.returncode,
                        'stdout': run.stdout, 'stderr': run.stderr})
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(run.stdout, end='')
        print(run.stderr, end='')
        if run.returncode:
            return 1
    inputs = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
              for name in sorted(set(SOURCES + CHECK_INPUTS))}
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
