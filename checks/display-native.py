#!/usr/bin/env python3
"""Archive and check native display ownership publication in one composed model."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--evidence', type=pathlib.Path, required=True)
p.add_argument('--cc', default='clang-21')
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
a.evidence.mkdir(parents=True, exist_ok=False)
compiled = ['drivers/display/a733_display_native.c', 'drivers/iommu/a733_iommu.c', 'runtime/device_memory_pool.c', 'drivers/display/a733_display_handoff.c', 'drivers/display/a733_display_pll.c',
            'drivers/clock/a733_media_clock.c', 'drivers/clock/a733_clock_leases.c',
            'drivers/gpu/a733_gpu_resources.c', 'drivers/display/display_scanout.c',
            'drivers/display/a733_dsi.c', 'drivers/display/a733_dsi_phy.c',
            'drivers/display/a733_dsi_video.c', 'drivers/display/a733_tcon_lcd.c',
            'drivers/display/a733_display_engine.c']
inputs = set(compiled + ['checks/display-native.c', 'checks/display-native.py', 'cmake/a733-dsi-native.cmake',
                         'runtime/include/string.h'])
pending = list(inputs)
while pending:
    name = pending.pop()
    for header in re.findall(r'^#include "([^"]+)"', (root/name).read_text(), re.M):
        candidates = [(root/name).parent/header, root/'include'/header, root/'runtime/include'/header]
        found = next((x for x in candidates if x.exists()), None)
        assert found, (name, header)
        relative = str(found.relative_to(root))
        if relative not in inputs:
            inputs.add(relative)
            pending.append(relative)

hashes = {}
for name in sorted(inputs):
    data = (root/name).read_bytes()
    dest = a.evidence/'inputs'/name
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    hashes[name] = hashlib.sha256(data).hexdigest()
(a.evidence/'inputs.json').write_text(json.dumps(hashes, indent=2)+'\n')
compiler = pathlib.Path(shutil.which(a.cc)).resolve()
commands = []


def run(command):
    result = subprocess.run([str(x) for x in command], cwd=root, capture_output=True, text=True)
    commands.append(dict(command=[str(x) for x in command], returncode=result.returncode,
                         stdout=result.stdout, stderr=result.stderr))
    (a.evidence/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    assert result.returncode == 0, commands[-1]
    return result.stdout


binary = a.evidence/'check'
run([compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
     '-Wno-unused-function', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Iinclude',
     'checks/display-native.c', *compiled, '-o', binary])
result = run([binary])
for name in ['drivers/display/a733_display_native.c']:
    obj = a.evidence/(pathlib.Path(name).stem+'.o')
    run([compiler, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
         '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-mstrict-align',
         '-mno-outline-atomics', '-fstack-usage', '-Iinclude', '-Iruntime/include',
         '-c', name, '-o', obj])
for name, expected in hashes.items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest() == expected, name
record = dict(result=result, inputs=hashes,
              compiler=dict(path=str(compiler), sha256=hashlib.sha256(compiler.read_bytes()).hexdigest()),
              artifacts={f.name: hashlib.sha256(f.read_bytes()).hexdigest()
                         for f in a.evidence.iterdir() if f.is_file()},
              purpose='Native retained allocator/IOMMU provider with actual media/PLL publication; new provider cases only',
              limits='Modeled native MMIO/translation/locks and selected-panel authority; actual allocator/IOMMU tables; no physical execution or qualified profile/panel')
(a.evidence/'results.json').write_text(json.dumps(record, indent=2)+'\n')
print(result, end='')
