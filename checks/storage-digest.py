#!/usr/bin/env python3
"""Compare the portable storage SHA-256 with hashlib on the host and AArch64.

Usage: storage-digest.py --output DIR [--baseline OLD_storage_digest.c]

The host build runs NIST FIPS 180-2 example messages and random lengths, split
at random chunk boundaries and buffer offsets. The AArch64 build uses the
cached-SMP C flags (strict alignment, general registers only, no builtins) and
executes under Unicorn. Its instruction counts are an emulator sanity check,
not cycle timing. With --baseline, the same counts are reported for an earlier
source. No hardware access.
"""
import argparse
import ctypes
import hashlib
import json
import random
import subprocess
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
LLVM = Path('/usr/lib/llvm-21/bin')
LLD = Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
HEADERS = Path.home() / '.cache/sunstoneos/host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include'
NIST = [(b'abc', 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'),
        (b'', 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'),
        (b'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
         '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1'),
        (b'a' * 1000000, 'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0')]
# Minimal freestanding memcpy/memset: -fno-builtin turns copies and zeroing into calls.
RUNTIME = 'void *memcpy(void *d,const void *s,unsigned long n)' \
          '{unsigned char *o=d;const unsigned char *i=s;while(n--)*o++=*i++;return d;}\n' \
          'void *memset(void *d,int c,unsigned long n){unsigned char *o=d;while(n--)*o++=(unsigned char)c;return d;}\n'
CODE, DATA, STOP = 0x40000000, 0x40400000, 0x40fff000


class Digest(ctypes.Structure):
    _fields_ = [('h', ctypes.c_uint32 * 8), ('bytes', ctypes.c_uint64),
                ('tail', ctypes.c_uint8 * 64), ('used', ctypes.c_uint)]


def host(source, out):
    library = out / 'host-digest.so'
    subprocess.check_call(['/usr/bin/clang-21', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC',
                           '-shared', '-fno-builtin', '-I', ROOT / 'include', source, '-o', library])
    lib = ctypes.CDLL(str(library))
    rng = random.Random(733)
    cases = 0
    for message, expected in NIST:
        out32 = (ctypes.c_uint8 * 32)()
        lib.storage_digest_bytes(message, len(message), out32)
        assert bytes(out32).hex() == expected
        cases += 1
    for _ in range(400):
        length = rng.choice([rng.randrange(0, 300), rng.randrange(0, 70000)])
        offset = rng.randrange(0, 8)
        backing = ctypes.create_string_buffer(rng.randbytes(length + offset), length + offset)
        message = backing.raw[offset:offset + length]
        digest = Digest()
        lib.storage_digest_init(ctypes.byref(digest))
        at = 0
        while at < length:
            n = min(length - at, rng.choice([1, 3, 63, 64, 65, 128, 4096, rng.randrange(1, 9000)]))
            assert lib.storage_digest_add(ctypes.byref(digest), ctypes.byref(backing, offset + at), ctypes.c_size_t(n)) == 1
            at += n
        out32 = (ctypes.c_uint8 * 32)()
        lib.storage_digest_end(ctypes.byref(digest), out32)
        assert bytes(out32) == hashlib.sha256(message).digest(), (length, offset)
        cases += 1
    return cases


def aarch64(source, out, tag):
    obj, runtime, elf, text = (out / (tag + suffix) for suffix in ('.o', '-runtime.o', '.elf', '.bin'))
    flags = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-mgeneral-regs-only', '-ffunction-sections', '-O2', '-I', ROOT / 'include',
             '-isystem', HEADERS]
    subprocess.check_call([LLVM / 'clang', *flags, '-c', source, '-o', obj])
    (out / 'runtime.c').write_text(RUNTIME)
    subprocess.check_call([LLVM / 'clang', *flags, '-c', out / 'runtime.c', '-o', runtime])
    subprocess.check_call([LLD, '-static', '-e', '0', f'--image-base={CODE:#x}', f'-Ttext={CODE:#x}',
                           obj, runtime, '-o', elf])
    subprocess.check_call([LLVM / 'llvm-objcopy', '-O', 'binary', elf, text])
    symbols = {}
    for line in subprocess.check_output([LLVM / 'llvm-nm', elf], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    image = text.read_bytes()
    count = [0]

    def run(message, offset=0):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(CODE, 0x00400000)
        u.mem_map(DATA, 0x00c00000)
        u.mem_write(CODE, image)
        u.mem_write(DATA + offset, message)
        u.hook_add(UC_HOOK_CODE, lambda *args: count.__setitem__(0, count[0] + 1), begin=CODE, end=CODE + len(image))
        u.reg_write(UC_ARM64_REG_SP, STOP - 0x100)
        u.reg_write(UC_ARM64_REG_LR, STOP)
        for i, value in enumerate((DATA + offset, len(message), DATA + 0x00b00000)):
            u.reg_write(UC_ARM64_REG_X0 + i, value)
        count[0] = 0
        u.emu_start(symbols['storage_digest_bytes'], STOP)
        assert u.reg_read(UC_ARM64_REG_PC) == STOP
        return bytes(u.mem_read(DATA + 0x00b00000, 32)), count[0]

    rng = random.Random(55)
    for message, expected in NIST[:3]:
        assert run(message)[0].hex() == expected
    for length in (55, 56, 64, 65, 127, 1000, 4097):
        for offset in (0, 1, 2, 4):
            message = rng.randbytes(length)
            assert run(message, offset)[0] == hashlib.sha256(message).digest(), (length, offset)
    message = rng.randbytes(65536)
    aligned = run(message)
    unaligned = run(message, 1)
    assert aligned[0] == unaligned[0] == hashlib.sha256(message).digest()
    return {'aligned_instructions_per_byte': round(aligned[1] / len(message), 2),
            'unaligned_instructions_per_byte': round(unaligned[1] / len(message), 2)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path)
    a = parser.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'drivers/storage/storage_digest.c'
    result = {'host_cases': host(source, a.output), 'aarch64': aarch64(source, a.output, 'current')}
    if a.baseline:
        result['aarch64_baseline'] = aarch64(a.baseline.resolve(), a.output, 'baseline')
    (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS storage SHA-256 against NIST examples and hashlib on host and AArch64 Unicorn:', json.dumps(result))


if __name__ == '__main__':
    main()
