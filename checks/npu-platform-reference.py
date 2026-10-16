#!/usr/bin/env python3
"""Extract the pinned NPU startup fields for the focused host model."""
import argparse
import hashlib
import json
from pathlib import Path
import re

p = argparse.ArgumentParser()
p.add_argument('--sources', type=Path, default=Path.home()/'.cache/sunstoneos/sources')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--gates-only', action='store_true', help='Check only the changed parent-gate semantics')
a = p.parse_args()
paths = {
    'ccu': a.sources/'board-kernel/ccu-sun60iw2.c',
    'soc': a.sources/'board-kernel/sun60iw2p1.dtsi',
    'board': a.sources/'board-kernel/sun60i-a733-orangepi-zero3w.dts',
    'opp': a.sources/'npu-platform/sun60iw2p1-cpu-vf.dtsi',
    'pck': a.sources/'gpu-power/drivers/pm_domain/pck600_domains.c',
    'domain': a.sources/'gpu-power/include/dt-bindings/power/sun60iw2-power.h',
    'r_ccu': a.sources/'gpu-power/drivers/clk/sunxi-ng/ccu-sun60iw2-r.c',
    'platform': a.sources/'npu/board-vip2-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/vip2/os/linux/platform/allwinner/vip_drv_device_platform.c',
    'os': a.sources/'npu/board-vip2-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/vip2/os/linux/vip_drv_os.c',
    'manual': a.sources/'camera-research/A733_User_Manual_V1.00.txt',
}
s = {k: v.read_text() for k, v in paths.items()}
if a.gates_only:
    manual = s['manual']
    sections = {}
    for name, start, end in [('npu_control', '4.1.6.255 ', '4.1.6.256 '),
                             ('npu_status', '4.1.6.264 ', '4.1.6.265 '),
                             ('peri_control', '4.1.6.249 ', '4.1.6.250 '),
                             ('peri_status', '4.1.6.258 ', '4.1.6.259 ')]:
        section = manual[manual.index(start):manual.index(end)]
        sections[name] = section
    assert re.search(r'NPUPLL_GATE_SW_CFG\..*?16\s+R/W\s+0x1.*?0: Disable\s+1: Enable', sections['npu_control'], re.S)
    assert re.search(r'NPUPLL_AUTO_GATE_EN\..*?0\s+R/W\s+0x1.*?0: Auto\s+1: No-Auto', sections['npu_control'], re.S)
    assert re.search(r'NPUPLL_GATE_STAT\..*?16\s+R\s+0x1.*?0: Disable\s+1: Enable', sections['npu_status'], re.S)
    for stem, low in [('PERI0PLL2X', 11), ('PERI0_800M', 10), ('PERI0_600M', 9),
                      ('PERI0_480M', 7), ('PERI0_400M', 1)]:
        assert re.search(stem + r'_AUTO_GATE_EN\..*?\b' + str(low) + r'\s+R/W\s+0x1.*?0: Auto\s+1: No-Auto', sections['peri_control'], re.S)
        assert re.search(stem + r'_GATE_SW_CFG\..*?\b' + str(low + 16) + r'\s+R/W\s+0x1', sections['peri_control'], re.S)
        assert re.search(stem + r'_GATE_STAT\..*?\b' + str(low + 16) + r'\s+R\s+0x1', sections['peri_status'], re.S)
    for stem, low in [('PERI0_480M', 8), ('PERI0_400M', 2)]:
        assert re.search(stem + r'_AUTO_GATE_EN_ALL\..*?\b' + str(low) + r'\s+R/W\s+0x1.*?0: Auto\s+1: No-Auto', sections['peri_control'], re.S)
        assert re.search(stem + r'_GATE_ALL_CFG\..*?\b' + str(low + 16) + r'\s+R/W\s+0x1', sections['peri_control'], re.S)
        assert re.search(stem + r'_GATE_ALL_STAT\..*?\b' + str(low + 16) + r'\s+R\s+0x1', sections['peri_status'], re.S)
    a.output.mkdir(parents=True, exist_ok=True)
    record = {'manual_sha256': hashlib.sha256(paths['manual'].read_bytes()).hexdigest(),
              'sections': list(sections), 'npu_control_mask': '0x10001', 'npu_status_mask': '0x10000',
              'peri_no_auto_masks': {'600': '0xa00', '400': '0x806', '800': '0x400', '480': '0x180'},
              'rule': 'Selected low No-Auto bits and their corresponding high software/status bits must all be one.'}
    (a.output/'gate-reference.json').write_text(json.dumps(record, indent=2)+'\n')
    print('Primary manual NPU/PERI0 No-Auto, software-enable and effective-status fields match.')
    raise SystemExit(0)
