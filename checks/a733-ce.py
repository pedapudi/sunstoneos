#!/usr/bin/env python3
"""Check pinned A733 CE facts and the compiled native MMIO/cache instructions."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--sources', type=Path, required=True)
p.add_argument('--manual', type=Path, required=True)
p.add_argument('--ccu', type=Path, required=True)
p.add_argument('--rtc', type=Path, required=True)
p.add_argument('--object', type=Path, required=True)
p.add_argument('--objdump', default='/usr/lib/llvm-21/bin/llvm-objdump')
a = p.parse_args()
for row in json.loads((a.sources / 'sources.json').read_text()):
    assert hashlib.sha256(Path(row['path']).read_bytes()).hexdigest() == row['sha256'], row['path']
assert hashlib.sha256(a.manual.read_bytes()).hexdigest() == '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2'
assert hashlib.sha256(a.ccu.read_bytes()).hexdigest() == '1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77'
reg = (a.sources / 'v5/sunxi_ce_reg.h').read_text()
source = (a.sources / 'v5/sunxi_ce_reg.c').read_text()
proc = (a.sources / 'v5/sunxi_ce_proc.c').read_text()
make = (a.sources / 'Makefile').read_text()
manual = ' '.join(a.manual.read_text().split())
ccu = ' '.join(a.ccu.read_text().split())
rtc = ' '.join(a.rtc.read_text().split())
assert 'ifdef CONFIG_ARCH_SUN60IW2\n\tAW_CE_VER = v5' in make
for name, value in [('CE_REG_TSK0','0x00'),('CE_REG_TSK1','0x04'),('CE_REG_ISR','0x0C'),
                    ('CE_REG_TLR','0x10'),('CE_REG_ERR','0x18'),('CE_REG_LPC','0xD0'),
                    ('CE_REG_TLR_HASH_RBG_TYPE_SHIFT','10'),('SS_METHOD_TRNG','2')]:
    assert any(line.split()[:3] == ['#define', name, value] for line in reg.splitlines()), name
assert 'task->ce_sg[0].dst_len = rng_len;' in proc
assert 'ss_rng_method_set(SS_METHOD_SHA256, SS_METHOD_TRNG, task);' in proc
assert 'ss_writel(CE_REG_TLR, 0x1 << CE_REG_TLR_HASH_RBG_TYPE_SHIFT);' in source
for text in ['0x04603000', '0x04603800', 'Read 0x1: Task completed successfully',
             'Read 0x2: task fail and stop', 'Write 0x3 to clear it.',
             'The Length (by bytes) of output Datax.', '0x43: TRNG rep_cnt',
             '0x4c: TRNG adap_pro', '0x4f: TRNG rep_cnt and adap_pro',
             '0x0090 Security CE Repetition Count Test C Register',
             '0x009c Security CE Healthy Test Control Register',
             '1024: noise source is binary', '512: noise source is not binary',
             'ce_clk2m_rc clock is derived from a one-eight frequency division of RC16M.']:
    assert text in manual, text
assert 'ce_parents[] = { "sys24M", "pll-peri0-400m", "pll-peri0-600m" }' in ccu
assert '"ce-sys", "dcxo", 0x0AC4, BIT(1)' in ccu
assert '[RST_BUS_CE_SY] = { 0x0ac4, BIT(17) }' in ccu
assert '"ce-mclk", "dcxo", 0x05E4, MBUS_GATE_KEY_VALUE, BIT(2)' in ccu
assert 'iosc_clk, "iosc", "rc-16m", 0x160, BIT(0)' in rtc
assembly = subprocess.check_output([a.objdump, '-dr', str(a.object)], text=True)
# Ordered MMIO stays local; cache maintenance uses the shared helper, which
# also refuses an unknown writeback granule, non-EL1 callers and a disabled MMU.
assert 'dsb\tsy' in assembly and 'aarch64_device_cache' in assembly and 'dc\t' not in assembly
print('PASS pinned A733 descriptor, status, health and clock source references; native MMIO and shared cache maintenance')
