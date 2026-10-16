#!/usr/bin/env python3
"""Check only the two additional MMC PMIC outputs and archive their evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def block(text, pattern):
    start = re.search(pattern, text).start()
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('uboot', type=Path)
    ap.add_argument('evidence', type=Path)
    a = ap.parse_args()
    out = a.evidence.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'inputs').mkdir()
    inputs = {}

    def save(path):
        path = path.resolve()
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        inputs[str(path)] = digest
        target = out / 'inputs' / digest
        if not target.exists():
            target.write_bytes(data)

    def run(name, argv, stdin=None):
        r = subprocess.run(list(map(str, argv)), cwd=ROOT, text=True, input=stdin,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (name + '.json')).write_text(json.dumps({'argv': list(map(str, argv)),
            'returncode': r.returncode, 'output': r.stdout}, indent=2) + '\n')
        if r.returncode:
            raise RuntimeError(f'{name} failed: {r.stdout}')
        return r.stdout

    source = a.uboot / 'drivers/sunxi_power/pmu_axp8191.c'
    header = a.uboot / 'include/sunxi_power/pmu_axp8191.h'
    for path in [source, header, Path(__file__), ROOT / 'checks/a733-pmic.c']:
        save(path)
    pmic, definitions = source.read_text(), header.read_text()
    constants = {'AXP8191_DCDC_POWER_ON_OFF_CTL2': 0x11, 'AXP8191_DC1OUT_VOL': 0x12,
                 'AXP8191_LDO_POWER_ON_OFF_CTL2': 0x21, 'AXP8191_CLDO5OUT_VOL': 0x33}
    for name, value in constants.items():
        found = re.search(r'#define\s+' + name + r'\s+\((0x[0-9a-fA-F]+)\)', definitions)
        assert found and int(found[1], 16) == value
    entries = [re.search(r'\{ "' + name + r'",[^}]+\}', pmic)[0] for name in ['dc1sw2', 'cldo5']]
    for entry, terms in zip(entries, [
        ['1000, 3800, AXP8191_DC1OUT_VOL, 0x1f, 100', 'AXP8191_DCDC_POWER_ON_OFF_CTL2, 4'],
        ['500, 3400, AXP8191_CLDO5OUT_VOL, 0x1f, 100', 'AXP8191_LDO_POWER_ON_OFF_CTL2, 7']]):
        for term in terms:
            assert term in entry
    units = ['checks/a733-pmic-mmc.c', 'drivers/power/a733_pmic.c',
             'drivers/power/a733_rail_leases.c', 'drivers/clock/a733_clock_leases.c',
             'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c',
             'drivers/pinctrl/a733_pin_tables.c']
    for name in units:
        save(ROOT / name)
    for path in (ROOT / 'include').glob('*.h'):
        save(path)
    clang = Path(shutil.which('clang-21')).resolve()
    save(clang)
    common = [clang, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Iinclude']
    model = out / 'pmic-mmc'
    run('host-build', [*common, '-O1', '-g', '-fsanitize=address,undefined',
                      '-fno-omit-frame-pointer', *units, '-o', model])
    (out / 'executed-image.json').write_text(json.dumps({
        'path': str(model), 'sha256': hashlib.sha256(model.read_bytes()).hexdigest()}, indent=2) + '\n')
    run('snapshots', [model])
    decoded = run('decode-boundaries', [model, '--decode'])
    (out / 'decodes.txt').write_text(decoded)
    # Preserve the original source notices and execute its actual getter with
    # only the two new table entries. The I2C transport remains modeled.
    code = pmic[:pmic.index('#include')] + r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define CONFIG_SYS_SUNXI_R_I2C0_SLAVE 0x36
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define axp_err(...) assert(0)
static unsigned selector;
static int pmic_bus_read(unsigned a,unsigned r,u8 *out) {
    assert(a==0x36);
    if(r==0x11 || r==0x21)*out=255;
    else {assert(r==0x12 || r==0x33);*out=selector;}
    return 0;
}
'''
    code += definitions + '\n'
    code += block(pmic, r'typedef struct _axp_contrl_info') + ' axp_contrl_info;\n'
    code += 'axp_contrl_info pmu_axp8191_ctrl_tbl[]={\n' + ',\n'.join(entries) + '\n};\n'
    code += block(pmic, r'static axp_contrl_info \*get_ctrl_info_from_tbl\(') + '\n'
    code += block(pmic, r'static int pmu_axp8191_get_voltage\(') + '\n'
    code += 'int main(void){char name[32];unsigned want;while(scanf("%31s %u %u",name,&selector,&want)==3)'
    code += '{assert(selector<256);assert(pmu_axp8191_get_voltage(name)==(int)want);}return 0;}\n'
    (out / 'reference.c').write_text(code)
    run('reference-build', [*common, '-Wno-missing-field-initializers', out / 'reference.c', '-o', out / 'reference'])
    save(out / 'reference.c')
    (out / 'reference-image.json').write_text(json.dumps({'sha256': hashlib.sha256((out / 'reference').read_bytes()).hexdigest()}) + '\n')
    run('reference-result', [out / 'reference'], decoded)
    run('native-build', [*common, '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align',
        '-ffreestanding', '-fno-builtin', '-O2', '-MMD', '-MF', out / 'native.d',
        '-c', 'drivers/power/a733_pmic.c', '-o', out / 'native.o'])
    for path, digest in inputs.items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
    artifacts = {name: hashlib.sha256((out / name).read_bytes()).hexdigest()
                 for name in ['pmic-mmc', 'reference', 'reference.c', 'native.o', 'native.d', 'decodes.txt']}
    (out / 'results.json').write_text(json.dumps({'status': 'PASS', 'inputs': inputs,
        'artifacts': artifacts, 'matched_getter_cases': len(decoded.splitlines()),
        'scope': 'Two new actual-provider snapshots; raw selectors and enable states for only DC1SW2/CLDO5; original getter comparison; changed core native compile.',
        'limits': 'No physical voltage, wiring, settling, bus timing or recovery proof. Prior provider scenarios are not executed.'}, indent=2) + '\n')
    print(f'PASS: two new snapshots, {len(decoded.splitlines())} getter matches and changed core native compile')


if __name__ == '__main__':
    main()
