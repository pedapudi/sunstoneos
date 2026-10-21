#!/usr/bin/env python3
"""Check pinned AES descriptors, independent cipher output and native cache code."""
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
p.add_argument('--openssl', default='/usr/bin/openssl')
a = p.parse_args()
manifest = json.loads((a.sources / 'sources.json').read_text())
source = {}
for name in ['v5/sunxi_ce_proc.c', 'v5/sunxi_ce_reg.c', 'v5/sunxi_ce_reg.h', 'sunxi_ce_cdev.h']:
    path = a.sources / name
    row = next(r for r in manifest if Path(r['path']) == path)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256'], name
    source[name] = path.read_text()
assert hashlib.sha256(a.manual.read_bytes()).hexdigest() == '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2'
manual = ' '.join(a.manual.read_text().split())
for text in ['19.1.4.15 Task Descriptor of Other Algorithms', '0: Encryption 1: Decryption',
             '0: Select input CE_KEYx (Normal Mode)', '0x0: Electronic Code Book (ECB) mode',
             '0: 128-bits 1: 192-bits 2: 256-bits',
             'src/dst sgX size field indicates size for each sg respectively (by byte)',
             'Read 0x1: Task completed successfully']:
    assert text in manual, text
proc = source['v5/sunxi_ce_proc.c'].split('static int ss_aes_start(', 1)[1].split('/* verify the key_len */', 1)[0]
for text in ['ss_key_set(ctx->key, ctx->key_size, task);',
             'ss_data_len_set(DIV_ROUND_UP(src_len, align_size) * align_size, task);',
             'ss_ctrl_start(task, req_ctx->type, req_ctx->mode);',
             'wait_for_completion_timeout', 'virt_to_phys(ctx->key), ctx->key_size, DMA_TO_DEVICE);']:
    assert text in proc, text
assert proc.index('wait_for_completion_timeout') < proc.index('virt_to_phys(ctx->key), ctx->key_size, DMA_TO_DEVICE);')
reg = source['v5/sunxi_ce_reg.c']
assert 'ss_writel(CE_REG_TLR, 0x1 << CE_REG_TLR_SYMM_TYPE_SHIFT);' in reg
for text in ['#define CE_REG_TLR_SYMM_TYPE_SHIFT\t0', '#define SS_AES_MODE_ECB\t\t\t\t0',
             '#define CE_AES_KEY_SIZE_128\t\t\t0']:
    assert text in source['v5/sunxi_ce_reg.h'], text
cdev = source['sunxi_ce_cdev.h']
for text in ['u32 chan_id;', 'u32 sym_ctl;', 'u8 key_addr[5];', 'u8 ctr_addr[5];', 'u8 pad2[2];']:
    assert text in cdev, text
# NIST SP 800-38A Appendix F.1.1/F.1.2. These fixed vectors exercise complete
# blocks; repeated blocks cross task boundaries without defining a new mode.
key = '2b7e151628aed2a6abf7158809cf4f3c'
plain = bytes.fromhex('6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710')
cipher = bytes.fromhex('3ad77bb40d7a3660a89ecaf32466ef97f5d3d58503b9699de785895a96fdbaaf43b1cd7f598ece23881b00e3ed0306887b0c785e27e8ad3f8223207104725dd4')
records = [line.split() for line in a.model_log.read_text().splitlines() if line.startswith('cipher ')]
assert [(int(r[1]), int(r[2])) for r in records] == [(0,16),(0,64),(0,1024),(0,1040),(0,2048),(1,64),(1,2048)]
for _, direction, length, result in records:
    decrypt, n = int(direction), int(length)
    input_bytes = ((cipher if decrypt else plain) * ((n+63)//64))[:n]
    expected = ((plain if decrypt else cipher) * ((n+63)//64))[:n]
    command = [a.openssl, 'enc', '-aes-128-ecb', '-K', key, '-nopad', '-nosalt']
    if decrypt:
        command.append('-d')
    independent = subprocess.check_output(command, input=input_bytes)
    assert bytes.fromhex(result) == expected == independent, (decrypt, n)
sha = [line.split() for line in a.model_log.read_text().splitlines() if line.startswith('digest ')]
assert sha == [['digest', '0', hashlib.sha256(b'').hexdigest()]]
assembly = subprocess.check_output([a.objdump, '-dr', str(a.native_object)], text=True)
# Cache maintenance goes through the shared CWG/EL/MMU-checked helper.
assert 'aarch64_device_cache' in assembly and '\tdc\t' not in assembly and 'dsb\tsy' in assembly
for symbol in ['a733_ce_session_native_claim', 'a733_memory_reservations_init',
               'device_memory_acquire', 'a733_ce_session_bind']:
    assert symbol in assembly, symbol
print('PASS pinned symmetric descriptors, NIST/OpenSSL AES answers, shared SHA digest and native cache/session composition; no physical qualification')
