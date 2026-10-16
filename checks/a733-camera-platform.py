#!/usr/bin/env python3
"""Check cold analog setup against pinned source and manual register semantics."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--sources', type=Path, required=True, help='Pinned combo_csi directory')
p.add_argument('--manual', type=Path, required=True, help='Pinned A733 manual PDF')
p.add_argument('--text', type=Path, required=True, help='Pinned pdftotext -layout output')
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--cc', default='clang-21')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
inputs = {}
commands = []


def pinned(path, expected):
    data = path.read_bytes()
    actual = hashlib.sha256(data).hexdigest()
    assert actual == expected, path
    inputs[str(path)] = actual
    return data


def function(source, name):
    m = re.search(r'^void ' + name + r'\(', source, re.M)
    assert m, name
    i = source.index('{', m.end()) + 1
    depth = 1
    while depth:
        depth += (source[i] == '{') - (source[i] == '}')
        i += 1
    return source[m.start():i]


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    commands.append(dict(command=command, returncode=result.returncode,
                         stdout=result.stdout, stderr=result.stderr))
    (a.evidence / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    assert result.returncode == 0, commands[-1]
    return result.stdout


c = pinned(a.sources / 'combo_csi_reg.c',
           'e6368908f8393707c82aafaf7704af52a1c2b26c829b54553c123ed8bcfe09b4').decode()
h = pinned(a.sources / 'combo_csi_reg_i.h',
           '33cd56ebac15fcb1b9457b221d983f004576a0ebf0766224f4b256dbafdf282d').decode()
pinned(a.manual, 'cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d')
t = pinned(a.text, '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2').decode()
# Locate the register description, excluding the contents/table-of-registers.
start = t.index('16.1.10.2      0x0004 PHY Terminal Resistor Calibration Register')
end = t.index('16.1.11 CSIC_PHY DIGITAL LAYER Register Description', start)
calibration = ' '.join(t[start:end].split())
assert '20:16 R 0x0' in calibration
assert 'In software calibration mode, the result is the value of PHYA_TRESCAL_SET bit.' in calibration
assert 'This bit is fixed as 1 in software calibration mode.' in calibration
assert 'This bit is only valid for automatic calibration' in calibration
assert 'Enable (High Active)' in calibration
# Pin the source defect as a deliberate difference, never a reference oracle.
bad = function(c, 'cmb_phy_set_trescal')
assert 'CMB_PHYA_TRESCAL_RESULT, val << CMB_PHYA_TRESCAL_RESULT' in bad
pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define IS_ENABLED(x) x
#define CONFIG_ARCH_SUN60IW2 1
static uint32_t reg,trace[8];static unsigned n;
static volatile void *cmb_csi_top_base_addr=&reg;
static void vin_reg_clr_set(volatile void *p,uint32_t mask,uint32_t v){
 assert(p==&reg && n<8);reg=(reg&~mask)|(v&mask);trace[n++]=reg;
}
'''
names = ['cmb_phy_set_vref_0p9', 'cmb_phy_set_vref_0p2',
         'cmb_phy_reset_deassert', 'cmb_phy_power_enable']
main = r'''
int main(int argc,char **argv){
 assert(argc==2);reg=0x360500;
 cmb_phy_set_vref_0p9(2);cmb_phy_set_vref_0p2(3);
 cmb_phy_reset_deassert();cmb_phy_power_enable();assert(n==6);
 uint32_t image[5]={reg,trace[2],trace[3],trace[4],trace[5]};
 FILE *f=fopen(argv[1],"wb");assert(f);
 assert(fwrite(image,sizeof(image),1,f)==1);assert(!fclose(f));return 0;
}
'''
reference = a.evidence / 'reference.c'
reference.write_text(pre + h + '\n'.join(function(c, name) for name in names) + main)
host = a.evidence / 'native'
ref = a.evidence / 'reference'
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror']
run([a.cc, *flags, str(reference), '-o', str(ref)])
run([a.cc, *flags, '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
     '-Iinclude', 'checks/a733-camera-platform.c',
     'drivers/camera/a733_camera_platform.c', '-o', str(host)])
run([str(ref), str(a.evidence / 'reference.bin')])
run([str(host), str(a.evidence / 'native.bin')])
assert (a.evidence / 'reference.bin').read_bytes() == (a.evidence / 'native.bin').read_bytes()
obj = a.evidence / 'a733_camera_platform.o'
run([a.cc, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra',
     '-Werror', '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only',
     '-mstrict-align', '-mno-outline-atomics', '-fstack-usage', '-Iinclude',
     '-Iruntime/include', '-c', 'drivers/camera/a733_camera_platform.c', '-o', str(obj)])
size = run(['/usr/lib/llvm-21/bin/llvm-size', str(obj)])
undefined = run(['/usr/lib/llvm-21/bin/llvm-nm', '-u', str(obj)])
for path in ['include/a733_camera_platform.h', 'drivers/camera/a733_camera_platform.c',
             'checks/a733-camera-platform.c', 'checks/a733-camera-platform.py']:
    f = Path(path)
    inputs[path] = hashlib.sha256(f.read_bytes()).hexdigest()
    snapshot = a.evidence / 'snapshot' / path
    snapshot.parent.mkdir(parents=True, exist_ok=True)
    snapshot.write_bytes(f.read_bytes())
(a.evidence / 'results.json').write_text(json.dumps(dict(
    result='PASS', inputs=inputs, size=size, undefined=undefined,
    object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),
    scope='Cold analog source order and manual software-calibration model; no hardware execution.'
), indent=2) + '\n')
print('PASS: pinned analog source order, manual calibration model and native compilation')
