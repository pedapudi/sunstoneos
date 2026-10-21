#!/usr/bin/env python3
"""Build and run the MMU cache invalidation sanitizer check and its target object.

The host check links the actual invalidation, control, queue, startup, firmware
and state sources. It writes the three generated commands to commands.bin; with
--sources, the pinned driver's complete function is compared against them.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['checks/pvr-gpu-flush.c', 'drivers/gpu/pvr_gpu_flush.c', 'drivers/gpu/pvr_gpu_control.c',
           'drivers/gpu/pvr_gpu_start.c', 'drivers/gpu/pvr_firmware.c', 'drivers/gpu/pvr_ccb.c',
           'drivers/gpu/pvr_fw_state.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sources', type=Path, help='Pinned board driver imagination directory')
    parser.add_argument('--clang', default='clang-21')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    check = out / 'check'
    commands = [
        [args.clang, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-Iinclude', *SOURCES, '-o', str(check)],
        [str(check), str(out)],
        [str(check), str(out), 'clock-check'],
        [args.clang, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
         '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-mstrict-align', '-fstack-usage',
         '-Iinclude', '-Iruntime/include', '-c', 'drivers/gpu/pvr_gpu_flush.c', '-o', str(out / 'pvr_gpu_flush.o')]]
    if args.sources:
        commands.append(['python3', '-I', str(ROOT / 'checks/pvr-gpu-flush-reference.py'),
                         str(args.sources.resolve()), str(out)])
    records = []
    for index, command in enumerate(commands):
        run = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f'command-{index}.log').write_text(run.stdout)
        records.append({'command': command, 'returncode': run.returncode})
        (out / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
        print(run.stdout, end='')
        if run.returncode:
            raise RuntimeError(f'command {index} failed')
    inputs = {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in SOURCES}
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')


if __name__ == '__main__':
    main()
