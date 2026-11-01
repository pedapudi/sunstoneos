#!/usr/bin/env python3
"""Run the integrated OS image's capture stream and the recognizer's WAV
adapter against a modeled DMA service.

checks/os-image-audio.c includes apps/os/audio.c unchanged, links
apps/mimus-board/capture.c and drivers/audio/a733_i2s.c beside it, and models
the DMA service replies, the audio parent and the board
(checks/os-image-audio-model.h). This runner compiles it with the exact flags
of apps/os/audio.c from a configured audio OS build (configure it with
CMAKE_EXPORT_COMPILE_COMMANDS=ON) and runs each mode under unicorn:

- 0: the adapter captures two generations of each length (1 s, ten whole
  periods and 5 s); each WAV has the exact header and the modeled period
  bytes, and the second generation first reaps, closes and reopens the stream;
- 1: a discontinuous period refuses the capture with the stream held;
- 2: a stream that stops by itself refuses the capture with the stream held;
- 3: the recognizer's request rules (no generation 0, no earlier generation,
  nothing once the board has failed);
- 4: each task pass reports resident progress unless a DMA request is still
  unanswered or the capture has failed; a supervised build reports nothing;
- 5: the formats, period sizes and buffers that the stream accepts, and the
  periods shorter than 8 ms that it refuses; the
  accepted rates must equal the vendor's 24.576 MHz rate family read from
  the pinned vendor I2S source, and no rate of its 22.5792 MHz family;
- 6: 8, 16, 48, 96 and 192 kHz streams with 512 to 4,096-byte periods
  deliver their periods in place, in order, with their timestamps and bytes;
- 7: a full ring drops periods and shows the gap; release order and the
  start and stop handshakes.

With --baseline REVISION, checks/os-image-audio-baseline.c runs that
revision's capture client (its apps/os/audio.c and apps/os/os.h) against the
same model, and the WAV bytes of both generations of each length must be
identical. No device, DMA or audio hardware is modeled beyond the service
replies.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1

ROOT = Path(__file__).resolve().parents[1]
CASES = ['two-generations-wav-and-stream-reuse', 'discontinuous-period-refused',
         'self-stopped-stream-refused', 'request-rules', 'progress-reports',
         'formats-periods-and-buffers', 'rates-and-period-sizes', 'full-ring-and-handshakes']
LENGTHS = [16000, 20480, 80000]


def vendor_families(i2s, platform):
    """The vendor's rate families, from checks/i2s-rates-reference.py."""
    spec = importlib.util.spec_from_file_location('i2s_rates_reference', ROOT / 'checks/i2s-rates-reference.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.vendor_families(i2s, platform)


def image(command, out, name):
    path = out / (name + '.elf')
    subprocess.run(command + ['-o', str(path)], check=True)
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        segments = [(int(p['p_vaddr']), bytes(p.data()), int(p['p_memsz']))
                    for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
        symbols = {s.name: (int(s['st_value']), int(s['st_size']))
                   for s in elf.get_section_by_name('.symtab').iter_symbols()}
    return segments, symbols


def run(segments, symbols, mode, samples, read=()):
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
    uc.reg_write(UC_ARM64_REG_X1, samples)
    uc.hook_add(UC_HOOK_INTR, lambda machine, interrupt, unused: machine.emu_stop())
    uc.emu_start(symbols['os_check_run'][0], 0, count=400000000)
    result = int.from_bytes(uc.mem_read(symbols['os_check_result'][0], 4), 'little')
    return result, {name: bytes(uc.mem_read(*symbols[name])) for name in read}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='audio OS build with compile_commands.json')
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    parser.add_argument('--vendor', type=Path, default=Path.home() / '.cache/sunstoneos/sources/i2s')
    parser.add_argument('--vendor-platform', type=Path, default=Path.home() / '.cache/sunstoneos/sources/i2s-platform')
    parser.add_argument('--baseline', help='revision whose capture client must produce the same WAV bytes')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    families = vendor_families(args.vendor, args.vendor_platform)
    commands = json.loads((args.build / 'compile_commands.json').read_text())
    unit = next(u for u in commands if u['file'].endswith('/apps/os/audio.c'))
    flags = shlex.split(unit['command'])
    flags = flags[:flags.index('-o')]
    assert '-DSUNSTONE_A733_OS_AUDIO=1' in flags, 'an audio OS build is required'
    link = [str(ROOT / 'runtime/memory.c'), str(ROOT / 'runtime/memmove.c'), '-nostdlib', '--ld-path=' + str(args.lld),
            '-Wl,-Ttext=0x41000000', '-Wl,-e,os_check_run', '-Wl,--gc-sections']
    command = flags + [str(ROOT / 'checks/os-image-audio.c'), str(ROOT / 'apps/mimus-board/capture.c'),
                       str(ROOT / 'drivers/audio/a733_i2s.c')] + link
    segments, symbols = image(command, out, 'os-image-audio')
    results, wavs = {}, {}
    for mode, case in enumerate(CASES):
        lengths = LENGTHS if mode == 0 else [16000]
        for samples in lengths:
            read = ['os_check_wav'] if mode == 0 else ['os_check_accepted', 'os_check_rates'] if mode == 5 else []
            result, memory = run(segments, symbols, mode, samples, read)
            results[case if mode else '%s-%d' % (case, samples)] = result
            assert result == 1, (case, samples, result)
            if mode == 0:
                wavs[samples] = memory['os_check_wav']
            if mode == 5:
                rates = [int.from_bytes(memory['os_check_rates'][i:i + 4], 'little')
                         for i in range(0, len(memory['os_check_rates']), 4)]
                accepted = [r for r, ok in zip(rates, memory['os_check_accepted']) if ok]
                expected = [r for r in families[24576000] if r in rates]
                assert accepted == expected, (accepted, expected)
                assert not set(accepted) & set(families[22579200]), accepted
                assert set(families[24576000]) <= set(rates) and set(families[22579200]) <= set(rates)
                results['accepted-rates'] = accepted
    record = {'passed': True, 'physical_access': False, 'cases': results, 'command': command,
              'lifetime': 'resident' if '-DSUNSTONE_BOARD_RESIDENT=1' in flags else 'supervised',
              'vendor_rate_families': {str(k): v for k, v in families.items()},
              'sources_sha256': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in
                                 ('apps/os/audio.c', 'apps/os/audio-stream.h', 'apps/mimus-board/capture.c',
                                  'apps/mimus-board/capture.h', 'checks/os-image-audio.c',
                                  'checks/os-image-audio-model.h')}}
    if args.baseline:
        base = out / 'baseline' / 'apps' / 'os'
        base.mkdir(parents=True)
        for name in ('audio.c', 'os.h'):
            (base / name).write_bytes(subprocess.run(['git', '-C', str(ROOT), 'show', args.baseline + ':apps/os/' + name],
                                                     check=True, capture_output=True).stdout)
        command = flags + ['-iquote', str(base), str(ROOT / 'checks/os-image-audio-baseline.c')] + link
        segments, symbols = image(command, out, 'os-image-audio-baseline')
        identical = {}
        for samples in LENGTHS:
            result, memory = run(segments, symbols, 0, samples, ['os_check_wav'])
            assert result == 1, ('baseline', samples, result)
            size = 44 + 2 * samples
            new, old = wavs[samples], memory['os_check_wav']
            half_new, half_old = len(new) // 2, len(old) // 2
            for g in range(2):
                a, b = new[g * half_new:g * half_new + size], old[g * half_old:g * half_old + size]
                assert a == b, ('WAV differs from the baseline', samples, g + 1)
                identical['%d-generation-%d' % (samples, g + 1)] = hashlib.sha256(a).hexdigest()
        record['baseline'] = {'revision': subprocess.run(['git', '-C', str(ROOT), 'rev-parse', args.baseline],
                                                         check=True, capture_output=True, text=True).stdout.strip(),
                              'identical_wav_sha256': identical, 'command': command}
    (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS OS audio stream: ' + ', '.join(CASES) + (', baseline WAV identical' if args.baseline else ''))


if __name__ == '__main__':
    main()
