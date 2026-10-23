#!/usr/bin/env python3
"""Run the integrated OS image's radio entry against a modeled session and profile.

checks/os-image-radio.c includes apps/os/radio.c unchanged and, in a resident
build, apps/os/radio_profile.c. This runner compiles it with the exact flags of
apps/os/radio.c from a configured radio OS build (configure it with
CMAKE_EXPORT_COMPILE_COMMANDS=ON) and runs it once under unicorn:

- the radio pins entry admits workload 1 with an authenticated profile whose
  radio and crypto engine owners differ from the thermal owner and, in an
  audio build, the DMA owner;
- it refuses a changed profile, either owner equal to one of those owners,
  another workload, another profile size or address, and a profile page with
  other memory attributes;
- the start entry requires the board's radio owner and network client tasks;
- in a resident build, the radio profile bounds give bootstrap and startup
  fixed offsets from the session start and the radio and its client the
  session's lifetime, and refuse a 60-second session, a profile that already
  carries a deadline and a changed profile.

No device, radio or network is modeled.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='radio OS build with compile_commands.json')
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = json.loads((args.build / 'compile_commands.json').read_text())
    unit = next(u for u in commands if u['file'].endswith('/apps/os/radio.c'))
    flags = shlex.split(unit['command'])
    flags = flags[:flags.index('-o')]
    assert '-DSUNSTONE_A733_OS_RADIO=1' in flags, 'a radio OS build is required'
    image = out / 'os-image-radio.elf'
    command = flags + [str(ROOT / 'checks/os-image-radio.c'), str(ROOT / 'drivers/storage/storage_digest.c'),
                       str(ROOT / 'runtime/memory.c'), str(ROOT / 'runtime/memmove.c'), '-nostdlib',
                       '--ld-path=' + str(args.lld), '-Wl,-Ttext=0x41000000', '-Wl,-e,os_check_run',
                       '-Wl,--gc-sections', '-o', str(image)]
    subprocess.run(command, check=True)
    with image.open('rb') as stream:
        elf = ELFFile(stream)
        segments = [(int(p['p_vaddr']), bytes(p.data()), int(p['p_memsz']))
                    for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
        symbols = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    pages = set()
    for address, data, size in segments:
        for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
            if page not in pages:
                uc.mem_map(page, 4096)
                pages.add(page)
        uc.mem_write(address, data)
    uc.mem_map(0x43000000, 0x1000)
    uc.mem_map(0x50000000, 0x10000)
    uc.reg_write(UC_ARM64_REG_SP, 0x5000f000)
    uc.hook_add(UC_HOOK_INTR, lambda machine, interrupt, unused: machine.emu_stop())
    uc.emu_start(symbols['os_check_run'], 0, count=50000000)
    result = int.from_bytes(uc.mem_read(symbols['os_check_result'], 4), 'little')
    assert result == 1, 'radio entry check stopped with code %d' % result
    lifetime = 'resident' if '-DSUNSTONE_BOARD_RESIDENT=1' in flags else 'supervised'
    audio = '-DSUNSTONE_A733_OS_AUDIO=1' in flags
    sources = ['apps/os/radio.c', 'checks/os-image-radio.c'] + (['apps/os/radio_profile.c'] if lifetime == 'resident' else [])
    (out / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'lifetime': lifetime, 'audio': audio, 'command': command,
        'sources': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources}},
        indent=2) + '\n')
    print('PASS OS radio entry (%s%s): owner admission and refusals, start%s' %
          (lifetime, ', audio' if audio else '', ', resident profile bounds' if lifetime == 'resident' else ''))


if __name__ == '__main__':
    main()
