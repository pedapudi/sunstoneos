#!/usr/bin/env python3
"""Run the integrated OS image's headset capture client against a modeled headset.

checks/os-image-headset.c includes apps/os/headset.c unchanged, links the real
WAV conversions (apps/bluetooth/pcm.c) and models the Bluetooth application's
segment publication with its request word, borrow, acknowledgment and ticket
rules. This
runner compiles it with the exact flags of apps/os/headset.c from a configured
headset OS build (configure it with CMAKE_EXPORT_COMPILE_COMMANDS=ON), so the
model follows that build's radio, resident and mSBC options, and runs each
mode under unicorn:

- 0: the pin entry admits the nominal profile and adds PG0..PG9, PM0, PM1, PM3,
  PM4 and the profile's PM latch; it refuses pins already held, other
  workloads, other headset modes (mode 3 is admitted only with mSBC) and
  profile versions, another SID authority or profile digest, owners that
  collide with a board, thermal or each other's owner, another topology or
  fabric rate, other memory attributes and a missing profile. With the radio
  it also refuses a module the radio entry has not admitted, missing board
  pin authority, another PM latch, other radio platform or session owners and
  a UART owner equal to the crypto engine owner. Startup starts the Bluetooth
  application for the session's lifetime (60 s, or unbounded when resident)
  and binds the client as consumer. Without the radio it creates both tasks
  and only the Bluetooth owner runs privileged; with the radio it creates the
  client only and attaches the application to the radio's module, with the
  radio owner as Bluetooth owner and the client's own recovery latch;
- 1: keyboard generations (workload 4): no request while no generation
  waits; a new generation requests START and the next segment becomes its
  WAV; a stray segment is acknowledged without touching that WAV; after a
  headset cancel the client requests ACCEPT, so the button records the
  generation; a newer generation upgrades a pending ACCEPT to START;
- 2: an activation that publishes between the client's take and its request
  refuses the request, and a START refused while an earlier activation runs
  is requested again after that activation ends without a segment;
- 3, 5: a refused borrow and a refused request each end the session, stop
  capture and leave any segment unacknowledged;
- 4: the recognizer's request rules, and the wake-up of the client;
- 6: a failed Bluetooth application ends the session;
- 7: one owner iteration (with the radio, the module poll that the radio
  owner runs) polls the live application, retains it once the board fails
  and, in the resident build, reports progress only while it has not failed;
- 8: capture stopped by the supervised window continues; a stop for a
  capture deadline, backlog or loss ends the session;
- 9: with mSBC, a wideband segment becomes a 16 kHz WAV unchanged and an
  unknown capture state is refused; without it, a wideband segment is refused;
- 10: workload 0 admits the headset button only (ACCEPT), also after a cancel.

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
CASES = ['pins-and-startup', 'keyboard-generations', 'request-after-publication', 'refused-borrow',
         'request-rules', 'refused-request', 'failed-application', 'owner-iteration',
         'stopped-capture', 'wideband-segment', 'button-only-workload']


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
    options = {name: '-D' + macro + '=1' in flags for name, macro in
               (('radio', 'SUNSTONE_A733_OS_RADIO'), ('resident', 'SUNSTONE_BOARD_RESIDENT'), ('msbc', 'SUNSTONE_A733_OS_MSBC'))}
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
        'passed': True, 'physical_access': False, 'options': options, 'cases': results, 'command': command,
        'sources': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                    for name in ('apps/os/headset.c', 'apps/bluetooth/pcm.c', 'checks/os-image-headset.c')}},
        indent=2) + '\n')
    print('PASS OS headset client (%s): ' % ', '.join(k for k, v in options.items() if v) + ', '.join(CASES))


if __name__ == '__main__':
    main()
