#!/usr/bin/env python3
"""Build and run the CE clock bring-up against a register model that enforces the native write rules (checks/ce-clock.c)."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ce-clock-') as directory:
    binary = pathlib.Path(directory) / 'ce-clock'
    build = subprocess.run(['/usr/bin/clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(root / 'include'),
                            str(root / 'checks/ce-clock.c'), str(root / 'drivers/crypto/a733_ce_clock.c'),
                            str(root / 'drivers/clock/a733_clock_leases.c'), '-o', str(binary)],
                           capture_output=True, text=True)
    if build.returncode:
        sys.exit(build.stdout + build.stderr)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    sys.stdout.write(run.stdout + run.stderr)
    if run.returncode or 'PASS' not in run.stdout:
        sys.exit(1)
