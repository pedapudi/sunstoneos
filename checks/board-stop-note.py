#!/usr/bin/env python3
"""Compile and run checks/board-stop-note.c: the storage-maintenance hook writes its failure note only for a non-zero exit status."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--output', type=Path, required=True)
ap.add_argument('--cc', default='/usr/bin/clang-21')
a = ap.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
binary = a.output / 'stop-note'
built = subprocess.run([a.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(ROOT / 'checks/board-stop-note.c'), '-o', str(binary)], capture_output=True, text=True)
assert built.returncode == 0, built.stderr
ran = subprocess.run([str(binary)], capture_output=True, text=True)
assert ran.returncode == 0, ran.stdout + ran.stderr
# Mutation: a note for every status must fail the check.
source = (ROOT / 'apps/board/stop_note.h').read_text()
assert source.count('    if(!status)return;\n') == 1
mutant = a.output / 'mutant'
mutant.mkdir(exist_ok=True)
(mutant / 'stop_note.h').write_text(source.replace('    if(!status)return;\n', ''))
(mutant / 'check.c').write_text((ROOT / 'checks/board-stop-note.c').read_text().replace('"../apps/board/stop_note.h"', '"stop_note.h"'))
built = subprocess.run([a.cc, '-std=gnu11', '-O1', '-Wall', str(mutant / 'check.c'), '-o', str(mutant / 'run')], capture_output=True, text=True)
assert built.returncode == 0, built.stderr
assert subprocess.run([str(mutant / 'run')], capture_output=True).returncode != 0, 'mutation survived: note for a zero status'
print(ran.stdout.strip() + '; mutation killed: note for every status')
