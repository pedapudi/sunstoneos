#!/usr/bin/env python3
"""Run the continuous camera capture composition model and compile its units."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--cc', default='clang-21')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[1]
commands = []
sources = ['drivers/camera/ov5640.c', 'drivers/camera/a733_camera_platform.c',
           'drivers/camera/a733_csi_receiver.c', 'drivers/camera/a733_camera_route.c',
           'drivers/camera/a733_camera_dma.c', 'drivers/iommu/a733_iommu.c',
           'drivers/camera/a733_camera_capture.c']
changed = ['drivers/camera/ov5640.c', 'drivers/camera/a733_csi_receiver.c',
           'drivers/camera/a733_camera_route.c', 'drivers/camera/a733_camera_dma.c',
           'drivers/camera/a733_camera_capture.c']
inputs = sources + ['drivers/camera/ov5640_tables.h', 'checks/a733-camera-stream.c',
                    'checks/a733-camera-stream.py', 'runtime/include/string.h']
inputs += ['include/' + name + '.h' for name in ['ov5640', 'a733_camera_common', 'a733_camera_platform',
           'a733_csi_receiver', 'a733_camera_route', 'a733_camera_dma', 'a733_iommu',
           'a733_camera_capture']]


def run(command):
    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
    commands.append(dict(command=command, returncode=result.returncode,
                         stdout=result.stdout, stderr=result.stderr))
    (a.evidence / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    assert result.returncode == 0, commands[-1]
    return result.stdout


binary = a.evidence / 'stream-model'
run([a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
     '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
     '-Iinclude', 'checks/a733-camera-stream.c', *sources, '-o', str(binary)])
result = run([str(binary)])
# Without the status-interval check, the stalled owner's merged frame starts
# leave the owed count short, and a late flag then lets the model deliver a
# slot that the device is still writing.
dma = (root / 'drivers/camera/a733_camera_dma.c').read_text()
check = '    if(s->now-g->status_at>g->interval)g->owed=UNKNOWN;\n'
assert dma.count(check) == 1
mutant = a.evidence / 'mutant' / 'a733_camera_dma.c'
mutant.parent.mkdir(parents=True, exist_ok=True)
mutant.write_text(dma.replace(check, ''))
mutant_binary = a.evidence / 'stream-model-without-interval'
run([a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Iinclude',
     'checks/a733-camera-stream.c', *[str(mutant) if s.endswith('/a733_camera_dma.c') else s
                                      for s in sources], '-o', str(mutant_binary)])
outcome = subprocess.run([str(mutant_binary)], cwd=root, capture_output=True, text=True)
commands.append(dict(command=[str(mutant_binary)], returncode=outcome.returncode,
                     stdout=outcome.stdout, stderr=outcome.stderr))
(a.evidence / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
assert outcome.returncode != 0 and ('device_owned' in outcome.stderr or 'check_frame' in outcome.stderr), commands[-1]
objects = {}
for source in changed:
    obj = a.evidence / (Path(source).stem + '.o')
    run([a.cc, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
         '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-mstrict-align',
         '-mno-outline-atomics', '-fstack-usage', '-Iinclude', '-Iruntime/include', '-c',
         source, '-o', str(obj)])
    objects[source] = dict(sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),
                           size=run(['/usr/lib/llvm-21/bin/llvm-size', str(obj)]),
                           undefined=run(['/usr/lib/llvm-21/bin/llvm-nm', '-u', str(obj)]))
hashes = {}
for path in inputs:
    data = (root / path).read_bytes()
    hashes[path] = hashlib.sha256(data).hexdigest()
    target = a.evidence / 'snapshot' / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
(a.evidence / 'results.json').write_text(json.dumps(dict(
    result=result, without_interval_check=outcome.stderr, inputs=hashes, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
    objects=objects,
    qualification='Composition model only: modeled frame timing, address latching and memory. '
                  'Physical latching, frame-done timing and qualification remain outstanding.'
), indent=2) + '\n')
print(result, end='')
