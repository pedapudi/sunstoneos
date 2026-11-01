#!/usr/bin/env python3
"""Check the native sensor class and pinned upstream VGA register table."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from harness import CLANG, SANITIZE, environment

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence', required=True, type=Path)
p.add_argument('--source', type=Path, default=Path.home()/'.cache/sunstoneos/sources/camera-sensors-linux-v6.12/ov5647.c')
p.add_argument('--cc', default=str(CLANG))
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
sources = ['drivers/camera/camera_sensor.c', 'drivers/camera/sensor_registry.c',
           'drivers/camera/ov5640_sensor.c', 'drivers/camera/ov5647_sensor.c', 'drivers/camera/ov5640.c']
record = json.loads((root/'drivers/camera/sensor-sources.json').read_text())
assert hashlib.sha256(a.source.read_bytes()).hexdigest() == record['sha256']
upstream = re.search(r'static struct regval_list ov5647_640x480_10bpp\[\] = \{(.*?)\n\};', a.source.read_text(), re.S)[1]
adapter = re.search(r'static const struct regval vga\[\] = \{(.*?)\n\};', (root/sources[3]).read_text(), re.S)[1]
entries = lambda s: re.findall(r'\{\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+)\s*\}', s)
assert entries(upstream) == entries(adapter)
commands = []

def run(command):
    result = subprocess.run(list(map(str, command)), cwd=root, capture_output=True, text=True, env=environment())
    commands.append(dict(command=list(map(str, command)), returncode=result.returncode,
                         stdout=result.stdout, stderr=result.stderr))
    (a.evidence/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    assert result.returncode == 0, commands[-1]
    return result.stdout

binary = a.evidence/'sensor-model'
run([a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
     '-Iinclude', 'checks/camera-sensor.c', *sources, '-o', binary])
result = run([binary])
for source in sources:
    run([a.cc, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
         '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-Iinclude', '-Iruntime/include',
         '-c', source, '-o', a.evidence/(Path(source).stem+'.o')])
inputs = {}
for source in [*sources, 'checks/camera-sensor.c', 'checks/camera-sensor.py',
               'include/camera_sensor.h', 'include/ov5640.h', 'drivers/camera/ov5640_tables.h',
               'drivers/camera/sensor-sources.json', str(a.source)]:
    path = Path(source) if Path(source).is_absolute() else root/source
    inputs[source] = hashlib.sha256(path.read_bytes()).hexdigest()
(a.evidence/'results.json').write_text(json.dumps(dict(status='PASS', result=result, inputs=inputs,
    upstream_table_entries=len(entries(upstream)), physical=False,
    limitations='Host sensor model and native compilation only. OV5647 RAW10 requires a compatible ISP path; current NV16 capture refuses it.'), indent=2)+'\n')
print(result, end='')
