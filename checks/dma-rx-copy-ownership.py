#!/usr/bin/env python3
"""Check cyclic DMA staging exclusion against every retained allocation and metadata range."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--controller', type=Path, default=ROOT / 'drivers/dma/a733_dma_controller.c',
                        help='controller source, also allows checking the unchanged baseline')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    compiler = 'clang-21'
    binary = args.output / 'dma-rx-copy-ownership'
    commands = [
        [str(compiler), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(ROOT / 'include'),
         str(ROOT / 'checks/dma-rx-copy-ownership.c'), str(args.controller),
         str(ROOT / 'drivers/dma/a733_dma.c'), '-o', str(binary)],
        [str(binary)],
    ]
    results = []
    for index, command in enumerate(commands):
        result = subprocess.run(command, capture_output=True, text=True,
                                env={**harness.environment(), 'ASAN_OPTIONS': 'detect_leaks=0'})
        (args.output / f'{index}.log').write_text(result.stdout + result.stderr)
        results.append({'command': command, 'exit': result.returncode})
        harness.save(args.output / 'result.json', {'physical_access': False, 'steps': results})
        print(result.stdout + result.stderr, end='')
        result.check_returncode()


if __name__ == '__main__':
    main()
