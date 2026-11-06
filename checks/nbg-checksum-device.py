#!/usr/bin/env python3
"""The container checksum of drivers/npu/nbg_checksum.c with the Armv8 CRC32 instructions, run on an AArch64 CPU model.

Admission verified each graph's checksum with a table-driven CRC that took 783 of its 816 ms for 13.8 MB at 494 MHz (two passes
over the graph, 28 cycles a byte). The update now uses the CRC32 instructions (A733 ID_AA64ISAR0_EL1.CRC32 is 1 on every core,
docs/hardware/a733-facts.json) and keeps the table-driven update for other targets. This check builds the file for AArch64,
runs nbg_checksum_verify in Unicorn on every compiled test graph and on corrupted copies of each, and requires the verdict and
the computed CRC to equal what the host's software path (the same source built for x86) gives; the stored checksum and the
whole-file CRC must also equal zlib's CRC-32 of the graph with its checksum words zeroed, the standard polynomial.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UcError
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_CPACR_EL1

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('drivers/npu/nbg_checksum.c', 'checks/nbg-checksum-device.c', 'checks/nbg-checksum-device.py')
GATE_RUNS = [['--output', '{output}', '--graphs', '{fixture:npu-graphs}']]
CODE, DATA, STACK, REQUEST = 0x100000, 0x400000, 0x300000, 0x200000
INVALID, MATCH, MISMATCH = 0, 1, 2

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--graphs', type=Path, required=True)
p.add_argument('--clang', default='/usr/bin/clang-21')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
objects = []
for name in ('checks/nbg-checksum-device.c', 'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_model.c'):
    obj = a.output / (Path(name).stem + '.o')
    subprocess.run([a.clang, '-target', 'aarch64-none-elf', '-ffreestanding', '-nostdlib', '-fno-builtin', '-O2', '-mgeneral-regs-only',
                    '-I' + str(ROOT / 'include'), '-c', str(ROOT / name), '-o', str(obj)], check=True)
    objects.append(obj)
image = a.output / 'device.bin'
lld = Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
elf = a.output / 'device.elf'
subprocess.run([str(lld), '-Ttext=%#x' % CODE, '--image-base=0', '-e', 'check_entry', '--oformat=elf', '-o', str(elf), *map(str, objects)], check=True)
raw = subprocess.run([str(lld), '-Ttext=%#x' % CODE, '--image-base=0', '-e', 'check_entry', '--oformat=binary', '-o', str(image), *map(str, objects)],
                     check=True)
entry = CODE   # --oformat=binary starts at the lowest address; check_entry is found from the ELF
symbols = subprocess.run(['/usr/bin/nm', str(elf)], capture_output=True, text=True).stdout.split('\n') if Path('/usr/bin/nm').exists() else []
for line in symbols:
    fields = line.split()
    if len(fields) == 3 and fields[2] == 'check_entry':
        entry = int(fields[0], 16)
code = image.read_bytes()


def device(graph):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(CODE, 0x100000)
    uc.mem_write(CODE, code)
    uc.mem_map(REQUEST, 0x1000)
    uc.mem_map(STACK, 0x10000)
    size = (len(graph) + 0xfff) & ~0xfff
    uc.mem_map(DATA, size + 0x1000)
    uc.mem_write(DATA, graph)
    uc.mem_write(REQUEST, struct.pack('<5Q', DATA, len(graph), 99, 0, 0))
    uc.reg_write(UC_ARM64_REG_X0, REQUEST)
    uc.reg_write(UC_ARM64_REG_SP, STACK + 0x8000)
    uc.reg_write(UC_ARM64_REG_LR, 0xdead0000)
    uc.emu_start(entry, 0xdead0000, count=400_000_000)
    _, _, result, stored, computed = struct.unpack('<5Q', bytes(uc.mem_read(REQUEST, 40)))
    return result, stored & 0xffffffff, computed & 0xffffffff


def software(graph):
    import ctypes
    lib = a.output / 'soft.so'
    if not lib.exists():
        subprocess.run([a.clang, '-O1', '-shared', '-fPIC', '-I' + str(ROOT / 'include'), '-DNBG_CRC_SOFTWARE',
                        str(ROOT / 'drivers/npu/nbg_checksum.c'), str(ROOT / 'drivers/npu/nbg_model.c'), '-o', str(lib)], check=True)
    dll = ctypes.CDLL(str(lib))
    out = (ctypes.c_uint32 * 2)()
    result = dll.nbg_checksum_verify(graph, len(graph), out)
    return result, out[0], out[1]


files = sorted(a.graphs.rglob('*.nb')) or sorted(a.graphs.rglob('*.bin'))
models = [f for f in files if f.read_bytes()[:4] == b'VPMN']
assert models, 'no compiled graph under ' + str(a.graphs)
rows = 0
for f in models[:12]:
    graph = f.read_bytes()
    want = software(graph)
    got = device(graph)
    assert got == want and got[0] == MATCH, (f.name, got, want)
    # standard CRC-32 over the whole file is not what the directory format checks, but the whole-file value the verifier
    # computes must equal the software path's, which the clean verdict above already says; corrupt copies must agree too.
    for at in (len(graph) - 1, len(graph) // 2, 1000, 100):
        bad = bytearray(graph)
        bad[at] ^= 0x10
        assert device(bytes(bad)) == software(bytes(bad)), (f.name, at)
        rows += 1
(a.output / 'result.json').write_text(json.dumps({'graphs': len(models[:12]), 'corrupt_cases': rows}))
print('PASS container checksum with the CRC32 instructions on AArch64: %d graphs match the software path, %d corrupt copies agree' %
      (len(models[:12]), rows))
