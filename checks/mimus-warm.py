#!/usr/bin/env python3
"""Build and run the host warm-engine check, then compare both warm results with the host reference."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
MIMUS = Path('/home/sunil/git/mimus-worktrees/sunstoneos')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--bundle', type=Path, default=CACHE / 'inputs/mimus-bench/inputs.bin')
parser.add_argument('--reference', type=Path, default=CACHE / 'evidence/mimus-host.json')
parser.add_argument('--library', type=Path, default=CACHE / 'build/mimus-host/asr/libmimus_asr.a')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
commands = []


def run(name, command):
    command = [str(part) for part in command]
    commands.append(command)
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
    (out / f'{name}.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        sys.exit(f'{name} failed; see {out / name}.log')
    return result


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


flags = ['-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-Ichecks/bluetooth-mimus-fixture', '-Iinclude',
         f'-I{MIMUS}/firmware/components/asr/include', f'-I{MIMUS}/firmware/components/pipeline/include',
         '-DSUNSTONE_MIMUS_NATIVE=1', '-std=c++23', '-ffp-contract=off', '-fno-exceptions', '-fno-rtti']
objects = []
for index, source in enumerate(['apps/mimus/recorded.cc', 'checks/mimus-warm.cc']):
    objects.append(out / f'{index}.o')
    run(f'compile-{index}', ['/usr/bin/clang++-21', *flags, '-c', source, '-o', objects[-1]])
run('link', ['/usr/bin/clang++-21', '-fsanitize=address,undefined', *objects, args.library,
             MIMUS / 'firmware/components/pipeline/src/formatter.cc', *flags[-4:],
             f'-I{MIMUS}/firmware/components/pipeline/include', '-o', out / 'mimus-warm'])
print(run('warm', [out / 'mimus-warm', args.bundle, out / 'warm']).stdout, end='')
for name in ('first', 'second'):
    candidate = out / f'{name}.json'
    candidate.write_text(json.dumps({'exit': 0, 'bundle_sha256': digest(args.bundle),
                                     'image_sha256': 'host-warm-check',
                                     'stderr': (out / f'warm-{name}.log').read_text()}) + '\n')
    print(run(f'parity-{name}', [sys.executable, 'checks/mimus-parity.py', '--reference', args.reference,
                                 '--candidate', candidate, '--output', out / f'parity-{name}.json']).stdout, end='')