def match(key, pattern):
    m = re.search(pattern, s[key], re.S)
    assert m, (key, pattern)
    return m

def number(x):
    return int(x, 0)

clock = match('ccu', r'SUNXI_CCU_M_WITH_MUX_GATE\(npu_clk,.*?npu_parents,\s*(0x[0-9A-F]+),\s*(\d+),\s*(\d+),.*?\n\s*(\d+),\s*(\d+),.*?BIT\((\d+)\)')
reg, shift, width, mux_shift, mux_width, gate = map(number, clock.groups())
assert (shift, width, mux_shift, mux_width, gate) == (0, 5, 24, 3, 31)
parents = match('ccu', r'npu_parents\[\] = \{([^}]+)').group(1)
assert re.findall(r'"([^"]+)"', parents) == ['pll-npu', 'pll-peri0-800m', 'pll-peri0-600m', 'pll-peri0-480m', 'pll-ve0', 'pll-ve1', 'pll-de-3x']
reset = [tuple(map(number, match('ccu', r'\[RST_BUS_NPU_'+n+r'\]\s*=\s*\{\s*(0x[0-9a-f]+), BIT\((\d+)\)').groups())) for n in ['AHB', 'AXI', 'CORE']]
assert len({v[0] for v in reset}) == 1
bus, = map(number, match('ccu', r'SUNXI_CCU_GATE\(npu_bus_clk,.*?(0x[0-9A-F]+), BIT\(0\)').groups())
assert bus == reset[0][0]
pll = match('ccu', r'static struct ccu_nm pll_npu_clk = \{(.*?)\n\};').group(1)
for fact in ['BIT(27)', 'BIT(28)', 'BIT(29)', 'BIT(30)', 'BIT(31)', '_SUNXI_CCU_MULT_MIN_MAX(8, 8, 53, 105)', '_SUNXI_CCU_DIV(20, 3)', '0x02A0', '0x02A8', '0x02AC', '"pll-ref"']:
    assert fact in pll, fact
