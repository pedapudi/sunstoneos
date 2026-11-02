#!/usr/bin/env python3
"""Check that a read-only SD image cannot reach a write or erase command.

The core builds with SUNXI_MMC_READ_ONLY. The check proves four facts: the
write functions are absent from the core's object code; queue() is the only
function that assigns the controller command word and refuses every index
outside the read set, including CMD24, CMD25 and CMD38 (run directly on the
host); the command-register store of the core sits only in command_step,
which takes h->command; and, with --elf, the linked image defines none of the
write-path symbols and holds no call to them.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_SYMBOLS = ('sunxi_mmc_write', 'next_write', 'write_admitted', 'a733_storage_update_authorize',
                     'a733_storage_update_write')
FORBIDDEN_COMMANDS = (24, 25, 38)


def run(command, log, env=None):
    result = subprocess.run(command, capture_output=True, text=True, env=env)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stdout + result.stderr, end='')
        raise SystemExit(f'failed: {command[0]}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--elf', type=Path, help='linked read-only image to scan')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'drivers/storage/sunxi_mmc.c').read_text()

    # The command word has one writer, and the read set excludes the write commands.
    assigns = re.findall(r'\bh->command\s*=[^=]', source)
    assert len(assigns) == 1, assigns
    mask = re.search(r'#define READ_ONLY_COMMANDS (.*?)\n#endif', source, re.S)[1]
    bits = {int(n) for n in re.findall(r'UINT64_C\(1\)<<(\d+)', mask)}
    assert not bits & set(FORBIDDEN_COMMANDS), bits
    assert bits == {0, 1, 2, 3, 6, 7, 8, 9, 11, 13, 16, 17, 18, 19, 41, 51, 55}, sorted(bits)

    # Host funnel test of queue().
    binary = args.output / 'sd-read-only'
    env = {**harness.environment(), 'ASAN_OPTIONS': 'detect_leaks=0'}
    run(['clang-21', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
         '-fsanitize=address,undefined', '-DSUNXI_MMC_READ_ONLY', '-I' + str(ROOT / 'include'),
         str(ROOT / 'checks/sd-read-only.c'), '-o', str(binary)], args.output / 'build.log', env)
    print(run([str(binary)], args.output / 'funnel.log', env), end='')

    # Object code of the read-only core: no write symbol, with the write build as a control.
    flags = ['clang-21', '--target=aarch64-none-elf', '-O2', '-march=armv8-a', '-mstrict-align',
             '-mgeneral-regs-only', '-ffreestanding', '-fno-builtin', '-I' + str(ROOT / 'include'), '-c',
             str(ROOT / 'drivers/storage/sunxi_mmc.c')]
    for name, extra in (('read-only', ['-DSUNXI_MMC_READ_ONLY']), ('control', [])):
        run(flags + extra + ['-o', str(args.output / f'{name}.o')], args.output / f'{name}-cc.log')
    nm = {name: run(['llvm-nm-21', str(args.output / f'{name}.o')], args.output / f'{name}-nm.log')
          for name in ('read-only', 'control')}
    assert 'sunxi_mmc_read_only_build' in nm['read-only'] and 'sunxi_mmc_read_only_build' not in nm['control']
    assert 'sunxi_mmc_write' in nm['control'] and 'next_write' in nm['control'], 'control lost its write path'
    for symbol in FORBIDDEN_SYMBOLS:
        assert not re.search(rf'\b{symbol}\b', nm['read-only']), symbol

    if args.elf:
        table = run(['llvm-nm-21', str(args.elf)], args.output / 'image-nm.log')
        for symbol in FORBIDDEN_SYMBOLS:
            assert not re.search(rf'\b{symbol}\b', table), f'{symbol} is linked in {args.elf}'
        listing = run(['llvm-objdump-21', '-d', '--no-show-raw-insn', str(args.elf)], args.output / 'image-objdump.log')
        for symbol in FORBIDDEN_SYMBOLS:
            assert not re.search(rf'\b(bl|b)\s+.*<{symbol}>', listing), f'call to {symbol}'
        for needed in ('sunxi_mmc_read',):
            assert re.search(rf'\b{needed}\b', table), f'{needed} is missing: not an SD image'
    print('sd-read-only check passed')


if __name__ == '__main__':
    main()
