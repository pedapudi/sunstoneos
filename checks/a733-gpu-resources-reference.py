#!/usr/bin/env python3
"""Check GPU shared-gate writes, parent rate and rail setting against pinned functions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def block(text, pattern):
    start = re.search(pattern, text).start()
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('power_sources', type=Path)
p.add_argument('resource_sources', type=Path)
p.add_argument('uboot', type=Path)
p.add_argument('evidence', type=Path)
a = p.parse_args()
inputs = {}
def source(root, path, archive=False):
    data = (root / path).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if not archive:
        manifest = json.loads((root / 'manifest.json').read_text())
        row = next(r for r in manifest if r['path'] == path)
        assert row['sha256'] == digest
        assert row['git_blob'] == hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
    inputs[str(root / path)] = digest
    return data.decode()
gates = source(a.power_sources, 'drivers/clk/sunxi-ng/ccu_gate.c')
nm = source(a.power_sources, 'drivers/clk/sunxi-ng/ccu_nm.c')
ccu = source(a.power_sources, 'drivers/clk/sunxi-ng/ccu-sun60iw2.c')
rtc = source(a.resource_sources, 'drivers/clk/sunxi-ng/ccu-sun60iw2-rtc.c')
mult = source(a.resource_sources, 'drivers/clk/sunxi-ng/ccu_mult.h')
div = source(a.resource_sources, 'drivers/clk/sunxi-ng/ccu_div.h')
sdm = source(a.resource_sources, 'drivers/clk/sunxi-ng/ccu_sdm.h')
pmic = source(a.uboot, 'drivers/sunxi_power/pmu_axp8191.c', True)
pmic_header = source(a.uboot, 'include/sunxi_power/pmu_axp8191.h', True)
assert 'AHB_MASTER_KEY_VALUE\t0x10000FF' in ccu and 'MBUS_MASTER_KEY_VALUE\t0x41055800' in ccu
assert '"pll-peri0-2x", 3, 1, 0' in ccu
for name, fields in [('pll_ref_clk', ['_SUNXI_CCU_MULT_MIN_MAX(8, 8, 80, 116)', '_SUNXI_CCU_DIV(16, 7)']),
                     ('pll_peri0_clk', ['_SUNXI_CCU_MULT_MIN_MAX(8, 8, 53, 105)', 'BIT(25) | BIT(26) | BIT(27)',
                                        '0x00A8, BIT(31)', '0x00AC, BIT(27)'])]:
    body = block(ccu, r'static struct ccu_nm ' + name + r'\s*=')
    for text in fields: assert text in body
assert re.search(r'pll_peri0_2x_clk,[\s\S]{0,100}0x00A0,\s*20, 3', ccu)
assert '_SUNXI_CCU_MULT_OFFSET_MIN_MAX(_shift, _width, 1, _min, _max)' in mult
assert '_SUNXI_CCU_DIV_OFFSET_MAX_MIN_FLAGS(_shift, _width, 1, _max, 1,  _flags)' in div
assert '_SUNXI_CCU_DIV_MAX_FLAGS(_shift, _width, 0, _flags)' in div
assert '.tuning_enable\t= _reg_enable' in sdm and '.pattern1_enable\t= _reg_1_enable' in sdm
assert '{ "dcxo24M", "dcxo19_2M", "dcxo26M", "dcxo24M" }' in rtc
assert re.search(r'dcxo_parents, 0x160,\s*14, 2', rtc)
code = gates[:gates.index('#include')] + nm[:nm.index('#include')] + pmic[:pmic.index('#include')]
code += r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define CONFIG_SYS_SUNXI_R_I2C0_SLAVE 0x36
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define do_div(x,d) ((x)/=(d))
#define CCU_FEATURE_KEY_FIELD_MOD 1
#define CCU_FEATURE_GATE_IS_REVERSE 2
#define spin_lock_irqsave(lock,flags) ((flags)=0)
#define spin_unlock_irqrestore(lock,flags) ((void)(flags))
#define trace_clk_ng_enable(hw) ((void)(hw))
#define axp_err(...) assert(0)
struct ccu_common { unsigned char *base; unsigned reg,key_reg,key_value,features,hw; };
static unsigned readl(void *p) { return *(unsigned *)p; }
static void writel(unsigned v,void *p) { *(unsigned *)p=v; }
static void ccu_common_helper_enable(struct ccu_common *c) { assert(c->features==1 || c->features==0); }
static int pmic_bus_read(unsigned address,unsigned reg,u8 *out) {
    assert(address==0x36);
    assert(reg==0x10 || reg==0x15);
    *out=reg==0x10 ? 0xff : 0xae; return 0; /* DCDC4 raised to the GPU operating point */
}
'''
code += pmic_header + '\n'
code += block(gates, r'int ccu_gate_helper_enable\(') + '\n'
code += block(nm, r'static u64 ccu_nm_calc_rate\(') + '\n'
code += block(pmic, r'typedef struct _axp_contrl_info') + ' axp_contrl_info;\n'
row = re.search(r'\{ "dcdc4",[^}]+\}', pmic)[0]
code += 'axp_contrl_info pmu_axp8191_ctrl_tbl[] = {' + row + '};\n'
code += block(pmic, r'static axp_contrl_info \*get_ctrl_info_from_tbl\(') + '\n'
code += block(pmic, r'static int pmu_axp8191_get_voltage\(') + '\n'
code += r'''
int main(void) {
    unsigned values[]={0x40,0x1100007f,0x61045802};
    unsigned keys[]={0,0x010000ff,0x41055800},bits[]={1,0x80,0x10000};
    unsigned addresses[]={0x070101ac,0x020025c0,0x020025e0};
    for(unsigned i=0;i<3;++i) {
        struct ccu_common c={(unsigned char *)&values[i],0,0,keys[i],i?1u:0u,0};
        assert(!ccu_gate_helper_enable(&c,bits[i]));
        printf("%x %08x\n",addresses[i],values[i]);
    }
    u64 ref=ccu_nm_calc_rate(24000000,100,100);
    u64 peri=ccu_nm_calc_rate(ref,100,1);
    assert(ref==24000000 && peri/2/3==400000000);
    assert(pmu_axp8191_get_voltage("dcdc4")==960);
}
'''
(a.evidence / 'reference.c').write_text(code)
command = ['clang-21', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
           str(a.evidence / 'reference.c'), '-o', str(a.evidence / 'reference')]
subprocess.run(command, check=True)
output = subprocess.check_output([str(a.evidence / 'reference')], text=True)
(a.evidence / 'reference-gates.txt').write_text(output)
native = dict(tuple(int(x, 16) for x in line.split()) for line in (a.evidence / 'native-registers.txt').read_text().splitlines())
for line in output.splitlines():
    address, value = (int(x, 16) for x in line.split())
    assert native[address] == value
(a.evidence / 'reference-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
(a.evidence / 'reference-command.json').write_text(json.dumps(command) + '\n')
print('PASS original shared-gate writes, PLL integer rate calculation and AXP8191 DCDC4 960 mV voltage decode')
