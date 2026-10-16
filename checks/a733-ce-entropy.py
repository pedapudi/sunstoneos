#!/usr/bin/env python3
"""Verify source basis and native composition for changed CE service behavior."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--sources', type=Path, required=True)
p.add_argument('--manual', type=Path, required=True)
p.add_argument('--uboot', type=Path, required=True)
p.add_argument('--native-object', type=Path, required=True)
p.add_argument('--objdump', default='/usr/lib/llvm-21/bin/llvm-objdump')
a = p.parse_args()
assert hashlib.sha256(a.manual.read_bytes()).hexdigest() == '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2'
assert hashlib.sha256(a.uboot.read_bytes()).hexdigest() == '5e07db37ee6d341b996c018881c7c2d8dadb16d0d77667f8b5aa7b295fe6a010'
proc = a.sources / 'v5/sunxi_ce_proc.c'
row = next(r for r in json.loads((a.sources / 'sources.json').read_text()) if Path(r['path']) == proc)
assert hashlib.sha256(proc.read_bytes()).hexdigest() == row['sha256']
manual = ' '.join(a.manual.read_text().split())
for text in ['0x0090 Security CE Repetition Count Test C Register',
             '0x0094 Security CE Adaptive Proportion Test C Register',
             '0x0098 Security CE Adaptive Proportion Test W Period Register',
             '0: no wait, start next test directly',
             '1: adap_pro healthy test enable', '1: rep_cnt healthy test enable',
             '1024: noise source is binary', '512: noise source is not binary',
             'Read 0x1: Task completed successfully', 'Write 0x3 to clear it.']:
    assert text in manual, text
# Successful-task memory reuse is corroboration; source timeout/error handling
# is not adopted, and this check makes no global drain or entropy-quality claim.
rng = proc.read_text().split('static int ss_rng_start(', 1)[1].split('int ss_rng_get_random(', 1)[0]
for text in ['&ss_dev->flows[flow].task', 'wait_for_completion_timeout',
             'dma_unmap_single', 'memcpy(rdata, buf, dlen);', 'kfree(buf);']:
    assert text in rng, text
uboot = a.uboot.read_text().split('int sunxi_trng_gen(', 1)[1].split('int sunxi_create_rssk(', 1)[0]
for text in ['ss_wait_finish(CHANNEL_2);', 'ss_pending_clear(CHANNEL_2);',
             'ss_check_err(CHANNEL_2)', 'invalidate_dcache_range', 'memcpy(rng_buf, p_sign, RNG_LEN);']:
    assert text in uboot, text
assembly = subprocess.check_output([a.objdump, '-dr', str(a.native_object)], text=True)
constructor = assembly.split('<a733_ce_session_native_init>:', 1)[1]
for symbol in ['a733_memory_reservations_init', 'device_memory_acquire', 'a733_ce_session_bind',
               'a733_ce_native_read', 'a733_ce_native_write', 'a733_ce_native_sync', 'device_memory_release']:
    assert symbol in constructor, symbol
write = assembly.split('<a733_ce_native_write>:', 1)[1].split('<a733_ce_native_sync>:', 1)[0]
for instruction in ['dsb\tsy', 'ldr\tw', 'str\tw']:
    assert instruction in write, instruction
print('PASS source-defined health controls and same-reservation completion reuse; native singleton-pool/controller composition; no startup or entropy qualification')
