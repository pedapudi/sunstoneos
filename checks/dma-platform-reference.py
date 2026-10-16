#!/usr/bin/env python3
"""Compare the modeled startup trace with pinned A733 clock/DMA source fields.

Usage: dma-platform-reference.py SOURCE_DIRECTORY PLATFORM_LOG
Reads cached sources only; performs no hardware access or downloads.
"""
import hashlib
import json
from pathlib import Path
import re
import sys

cache, log = map(Path, sys.argv[1:])
expected = {
    "ccu-sun60iw2.c": "1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77",
    "sunxi-dma.c": "d60e6598835ddf16b1f2374f746abd82da507dfefeb16edade9a0f7021b3d3bb",
    "sun60iw2p1.dtsi": "1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa",
    "ccu_mult.h": "1ecf95b5e4d35807b4b8fa5dca3b454af5b0924991fcee276e730b8e528d7c79",
    "ccu_div.h": "a379fcb476803bc3fda7143f79d18cfcecb4320bc9f04facbea2a5b0d7c1d17e",
    "ccu_gate.h": "02777af3356f168356d478ac8bcd9710acac97cc2a75b5354302c2787f9eac19",
}
sources = {}
for name, digest in expected.items():
    raw = (cache / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    sources[name] = raw.decode()
ccu = re.sub(r"/\*.*?\*/", "", sources["ccu-sun60iw2.c"], flags=re.S)
dma = sources["sunxi-dma.c"]
dts = sources["sun60iw2p1.dtsi"]
record, = [json.loads(line) for line in log.read_text().splitlines() if line.startswith("{")]
field = lambda word, start, bits: (word >> start) & ((1 << bits) - 1)
assert "_SUNXI_CCU_MULT_OFFSET_MIN_MAX(_shift, _width, 1, _min, _max)" in sources["ccu_mult.h"]
assert "_SUNXI_CCU_DIV_OFFSET_MAX_MIN_FLAGS(_shift, _width, 1, _max, 1,  _flags)" in sources["ccu_div.h"]

def multiply(name, word):
    body = re.search(r"static struct ccu_nm " + name + r"_clk = \{(.*?)\n\};", ccu, re.S)[1]
    shift, width, minimum, maximum = map(int, re.search(
        r"_SUNXI_CCU_MULT_MIN_MAX\((\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)", body).groups())
    n = field(word, shift, width) + 1
    assert minimum <= n <= maximum
    return n, body

ref_n, ref_body = multiply("pll_ref", record["ref"])
shift, width = map(int, re.search(r"\.m\s*= _SUNXI_CCU_DIV\((\d+),\s*(\d+)\)", ref_body).groups())
ref_hz = 24000000 * ref_n // (field(record["ref"], shift, width) + 1)
assert ref_hz == 24000000
peri_n, _ = multiply("pll_peri0", record["peri"])
rates = {"sys24M": 24000000, "pll-peri0": ref_hz * peri_n}
for suffix in ("2x", "800m", "480m"):
    parent, shift, width = re.search(
        r'SUNXI_CCU_M\(pll_peri0_' + suffix + r'_clk,\s*"[^"]+",\s*"([^"]+)",\s*0x00A0,\s*(\d+),\s*(\d+)', ccu).groups()
    rates["pll-peri0-" + suffix] = rates[parent] // (field(record["peri"], int(shift), int(width)) + 1)
for suffix in ("600m", "400m"):
    parent, div, mul = re.search(
        r'CLK_FIXED_FACTOR\(pll_peri0_' + suffix + r'_clk,\s*"[^"]+",\s*"([^"]+)",\s*(\d+),\s*(\d+)', ccu).groups()
    rates["pll-peri0-" + suffix] = rates[parent] * int(mul) // int(div)
for i, name in enumerate(("ahb", "nsi", "mbus")):
    parent_name, offset, mshift, mwidth, muxshift, muxwidth = re.search(
        r'SUNXI_CCU_M_WITH_MUX(?:_GATE_KEY)?\(' + name + r'_clk,\s*"[^"]+",\s*(\w+),\s*(0x[0-9A-Fa-f]+),\s*'
        r'(\d+),\s*(\d+),\s*(\d+),\s*(\d+)', ccu).groups()
    parents = re.findall(r'"([^"]+)"', re.search(parent_name + r'\[\] = \{([^}]+)\}', ccu)[1])
    word = record["fabric"][i]
    parent = parents[field(word, int(muxshift), int(muxwidth))]
    rate = rates[parent] // (field(word, int(mshift), int(mwidth)) + 1)
    assert rate == record["rates"][i], (name, rate)
    assert int(offset, 16) == (0x500, 0x580, 0x588)[i]

node = re.search(r"dma: dma-controller@4601000 \{(.*?)\n\s*\};", dts, re.S)[1]
assert '"allwinner,dma-v106"' in node
assert "CLK_DMA0" in node and "CLK_MBUS_DMA0" in node and "RST_BUS_DMA0" in node
base = int(re.search(r"reg = <0x0 (0x\w+)", node)[1], 16)
assert int(re.search(r"dma-channels = <(\d+)>", node)[1]) == 16
bus_offset, bus_bit = re.search(r'SUNXI_CCU_GATE\(dma0_clk,\s*"[^"]+",\s*"[^"]+",\s*(0x\w+),\s*BIT\((\d+)\)', ccu).groups()
reset_offset, reset_bit = re.search(r'\[RST_BUS_DMA0\]\s*=\s*\{\s*(0x\w+),\s*BIT\((\d+)\)', ccu).groups()
gate_offset, gate_bit = re.search(r'SUNXI_CCU_GATE_WITH_KEY\(dma0_mbus_clk,\s*"[^"]+",\s*"[^"]+",\s*(0x\w+),\s*MBUS_GATE_KEY_VALUE,\s*BIT\((\d+)\)', ccu).groups()
key = int(re.search(r"#define MBUS_GATE_KEY_VALUE\s+(0x\w+)", ccu)[1], 16)
critical_bits = re.findall(r'0x05E4,\s*MBUS_GATE_KEY_VALUE,\s*BIT\((\d+)\), CLK_IS_CRITICAL', ccu)
assert key == sum(1 << int(bit) for bit in critical_bits)
assert re.search(r"\.key_value\s*= _key_value", sources["ccu_gate.h"])
assert bus_offset == reset_offset
bus_address = 0x02002000 + int(bus_offset, 16)
gate_address = 0x02002000 + int(gate_offset, 16)
auto_offset = int(re.search(r"#define SUNXI_H3_DMA_GATE\s+(0x\w+)", dma)[1], 16)
auto_value = int(re.search(r"#define SUNXI_H3_DMA_GATE_ENABLE\s+(0x\w+)", dma)[1], 16)
cpu_offset = int(re.search(r"#define DMA_IRQ_CPU_EN_REG\s+(0x\w+)", dma)[1], 16)
cpu_mask = int(re.search(r"#define DMA_IRQ_CPU_ENABLE_MASK\s+(0x\w+)", dma)[1], 16)
writes = record["writes"]
assert [a for a, _ in writes] == [bus_address, bus_address, gate_address, base+auto_offset, base+cpu_offset]
assert writes[0][1] & (1 << int(reset_bit)) and not writes[0][1] & (1 << int(bus_bit))
assert writes[1][1] & (1 << int(bus_bit))
assert writes[2][1] & key == key and writes[2][1] & (1 << int(gate_bit))
assert writes[3][1] == auto_value and writes[4][1] & 0xffff == cpu_mask
probe = dma[dma.index("if (sdc->cfg->has_rst)"):]
assert probe.index("reset_control_deassert") < probe.index("clk_prepare_enable(sdc->clk)") < probe.index("clk_prepare_enable(sdc->clk_mbus)")
assert "usleep_range(20, 25)" in probe
print("Pinned fields match fabric rates, reset/bus/MBUS order, critical gates and DMA0 routing trace")