opp = match('opp', r'npu_opp_table_492: opp-492 \{(.*?)\n\s*\};').group(1)
hz = number(re.search(r'opp-hz = /bits/ 64 <(\d+)>', opp).group(1))
volts = re.findall(r'opp-microvolt(?:-26m)?-vf[0-9a-f]+ = <(\d+)>', opp)
assert len(volts) == 26 and set(volts) == {'800000'}
assert 'npu-supply = <&reg_dcdc2>' in s['board']
assert 'vdd-edp-supply = <&reg_dcdc2>' in s['board']
assert 'dcdc2-supply = <&reg_dcdc2>' in s['board']
node = match('soc', r'npu: npu@3600000 \{(.*?)\n\s*\};').group(1)
assert '<GIC_SPI 65 IRQ_TYPE_LEVEL_HIGH>' in node and '<&pd SUN60IW2_PCK_NPU>' in node
assert '"npu_rst", "npu_axi_rst", "npu_ahb_rst"' in node
index = number(match('domain', r'#define SUN60IW2_PCK_NPU\s+(\d+)').group(1))
domain = match('pck', r'\[SUN60IW2_PCK_NPU\] = DOMAIN\(SUN60IW2_PCK_NPU,\s*([^\)]+)').group(1)
values = [number(v.strip()) for v in domain.split(',')]
assert len(values) == 6
pck_offsets = [number(match('pck', r'sun60iw2_pck600_pmu = \{.*?\.'+field+r' = (0x[0-9a-f]+)').group(1)) for field in ['device_ctrl0_delay_offset', 'device_ctrl1_delay_offset', 'logic_power_switch0_delay_offset', 'logic_power_switch1_delay_offset', 'off2on_delay_offset']]
init = match('platform', r'static vip_status_e npu_clk_init\(void\)\s*\{(.*?)\n\}').group(1)
ordered = ['reset_control_deassert(aw_driver.hrst)', 'reset_control_deassert(aw_driver.arst)', 'reset_control_deassert(aw_driver.rst)', 'clk_prepare_enable(aw_driver.ahb_gate)', 'set_clk_rate()', 'clk_prepare_enable(aw_driver.mbus_gate)', 'clk_prepare_enable(aw_driver.bus)', 'clk_prepare_enable(aw_driver.mclk)']
positions = [init.index(t) for t in ordered]
assert positions == sorted(positions)
power = match('platform', r'static vip_status_e set_vip_power_clk\(.*?case CLK_ON:(.*?)break;').group(1)
assert power.index('npu_clk_init()') < power.index('pm_runtime_get_sync(dev)')
assert 'vipdrv_os_delay(2);' in s['platform']
assert 'Delay execution of the current thread for a number of milliseconds.' in s['os']
manual_pdf = a.sources/'camera-research/A733_User_Manual_V1.00.pdf'
assert hashlib.sha256(manual_pdf.read_bytes()).hexdigest() == 'cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d'
for start, end in [('4.1.6.56', '4.1.6.57'), ('4.1.6.6 ', '4.1.6.7 ')]:
    block = s['manual'].split(start, 1)[1].split(end, 1)[0]
    assert re.search(r'PLL Input Div M1\.?\s+1\s+R/W\s+0x0\s+M1=PLL_INPUT_DIV \+ 1', block)
reset_block = s['manual'].split('4.1.6.120', 1)[1].split('4.1.6.121', 1)[0]
assert re.search(r'NPU SRAM Reset\s+19\s+R/W\s+0x1', reset_block)
cm = s['manual'].split('4.1.6.271', 1)[1].split('4.1.6.272', 1)[0]
assert re.search(r'CM NPU module mode\s+0\s+R/W\s+0x0', cm)
ccu = 0x02002000
pck = 0x07060000 + index*0x1000
# 984 MHz inherited parent gives divisor 2. Upper sentinel bits come from the host model.
addresses = [ccu+v[0] for v in reset] + [ccu+reg, ccu+bus, ccu+reg] + [pck+v for v in pck_offsets] + [pck]
reset_bits = [1 << v[1] for v in reset]
a.output.mkdir(parents=True, exist_ok=True)
header = '#define SOURCE_HZ '+str(hz)+'u\n#define SOURCE_UV '+volts[0]+'u\n'
header += 'static const unsigned source_addresses[]={' + ','.join(hex(v)+'u' for v in addresses) + '};\n'
header += 'static const unsigned source_reset_bits[]={' + ','.join(hex(v)+'u' for v in reset_bits) + '};\n'
header += 'static const unsigned source_pck_values[]={' + ','.join(hex(v)+'u' for v in values[:5]) + '};\n'
(a.output/'npu-platform-reference.h').write_text(header)
record = {'inputs': {k: {'path': str(v), 'sha256': hashlib.sha256(v.read_bytes()).hexdigest()} for k,v in paths.items()}, 'manual_pdf_sha256': hashlib.sha256(manual_pdf.read_bytes()).hexdigest(), 'module': reg, 'bus': bus, 'domain': index, 'irq': 97, 'profile_hz': hz, 'profile_uv': int(volts[0]), 'expected_write_addresses': addresses}
(a.output/'reference.json').write_text(json.dumps(record, indent=2)+'\n')
print('Pinned NPU clock, reset, power, operating point and startup order extracted.')
