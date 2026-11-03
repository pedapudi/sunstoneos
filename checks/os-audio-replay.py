#!/usr/bin/env python3
"""Check generic recorded and paced capture under sanitizers and AArch64 emulation.

The production capture unit is compiled with I2S/headset features absent. Only
the authenticated grant, clock and application liveness are modeled. Output
bytes, frame counts, source preservation, container headroom, pacing, EOF and
refusals execute the actual C implementation. No board or USB device is opened.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
from harness import machine as harness_machine, CLANG, LLD, SANITIZE, Image, environment
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build', '{build:sup}', '--output', '{output}']]
CASES = ['recorded-sequences-tail-and-headroom', 'paced-windows-and-stale-sequences',
         'short-tail-refused', 'grant-range-and-lifecycle-refusals',
         'all-container-formats-after-WAV-header', 'clock-regression-retained',
         'source-required-and-ended-session', 'fractional-deadline-and-late-consumer']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    units = json.loads((a.build / 'compile_commands.json').read_text())
    unit = next(v for v in units if v['file'].endswith('/apps/os/capture.c'))
    flags = shlex.split(unit['command'])
    flags = flags[:flags.index('-o')]
    flags = [v for v in flags if v != '-DSUNSTONE_A733_OS_AUDIO=1']
    assert not any(v in flags for v in ('-DSUNSTONE_A733_OS_AUDIO=1', '-DSUNSTONE_A733_OS_HEADSET=1'))
    cross = flags + [str(ROOT / 'checks/os-audio-replay.c'), str(ROOT / 'runtime/memory.c'),
                     '-nostdlib', '--ld-path=' + str(LLD), '-Wl,-Ttext=0x41000000',
                     '-Wl,-e,audio_replay_run', '-Wl,--gc-sections', '-o', str(a.output / 'replay.elf')]
    # Host compilation uses the same production headers, native system headers,
    # and no target-specific instruction flags or cross C library headers.
    includes = [v for v in flags if v.startswith('-I')]
    host = [str(CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-DAUDIO_REPLAY_HOST=1', *includes, *SANITIZE,
            str(ROOT / 'checks/os-audio-replay.c'), '-o', str(a.output / 'replay-host')]
    for command in (host, cross):
        subprocess.run(command, check=True, env=environment())
    subprocess.run([str(a.output / 'replay-host')], check=True, env=environment())
    elf = Image(a.output / 'replay.elf', [])
    with (a.output / 'replay.elf').open('rb') as stream:
        parts = [(int(s['p_vaddr']), bytes(s.data()), int(s['p_memsz']))
                 for s in ELFFile(stream).iter_segments() if s['p_type'] == 'PT_LOAD']
    for selected, name in enumerate(CASES):
        uc = harness_machine()
        pages = set()
        for address, data, size in parts:
            for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
                if page not in pages:
                    uc.mem_map(page, 4096)
                    pages.add(page)
            uc.mem_write(address, data)
        uc.mem_map(0x50000000, 0x10000)
        uc.reg_write(UC_ARM64_REG_SP, 0x5000f000)
        uc.reg_write(UC_ARM64_REG_X0, selected)
        uc.hook_add(UC_HOOK_INTR, lambda m, *_: m.emu_stop())
        uc.emu_start(elf.address('audio_replay_run'), 0, count=4000000)
        value = int.from_bytes(uc.mem_read(elf.address('audio_replay_result'), 4), 'little')
        assert value == 1, (name, value)
    record = {'passed': True, 'physical_access': False, 'cases': CASES, 'commands': [host, cross],
              'image_sha256': hashlib.sha256((a.output / 'replay.elf').read_bytes()).hexdigest(),
              'sources_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in
                                 ('apps/os/capture.c', 'apps/os/capture.h', 'checks/os-audio-replay.c',
                                  'checks/os-audio-replay.py')}}
    (a.output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS recorded/paced capture: sanitizers and AArch64, no microphone or application dependency')


if __name__ == '__main__':
    main()
