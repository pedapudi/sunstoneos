#!/usr/bin/env python3
"""Run the compiled mimus read-out entry under Unicorn.

Builds include/pvr_compute_mimus_readout.h for AArch64 with mimus's compute
options (-march=armv8.2-a+nolse -mtune=cortex-a55 -ffp-contract=off -O2), as
C++ and, for a compile-only portability check, as C. Each of the four entries
(int32 or int64 sums, plain or residual) runs under Unicorn on edge inputs;
every output must equal, bit for bit, mimus's scalar order evaluated in
float32: (0 + (float(sum) * ws) * xs) + (bias or 0), then y + alpha * value.
The int64 inputs include sums rebuilt from 32-bit chunk sums with the
kernel's lo, t and high-word rule. The check also counts the instructions
each entry retires per output. No hardware is accessed."""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_CPACR_EL1
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['--target=aarch64-none-elf', '-march=armv8.2-a+nolse', '-mtune=cortex-a55', '-mno-strict-align', '-O2',
         '-ffp-contract=off', '-ffreestanding', '-fno-builtin', '-I' + str(ROOT / 'include')]
ENTRIES = ['readout_i32', 'readout_i64', 'readout_i32_accumulate', 'readout_i64_accumulate']
SOURCE = '#include "pvr_compute_mimus_readout.h"\n' + ''.join(
    f'extern "C" void {e}(const struct pvr_mimus_readout *a) {{ pvr_mimus_{e}(a); }}\n' for e in ENTRIES)
CODE, DATA, STACK, RETURN = 0x200000, 0x10000000, 0x20000000, 0x30000000


def build(out, lld):
    (out / 'readout.cc').write_text(SOURCE)
    (out / 'readout-c.c').write_text(SOURCE.replace('extern "C" ', ''))
    subprocess.run(['clang-21', '-x', 'c', '-std=c11', '-Wall', '-Wextra', '-Werror', '-c', str(out / 'readout-c.c'),
                    '-o', str(out / 'readout-c.o')] + FLAGS, check=True)
    subprocess.run(['clang++-21', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-c',
                    str(out / 'readout.cc'), '-o', str(out / 'readout.o')] + FLAGS, check=True)
    subprocess.run([str(lld), '-static', '-nostdlib', '--entry=0', f'-Ttext=0x{CODE:x}', str(out / 'readout.o'),
                    '-o', str(out / 'readout.elf')], check=True)
    dump = subprocess.run(['llvm-objdump-21', '-d', '--no-show-raw-insn', str(out / 'readout.elf')],
                          capture_output=True, text=True, check=True).stdout
    (out / 'readout.lst').write_text(dump)
    with open(out / 'readout.elf', 'rb') as f:
        elf = ELFFile(f)
        segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name in ENTRIES}
    assert set(symbols) == set(ENTRIES)
    return segments, symbols, dump


def emulator(segments):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(CODE, 0x100000)
    for address, data in segments:
        uc.mem_write(address, data)
    uc.mem_map(DATA, 0x2000000); uc.mem_map(STACK, 0x10000); uc.mem_map(RETURN, 0x1000)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    counter = [0]
    uc.hook_add(UC_HOOK_CODE, lambda u, a, s, c: c.__setitem__(0, c[0] + 1), counter)
    return uc, counter


