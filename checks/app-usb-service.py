#!/usr/bin/env python3
"""Compile and exercise the real USB application ownership broker on the host."""
from pathlib import Path
import argparse
import subprocess
from harness import CLANG, SANITIZE, environment
ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
OUT = parser.parse_args().output
OUT.mkdir(parents=True, exist_ok=False)
subprocess.run([str(CLANG), '-std=c11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
                '-I' + str(ROOT / 'include'), str(ROOT / 'apps/os/usb-service.c'),
                str(ROOT / 'checks/app-usb-service.c'), '-o', str(OUT / 'broker')], check=True, env=environment())
subprocess.run([str(OUT / 'broker')], check=True, env=environment())
print('PASS: USB grants, pending, borrowed payloads, disconnect and explicit release')
