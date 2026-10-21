#!/usr/bin/env python3
"""Run the integrated OS image's capture client against a modeled DMA service.

checks/os-image-audio.c includes apps/os/audio.c unchanged and models the DMA
service replies, the audio parent and the board. This runner compiles it with
the exact flags of apps/os/audio.c from a configured audio OS build (configure
it with CMAKE_EXPORT_COMPILE_COMMANDS=ON) and runs each mode under unicorn:

- 0: two generations; each captures 32,000 bytes over eight finite periods,
  publishes one WAV with the exact header and copied bytes, and the second
  first returns the held final period, reaps, closes and reopens the stream;
- 1: a discontinuous period refuses the capture with the stream held;
- 2: a stream that stops by itself refuses the capture with the stream held;
- 3: the recognizer's request rules (no generation 0, no earlier generation,
  nothing once the board has failed).

No device, DMA or audio hardware is modeled beyond the service replies.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[1]
CASES = ['two-generations-wav-and-stream-reuse', 'discontinuous-period-refused',
         'self-stopped-stream-refused', 'request-rules']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='audio OS build with compile_commands.json')
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = json.loads((args.build / 'compile_commands.json').read_text())
    unit = next(u for u in commands if u['file'].endswith('/apps/os/audio.c'))
    flags = shlex.split(unit['command'])
    flags = flags[:flags.index('-o')]
    assert '-DSUNSTONE_A733_OS_AUDIO=1' in flags, 'an audio OS build is required'
    image = out / 'os-image-audio.elf'
    command = flags + [str(ROOT / 'checks/os-image-audio.c'), str(ROOT / 'runtime/memory.c'),
                       str(ROOT / 'runtime/memmove.c'), '-nostdlib', '--ld-path=' + str(args.lld),
                       '-Wl,-Ttext=0x41000000', '-Wl,-e,os_check_run', '-Wl,--gc-sections', '-o', str(image)]
    subprocess.run(command, check=True)
    with image.open('rb') as stream:
        elf = ELFFile(stream)
        segments = [(int(p['p_vaddr']), bytes(p.data()), int(p['p_memsz']))
                    for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
        symbols = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
    results = {}
    for mode, case in enumerate(CASES):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        pages = set()
        for address, data, size in segments:
            for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
                if page not in pages:
                    uc.mem_map(page, 4096)
                    pages.add(page)
            uc.mem_write(address, data)
        uc.mem_map(0x50000000, 0x10000)
        uc.reg_write(UC_ARM64_REG_SP, 0x5000f000)
        uc.reg_write(UC_ARM64_REG_X0, mode)
        uc.hook_add(UC_HOOK_INTR, lambda machine, interrupt, unused: machine.emu_stop())
        uc.emu_start(symbols['os_check_run'], 0, count=20000000)
        result = int.from_bytes(uc.mem_read(symbols['os_check_result'], 4), 'little')
        results[case] = result
        assert result == 1, (case, result)
    (out / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'cases': results, 'command': command,
        'audio_c_sha256': hashlib.sha256((ROOT / 'apps/os/audio.c').read_bytes()).hexdigest(),
        'fixture_sha256': hashlib.sha256((ROOT / 'checks/os-image-audio.c').read_bytes()).hexdigest()}, indent=2) + '\n')
    print('PASS OS audio client: ' + ', '.join(CASES))


if __name__ == '__main__':
    main()