def f32(x):
    """Round a double to float32 (RNE). Every float32 sum or product is exact
    in double or rounds correctly through it (53 >= 2 * 24 + 2 bits)."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def reference(sums, ws, xs, bias, y, alpha, residual):
    """mimus's order in float32. float() of a sum below 2^53 is exact, so
    f32 rounds it once."""
    out = [row[:] for row in y]
    for r, row in enumerate(sums):
        for c, s in enumerate(row):
            assert abs(s) < 1 << 53
            acc = f32(0.0 + f32(f32(f32(float(s)) * ws[c]) * xs[r]))
            value = f32(acc + (bias[c] if bias is not None else 0.0))
            out[r][c] = f32(out[r][c] + f32(f32(alpha) * value)) if residual else value
    return out


def bits(rows):
    return b''.join(struct.pack(f'<{len(r)}f', *r) for r in rows)


def run(uc, counter, entry, sums, wide, ws, xs, bias, y, alpha):
    rows, n, stride = len(sums), len(sums[0]), len(y[0])
    blobs, at = {}, DATA + 0x1000
    packed = {'sums': b''.join(struct.pack(f'<{n}{"q" if wide else "i"}', *r) for r in sums),
              'ws': struct.pack(f'<{n}f', *ws), 'xs': struct.pack(f'<{rows}f', *xs),
              'bias': struct.pack(f'<{n}f', *bias) if bias is not None else None, 'y': bits(y)}
    for name, data in packed.items():
        if data is None:
            blobs[name] = 0; continue
        uc.mem_write(at, data); blobs[name] = at
        at = (at + len(data) + 63) & ~63
    record = struct.pack('<5Q3If', blobs['sums'], blobs['ws'], blobs['xs'], blobs['bias'], blobs['y'],
                         rows, n, stride, alpha)
    uc.mem_write(DATA, record)
    uc.reg_write(UC_ARM64_REG_X0, DATA); uc.reg_write(UC_ARM64_REG_LR, RETURN)
    uc.reg_write(UC_ARM64_REG_SP, STACK + 0x8000)
    counter[0] = 0
    uc.emu_start(entry, RETURN)
    raw = bytes(uc.mem_read(blobs['y'], 4 * rows * stride))
    return [list(struct.unpack_from(f'<{stride}f', raw, 4 * r * stride)) for r in range(rows)], counter[0]


def chunked(chunks):
    """The kernel's rule: lo is the sum modulo 2^32, t the sum of c >> 8, and
    the high word (t >> 24) + ((t << 8) mod 2^32 > lo)."""
    lo, t = 0, 0
    for c in chunks:
        assert -(1 << 31) <= c < 1 << 31
        lo = (lo + c) & 0xffffffff
        t += c >> 8
    assert -(1 << 31) <= t < 1 << 31
    hi = ((t >> 24) + (((t << 8) & 0xffffffff) > lo)) & 0xffffffff
    value = (hi << 32 | lo) - ((1 << 64) if hi >> 31 else 0)
    assert value == sum(chunks)
    return value


def edge_sums(wide, rng):
    values = [0, 1, -1, (1 << 24) + 1, -((1 << 24) + 1), (1 << 31) - 1, -(1 << 31)]
    values += [((1 << 24) + 1) << k for k in range(10 if wide else 7)]
    values += [-v for v in values]
    if not wide:
        return [v for v in values if -(1 << 31) <= v < 1 << 31]
    if wide:
        limit = 512 * 32767 * 128
        values += [1 << 34, -(1 << 34), (1 << 34) - 1, 3520 * 32767 * 128, -3520 * 32767 * 128]
        for _ in range(64):
            chunks = [rng.choice([-limit, limit, rng.randint(-limit, limit)]) for _ in range(rng.randint(1, 7))]
            values.append(chunked(chunks))
        values += [chunked([-limit] * 7), chunked([-limit, limit - 1, -1]), chunked([limit] * 4 + [-limit])]
    return values


def floats(n, rng):
    v = [f32(rng.uniform(-1, 1) * 2.0 ** rng.randint(-24, 4)) for _ in range(n)]
    v[0] = -0.0
    if n > 1: v[-1] = 0.0
    return v


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    segments, symbols, dump = build(out, a.lld)
    uc, counter = emulator(segments)
    rng = random.Random(20261022)
    compared, per_output = 0, {}
    for entry in ENTRIES:
        wide, residual = 'i64' in entry, 'accumulate' in entry
        values = edge_sums(wide, rng)
        for with_bias in (True, False):
            for n in (1, 3, 8, 37, len(values)):
                rows = 3
                sums = [[values[(r * 7 + c) % len(values)] for c in range(n)] for r in range(rows)]
                ws, xs = floats(n, rng), floats(rows, rng)
                bias = floats(n, rng) if with_bias else None
                y = [floats(n, rng) + [7.0] * 5 for _ in range(rows)]
                for alpha in (0.5, 1.0):
                    got, _ = run(uc, counter, symbols[entry], sums, wide, ws, xs, bias, y, alpha)
                    want = reference(sums, ws, xs, bias, y, alpha, residual)
                    if bits(got) != bits(want):
                        raise AssertionError(f'{entry} bias={with_bias} n={n}: {got} != {want}')
                    compared += rows * n
            # Instructions per output on a bench-like block of 256 channels.
            rows, n = 8, 256
            sums = [[rng.choice(values) for _ in range(n)] for _ in range(rows)]
            y = [floats(n, rng) for _ in range(rows)]
            _, count = run(uc, counter, symbols[entry], sums, wide, floats(n, rng), floats(rows, rng),
                           floats(n, rng) if with_bias else None, y, 0.5)
            per_output[f'{entry}{"" if with_bias else "_no_bias"}'] = count / (rows * n)
    vector = {name: name in dump for name in ('scvtf\tv', 'fcvtn\tv', 'fmla', 'fmadd')}
    assert vector['scvtf\tv'] and vector['fcvtn\tv'] and not vector['fmla'] and not vector['fmadd'], vector
    result = {'scope': 'AArch64 read-out under Unicorn against the float32 reference; no hardware',
              'flags': FLAGS, 'outputs_compared': compared, 'instructions_per_output': per_output}
    (out / 'readout-check.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(per_output))
    print(f'PASS {compared} read-out outputs equal the scalar order bit for bit; no fused multiply-add')


if __name__ == '__main__':
    main()
