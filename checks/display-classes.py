#!/usr/bin/env python3
"""Check display classes with pinned public EDIDs and host fault inputs."""
import argparse
import hashlib
import json
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
