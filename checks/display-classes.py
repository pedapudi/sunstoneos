#!/usr/bin/env python3
"""Check display classes with pinned public EDIDs and host fault inputs."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

CHECK_INPUTS = ['include/sunstone_display.h', 'include/a733_display_modes.h',
                'drivers/display', 'checks/display-classes.c', 'docs/display-class-sources.json']
p = argparse.ArgumentParser()
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--source-cache', type=Path, default=Path.home()/'.cache/sunstoneos/sources/display-class')
p.add_argument('--cc', default=str(CLANG))
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
a.evidence.mkdir(parents=True, exist_ok=True)
manifest = json.loads((root/'docs/display-class-sources.json').read_text())
for source in manifest['sources']:
    data = (a.source_cache/source['file']).read_bytes()
    assert hashlib.sha256(data).hexdigest() == source['sha256'], source['file']
# Compare every implemented CTA timing against an independently pinned table.
reference = (a.source_cache/'drm_edid.c').read_text()
parser = (root/'drivers/display/sunstone_edid.c').read_text()
for vic, fields, flags in re.findall(r'\{(\d+),\{([0-9,]+),(PP|0)\}\}', parser):
    match = re.search(r'/\* '+vic+r' -.*?DRM_MODE\([^,]+, DRM_MODE_TYPE_DRIVER,\s*(.*?)\)', reference, re.S)
    assert match, vic
    numbers = [int(x) for x in re.findall(r'\b\d+\b', match.group(1))]
    assert len(numbers) == 11, (vic, numbers)
    n = numbers
    expected = [n[0]*1000,n[1],n[6],n[2]-n[1],n[3]-n[2],n[4]-n[3],n[7]-n[6],n[8]-n[7],n[9]-n[8]]
    assert list(map(int,fields.split(','))) == expected, (vic, expected)
    assert ('DRM_MODE_FLAG_PHSYNC' in match.group(1)) == (flags == 'PP'), vic
    assert ('DRM_MODE_FLAG_PVSYNC' in match.group(1)) == (flags == 'PP'), vic
sources = ['drivers/display/sunstone_edid.c', 'drivers/display/sunstone_connector.c',
           'drivers/display/sunstone_panels.c', 'drivers/display/a733_display_modes.c',
           'drivers/display/a733_hdmi.c', 'drivers/display/a733_hdmi_phy.c', 'drivers/display/a733_dp.c',
           'drivers/display/a733_display_engine.c', 'drivers/display/a733_tcon_lcd.c',
           'drivers/display/a733_dsi_video.c', 'drivers/display/a733_dsi_phy.c']
commands = []
def run(command):
    r = subprocess.run(command, cwd=root, capture_output=True, text=True, env=environment())
    commands.append(dict(command=command, returncode=r.returncode, stdout=r.stdout, stderr=r.stderr))
    (a.evidence/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    assert r.returncode == 0, commands[-1]
    return r.stdout
binary = a.evidence/'check'
run([a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
     '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
     *SANITIZE, '-Iinclude',
     'checks/display-classes.c', *sources, '-o', str(binary)])
fixtures = [a.source_cache/name for name in manifest['edid_fixtures']]
result = run([str(binary), *map(str, fixtures)])
for source in sources[:4]:
    run([a.cc, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
         '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-mstrict-align',
         '-mno-outline-atomics', '-Iinclude', '-Iruntime/include', '-c', source,
         '-o', str(a.evidence/(Path(source).stem+'.o'))])
record = dict(result=result, inputs={name:hashlib.sha256((root/name).read_bytes()).hexdigest()
              for name in sources+['checks/display-classes.c','checks/display-classes.py',
                                  'include/sunstone_display.h','include/a733_display_modes.h',
                                  'docs/display-class-sources.json']},
              scope='Host parser, negotiation, panel sequence and native compilation; no board output or electrical qualification')
(a.evidence/'results.json').write_text(json.dumps(record, indent=2)+'\n')
print(result, end='')
