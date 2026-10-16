#!/usr/bin/env python3
"""Build one composed scanout model and preserve input/artifact identities."""
import argparse
import hashlib
import json
import pathlib
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--evidence', type=pathlib.Path, required=True)
p.add_argument('--cc', default='clang-21')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(__file__).resolve().parents[1]
compiled = ['drivers/display/display_scanout.c', 'drivers/display/a733_dsi.c',
            'drivers/display/a733_dsi_phy.c', 'drivers/display/a733_dsi_video.c',
            'drivers/display/a733_tcon_lcd.c', 'drivers/display/a733_display_engine.c']
inputs = compiled + ['include/display_scanout.h', 'include/a733_dsi.h',
                    'include/a733_dsi_phy.h', 'include/a733_dsi_video.h',
                    'include/a733_tcon_lcd.h', 'include/a733_display_engine.h',
                    'runtime/include/string.h', 'checks/display-scanout.c', 'checks/display-scanout.py']
commands = []


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    commands.append(dict(command=cmd, returncode=r.returncode, stdout=r.stdout, stderr=r.stderr))
    (a.evidence/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    assert r.returncode == 0, commands[-1]
    return r.stdout


binary = a.evidence/'check'
run([a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I'+str(root/'include'),
     str(root/'checks/display-scanout.c'), *[str(root/n) for n in compiled], '-o', str(binary)])
result = run([str(binary)])
obj = a.evidence/'display_scanout.o'
run([a.cc, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
     '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-mstrict-align',
     '-mno-outline-atomics', '-fstack-usage', '-I'+str(root/'include'), '-I'+str(root/'runtime/include'),
     '-c', str(root/compiled[0]), '-o', str(obj)])
record = dict(result=result, inputs={n: hashlib.sha256((root/n).read_bytes()).hexdigest() for n in inputs},
              binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
              object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),
              size=run(['/usr/lib/llvm-21/bin/llvm-size', str(obj)]),
              undefined=run(['/usr/lib/llvm-21/bin/llvm-nm', '-u', str(obj)]),
              stack=obj.with_suffix('.su').read_text(),
              scope='New controller composition only; existing leaf source evidence reused. No board or benchmarks.')
(a.evidence/'results.json').write_text(json.dumps(record, indent=2)+'\n')
print(result, end='')
