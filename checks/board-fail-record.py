#!/usr/bin/env python3
"""The C side of the retained failure record: which failure notes reach the vector (apps/board/fail_record.h), with two mutants."""
import argparse
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
header = (ROOT / 'apps/board/fail_record.h').read_text()


def build(text, name):
    d = args.output / name
    (d / 'apps/board').mkdir(parents=True, exist_ok=True)
    (d / 'checks').mkdir(exist_ok=True)
    (d / 'apps/board/fail_record.h').write_text(text)
    (d / 'checks/board-fail-record.c').write_text((ROOT / 'checks/board-fail-record.c').read_text())
    binary = d / 'check'
    subprocess.run(['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined', str(d / 'checks/board-fail-record.c'), '-o', str(binary)],
                   check=True, capture_output=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


real = build(header, 'real')
assert real.returncode == 0, real.stderr
second = 'return !atomic_exchange_explicit(second,1,memory_order_acq_rel);'
assert header.count(second) == 1
assert build(header.replace(second, 'return 0;'), 'mutant-first-only').returncode != 0, 'the check did not notice a first-only record'
assert build(header.replace(second, 'return 1;'), 'mutant-every-later').returncode != 0, 'the check did not notice a record of every later failure'
print('PASS failure record rule: the first note and one device note after an application failure reach the vector; 2 mutants fail')
