#!/usr/bin/env python3
"""Check which control requests reset the RAM diagnostics channel.

audit-device-minors-usb-control.c compiles the actual drivers/usb/diagnostic.c
through the protocol model for AArch64. The run offers the reset request with
standard, class, vendor-interface, vendor-endpoint, reserved and IN request
types, which must all stall without a reset, and then the documented
bmRequestType 0x40, which must reset the channel once. No hardware is
accessed."""
import argparse, json, struct, subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tinyusb', type=Path, required=True)
    parser.add_argument('--headers', type=Path, required=True, help='freestanding C library headers')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default='clang')
    parser.add_argument('--linker', default=str(Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image = args.output / 'usb-control.elf'
    model = HERE / 'usb-diagnostic-protocol'
    subprocess.run([args.compiler, '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only',
                    '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-ffunction-sections', '-fdata-sections',
                    '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-DSUNSTONE_MIMUS_USB=1',
                    '-I' + str(HERE), '-I' + str(model / 'include'), '-I' + str(ROOT / 'drivers/usb'),
                    '-I' + str(args.tinyusb / 'src'), '-I' + str(args.headers),
                    str(HERE / 'audit-device-minors-usb-control.c'), str(ROOT / 'runtime/memory.c'),
                    '-nostdlib', '--ld-path=' + args.linker, '-Wl,-Ttext=0x41000000', '-Wl,--gc-sections',
                    '-Wl,-e,control_run', '-o', str(image)], check=True)
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    with image.open('rb') as f:
        elf = ELFFile(f)
        loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD' and s['p_memsz']]
        start = min(s['p_vaddr'] for s in loads) & ~0xfff
        end = (max(s['p_vaddr'] + s['p_memsz'] for s in loads) + 0xfff) & ~0xfff
        cpu.mem_map(start, end - start)
        for segment in loads:
            cpu.mem_write(segment['p_vaddr'], segment.data())
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
    cpu.mem_map(0x50000000, 0x40000)
    cpu.reg_write(UC_ARM64_REG_SP, 0x5003f000)
    cpu.hook_add(UC_HOOK_INTR, lambda m, number, unused: m.emu_stop())
    cpu.emu_start(symbols['control_run'], 0, count=1_000_000)
    done, failed, accepted, resets = struct.unpack('<4I', bytes(cpu.mem_read(symbols['control_report'], 16)))
    result = dict(passed=bool(done and not failed and accepted and resets == 1), physical_access=False,
                  failed=failed, accepted=accepted, resets=resets)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    assert result['passed'], result
    print('PASS: only the vendor device-recipient OUT request resets the diagnostics channel')


if __name__ == '__main__':
    main()
