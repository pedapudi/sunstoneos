#!/usr/bin/env python3
"""Build and run the RAM diagnostics protocol model under Unicorn.

model.c compiles the actual drivers/usb/diagnostic.c for AArch64 with a
modeled TinyUSB vendor stream, controller, host and clock. The run covers
status, a full 2,048-byte echo, log reads at offsets 0 to 6,000, rejected
headers (bad magic and oversized length), resynchronization of a truncated
request and of an abandoned response through the reset control request, a
misaligned stream, and refusal of other control requests, including the reset
request addressed to the interface. It also counts the recovery-timer SMCs. No
hardware is accessed."""
import argparse, hashlib, json, struct, subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_PC

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SMC = struct.pack('<I', 0xd4000003)
STAGES = ['status', 'echo-2048', 'log-0', 'log-2048', 'log-4096', 'log-6000', 'bad-magic', 'oversized-length',
          'truncated-reset', 'abandoned-response-reset', 'misaligned-echo', 'misaligned-reject', 'other-control-stall']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tinyusb', type=Path, required=True)
    parser.add_argument('--headers', type=Path, required=True, help='freestanding C library headers')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default='clang')
    parser.add_argument('--linker', default=str(Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image = args.output / 'usb-diagnostic-protocol.elf'
    subprocess.run([args.compiler, '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only',
                    '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-ffunction-sections', '-fdata-sections',
                    '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-DSUNSTONE_MIMUS_USB=1',
                    '-I' + str(HERE / 'include'), '-I' + str(ROOT / 'drivers/usb'), '-I' + str(args.tinyusb / 'src'),
                    '-I' + str(args.headers), str(HERE / 'model.c'), str(ROOT / 'runtime/memory.c'),
                    '-nostdlib', '--ld-path=' + args.linker, '-Wl,-Ttext=0x41000000', '-Wl,--gc-sections',
                    '-Wl,-e,model_run', '-o', str(image)], check=True)
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    smc = []
    with image.open('rb') as f:
        elf = ELFFile(f)
        loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD' and s['p_memsz']]
        merged = []
        for start, end in sorted((s['p_vaddr'] & ~0xfff, (s['p_vaddr'] + s['p_memsz'] + 0xfff) & ~0xfff) for s in loads):
            if merged and start <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([start, end])
        for start, end in merged:
            cpu.mem_map(start, end - start)
        for segment in loads:
            data = segment.data()
            cpu.mem_write(segment['p_vaddr'], data)
            if segment['p_flags'] & 1:
                smc += [segment['p_vaddr'] + i for i in range(0, len(data) - 3, 4) if data[i:i + 4] == SMC]
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
    assert smc, 'recovery-timer SMC not found'
    cpu.mem_map(0x50000000, 0x40000)
    cpu.reg_write(UC_ARM64_REG_SP, 0x5003f000)
    renewals = [0]

    def secure_call(m, address, size, unused):
        renewals[0] += 1
        m.reg_write(UC_ARM64_REG_X0, 0)
        m.reg_write(UC_ARM64_REG_PC, address + 4)

    for address in smc:
        cpu.hook_add(UC_HOOK_CODE, secure_call, begin=address, end=address)
    cpu.hook_add(UC_HOOK_INTR, lambda m, number, unused: m.emu_stop())
    cpu.emu_start(symbols['model_run'], 0, count=50_000_000)
    fields = struct.unpack('<8I16I', bytes(cpu.mem_read(symbols['model_report'], 96)))
    done, failed_stage, failed_line, panics, exits, ticks, rejected, resets = fields[:8]
    stage_ticks = dict(zip(STAGES, fields[8:8 + len(STAGES)]))
    result = dict(passed=bool(done and not failed_stage and not panics and not exits), physical_access=False,
                  image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                  diagnostic_sha256=hashlib.sha256((ROOT / 'drivers/usb/diagnostic.c').read_bytes()).hexdigest(),
                  failed_stage=STAGES[failed_stage - 1] if failed_stage else None, failed_line=failed_line,
                  panics=panics, ticks=ticks, rejected=rejected, resets=resets,
                  recovery_renewals=renewals[0], stage_ticks=stage_ticks,
                  limit='Modeled TinyUSB stream, controller and host; physical resynchronization is unqualified.')
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    assert result['passed'], result
    assert rejected == 3 and resets == 2, result
    # One renewal at start and one per tick, independent of the burst length.
    assert renewals[0] == ticks + 1, result
    # Five OUT and five IN packets used ten 1 ms polls before the burst loop.
    assert stage_ticks['echo-2048'] <= 2, result
    print('PASS: diagnostics protocol rejects, resynchronizes and pages the log without panic')


if __name__ == '__main__':
    main()
