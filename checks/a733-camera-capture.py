#!/usr/bin/env python3
"""Run only the native camera composition model and compile its coordinator."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--cc', default='clang-21')
p.add_argument('--case', choices=['all', 'publication'], default='all')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[1]
commands = []
sources = ['drivers/camera/ov5640.c', 'drivers/camera/a733_camera_platform.c',
           'drivers/camera/a733_csi_receiver.c', 'drivers/camera/a733_camera_route.c',
           'drivers/camera/a733_camera_dma.c', 'drivers/iommu/a733_iommu.c',
           'drivers/camera/a733_camera_capture.c']
inputs = sources + ['drivers/camera/ov5640_tables.h', 'checks/a733-camera-capture.c',
                    'checks/a733-camera-capture.py', 'runtime/include/string.h']
inputs += ['include/' + name + '.h' for name in ['ov5640', 'a733_camera_platform',
           'a733_csi_receiver', 'a733_camera_route', 'a733_camera_dma', 'a733_iommu',
           'a733_camera_capture']]


def run(command):
    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
    commands.append(dict(command=command, returncode=result.returncode,
                         stdout=result.stdout, stderr=result.stderr))
    (a.evidence / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    assert result.returncode == 0, commands[-1]
    return result.stdout


binary = a.evidence / 'capture-model'
run([a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Iinclude',
     'checks/a733-camera-capture.c', *sources, '-o', str(binary)])
result = run([str(binary)] + ([] if a.case == 'all' else [a.case]))
obj = a.evidence / 'a733_camera_capture.o'
run([a.cc, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra',
     '-Werror', '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only',
     '-mstrict-align', '-mno-outline-atomics', '-fstack-usage', '-Iinclude',
     '-Iruntime/include', '-c', 'drivers/camera/a733_camera_capture.c', '-o', str(obj)])
size = run(['/usr/lib/llvm-21/bin/llvm-size', str(obj)])
undefined = run(['/usr/lib/llvm-21/bin/llvm-nm', '-u', str(obj)])
hashes = {}
for path in inputs:
    data = (root / path).read_bytes()
    hashes[path] = hashlib.sha256(data).hexdigest()
    target = a.evidence / 'snapshot' / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
(a.evidence / 'results.json').write_text(json.dumps(dict(
    result=result, inputs=hashes, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
    object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(), size=size, undefined=undefined,
    qualification='Composition model only; physical providers and hardware qualification remain outstanding.'
), indent=2) + '\n')
print(result, end='')
