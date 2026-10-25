#!/usr/bin/env python3
"""Compare native TOP/CCU writes with pinned functions; CORE has a separate BXM audit."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def block(source, pattern):
    start = re.search(pattern, source).start()
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('sources', type=Path)
    p.add_argument('evidence', type=Path)
    a = p.parse_args()
    manifest = json.loads((a.sources / 'manifest.json').read_text())
    inputs = {}
    def read(name):
        data = (a.sources / name).read_bytes()
        identity = next(x for x in manifest if x['path'] == name)
        assert hashlib.sha256(data).hexdigest() == identity['sha256']
        assert hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest() == identity['git_blob']
        inputs[name] = identity
        return data.decode()
    pd = read('drivers/pm_domain/pck600_domains.c')
    binding = read('include/dt-bindings/power/sun60iw2-power.h')
    ccu = read('drivers/clk/sunxi-ng/ccu-sun60iw2.c')
    rccu = read('drivers/clk/sunxi-ng/ccu-sun60iw2-r.c')
    dtsi = read('arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi')
    board = read('arch/arm64/boot/dts/allwinner/sun60i-a733-orangepi-zero3w.dts')
    native = [tuple(int(v, 16 if i >= 2 else 10) for i, v in enumerate(line.split()))
              for line in (a.evidence / 'native-writes.txt').read_text().splitlines()]
    # Compile the original PCK initialization and transition functions unchanged.
    code = pd[:pd.index('#include')] + '\n' + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))
#define dsb(x) ((void)0)
#define sunxi_err(...) do { (void)genpd; assert(0); } while (0)
#define readx_poll_timeout_atomic(fn,pd,v,cond,delay,timeout) ((v)=fn(pd), !(cond))
struct generic_pm_domain { const char *name; };
'''
    for name in ['sunxi_domain_info', 'sunxi_pmu_info']:
        code += block(pd, r'struct ' + name + r'\s*\{') + ';\n'
    code += r'''
struct sunxi_pmu { unsigned *regmap; const struct sunxi_pmu_info *info; };
struct sunxi_pm_domain { struct sunxi_pmu *pmu; const struct sunxi_domain_info *info;
    struct generic_pm_domain genpd; };
static int regmap_write(unsigned *r, unsigned off, unsigned value) {
    printf("0 2 %x 0 %x\n", off, value); r[off/4]=value; return 0;
}
static int regmap_write_bits(unsigned *r,unsigned off,unsigned mask,unsigned value) {
    printf("1 2 %x %x %x\n",off,mask,value);
    r[off/4]=(r[off/4]&~mask)|value; r[(off+8)/4]=value; return 0;
}
static int regmap_read(unsigned *r,unsigned off,unsigned *out) { *out=r[off/4]; return 0; }
'''
    code += binding + '\n'
    code += pd[pd.index('#define DOMAIN('):pd.index('static bool sunxi_pmu_domain_is_on')]
    for name in ['sunxi_pmu_domain_is_on', 'sunxi_do_pmu_set_power_domain', 'sunxi_pd_init']:
        code += block(pd, r'static (?:bool|void|int) ' + name + r'\(') + '\n'
    code += block(pd, r'static const struct sunxi_domain_info sun60iw2_pck600_pm_domains\[\]') + ';\n'
    code += block(pd, r'static const struct sunxi_pmu_info sun60iw2_pck600_pmu') + ';\n'
    code += r'''
int main(void) {
    unsigned regs[0xb000/4]={0};
    struct sunxi_pmu pmu={regs,&sun60iw2_pck600_pmu};
    struct sunxi_pm_domain pd={&pmu,&sun60iw2_pck600_pm_domains[SUN60IW2_PCK_GPU_TOP],{"gpu"}};
    assert(!sunxi_pd_init(&pd));
    sunxi_do_pmu_set_power_domain(&pd,true);
    sunxi_do_pmu_set_power_domain(&pd,false);
}
'''
    c = a.evidence / 'reference.c'
    c.write_text(code)
    exe = a.evidence / 'reference'
    command = ['clang-21', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)]
    subprocess.run(command, check=True)
    output = subprocess.check_output([str(exe)], text=True)
    (a.evidence / 'reference-writes.txt').write_text(output)
    expected = [tuple(int(v, 16 if i >= 2 else 10) for i, v in enumerate(line.split()))
                for line in output.splitlines()]
    assert [x for x in native if x[1] == 2] == expected
    # CORE is not initialized by the generic domain driver. Its three writes
    # follow the selected BXM helpers documented in docs/gpu-power.md.
    assert [x for x in native if x[1] == 3] == [
        (1, 3, 0x20, 0xffffffff, 0), (1, 3, 0, 0xffffffff, 8),
        (1, 3, 0, 0xffffffff, 0)]
    # BXM sunxi_platform_init writes PWCR 0 and policy 8 after the TOP request
    # and before it enables any GPU clock (sunxi_platform.c:911-920). The trace
    # starts from the board's power-up CORE state, policy 8 and status 0.
    first_ccu = next(i for i, x in enumerate(native) if x[1] == 0)
    top_on = native.index((1, 2, 0x5000, 0xf, 8))
    assert top_on < native.index((1, 3, 0x20, 0xffffffff, 0)) < native.index((1, 3, 0, 0xffffffff, 8)) < first_ccu
    core = block(ccu, r'static struct ccu_div gpu0_clk\s*=')
    assert '_SUNXI_CCU_DIV_TABLE(0, 4, gpu_div_table)' in core
    assert '_SUNXI_CCU_MUX(24, 3)' in core and '.enable         = BIT(31)' in core
    assert '.reg            = 0x0B20' in core and '.key_value\t= UPD_KEY_VALUE' in core
    key = int(re.search(r'#define UPD_KEY_VALUE (0x[0-9a-fA-F]+)', ccu)[1], 16)
    parents = re.search(r'gpu0_parents\[\] = \{([^}]+)\}', ccu)[1]
    mux = re.findall(r'"([^"]+)"', parents).index('pll-peri0-400m')
    table = block(ccu, r'static struct clk_div_table gpu_div_table\[\]')
    divisor = int(re.search(r'\.val = (\d+), \.div = 1', table)[1])
    assert re.search(r'gpu0_bus_clk, "gpu0-gate",\s*"dcxo",\s*0x0B24, BIT\(0\)', ccu)
    assert re.search(r'\[RST_BUS_GPU0\]\s*= \{ 0x0b24, BIT\(16\) \}', ccu)
    assert re.search(r'r_ppu_clk, "r-ppu",\s*"dcxo",\s*0x01AC, BIT\(0\)', rccu)
    assert re.search(r'gpu0_ahb_gate_clk,[\s\S]{0,120}0x05C0,[\s\S]{0,80}BIT\(7\)', ccu)
    assert re.search(r'gpu0_mbus_gate_clk,[\s\S]{0,120}0x05E0,[\s\S]{0,80}BIT\(16\)', ccu)
    assert [x for x in native if x[1] == 0] == [
        (1, 0, 0xb20, 0x8700000f, key | (mux << 24) | divisor),
        (1, 0, 0xb20, 0, key | (1 << 31)),
        (1, 0, 0xb24, 0, 1), (1, 0, 0xb24, 0, 1 << 16),
        (1, 0, 0xb24, 1 << 16, 0), (1, 0, 0xb24, 1, 0),
        (1, 0, 0xb20, 1 << 31, key)]
    assert 'reg = <0x0 0x02002000 0x0 0x2000>' in dtsi
    assert 'reg = <0x0 0x07010000 0x0 0x340>' in dtsi
    assert 'reg = <0x0 0x07060000 0x0 0xB000>' in dtsi
    opp = block(dtsi, r'opp@400000000\s*\{')
    assert 'opp-microvolt = <800000>' in opp
    assert re.search(r'&gpu\s*\{\s*gpu-supply = <&reg_dcdc4>', board)
    assert 'regulator-always-on' in block(board, r'reg_dcdc4: dcdc4\s*\{')
    (a.evidence / 'reference-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    (a.evidence / 'reference-command.json').write_text(json.dumps(command) + '\n')
    print('PASS pinned PCK functions, GPU clock/reset definitions, board supply and 400 MHz operating point')


if __name__ == '__main__':
    main()
