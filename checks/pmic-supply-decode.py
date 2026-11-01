#!/usr/bin/env python3
"""Compare disputed supply selectors with both pinned sources and the real provider."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

import harness

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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = {}

    def preserve(path):
        path = Path(path).resolve()
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        inputs[str(path)] = digest
        dest = out / 'inputs' / digest
        dest.parent.mkdir(exist_ok=True)
        if not dest.exists():
            dest.write_bytes(data)
        return data

    def run(label, command):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=ROOT, env=harness.environment(),
                                capture_output=True, text=True)
        (out / (label + '.json')).write_text(json.dumps({
            'command': command, 'returncode': result.returncode,
            'stdout': result.stdout, 'stderr': result.stderr}, indent=2) + '\n')
        if result.returncode:
            raise RuntimeError(f'{label} failed: {result.stdout}{result.stderr}')
        return result.stdout

    source_record = ROOT / 'apps/power-supply-observe/sources.json'
    record = json.loads(preserve(source_record))
    texts = {}
    for name, item in record['sources'].items():
        path = harness.CACHE / 'sources' / item['cache']
        texts[name] = preserve(path).decode()
        assert inputs[str(path.resolve())] == item['sha256'], name
    preserve(__file__)
    preserve(ROOT / 'checks/harness.py')
    preserve(harness.CLANG)
    units = ['checks/pmic-supply-decode.c', 'drivers/power/a733_pmic.c',
             'drivers/power/a733_rail_leases.c', 'drivers/clock/a733_clock_leases.c',
             'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c',
             'drivers/pinctrl/a733_pin_tables.c']
    objects = []
    for i, source in enumerate(units):
        obj, dep = out / f'unit-{i}.o', out / f'unit-{i}.d'
        run(f'compile-{i}', [harness.CLANG, '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-Iinclude', *harness.SANITIZE, '-MMD', '-MF', dep,
                            '-c', source, '-o', obj])
        for path in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]):
            preserve(ROOT / path)
        objects.append(obj)
    run('link', [harness.CLANG, *harness.SANITIZE, *objects, '-o', out / 'native'])
    run('snapshots', [out / 'native'])
    native = run('decodes', [out / 'native', '--decode'])

    # The original getter executes on the host; only its I2C transport is modeled.
    pmic = texts['bootloader']
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
    if(r==0x10 || r==0x11)*out=255;
    else {assert(r==0x19 || r==0x1a);*out=selector;}
    return 0;
}
'''
    code += texts['bootloader-registers'] + '\n'
    code += block(pmic, r'typedef struct _axp_contrl_info') + ' axp_contrl_info;\n'
    code += 'axp_contrl_info pmu_axp8191_ctrl_tbl[]={\n'
    code += ',\n'.join(re.search(r'\{ "' + name + r'",[^}]+\}', pmic)[0]
                         for name in ('dcdc8', 'dcdc9')) + '\n};\n'
    code += block(pmic, r'static axp_contrl_info \*get_ctrl_info_from_tbl\(') + '\n'
    code += block(pmic, r'static int pmu_axp8191_get_voltage\(') + '\n'
    code += ('int main(void){for(unsigned r=8;r<=9;++r){char name[16];'
             'snprintf(name,sizeof(name),"dcdc%u",r);for(selector=0;selector<256;++selector)'
             'printf("%s %u %d\\n",name,selector,pmu_axp8191_get_voltage(name)*1000);}return 0;}\n')
    (out / 'reference.c').write_text(code)
    run('reference-build', [harness.CLANG, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-missing-field-initializers', *harness.SANITIZE,
                            out / 'reference.c', '-o', out / 'reference'])
    reference = run('reference', [out / 'reference'])
    boot = {(name, int(raw)): int(uv) for name, raw, uv in map(str.split, reference.splitlines())}
    ranges = {}
    for name in ('dcdc8', 'dcdc9'):
        table = block(texts['linux'], r'static struct linear_range axp8191_' + name + '_ranges')
        ranges[name] = [tuple(int(x, 0) for x in row) for row in re.findall(
            r'REGULATOR_LINEAR_RANGE\((\d+), (0x[0-9a-f]+), (0x[0-9a-f]+), (\d+)\)', table)]
        assert len(ranges[name]) == 3
    disagreements = []
    for line in native.splitlines():
        name, raw, enabled, decoded, uv = line.split()
        raw, enabled, decoded, uv = map(int, (raw, enabled, decoded, uv))
        code = raw & 127
        linux = next((base + step * (code - low) for base, low, high, step in ranges[name]
                      if low <= code <= high), None)
        assert decoded == (code <= 87)
        assert enabled in (0, 1)
        if decoded:
            assert uv == boot[name, raw] == linux
        else:
            assert uv == 0
        if enabled and code >= 88:
            disagreements.append({'rail': name, 'raw': raw, 'uboot_uv': boot[name, raw], 'linux_uv': linux})
    assert len(native.splitlines()) == 1024 and len(boot) == 512
    assert boot['dcdc8', 0xe4] == 1800000 and ranges['dcdc8'][2] == (1900000, 88, 103, 100000)
    run('native-compile', [harness.CLANG, '--target=aarch64-none-elf', '-march=armv8-a',
                           '-mstrict-align', '-ffreestanding', '-fno-builtin', '-O2', '-Wall',
                           '-Wextra', '-Werror', '-Iinclude', '-c', 'drivers/power/a733_pmic.c',
                           '-o', out / 'a733_pmic.o'])
    for path, digest in inputs.items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest, path
    (out / 'results.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'inputs': inputs,
        'selector_cases': 1024, 'uboot_getter_cases': 512,
        'disputed_or_unsupported': disagreements,
        'artifacts': {name: hashlib.sha256((out / name).read_bytes()).hexdigest()
                      for name in ('native', 'reference', 'reference.c', 'a733_pmic.o')},
        'limits': 'No physical voltage, wiring, timing or recovery proof.'}, indent=2) + '\n')
    print('PASS: both source tables, 1024 native decodes, 512 original getter results and six provider snapshots')


if __name__ == '__main__':
    main()
