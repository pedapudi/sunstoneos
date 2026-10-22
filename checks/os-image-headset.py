#!/usr/bin/env python3
"""Run the integrated OS image's headset capture client against a modeled headset.

checks/os-image-headset.c includes apps/os/headset.c unchanged, links the real
8-to-16 kHz conversion (apps/bluetooth/pcm.c) and models the Bluetooth
application's segment publication with its copy, acknowledgment and ticket
rules. This runner compiles it with the exact flags of apps/os/headset.c from
a configured headset OS build (configure it with
CMAKE_EXPORT_COMPILE_COMMANDS=ON) and runs each mode under unicorn:

- 0: the pin entry admits the nominal profile and adds PG0..PG9, PM0, PM1, PM3,
  PM4 and the profile's PM latch; it refuses pins already held, workloads 1
  and 2, other headset modes and profile versions, another SID authority or
  profile digest, owners that collide with a board, thermal or each other's
  owner, another topology or fabric rate, other memory attributes and a
  missing profile; startup creates both tasks, starts the Bluetooth
  application for exactly the 60 s session and binds the client as consumer,
  and only the Bluetooth owner runs privileged;
- 1: two generations; segments before a request and the segment being filled
  at the request are discarded, the next one becomes the generation's WAV, and
  later segments are drained without changing it;
- 2, 3, 5: a segment with a lost packet, a record that changes during the copy
  and a wrong digest each end the session, stop capture and leave the segment
  unacknowledged;
- 4: the recognizer's request rules;
- 6: a failed Bluetooth application ends the session.

No device, UART, radio or Bluetooth controller is modeled.
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
CASES = ['pins-and-startup', 'two-generations', 'lost-packet-refused', 'changed-record-refused',
         'request-rules', 'wrong-digest-refused', 'failed-application']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='headset OS build with compile_commands.json')
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = json.loads((args.build / 'compile_commands.json').read_text())
    unit = next(u for u in commands if u['file'].endswith('/apps/os/headset.c'))
    flags = shlex.split(unit['command'])
    flags = flags[:flags.index('-o')]
    assert '-DSUNSTONE_A733_OS_HEADSET=1' in flags, 'a headset OS build is required'
    image = out / 'os-image-headset.elf'
    command = flags + [str(ROOT / 'checks/os-image-headset.c'), str(ROOT / 'apps/bluetooth/pcm.c'),
                       str(ROOT / 'drivers/storage/storage_digest.c'), str(ROOT / 'runtime/memory.c'),
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
        uc.emu_start(symbols['os_check_run'], 0, count=200000000)
        result = int.from_bytes(uc.mem_read(symbols['os_check_result'], 4), 'little')
        results[case] = result
        assert result == 1, (case, result)
    (out / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'cases': results, 'command': command,
        'sources': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                    for name in ('apps/os/headset.c', 'apps/bluetooth/pcm.c', 'checks/os-image-headset.c')}},
        indent=2) + '\n')
    print('PASS OS headset client: ' + ', '.join(CASES))


if __name__ == '__main__':
    main()
