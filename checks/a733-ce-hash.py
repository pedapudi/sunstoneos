#!/usr/bin/env python3
"""Authenticate streaming SHA references and compare modeled hardware with hashlib."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--sources', type=Path, required=True)
p.add_argument('--manual', type=Path, required=True)
p.add_argument('--model-log', type=Path, required=True)
p.add_argument('--native-object', type=Path, required=True)
p.add_argument('--objdump', default='/usr/lib/llvm-21/bin/llvm-objdump')
a = p.parse_args()
manifest = json.loads((a.sources / 'sources.json').read_text())
files = ['v5/sunxi_ce_proc.c', 'v5/sunxi_ce_reg.c', 'v5/sunxi_ce_reg.h', 'sunxi_ce_cdev.h', 'sunxi_ce_proc_comm.c']
source = {}
for name in files:
    path = a.sources / name
    row = next(r for r in manifest if Path(r['path']) == path)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256'], name
    source[name] = path.read_text()
assert hashlib.sha256(a.manual.read_bytes()).hexdigest() == '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2'
manual = ' '.join(a.manual.read_text().split())
for text in ['The hash algorithm of the CE supports block-aligned total length of the input data (padded by software)',
             'IVE [8] IV mode Enable', 'LPKG [12] Last Package flag',
             'data length in current task, data len[39:32]',
             '50 data [31:0] Total data len[63:32]',
             'Read 0x1: Task completed successfully']:
    assert text in manual, text
proc = source['v5/sunxi_ce_proc.c'].split('u32 ss_hash_start(', 1)[1].split('void ss_load_iv(', 1)[0]
for text in ['if (ctx->npackets++)', 'ss_hash_iv_mode_set(1, task);',
             'ss_hash_data_len_set(((len - (len % blk_size)) * 8), task);',
             'ss_hash_total_data_len_set(total_data_len, task);',
             'memcpy(ctx->md, digest, ctx->md_size);']:
    assert text in proc, text
reg = source['v5/sunxi_ce_reg.c']
for text in ['task->reserved[2] = len & 0xffffffff;', 'task->reserved[1] = len >> 32;',
             'task->comm_ctl |= mode << CE_CTL_IV_MODE_SHIFT;', 'memcpy(dst, src, size);']:
    assert text in reg, text
assert '#define CE_CTL_IV_MODE_SHIFT\t\t8' in source['v5/sunxi_ce_reg.h']
cdev = source['sunxi_ce_cdev.h']
assert '#if IS_ENABLED(CONFIG_ARCH_SUN55IW3)\n#define SS_LPKG_KEEP_ON' in cdev
assert 'IS_ENABLED(CONFIG_ARCH_SUN60IW2)' in cdev.split('#define SS_TOTAL_DATALEN_ENABLE', 1)[0][-240:]
assert 'p[n] = 0x80;' in source['sunxi_ce_proc_comm.c']
records = [line.split() for line in a.model_log.read_text().splitlines() if line.startswith('digest ')]
lengths = [0, 3, 55, 56, 63, 64, 65, 2040, 2048, 5003]
assert [int(row[1]) for row in records] == lengths
for row in records:
    n = int(row[1])
    expected = hashlib.sha256(bytes((i * 37 + 11) & 255 for i in range(n))).hexdigest()
    assert row[2] == expected, (n, row[2], expected)
assembly = subprocess.check_output([a.objdump, '-dr', str(a.native_object)], text=True)
def body(function):
    text = assembly.split('<' + function + '>:', 1)[1]
    return text.split('\n00000000', 1)[0] if '\n00000000' in text else text
for symbol in ['a733_ce_session_native_claim', 'a733_ce_session_native_abort',
               'a733_memory_reservations_init', 'device_memory_acquire', 'a733_ce_session_bind',
               'a733_ce_native_read', 'a733_ce_native_write', 'a733_ce_native_sync']:
    assert symbol in body('a733_ce_session_native_init'), symbol
# The entropy owner reuses the session constructor instead of a second copy.
entropy = body('a733_ce_entropy_native_init')
for symbol in ['a733_memory_reservations_init', 'a733_ce_session_native_init',
               'a733_ce_entropy_attach', 'a733_ce_session_retain']:
    assert symbol in entropy, symbol
for symbol in ['device_memory_acquire', 'a733_ce_session_bind', 'a733_ce_entropy_bind']:
    assert symbol not in entropy, symbol
# Cache maintenance goes through the shared CWG/EL/MMU-checked helper.
sync = body('a733_ce_native_sync')
assert 'aarch64_device_cache' in sync
assert '\tdc\t' not in assembly
assert 'dsb\tsy' in body('a733_ce_native_read')
print('PASS pinned SHA streaming descriptors, independent hashlib digests and actual native singleton/session/cache composition; no physical qualification')
