#!/usr/bin/env python3
"""Build and run the shared-service and model-session FreeRTOS checks in QEMU.

The configured central build supplies the npu-service-model target's compiler
flags, FreeRTOS port objects and link line; this driver compiles the changed
check and driver sources with exactly those flags and replaces only the
check object. QEMU supplies two Cortex-A55 cores and no NPU: each check models
device registers itself. A pass requires exit status 0 and the check's PASS
line. Evidence goes to a new output directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402,F401  (assertion policy)

ROOT = Path(__file__).resolve().parents[1]
QEMU = harness.CACHE / 'host/usr/bin/qemu-system-aarch64'
# Each mode lists the sources that replace checks/npu-service.c and the PASS prefix.
MODES = {
    'service-shared': (['checks/npu-service-shared.c'], 'PASS NPU shared service:'),
    'session': (['checks/npu-session.c', 'drivers/npu/npu_session.c', 'drivers/npu/nbg_model.c',
                 'drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_checksum.c',
                 'drivers/npu/nbg_relocation.c', 'drivers/storage/storage_digest.c'],
                'PASS NPU sessions:'),
}


def flags(build):
    text = (build / 'CMakeFiles/npu-service-model.dir/flags.make').read_text()
    values = {}
    for line in text.splitlines():
        if line.startswith('C_') and ' = ' in line:
            key, value = line.split(' = ', 1)
            values[key] = shlex.split(value)
    return values['C_DEFINES'] + values['C_INCLUDES'] + values['C_FLAGS']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('build', type=Path, help='configured central build with npu-service-model built')
    ap.add_argument('output', type=Path, help='new evidence directory')
    ap.add_argument('--mode', choices=sorted(MODES), required=True)
    ap.add_argument('--include', type=Path, action='append', default=[],
                    help='extra include directory, such as generated fixture headers')
    args = ap.parse_args()
    build = args.build.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources, marker = MODES[args.mode]
    link = shlex.split((build / 'CMakeFiles/npu-service-model.dir/link.txt').read_text())
    check = 'CMakeFiles/npu-service-model.dir/checks/npu-service.c.obj'
    assert link.count(check) == 1, 'unexpected npu-service-model link line'
    compiler = link[0]
    target = [v for v in link if v.startswith('--target=')]
    assert len(target) == 1, 'link line names no single target'
    common = target + flags(build) + [f'-I{p.resolve()}' for p in args.include]
    objects, commands = [], []
    for source in sources:
        obj = out / (source.replace('/', '_') + '.obj')
        command = [compiler] + common + ['-c', str(ROOT / source), '-o', str(obj)]
        commands.append(command)
        subprocess.run(command, check=True, cwd=build)
        objects.append(str(obj))
    elf = out / f'npu-{args.mode}.elf'
    index = link.index(check)
    command = link[:index] + objects + link[index + 1:]
    command[command.index('-o') + 1] = str(elf)
    command = ['-Wl,-Map,' + str(out / f'npu-{args.mode}.map') if v.startswith('-Wl,-Map,') else v
               for v in command]
    commands.append(command)
    subprocess.run(command, check=True, cwd=build)
    qemu = [str(QEMU), '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
            '-cpu', 'cortex-a55', '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
            '-semihosting-config', 'enable=on,target=native', '-kernel', str(elf)]
    result = subprocess.run(qemu, capture_output=True, text=True, timeout=120,
                            env=harness.environment())
    (out / 'qemu.log').write_text(result.stdout + result.stderr)
    passed = result.returncode == 0 and any(l.startswith(marker) for l in result.stdout.splitlines())
    record = dict(mode=args.mode, result='pass' if passed else 'fail', exit=result.returncode,
                  elf_sha256=sha(elf), commands=commands, qemu=qemu,
                  sources={s: sha(ROOT / s) for s in sources}, modeled_device=True,
                  physical_access=False)
    harness.save(out / 'result.json', record)
    print(result.stdout.strip().splitlines()[-1] if result.stdout.strip() else result.stderr.strip())
    assert passed, f'{args.mode} failed; see {out / "qemu.log"}'


if __name__ == '__main__':
    main()
