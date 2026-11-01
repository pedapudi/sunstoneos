#!/usr/bin/env python3
"""Check crypto orchestration and CPU staging erasure without device access."""
import argparse
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
root = Path(__file__).resolve().parents[1]
command = [str(CLANG), '-std=c11', '-O1', '-g', *SANITIZE,
           '-Iinclude', '-Iapps/crypto',
           'checks/crypto-application-run.c', 'apps/crypto/run.c',
           'apps/crypto/request.c', 'apps/crypto/vectors.c',
           'drivers/storage/storage_digest.c', '-o', str(a.output / 'model')]
for name, cmd in [('build', command), ('model', [str(a.output / 'model')])]:
    result = subprocess.run(cmd, cwd=root, env=environment(), text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (a.output / (name + '.log')).write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout or result.returncode)
print('PASS crypto orchestration and complete CPU staging erasure; session unchanged')
