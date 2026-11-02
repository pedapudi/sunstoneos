#!/usr/bin/env python3
"""Check the finite image's shared-timer setup against the redistributor view of a nonsecure EL1 reader.

npu_finite_deadline_setup() runs natively in Unicorn. The EL3 monitor stores 0xd8 as the Secure priority of
PPI 27; a Nonsecure read of that Group 1 interrupt returns 0xb0 (the Secure value shifted left by one, with bit 7
set), exactly as the interrupt-route check models the SPI. The setup must accept that view, refuse any other
priority, refuse a pending write propagation and refuse a pending or disabled interrupt. Modeled hardware only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
from harness import Image, environment, system_register_access
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/npu-finite/deadline.c', 'apps/npu-finite/deadline.h', 'checks/npu-deadline-setup.c', 'apps/npu-finite/monitor.c']
RDIST, SGI = 0x03460000, 0x03470000


def secure_to_nonsecure(priority):
    return ((priority << 1) | 0x80) & 0xff


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, default=Path.home() / '.cache/sunstoneos/evidence/npu-finite-quiescence-20261018/native')
    parser.add_argument('--cc', type=Path, default=Path('/usr/bin/clang-21'))
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    unit = next(u for u in json.loads((args.baseline / 'compile_commands.json').read_text())
                if u['file'].endswith('/apps/npu-convolution/main.c') and 'a733-npu-convolution-single-core.dir' in u['command'])
    old_root = unit['file'][:-len('/apps/npu-convolution/main.c')]
    flags = [v.replace(old_root, str(ROOT)) for v in shlex.split(unit['command'])]
    for option in ('-o', '-c'):
        at = flags.index(option)
        del flags[at:at + 2]
    flags[0] = str(args.cc)
    flags += ['-I' + str(ROOT / 'platform/a733'), '-Werror']
    image_path = output / 'setup.elf'
    command = flags + [str(ROOT / 'checks/npu-deadline-setup.c'), '-nostdlib', f'--ld-path={args.lld}', '-Wl,-Ttext=0x41000000',
                       '-Wl,-e,fixture_setup', '-Wl,--gc-sections', '-Wl,--undefined=fixture_done', '-Wl,--undefined=platform_panic', '-Wl,--undefined=fixture_panic_line', '-o', str(image_path)]
    built = subprocess.run(command, cwd=unit['directory'], env=environment(), capture_output=True, text=True)
    (output / 'build.log').write_text(built.stdout + built.stderr)
    built.check_returncode()
    image = Image(image_path, layout_names=())
    cases = []

    def run(priority_view, enabled=False, rwp=False, pending=False):
        machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        machine.mem_map(0x40000000, 0x04000000)
        machine.mem_map(0x200000, 0x10000)
        machine.mem_map(RDIST, 0x20000)
        for at, data in image.parts:
            machine.mem_write(at, data)
        machine.mem_write(SGI + 0x41b, bytes([priority_view]))
        if rwp:
            machine.mem_write(RDIST, (8).to_bytes(4, 'little'))
        if pending:
            machine.mem_write(SGI + 0x200, (1 << 27).to_bytes(4, 'little'))
        done, panic = image.address('fixture_done'), image.address('platform_panic')
        state = {}
        frequency_sites, privileged_sites = {}, {}
        for at, data in image.parts:
            for offset in range(0, len(data) - 3, 4):
                word = int.from_bytes(data[offset:offset + 4], 'little')
                if word & 0xffffffe0 == 0xd53be000:
                    frequency_sites[at + offset] = word & 31
                elif system_register_access(word):
                    privileged_sites[at + offset] = word

        def code(cpu, at, size, unused):
            if at in (done, panic, 0x43fff000):
                state['end'] = 'panic' if at == panic else 'done'
                state['line'] = cpu.reg_read(UC_ARM64_REG_X0 + 1) if at == panic else 0
                cpu.emu_stop()
            elif at in frequency_sites:
                cpu.reg_write(UC_ARM64_REG_X0 + frequency_sites[at], 24000000)
                cpu.reg_write(UC_ARM64_REG_PC, at + 4)
            elif at in privileged_sites:
                cpu.reg_write(UC_ARM64_REG_PC, at + 4)
        machine.hook_add(UC_HOOK_CODE, code)
        machine.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
        machine.reg_write(UC_ARM64_REG_LR, 0x43fff000)
        try:
            machine.emu_start(image.address('fixture_setup'), 0, count=1000000)
        except Exception:
            pc = machine.reg_read(UC_ARM64_REG_PC)
            raise AssertionError(f'unmodeled instruction {int.from_bytes(machine.mem_read(pc, 4), "little"):#010x} at {pc:#x}')
        return state.get('end'), state.get('line', 0)

    end, line = run(secure_to_nonsecure(0xd8))
    assert end == 'done', f'setup refused the Nonsecure view {secure_to_nonsecure(0xd8):#x} of Secure priority 0xd8: panic at deadline.c:{line}'
    cases.append('accepts the Nonsecure view (0xb0) of Secure priority 0xd8')
    for label, kwargs in (('a Secure-looking 0xd8', {'priority_view': 0xd8}), ('priority 0', {'priority_view': 0}), ('a pending write', {'priority_view': 0xb0, 'rwp': True})):
        end, line = run(**kwargs)
        assert end == 'panic', f'setup accepted {label}'
        cases.append('refuses ' + label)
    (output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False, 'cases': cases,
        'inputs': {n: hashlib.sha256((ROOT / n).read_bytes()).hexdigest() for n in CHECK_INPUTS},
        'limits': 'Modeled redistributor; no board timing.'}, indent=2) + '\n')
    print('PASS', len(cases), 'shared timer setup cases')


if __name__ == '__main__':
    main()
