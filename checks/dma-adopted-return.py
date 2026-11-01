#!/usr/bin/env python3
"""Check adopted DMA retirement without hardware writes or deadline extension."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['drivers/dma/a733_dma_platform.c', 'include/a733_dma_platform.h',
                'checks/dma-platform.c', 'checks/dma-adopted-return.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    exe = args.output / 'dma-adopted-return'
    cmd = [str(CLANG), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
           '-I' + str(ROOT / 'include'), str(ROOT / 'checks/dma-adopted-return.c'),
           str(ROOT / 'drivers/dma/a733_dma_platform.c'),
           str(ROOT / 'drivers/clock/a733_clock_leases.c'), '-o', str(exe)]
    for name, command in [('build', cmd), ('run', [str(exe)])]:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=environment())
        (args.output / (name + '.log')).write_text(result.stdout)
        assert result.returncode == 0, result.stdout
    (args.output / 'result.json').write_text(json.dumps(dict(
        command=cmd, sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
        evidence='host register model; no physical access', result='PASS'), indent=2) + '\n')
    print('PASS adopted DMA return model')


if __name__ == '__main__':
    main()
