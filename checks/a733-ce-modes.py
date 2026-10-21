#!/usr/bin/env python3
"""Check CE hash methods, HMAC and AES modes against pinned sources and independent answers.

The script authenticates the pinned A733 manual and CE v5 sources, writes the
request file for the a733-ce-modes model, runs it and compares every result
with hashlib, hmac, OpenSSL and published NIST vectors. GCM answers also come
from an independent Python GHASH over OpenSSL AES blocks.
"""
import argparse
import hashlib
import hmac
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--sources', type=Path, required=True)
p.add_argument('--manual', type=Path, required=True)
p.add_argument('--model', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--openssl', default='/usr/bin/openssl')
a = p.parse_args()

manifest = json.loads((a.sources / 'sources.json').read_text())
source = {}
for name in ['v5/sunxi_ce_proc.c', 'v5/sunxi_ce_reg.c', 'v5/sunxi_ce_reg.h', 'sunxi_ce_cdev.h',
             'sunxi_ce.c', 'sunxi_ce_proc_comm.c']:
    path = a.sources / name
    row = next(r for r in manifest if Path(r['path']) == path)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256'], name
    source[name] = path.read_text()
assert hashlib.sha256(a.manual.read_bytes()).hexdigest() == '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2'
manual = ' '.join(a.manual.read_text().split())
for text in ['0: MD5 1: SHA1 2: SHA224 3: SHA256 4: SHA384 5: SHA512',
             'For the multi-packet operation, the first address of the digest output result of the previous HASH can be directly configured to the first address of the next IV',
             'IV0[63:32] BASE_ADDR IV0[31:00] BASE_ADDR + 0x04',
             '0x0: Electronic Code Book (ECB) mode 0x1: Cipher Block Chaining (CBC) mode 0x2: Counter (CTR) mode',
             '0: 16-bits Counter 1: 32-bits Counter, gcm mode always use this setting without software 2: 64-bits Counter 3: 128-bits Counter',
             '0: 128-bits 1: 192-bits 2: 256-bits',
             'ctr addr is address for next block’s IV. (By byte)']:
    assert text in manual, text
reg_h = source['v5/sunxi_ce_reg.h']
for text in ['#define SS_METHOD_SHA1\t\t\t1', '#define SS_METHOD_SHA384\t\t4', '#define SS_METHOD_SHA512\t\t5',
             '#define SS_AES_MODE_CBC\t\t\t\t1', '#define SS_AES_MODE_CTR\t\t\t\t2',
             '#define CE_SYM_CTL_OP_MODE_SHIFT\t\t8', '#define CE_SYM_CTL_CTR_SIZE_SHIFT\t\t2',
             '#define CE_AES_KEY_SIZE_256\t\t\t2']:
    assert text in reg_h, text
reg = source['v5/sunxi_ce_reg.c']
for text in ['ce_task_addr_set(cnt, 0, task->ctr_addr);\n\tss_cntsize_set(CE_CTR_SIZE_128, task);',
             'ce_task_addr_set(cnt, 0, task->ctr_addr);\n\tss_cntsize_set(CE_CTR_SIZE_32, task);',
             'task->sym_ctl |= mode << CE_SYM_CTL_OP_MODE_SHIFT;']:
    assert text in reg, text
proc = source['v5/sunxi_ce_proc.c']
aes = proc.split('static int ss_aes_start(', 1)[1].split('/* verify the key_len */', 1)[0]
for text in ['ss_iv_set(ctx->iv, ctx->iv_size, task);', 'ss_cnt_set(ctx->next_iv, ctx->iv_size, task);',
             'ctx->next_iv, ctx->iv_size, DMA_FROM_DEVICE);']:
    assert text in aes, text
hash_start = proc.split('u32 ss_hash_start(', 1)[1].split('void ss_load_iv(', 1)[0]
for text in ['task->ce_sg[0].dst_len = ctx->md_size;', 'memcpy(ctx->md, digest, ctx->md_size);']:
    assert text in hash_start, text
# Intermediate SHA-224/384 packages carry the full 32/64-byte state.
ce = source['sunxi_ce.c']
assert 'return ss_hash_init(req, SS_METHOD_SHA384, SHA512_DIGEST_SIZE, (char *)iv);' in ce
assert 'return ss_hash_init(req, SS_METHOD_SHA224, SHA256_DIGEST_SIZE, (char *)iv);' in ce
assert 'if (req_ctx->type == SS_METHOD_SHA384)\n\t\tctx->md_size = SHA384_DIGEST_SIZE;' in source['sunxi_ce_proc_comm.c']


def root(value, power, bits):
    """Fractional bits of value**(1/power), from exact integer arithmetic."""
    n = value << (power * bits)
    x = 1 << ((n.bit_length() + power - 1) // power)
    while True:
        y = ((power - 1) * x + n // x ** (power - 1)) // power
        if y >= x:
            break
        x = y
    while (x + 1) ** power <= n:
        x += 1
    return x & ((1 << bits) - 1)


primes = [n for n in range(2, 410) if all(n % d for d in range(2, int(n ** 0.5) + 1))][:80]
k512 = [root(q, 3, 64) for q in primes]
sha384 = [root(q, 2, 64) for q in primes[8:16]]
initial = {1: [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0, 0, 0, 0],
           2: [w & 0xffffffff for w in sha384], 3: [root(q, 2, 32) for q in primes[:8]],
           4: sha384, 5: [root(q, 2, 64) for q in primes[:8]]}
assert k512[0] == 0x428a2f98d728ae22 and initial[3][0] == 0x6a09e667 and initial[2][0] == 0xc1059ed8


def message(n):
    return bytes((i * 37 + 11) & 255 for i in range(n))


def pattern(n, seed):
    return bytes((i * 29 + seed) & 255 for i in range(n))


def openssl(cipher, key, data, iv=None, decrypt=False):
    command = [a.openssl, 'enc', '-' + cipher, '-K', key.hex(), '-nopad', '-nosalt']
    if iv is not None:
        command += ['-iv', iv.hex()]
    if decrypt:
        command.append('-d')
    return subprocess.check_output(command, input=data)


def ghash(h, data):
    hv, y = int.from_bytes(h, 'big'), 0
    for at in range(0, len(data), 16):
        x = y ^ int.from_bytes(data[at:at + 16].ljust(16, b'\0'), 'big')
        z, v = 0, hv
        for i in range(128):
            if x >> (127 - i) & 1:
                z ^= v
            v = (v >> 1) ^ (0xe1 << 120 if v & 1 else 0)
        y = z
    return y.to_bytes(16, 'big')


def gcm(key, iv, aad, plain, tag_bytes):
    """Independent SP 800-38D GCM encryption: OpenSSL AES blocks, Python GHASH."""
    algorithm = 'aes-%d-ecb' % (8 * len(key))
    h = openssl(algorithm, key, bytes(16))
    if len(iv) == 12:
        j0 = iv + b'\0\0\0\1'
    else:
        j0 = ghash(h, iv.ljust(-(-len(iv) // 16) * 16, b'\0') + (8 * len(iv)).to_bytes(16, 'big'))
    blocks = 1 + -(-len(plain) // 16)
    counters = b''.join(j0[:12] + ((int.from_bytes(j0[12:], 'big') + i) & 0xffffffff).to_bytes(4, 'big')
                        for i in range(blocks))
    stream = openssl(algorithm, key, counters)
    cipher = bytes(x ^ y for x, y in zip(plain, stream[16:]))
    pad = lambda b: b.ljust(-(-len(b) // 16) * 16, b'\0')
    s = ghash(h, pad(aad) + pad(cipher) + (8 * len(aad)).to_bytes(8, 'big') + (8 * len(cipher)).to_bytes(8, 'big'))
    return cipher, bytes(x ^ y for x, y in zip(s, stream[:16]))[:tag_bytes]


# NIST SP 800-38A F.1-F.5 vectors (64-byte plaintext, one key per size).
plain = bytes.fromhex('6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51'
                      '30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710')
keys = {16: bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c'),
        24: bytes.fromhex('8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b'),
        32: bytes.fromhex('603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4')}
cbc_iv = bytes(range(16))
ctr_iv = bytes.fromhex('f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff')
nist = {
    (0, 24): 'bd334f1d6e45f25ff712a214571fa5cc974104846d0ad3ad7734ecb3ecee4eef'
             'ef7afd2270e2e60adce0ba2face6444e9a4b41ba738d6c72fb16691603c18e0e',
    (0, 32): 'f3eed1bdb5d2a03c064b5a7e3db181f8591ccb10d410ed26dc5ba74a31362870'
             'b6ed21b99ca6f4f9f153e7b1beafed1d23304b7a39f9f3ff067d8d8f9e24ecc7',
    (1, 16): '7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2'
             '73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7',
    (1, 24): '4f021db243bc633d7178183a9fa071e8b4d9ada9ad7dedf4e5e738763f69145a'
             '571b242012fb7ae07fa9baac3df102e008b0e27988598881d920a9e64f5615cd',
    (1, 32): 'f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d'
             '39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b',
    (2, 16): '874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff'
             '5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee',
    (2, 24): '1abc932417521ca24f2b0459fe7e6e0b090339ec0aa6faefd5ccc2c6f4ce8e94'
             '1e36b26bd1ebc670d1bd1d665620abf74f78a7f6d29809585a97daec58c6b050',
    (2, 32): '601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5'
             '2b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6'}
# McGrew-Viega GCM test cases 1-8 and 13-16 (key, IV, AAD, plaintext, ciphertext, tag).
k3 = 'feffe9928665731c6d6a8f9467308308'
p3 = ('d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72'
      '1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255')
a4 = 'feedfacedeadbeeffeedfacedeadbeefabaddad2'
iv6 = ('9313225df88406e555909c5aff5269aa6a7a9538534f7da1e4c303d2a318a728'
       'c3c0c95156809539fcf0e2429a6b525416aedbf5a0de6a57a637b39b')
gcm_vectors = [
    ('0' * 32, '0' * 24, '', '', '', '58e2fccefa7e3061367f1d57a4e7455a'),
    ('0' * 32, '0' * 24, '', '0' * 32, '0388dace60b6a392f328c2b971b2fe78', 'ab6e47d42cec13bdf53a67b21257bddf'),
    (k3, 'cafebabefacedbaddecaf888', '', p3,
     '42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e'
     '21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985', '4d5c2af327cd64a62cf35abd2ba6fab4'),
    (k3, 'cafebabefacedbaddecaf888', a4, p3[:120],
     '42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e'
     '21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091', '5bc94fbc3221a5db94fae95ae7121a47'),
    (k3, 'cafebabefacedbad', a4, p3[:120],
     '61353b4c2806934a777ff51fa22a4755699b2a714fcdc6f83766e5f97b6c7423'
     '73806900e49f24b22b097544d4896b424989b5e1ebac0f07c23f4598', '3612d2e79e3b0785561be14aaca2fccb'),
    (k3, iv6, a4, p3[:120],
     '8ce24998625615b603a033aca13fb894be9112a5c3a211a8ba262a3cca7e2ca7'
     '01e4a9a4fba43c90ccdcb281d48c7c6fd62875d2aca417034c34aee5', '619cc5aefffe0bfa462af43c1699d050'),
    ('0' * 48, '0' * 24, '', '', '', 'cd33b28ac773f74ba00ed1f312572435'),
    ('0' * 48, '0' * 24, '', '0' * 32, '98e7247c07f0fe411c267e4384b0f600', '2ff58d80033927ab8ef4d4587514f0fb'),
    ('0' * 64, '0' * 24, '', '', '', '530f8afbc74536b9a963b4f1c4cb738b'),
    ('0' * 64, '0' * 24, '', '0' * 32, 'cea7403d4d606b6e074ec5d3baf39d18', 'd0d1c8a799996bf0265b98b5d48ab919'),
    (k3 * 2, 'cafebabefacedbaddecaf888', '', p3,
     '522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa'
     '8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad', 'b094dac5d93471bdec1a502270e3cc6c'),
    (k3 * 2, 'cafebabefacedbaddecaf888', a4, p3[:120],
     '522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa'
     '8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662', '76fc6ece0f4e1768cddf8853bb2d551b')]
for k, iv, aad, pt, ct, tag in gcm_vectors:
    assert gcm(*map(bytes.fromhex, (k, iv, aad, pt)), 16) == (bytes.fromhex(ct), bytes.fromhex(tag)), (k, iv)

h = lambda b: b.hex() if b else '-'
lines = ['k512 ' + ' '.join('%016x' % k for k in k512)]
lines += ['initial %d ' % m + ' '.join('%x' % w for w in initial[m]) for m in initial]
hash_lengths = [0, 3, 55, 56, 64, 111, 112, 127, 128, 129, 2048, 2049, 5003]
hmac_keys = [b'', pattern(20, 1), pattern(64, 2), pattern(100, 3), pattern(131, 4), pattern(1024, 5)]
for method in range(1, 6):
    lines += ['hash %d * %d' % (method, n) for n in hash_lengths]
    lines += ['hash %d %s %d' % (method, h(key), n) for key in hmac_keys for n in (0, 50, 3000)]
expected = {}
def cipher(mode, decrypt, key, iv, aad, tag_bytes, data, answer):
    line = 'cipher %d %d %s %s %s %d %s' % (mode, decrypt, h(key), h(iv), h(aad), tag_bytes, h(data))
    lines.append(line)
    expected[line] = answer
names = {0: 'ecb', 1: 'cbc', 2: 'ctr'}
for size, key in keys.items():
    for mode in (0, 1, 2):
        iv = None if mode == 0 else (cbc_iv if mode == 1 else ctr_iv)
        algorithm = 'aes-%d-%s' % (8 * size, names[mode])
        result = openssl(algorithm, key, plain, iv)
        if (mode, size) in nist:
            assert result.hex() == nist[(mode, size)], (mode, size)
        cipher(mode, 0, key, iv, None, 0, plain, result.hex())
        cipher(mode, 1, key, iv, None, 0, result, plain.hex())
        # Task boundaries: software chains CBC blocks and CTR counters.
        lengths = [1040, 2048] if mode < 2 else [1, 15, 17, 1023, 1025, 2048]
        for n in lengths:
            data = pattern(n, size + mode)
            start = bytes.fromhex('fffffffffffffffffffffffffffffff0') if mode == 2 and n == 2048 else iv
            pad = (-n) % 16
            out = openssl(algorithm, key, data + bytes(pad), start)[:n] if mode == 2 else openssl(algorithm, key, data, start)
            cipher(mode, 0, key, start, None, 0, data, out.hex())
            if mode == 1:
                cipher(mode, 1, key, start, None, 0, out, data.hex())
for k, iv, aad, pt, ct, tag in gcm_vectors:
    k, iv, aad, pt, ct, tag = map(bytes.fromhex, (k, iv, aad, pt, ct, tag))
    cipher(3, 0, k, iv, aad, 16, pt, (ct + tag).hex())
    cipher(3, 1, k, iv, aad, 16, ct + tag, pt.hex())
long_key, long_iv, long_aad, long_plain = pattern(32, 7), pattern(64, 8), pattern(512, 9), pattern(2048, 10)
for tag_bytes, iv in ((12, long_iv), (16, long_iv[:12]), (13, long_iv[:1])):
    ct, tag = gcm(long_key, iv, long_aad, long_plain, tag_bytes)
    cipher(3, 0, long_key, iv, long_aad, tag_bytes, long_plain, (ct + tag).hex())
    cipher(3, 1, long_key, iv, long_aad, tag_bytes, ct + tag, long_plain.hex())
# Changed tag, ciphertext and AAD bits must be rejected without output.
ct, tag = gcm(keys[24], cbc_iv[:12], b'aad', plain, 16)
for data, aad in ((ct + bytes([tag[0] ^ 1]) + tag[1:], b'aad'), (bytes([ct[0] ^ 128]) + ct[1:] + tag, b'aad'),
                  (ct + tag, b'aae')):
    cipher(3, 1, keys[24], cbc_iv[:12], aad, 16, data, 'auth')

a.out.mkdir(parents=True, exist_ok=True)
requests = a.out / 'a733-ce-modes.requests'
requests.write_text('\n'.join(lines) + '\n')
log = subprocess.run([str(a.model), str(requests)], check=True, capture_output=True, text=True).stdout
(a.out / 'a733-ce-modes.log').write_text(log)
rows = log.splitlines()
assert rows[-1].startswith('PASS '), rows[-1]
hashes = [r.split() for r in rows if r.startswith('hash ')]
ciphers = [r for r in rows if r.startswith('cipher ')]
assert len(hashes) == len([l for l in lines if l.startswith('hash ')]) and len(ciphers) == len(expected)
algorithms = {1: 'sha1', 2: 'sha224', 3: 'sha256', 4: 'sha384', 5: 'sha512'}
for _, method, key, n, digest in hashes:
    data, name = message(int(n)), algorithms[int(method)]
    want = hashlib.new(name, data).hexdigest() if key == '*' else \
        hmac.new(bytes.fromhex(key) if key != '-' else b'', data, name).hexdigest()
    assert digest == want, (method, key[:16], n)
for row in ciphers:
    line, result = row.rsplit(' ', 1)
    assert expected[line] == result or (expected[line] == '' and result == '-'), line[:80]
print('PASS pinned CE mode references, %d hash/HMAC digests, %d AES/GCM results and %d rejected tags; '
      'no physical qualification' % (len(hashes), len(ciphers), sum(v == 'auth' for v in expected.values())))
