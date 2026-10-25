#!/usr/bin/env python3
"""Compare native TOP, CORE and CCU writes with the pinned vendor functions and operating point.

TOP writes come from compiling the original PCK functions. CORE6 and GPU clock
writes follow the selected BXM rgx_sunxi adapter (sunxi_platform.c) and the
CCU helpers it calls, whose order, bits and factors this check reads from the
pinned sources; docs/designs/gpu-vendor-default.md compares the sequences.
The DCDC4 voltage and the speed-bin row of include/a733_gpu_opp.h must equal
the device tree's opp@1008000000 node."""
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
    p.add_argument('ddk', type=Path, help='BXM DDK checkout of the pinned board kernel (rogue_km parent)')
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
    /* The vendor table's CORE6 row, which sunxi_pd_init would write for a
     * registered CORE6 domain; its device-tree node is commented out. */
    puts("--");
    struct sunxi_pm_domain core={&pmu,&sun60iw2_pck600_pm_domains[SUN60IW2_PCK_GPU_CORE],{"gpu-core"}};
    assert(!sunxi_pd_init(&core));
}
'''
    c = a.evidence / 'reference.c'
    c.write_text(code)
    exe = a.evidence / 'reference'
    command = ['clang-21', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)]
    subprocess.run(command, check=True)
    output = subprocess.check_output([str(exe)], text=True)
    (a.evidence / 'reference-writes.txt').write_text(output)
    top_lines, core_lines = output.split('--\n')
    expected = [tuple(int(v, 16 if i >= 2 else 10) for i, v in enumerate(line.split()))
                for line in top_lines.splitlines()]
    assert [x for x in native if x[1] == 2] == expected
    # The vendor table holds a CORE6 delay row, which sunxi_pd_init writes
    # only for a registered domain; the CORE6 node is commented out, and no
    # native write touches PCK row 6 outside policy and PWCR.
    assert all(int(line.split()[2], 16) >> 12 == 6 for line in core_lines.splitlines())
    # The selected BXM adapter, identified by its Git blob in the DDK tree
    # record next to the checkout.
    tree = json.loads((a.ddk.parent / (a.ddk.name.split('-')[0] + '-tree.json')).read_text())
    platform_path = 'linux/rogue_km/services/system/rogue/rgx_sunxi/sunxi_platform.c'
    data = (a.ddk / platform_path).read_bytes()
    blob = next(x['sha'] for x in tree['tree'] if x['path'] == platform_path)
    assert hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest() == blob
    inputs[platform_path] = {'path': platform_path, 'git_blob': blob, 'sha256': hashlib.sha256(data).hexdigest()}
    plat = data.decode()

    def ordered(text, needles):
        at = [text.index(x) for x in needles]
        assert at == sorted(at), needles
    # CORE6: PWCR 0 and policy 8 at the first power-on, policy 8 at later
    # power-ons and policy 0 at power-off; no status read anywhere.
    assert re.search(r'#define PPU_GPU_CORE 0x07066000', plat)
    assert re.search(r'ioremap\(PPU_GPU_CORE \+ 0x20, 4\);\s*writel\(0x0, ioaddr\)', plat)
    assert re.search(r'sunxi_ppu_enable_gpu\(void\)[\s\S]{0,120}writel\(0x8, ioaddr\)', plat)
    assert re.search(r'sunxi_ppu_disable_gpu\(void\)[\s\S]{0,120}writel\(0x0, ioaddr\)', plat)
    init = block(plat, r'int sunxi_platform_init\(struct device \*dev\)')
    ordered(init, ['sunxiSetVoltage(sunxi_data->volt)', 'pm_runtime_get_sync(dev)', 'sunxi_ppu_init_gpu()',
                   'sunxi_ppu_enable_gpu()', 'sunxi_enable_clks_wrap(dev)', 'sunxi_set_device_clk_rate('])
    pre = block(plat, r'PVRSRV_ERROR sunxiPrePowerState\(')
    ordered(pre, ['sunxi_enable_clks_wrap(platform->dev)', 'pm_runtime_get_sync(platform->dev)', 'sunxi_ppu_enable_gpu()'])
    post = block(plat, r'PVRSRV_ERROR sunxiPostPowerState\(')
    ordered(post, ['sunxi_ppu_disable_gpu()', 'pm_runtime_put_sync(platform->dev)', 'sunxi_disable_clks_wrap(platform->dev)'])
    assert 'PPU_GPU_CORE + 0x8' not in plat
    assert [x for x in native if x[1] == 3] == [
        (1, 3, 0x20, 0xffffffff, 0), (1, 3, 0, 0xffffffff, 8), (1, 3, 0, 0xffffffff, 0)]
    # The GPU clock path of sunxi_enable_clks_wrap and sunxi_disable_clks_wrap.
    enable = block(plat, r'static void sunxi_enable_clks_wrap\(')
    ordered(enable, ['clk_prepare_enable(sunxi_data->clk_parent)', 'reset_control_deassert(sunxi_data->rst_bus)',
                     'clk_prepare_enable(sunxi_data->clk_bus)', 'clk_prepare_enable(sunxi_data->clk_core)',
                     'writel(value | 1 << 27, ioaddr)'])
    assert '#define GPU_CLK 0x02002b20' in plat
    disable = block(plat, r'static void sunxi_disable_clks_wrap\(')
    ordered(disable, ['clk_disable_unprepare(sunxi_data->clk_bus)', 'clk_disable_unprepare(sunxi_data->clk_core)',
                      'clk_disable_unprepare(sunxi_data->clk_parent)', 'reset_control_assert(sunxi_data->rst_bus)'])
    assert re.search(r'ui32FrequencyMHZ > 800\) \{\s*if \(clk_set_parent\(sunxi_data->clk_core, sunxi_data->clk_parent\)', plat)
    # PLL_GPU0 bits and factors, and the helpers' order (AW_STANDARD_CCU).
    gate = read('drivers/clk/sunxi-ng/ccu_gate.c')
    nm = read('drivers/clk/sunxi-ng/ccu_nm.c')
    pll = block(ccu, r'static struct ccu_nm pll_gpu0_clk\s*=')
    assert re.search(r'\.reg\s*= 0x00E0', pll) and '"pll-gpu", "pll-ref"' in pll
    bit = {k: 1 << int(re.search(r'\.' + k + r'\s*= BIT\((\d+)\)', pll)[1])
           for k in ['output', 'lock', 'lock_enable', 'ldo_en', 'enable']}
    nshift, nwidth, nmin, nmax = map(int, re.search(r'\.n\s*= _SUNXI_CCU_MULT_MIN_MAX\((\d+), (\d+), (\d+), (\d+)\)', pll).groups())
    mshift, mwidth = map(int, re.search(r'\.m\s*= _SUNXI_CCU_DIV\((\d+), (\d+)\)', pll).groups())
    opp_node = block(dtsi, r'opp@1008000000\s*\{')
    rate = int(re.search(r'opp-hz = /bits/ 64 <(\d+)>', opp_node)[1])
    m_div = next(m for m in range(1, 1 << mwidth) if rate * m % 24000000 == 0 and nmin <= rate * m // 24000000 <= nmax)
    n_mul = rate * m_div // 24000000
    factors_mask = ((1 << nwidth) - 1) << nshift | ((1 << mwidth) - 1) << mshift
    factors = (n_mul - 1) << nshift | (m_div - 1) << mshift
    assert (rate, n_mul, m_div, factors_mask, factors) == (1008000000, 84, 2, 0x70ff00, 0x105300)
    ordered(block(gate, r'int ccu_pll_gate_helper_enable\('), [
        'writel(reg & ~output', 'writel(reg | ldo_en', 'writel(reg | gate', 'writel(reg | lock_enable',
        'ccu_helper_wait_for_lock(common, lock)', 'writel(reg | output'])
    ordered(block(gate, r'void ccu_pll_gate_helper_disable\('), [
        'writel(reg & ~output', 'writel(reg & ~lock_en', 'ccu_gate_helper_disable(common, gate)',
        'writel(reg & ~ldo_en'])
    ordered(block(nm, r'static int ccu_nm_set_rate\('), [
        'ccu_pll_output_helper_disable(&nm->common, nm->output)', 'reg |= (_nm.n - nm->n.offset) << nm->n.shift',
        'reg &= ~(nm->lock_enable)', 'reg |= (nm->lock_enable)', 'ccu_helper_wait_for_lock(&nm->common, nm->lock)',
        'ccu_pll_output_helper_enable(&nm->common, nm->output)'])
    assert re.search(r'ccu_nm_disable[\s\S]{0,200}ccu_pll_gate_helper_disable', nm)
    core = block(ccu, r'static struct ccu_div gpu0_clk\s*=')
    assert '_SUNXI_CCU_DIV_TABLE(0, 4, gpu_div_table)' in core
    assert '_SUNXI_CCU_MUX(24, 3)' in core and '.enable         = BIT(31)' in core
    assert '.reg            = 0x0B20' in core and '.key_value\t= UPD_KEY_VALUE' in core
    key = int(re.search(r'#define UPD_KEY_VALUE (0x[0-9a-fA-F]+)', ccu)[1], 16)
    parents = re.search(r'gpu0_parents\[\] = \{([^}]+)\}', ccu)[1]
    assert re.findall(r'"([^"]+)"', parents).index('pll-gpu') == 0  # the reset source of the mux
    assert re.search(r'gpu0_bus_clk, "gpu0-gate",\s*"dcxo",\s*0x0B24, BIT\(0\)', ccu)
    assert re.search(r'\[RST_BUS_GPU0\]\s*= \{ 0x0b24, BIT\(16\) \}', ccu)
    assert re.search(r'r_ppu_clk, "r-ppu",\s*"dcxo",\s*0x01AC, BIT\(0\)', rccu)
    assert re.search(r'gpu0_ahb_gate_clk,[\s\S]{0,120}0x05C0,[\s\S]{0,80}BIT\(7\)', ccu)
    assert re.search(r'gpu0_mbus_gate_clk,[\s\S]{0,120}0x05E0,[\s\S]{0,80}BIT\(16\)', ccu)
    out, lock_en, ldo, en = bit['output'], bit['lock_enable'], bit['ldo_en'], bit['enable']
    pll_enable = [(1, 0, 0xe0, out, 0), (1, 0, 0xe0, 0, ldo), (1, 0, 0xe0, 0, en), (1, 0, 0xe0, 0, lock_en),
                  (1, 0, 0xe0, 0, out)]
    clocks_on = [(1, 0, 0xb24, 0, 1 << 16), (1, 0, 0xb24, 0, 1), (1, 0, 0xb20, 0, key | (1 << 31)),
                 (1, 0, 0xb20, 0, key)]
    set_rate = [(1, 0, 0xe0, out, 0), (1, 0, 0xe0, factors_mask, factors), (1, 0, 0xe0, lock_en, 0),
                (1, 0, 0xe0, 0, lock_en), (1, 0, 0xe0, 0, out)]
    clocks_off = [(1, 0, 0xb24, 1, 0), (1, 0, 0xb20, 1 << 31, key)]
    pll_disable = [(1, 0, 0xe0, out, 0), (1, 0, 0xe0, lock_en, 0), (1, 0, 0xe0, en, 0), (1, 0, 0xe0, ldo, 0)]
    assert [x for x in native if x[1] == 0] == (pll_enable + clocks_on + set_rate + clocks_off + pll_disable +
                                                [(1, 0, 0xb24, 1 << 16, 0)])
    # The vendor order in both directions: ON writes PWCR 0 (first power-on),
    # then every GPU clock write, then TOP5 ON and CORE6 policy 8
    # (sunxiPrePowerState); OFF writes CORE6 policy 0, then TOP5 OFF, then
    # every clock write (sunxiPostPowerState).
    top_on, top_off = native.index((1, 2, 0x5000, 0xf, 8)), native.index((1, 2, 0x5000, 0xf, 0))
    ccu_at = [i for i, x in enumerate(native) if x[1] == 0]
    pwcr, on8, off0 = (native.index((1, 3, 0x20, 0xffffffff, 0)), native.index((1, 3, 0, 0xffffffff, 8)),
                       native.index((1, 3, 0, 0xffffffff, 0)))
    on_ccu = [i for i in ccu_at if i < on8]
    off_ccu = [i for i in ccu_at if i > on8]
    assert pwcr < on_ccu[0] and on_ccu[-1] < top_on < on8 < off0 < top_off < off_ccu[0]
    assert 'reg = <0x0 0x02002000 0x0 0x2000>' in dtsi
    assert 'reg = <0x0 0x07010000 0x0 0x340>' in dtsi
    assert 'reg = <0x0 0x07060000 0x0 0xB000>' in dtsi
    # The operating point: 960 mV, opp-microvolt-vfdefault of the 1008 MHz
    # node, and the speed-bin row of include/a733_gpu_opp.h in node order.
    header = (Path(__file__).resolve().parents[1] / 'include/a733_gpu_opp.h').read_text()
    volts = re.findall(r'opp-microvolt-vf(default|[0-9a-f]{4}) = <(\d+)>', opp_node)
    row = [(0xffff if c == 'default' else int(c, 16), int(v)) for c, v in volts]
    assert row == [(int(c, 16), int(v)) for c, v in re.findall(r'\{(0x[0-9a-f]{4}), (\d+)\}', header)]
    assert len(row) == 22 and row[0] == (0xffff, 960000)
    assert re.search(r'opp-microvolt = <960000>', opp_node)
    assert re.search(r'#define A733_GPU_CORE_HZ %du' % rate, header)
    assert re.search(r'#define A733_GPU_SUPPLY_UV %du' % row[0][1], header)
    assert re.search(r'#define A733_GPU_DCDC4_SELECTOR 0x%xu' % ((row[0][1] - 500000) // 10000), header)
    assert re.search(r'&gpu\s*\{\s*gpu-supply = <&reg_dcdc4>', board)
    assert 'regulator-always-on' in block(board, r'reg_dcdc4: dcdc4\s*\{')
    (a.evidence / 'reference-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    (a.evidence / 'reference-command.json').write_text(json.dumps(command) + '\n')
    print('PASS pinned PCK functions, BXM CORE6 and GPU clock order, PLL_GPU0 bits and 1008 MHz factors, '
          'board supply and the 960 mV speed-bin row')


if __name__ == '__main__':
    main()
