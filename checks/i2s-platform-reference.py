#!/usr/bin/env python3
"""Decode observed cold-start clocks with fields from pinned A733 CCU sources.

Usage: i2s-platform-reference.py SOURCE_DIRECTORY PLATFORM_LOG
Reads local cache only. The log is produced by checks/i2s-platform.c.
"""
import hashlib
import json
from pathlib import Path
import re
import sys

cache, log = map(Path, sys.argv[1:])
expected = {
    "ccu-sun60iw2.c": "1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77",
    "ccu_gate.c": "3d8c88d8b00d83b64819a88e6cacaa80eef941dd4d1c164d00238ba29054cd51",
    "ccu_mult.h": "1ecf95b5e4d35807b4b8fa5dca3b454af5b0924991fcee276e730b8e528d7c79",
    "ccu_div.h": "a379fcb476803bc3fda7143f79d18cfcecb4320bc9f04facbea2a5b0d7c1d17e",
}
sources = {}
for name, digest in expected.items():
    raw = (cache/name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    sources[name] = raw.decode()
ccu = re.sub(r"/\*.*?\*/", "", sources["ccu-sun60iw2.c"], flags=re.S)
audio = re.search(r"static struct ccu_nm pll_audio1_clk = \{(.*?)\n\};", ccu, re.S)[1]
shift, width, minimum, maximum = map(int, re.search(
    r"_SUNXI_CCU_MULT_MIN_MAX\((\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)", audio).groups())
assert "_SUNXI_CCU_MULT_OFFSET_MIN_MAX(_shift, _width, 1, _min, _max)" in sources["ccu_mult.h"]
assert "_SUNXI_CCU_DIV_OFFSET_MAX_MIN_FLAGS(_shift, _width, 1, _max, 1,  _flags)" in sources["ccu_div.h"]
div_shift, div_width = map(int, re.search(
    r'SUNXI_CCU_M\(pll_audio1_div5_clk,\s*"[^"]+",\s*"[^"]+",\s*0x0280,\s*(\d+),\s*(\d+)', ccu).groups())
mod_shift, mod_width, mux_shift, mux_width = map(int, re.search(
    r'SUNXI_CCU_M_WITH_MUX_GATE\(i2spcm0_clk,\s*"[^"]+",\s*i2spcm_parents,\s*0x1200,\s*'
    r'(\d+),\s*(\d+),\s*(\d+),\s*(\d+)', ccu).groups())
parents = re.findall(r'"([^"]+)"', re.search(r'i2spcm_parents\[\] = \{([^}]+)\}', ccu)[1])
observed = [json.loads(line) for line in log.read_text().splitlines() if line.startswith("{")]
assert len(observed) == 1
v = observed[0]
field = lambda value, start, bits: (value >> start) & ((1 << bits)-1)
n = field(v["audio"], shift, width) + 1
div = field(v["audio"], div_shift, div_width) + 1
m = field(v["module"], mod_shift, mod_width) + 1
assert minimum <= n <= maximum
assert parents[field(v["module"], mux_shift, mux_width)] == "pll-audio1-div5"
assert 24000000*n == 3072000000
assert 24000000*n//div == 614400000
assert 24000000*n//div//m == 24576000
# Compare enabled control transitions against the source helper's ordering.
helper = sources["ccu_gate.c"].split("int ccu_pll_gate_helper_enable", 1)[1].split("int ccu_gate_helper_enable", 1)[0]
order = re.findall(r"writel\(reg \| (ldo_en|gate|lock_enable|output),", helper)
assert order == ["ldo_en", "gate", "lock_enable", "output"]
positions = {}
for source_name, member in (("ldo_en", "ldo_en"), ("gate", "enable"),
                            ("lock_enable", "lock_enable"), ("output", "output")):
    positions[source_name] = int(re.search(r"\."+member+r"\s*= BIT\((\d+)\)", audio)[1])
transitions = []
old = 0
mask = sum(1 << bit for bit in positions.values())
for value in v["pll_writes"]:
    changed = (value ^ old) & mask
    for name, bit in positions.items():
        if changed & (1 << bit):
            assert value & (1 << bit)
            transitions.append(name)
    old = value
assert transitions == order
print("Pinned CCU fields decode observed AUDIO1=3.072 GHz, DIV5=614.4 MHz, I2S0=24.576 MHz; PLL enable order agrees")
