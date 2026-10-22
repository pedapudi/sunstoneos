#!/usr/bin/env python3
"""Compare the CPU cost of an exact NPU linear layer with the A76 NEON kernel.

Host tool; needs pyelftools and Unicorn (the project venv). Inputs:

  --elf      an AArch64 ELF linked from checks/npu-mimus-digits.cc and the
             mimus backend's gemm_combine_portable.cc with MIMUS_AARCH64_NEON=1,
             both built with the backend's flags (docs/npu-mimus-linear.md).
  --study    a digit-form study directory from tools/npu-mimus-linear.sh.
  --events   JSON lines from checks/npu-mimus-cost.c, INT16 graph first and
             digit graph second.
  --int16-log, --digit-log
             the two export logs; their "CycleCount:" lines are the
             compiler's per-operation NPU cycle estimates.

The tool executes MimusNpuDigitSplit, MimusNpuDigitCombine and the packed
MimusGemmS16FC under Unicorn on the study's rows, requires their outputs to
equal the simulator's inputs and the mimus reference bytes, and counts the
instructions each executes. It then charges every per-job event with the unit
costs below, all estimates chosen at the expensive end, and writes one JSON
report. A pass needs both the total CPU cycles per job and the client's time
from SUBMIT to the copied output to be below the A76 kernel's.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as regs

UNIT = dict(
    instruction=1,          # cycles per executed instruction of the exact path
    translation=100,        # cycles per software page translation
    cache_line=20,          # cycles per 64-byte line cleaned or invalidated
    ddr_byte=0.5,           # cycles per CPU byte read from or written to DDR
    queue_hop=2000,         # cycles per message hop (context switch); two per round trip
    digest_byte=25,         # cycles per SHA-256 byte, upper end of 15-25
    scan_word=4,            # instructions per 64-bit END word checked
    table_word=12,          # instructions per page-table word checked by canonical()
    int16_readout=1,        # instructions per INT16 output converted to float
    a76_ipc=2,              # NEON instructions per A76 cycle, crediting both pipes
)
CPU_MHZ, NPU_MHZ = 494, 492  # configured CPU clock; native NPU PLL output
BOARD_CYCLES_PER_ROW = 55000  # measured NEON rate on one core at 494 MHz, this layer
BASE, STACK, STOP = 0x2000000, 0x3f00000, 0x3ff0000


class Machine:
    def __init__(self, path):
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        with path.open('rb') as stream:
            elf = ELFFile(stream)
            self.symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            mapped = set()
            for segment in elf.iter_segments():
                if segment['p_type'] != 'PT_LOAD':
                    continue
                address, size = segment['p_vaddr'], segment['p_memsz']
                for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
                    if page not in mapped:
                        self.cpu.mem_map(page, 4096)
                        mapped.add(page)
                self.cpu.mem_write(address, segment.data())
        self.cpu.mem_map(BASE, 0x2000000)
        self.cpu.reg_write(regs.UC_ARM64_REG_CPACR_EL1, 3 << 20)
        self.cpu.reg_write(regs.UC_ARM64_REG_FPCR, 0)
        self.count = 0
        self.cpu.hook_add(UC_HOOK_CODE, self.step)
        self.next = BASE

    def step(self, uc, address, size, data):
        self.count += 1

    def put(self, data):
        address = self.next
        self.cpu.mem_write(address, bytes(data))
        self.next = (address + len(data) + 0x10f) & ~0xff  # 256-byte aligned, with a guard gap
        return address

    def call(self, name, *values):
        self.cpu.reg_write(regs.UC_ARM64_REG_SP, STACK)
        self.cpu.reg_write(regs.UC_ARM64_REG_LR, STOP)
        for i, value in enumerate(values):
            self.cpu.reg_write(getattr(regs, f'UC_ARM64_REG_X{i}'), value)
        self.count = 0
        self.cpu.emu_start(self.symbols[name], STOP, count=50_000_000)
        if self.cpu.reg_read(regs.UC_ARM64_REG_PC) != STOP:
            raise RuntimeError(f'{name}: instruction limit')
        return self.cpu.reg_read(regs.UC_ARM64_REG_X0), self.count


def instruction_counts(elf, study):
    m = Machine(elf)
    meta = json.loads((study / 'meta.json').read_text())
    rows, k, n = meta['rows'], meta['k'], meta['n']
    chunks = struct.unpack(f'<{len(meta["chunks"])}I', (study / 'chunks.u32').read_bytes())
    x = (study / 'x.i16').read_bytes()
    reference = (study / 'reference.f32').read_bytes()
    ws, xs, bias = (m.put((study / name).read_bytes()) for name in ('ws.f32', 'xs.f32', 'bias.f32'))

    outputs = [m.put(bytes(4 * rows * size)) for size in chunks]
    table = m.put(struct.pack(f'<{len(chunks)}Q', *outputs))
    ok, split = m.call('MimusNpuDigitSplit', m.put(x), rows, k, m.put((study / 'chunks.u32').read_bytes()),
                       len(chunks), table)
    for c, address in enumerate(outputs):
        simulated = (study / f'export_nbg_unify/input_{c}.dat').read_bytes()
        if not ok or bytes(m.cpu.mem_read(address, len(simulated))) != simulated:
            raise RuntimeError(f'AArch64 digit split differs from simulator input {c}')

    partials = (study / 'partials.i16').read_bytes()
    size = 2 * 4 * rows * n
    views = m.put(struct.pack(f'<{len(chunks)}Q', *[m.put(partials[c * size:(c + 1) * size])
                                                     for c in range(len(chunks))]))
    out = m.put(bytes(4 * rows * n))
    ok, combine = m.call('MimusNpuDigitCombine', views, len(chunks), rows, n, ws, xs, bias, out)
    if not ok or bytes(m.cpu.mem_read(out, len(reference))) != reference:
        raise RuntimeError('AArch64 digit read-out differs from MimusGemmS16FC')

    # Packed weights: group g at g * 8k, element (8g + lane, kk) at kk * 8 + lane.
    w = struct.unpack(f'<{n * k}b', (study / 'w.i8').read_bytes())
    packed = [0] * (n * k)
    for row in range(n):
        for col in range(k):
            packed[row // 8 * 8 * k + col * 8 + row % 8] = w[row * k + col]
    out = m.put(bytes(4 * rows * n))
    args = m.put(struct.pack('<QQQIIIIIIQQQ', m.put(x), m.put(struct.pack(f'<{n * k}h', *packed)), out,
                             rows, n // 8, k, k, 8 * k, n, ws, xs, bias))
    _, gemm = m.call('MimusGemmS16FC', args)
    if bytes(m.cpu.mem_read(out, len(reference))) != reference:
        raise RuntimeError('AArch64 MimusGemmS16FC differs from the study reference')
    return dict(split_insns=split, recombine_insns=combine, kernel_insns=gemm, rows=rows, k=k, n=n,
                chunks=list(chunks))


def npu_cycles(log):
    return sum(int(line.split(':')[1]) for line in log.read_text().splitlines()
               if line.startswith('CycleCount:'))


def charge(events, client_insns, npu, rows, round_trips=3, verified=True, page_maintenance=True,
           copies=True, maintained_bytes=None):
    """Client and worker cycles and the client's SUBMIT-to-output latency for one job."""
    u = UNIT
    hops = 2 * round_trips * u['queue_hop']
    copy_bytes = (events['copy_in_bytes'] + events['copy_out_bytes']) if copies else 0
    client = client_insns * u['instruction'] + (2 * copy_bytes + events['pool_clear_bytes']) * u['ddr_byte']
    lines = (maintained_bytes if maintained_bytes is not None else
             events['clean_bytes'] + 2 * events['clean_invalidate_bytes']) / 64
    worker = lines * u['cache_line']
    if verified:
        worker += (events['digest_bytes'] * u['digest_byte'] + events['scan_words'] * u['scan_word'] +
                   events['table_words'] * u['table_word'] + events['translations'] * u['translation'] +
                   (events['digest_bytes'] + 8 * events['scan_words'] + 4 * events['table_words']) * u['ddr_byte'])
    elif page_maintenance:
        worker += events['translations'] * u['translation']
    npu_cpu = npu * CPU_MHZ / NPU_MHZ
    return dict(client_cycles=round(client + hops / 2), worker_cycles=round(worker + hops / 2),
                total_cpu_cycles=round(client + worker + hops), npu_cycles=npu,
                latency_cycles=round(client + worker + hops + npu_cpu), rows=rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', type=Path, required=True)
    ap.add_argument('--study', type=Path, required=True)
    ap.add_argument('--events', type=Path, required=True)
    ap.add_argument('--int16-log', type=Path, required=True)
    ap.add_argument('--digit-log', type=Path, required=True)
    ap.add_argument('--report', type=Path, required=True)
    args = ap.parse_args()
    counts = instruction_counts(args.elf, args.study)
    int16_events, digit_events = [json.loads(line) for line in args.events.read_text().splitlines()[:2]]
    rows, n = counts['rows'], counts['n']
    kernel = counts['kernel_insns'] / UNIT['a76_ipc']
    split_combine = counts['split_insns'] + counts['recombine_insns']
    digit = charge(digit_events, split_combine, npu_cycles(args.digit_log), rows)
    int16 = charge(int16_events, rows * n * UNIT['int16_readout'], npu_cycles(args.int16_log), rows)
    # Proposed runtime changes (separate designs): a buffer accessor removes both
    # copies, range maintenance covers tensor bytes only, read-only mappings
    # replace the digest, stream scan and per-page translations, and a
    # completion reply that also reaps removes one round trip.
    tensor_bytes = (digit_events['copy_in_bytes'] + 2 * digit_events['copy_out_bytes'] + 63) // 64 * 64
    changed = charge(digit_events, split_combine, npu_cycles(args.digit_log), rows, round_trips=2,
                     verified=False, page_maintenance=False, copies=False, maintained_bytes=tensor_bytes)

    def verdict(path):
        return dict(path, passes=path['total_cpu_cycles'] < kernel and path['latency_cycles'] < kernel,
                    cpu_shortfall=round(path['total_cpu_cycles'] / kernel, 2),
                    latency_shortfall=round(path['latency_cycles'] / kernel, 2))

    # Rows per job at which total CPU cycles match the kernel: per-row work is
    # the split, recombination, both copies and the cache maintenance of the
    # tensor bytes; verification and queue hops are per job.
    per_row = (split_combine + 2 * (digit_events['copy_in_bytes'] + digit_events['copy_out_bytes']) *
               UNIT['ddr_byte'] + (digit_events['copy_in_bytes'] + 2 * digit_events['copy_out_bytes']) / 64 *
               UNIT['cache_line']) / rows
    fixed = digit['total_cpu_cycles'] - per_row * rows
    kernel_per_row = kernel / rows
    report = dict(units=UNIT, cpu_mhz=CPU_MHZ, npu_mhz=NPU_MHZ, instruction_counts=counts,
                  a76_kernel_cycles=round(kernel), board_neon_cycles=BOARD_CYCLES_PER_ROW * rows,
                  digit_layer=verdict(digit), int16_layer=verdict(int16),
                  digit_layer_with_runtime_changes=verdict(changed),
                  break_even_rows=(round(fixed / (kernel_per_row - per_row))
                                   if kernel_per_row > per_row else None),
                  exact_path_cycles_per_row=round(per_row), kernel_cycles_per_row=round(kernel_per_row),
                  events=dict(int16=int16_events, digit=digit_events),
                  elf_sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(), physical_access=False)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('a76_kernel_cycles', 'board_neon_cycles', 'break_even_rows',
                                             'exact_path_cycles_per_row', 'kernel_cycles_per_row')}))
    for name in ('int16_layer', 'digit_layer', 'digit_layer_with_runtime_changes'):
        print(name, json.dumps(report[name]))


if __name__ == '__main__':
    main()
