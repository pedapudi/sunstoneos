#!/usr/bin/env python3
"""Compare the USB0 descriptors that two linked AArch64 images return.

Each image's actual TinyUSB descriptor callbacks run under Unicorn. The check
models only tud_speed_get(), which selects the high- or full-speed
configuration. Every pair must return identical device, qualifier,
configuration, other-speed configuration and string descriptors. No hardware
is accessed."""
import argparse, hashlib, json, struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC

RETURN = 0x7fff0000
STACK = 0x7ffe0000


def machine(path):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD' and s['p_memsz']]
        ranges = sorted((s['p_vaddr'] & ~0xfff, (s['p_vaddr'] + s['p_memsz'] + 0xfff) & ~0xfff) for s in loads)
        merged = []
        for start, end in ranges:
            if merged and start <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([start, end])
        for start, end in merged:
            cpu.mem_map(start, end - start)
        for segment in loads:
            cpu.mem_write(segment['p_vaddr'], segment.data())
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()
                   if s['st_info']['type'] == 'STT_FUNC' and s['st_info']['bind'] == 'STB_GLOBAL'}
    cpu.mem_map(STACK, 0x10000)
    cpu.mem_map(RETURN, 0x1000)
    speed = [2]

    def hook(m, address, size, unused):
        if address == symbols['tud_speed_get']:
            m.reg_write(UC_ARM64_REG_X0, speed[0])
            m.reg_write(UC_ARM64_REG_PC, m.reg_read(UC_ARM64_REG_LR))

    cpu.hook_add(UC_HOOK_CODE, hook, begin=symbols['tud_speed_get'], end=symbols['tud_speed_get'])
    return cpu, symbols, speed


def call(cpu, address, *args):
    for reg, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1), args):
        cpu.reg_write(reg, value)
    cpu.reg_write(UC_ARM64_REG_SP, STACK + 0xff00)
    cpu.reg_write(UC_ARM64_REG_LR, RETURN)
    cpu.emu_start(address, RETURN, count=200000)
    assert cpu.reg_read(UC_ARM64_REG_PC) == RETURN, hex(address)
    return cpu.reg_read(UC_ARM64_REG_X0)


def descriptors(path):
    cpu, symbols, speed = machine(path)
    result = {}

    def read(pointer, length):
        return bytes(cpu.mem_read(pointer, length)).hex() if pointer else None

    for name, value in (('high', 2), ('full', 0)):
        speed[0] = value
        device = call(cpu, symbols['tud_descriptor_device_cb'])
        result[name + '-device'] = read(device, 18)
        result[name + '-qualifier'] = read(call(cpu, symbols['tud_descriptor_device_qualifier_cb']), 10)
        for kind in ('configuration', 'other_speed_configuration'):
            pointer = call(cpu, symbols[f'tud_descriptor_{kind}_cb'], 0)
            total = struct.unpack('<H', bytes(cpu.mem_read(pointer + 2, 2)))[0]
            result[f'{name}-{kind}'] = read(pointer, total)
            result[f'{name}-{kind}-1'] = call(cpu, symbols[f'tud_descriptor_{kind}_cb'], 1)
    for index in range(6):
        pointer = call(cpu, symbols['tud_descriptor_string_cb'], index, 0x409)
        length = bytes(cpu.mem_read(pointer, 1))[0] if pointer else 0
        result[f'string-{index}'] = read(pointer, length)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pair', nargs=3, action='append', required=True, metavar=('NAME', 'BEFORE', 'AFTER'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows, failed = [], []
    for name, before, after in args.pair:
        old, new = descriptors(before), descriptors(after)
        same = old == new
        if not same:
            failed.append(name)
        device = bytes.fromhex(new['high-device'])
        rows.append(dict(name=name, identical=same, vid=hex(struct.unpack_from('<H', device, 8)[0]),
                         pid=hex(struct.unpack_from('<H', device, 10)[0]), bcd=hex(struct.unpack_from('<H', device, 12)[0]),
                         before_sha256=hashlib.sha256(Path(before).read_bytes()).hexdigest(),
                         after_sha256=hashlib.sha256(Path(after).read_bytes()).hexdigest(),
                         descriptors=new if same else dict(before=old, after=new)))
        print(f"{'PASS' if same else 'FAIL'} {name} {rows[-1]['vid']}:{rows[-1]['pid']} bcd {rows[-1]['bcd']}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(dict(passed=not failed, physical_access=False, images=rows), indent=2) + '\n')
    if failed:
        raise SystemExit('Descriptor bytes changed: ' + ', '.join(failed))


if __name__ == '__main__':
    main()
