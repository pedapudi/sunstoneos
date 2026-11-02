#!/usr/bin/env python3
"""Run the real watchdog driver against the strict register model; refusal and restoration, no reset qualification.

drivers/watchdog/a733_watchdog.c compiles unchanged and links with checks/watchdog.c. Only the register layer is
replaced (checks/host_link.py); the model in checks/watchdog.c cites a source for every register it defines and
refuses any access it does not define. Host only.
"""
import argparse
from pathlib import Path
import subprocess

import host_link
from harness import environment

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
executable = host_link.build(args.output, 'watchdog', 'checks/watchdog.c', ['drivers/watchdog/a733_watchdog.c'])
result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment())
(args.output / 'run.log').write_text(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit('watchdog check failed:\n' + result.stdout + result.stderr)
print(result.stdout.strip())
