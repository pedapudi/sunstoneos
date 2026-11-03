#!/usr/bin/env python3
"""Check cold-session freshness and bounded framing before linked watchdog arming.

Uses the real typed-session producer and slot-generation binder. Provider
policy and the board's electrical state are outside this framing model.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

from harness import CLANG, SANITIZE, Image, environment, hook_addresses, instruction_sites, system_register_access
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, arm64_const as a64

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build', '{build:stage2}', '--output', '{output}']]
SESSION, CONTRACT, RESULT, SLOT = 0x41f00000, 0x42004000, 0x42000000, 0x43000000
ENTRY, GENERATION = 0x41000000, 37


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def native_entry(image, raw):
    """Run actual enter() through refusal or image entry; model only MMIO/counter.

    SD loading/hashing and CPU ownership precede this boundary. No SD operation
    is possible here; finish() observes an uninitialized card and the modeled
    cold owner has no retained secondaries.
    """
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0x41000000, 0x7000000)
    for address, data in image.parts:
        cpu.mem_write(address, data)
    cpu.mem_write(SESSION, raw)
    header = bytearray(512)
    struct.pack_into('<Q', header, 16, GENERATION)
    struct.pack_into('<I', header, 32, 2)
    struct.pack_into('<Q', header, 40, ENTRY)
    struct.pack_into('<3Q', header, 128, 40960, len(raw), SESSION)
    cpu.mem_write(SLOT, bytes(header))
    cpu.mem_write(RESULT, struct.pack('<Q', 0x415337334f525231) + bytes([0xa5]) * 120)
    registers, writes, terminal = {}, [], []
    ticks = [24000000]

    def mmio_read(uc, offset, size, base):
        assert size == 4
        return registers.get(base + offset, 0)

    def mmio_write(uc, offset, size, value, base):
        assert size == 4
        writes.append([base + offset, value])
        registers[base + offset] = value & 0xffff if base == 0x02050000 else value

    for base in (0x02050000, 0x07050000, 0x07090000, 0x08001000):
        cpu.mmio_map(base, 0x1000, mmio_read, base, mmio_write, base)

    def special(uc, address, size, unused):
        word = int.from_bytes(uc.mem_read(address, 4), 'little')
        if word & ~31 == 0xd53be020:  # MRS CNTPCT_EL0
            ticks[0] += 24
            uc.reg_write(a64.UC_ARM64_REG_X0 + (word & 31), ticks[0])
        elif word == 0xd503205f or word == 0xd503207f:  # WFE / WFI
            terminal.append('refused')
            uc.emu_stop()
            return
        uc.reg_write(a64.UC_ARM64_REG_PC, address + 4)

    def unretained(uc, address, size, unused):
        uc.reg_write(a64.UC_ARM64_REG_X0, 0)
        uc.reg_write(a64.UC_ARM64_REG_PC, uc.reg_read(a64.UC_ARM64_REG_LR))

    def entered(uc, address, size, unused):
        terminal.append('entered')
        uc.emu_stop()

    sites = instruction_sites(image.parts, lambda word: system_register_access(word) or
                              word in (0xd503205f, 0xd503207f))
    hook_addresses(cpu, sites, special)
    hook_addresses(cpu, [image.address('a733_stage2_cpu_retained')], unretained)
    hook_addresses(cpu, [ENTRY], entered)
    cpu.reg_write(a64.UC_ARM64_REG_SP, 0x47fff000)
    cpu.reg_write(a64.UC_ARM64_REG_X0, SLOT)
    cpu.emu_start(image.address('enter'), 0, count=200000)
    assert len(terminal) == 1, 'entry did not terminate within instruction budget'
    return dict(terminal=terminal[0], writes=writes, ticks=ticks[0],
                session=bytes(cpu.mem_read(SESSION, len(raw))),
                contract=bytes(cpu.mem_read(CONTRACT, image.layouts['a733_smp_contract']['reserved'] + 48)),
                result=bytes(cpu.mem_read(RESULT, 128)),
                report=bytes(cpu.mem_read(image.address('a733_stage2_report'), 512)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='current a733-stage2 ELF/binary directory')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    host = module('stage2_host', 'apps/os/host.py')
    slots = module('stage2_slots', 'tools/boot-slots.py')
    manifest = host.manifest_tool.assign(host.manifest_tool.load(ROOT / 'apps/hello-compute/manifest.json'))
    manifest_hash = hashlib.sha256(host.manifest_tool.pack(manifest)).hexdigest()
    prefix = bytearray(host.PREFIX_BYTES)
    struct.pack_into('<3Q', prefix, 0, 0x4153373342535332, 2, host.PREFIX_BYTES)
    struct.pack_into('<Q', prefix, 40, 9)
    struct.pack_into('<I', prefix, 400, 2)

    def produced(grant=None):
        thermal = bytearray(host.SERVICE_BYTES[host.THERMAL])
        struct.pack_into('<3Q', thermal, 0, host.SERVICE_MAGIC[host.THERMAL], 1,
                         host.PREFIX_BYTES + len(thermal) + (len(grant) if grant else 0))
        services = {host.THERMAL: thermal}
        if grant:
            services[host.CPU_GRANT] = grant
        raw = host.session_bytes(prefix, services, manifest, manifest_hash, 0, host.BIND_SID, [b'Wikipedia'])
        # SID checking is independent of cold framing and uses MMIO. The model
        # leaves the optional flag clear so it exercises only this boundary.
        raw = bytearray(slots.os_session_generation(raw, GENERATION))
        struct.pack_into('<I', raw, host.PREFIX_BYTES + 28, 0)
        return bytes(raw)

    active_grant = struct.pack('<6Q4I32s', host.SERVICE_MAGIC[host.CPU_GRANT], 1, 96, 9,
                               0, 0, 3, 1794, 2002, 32, bytes([1]) * 32)
    inactive_grant = struct.pack('<6Q4I32s', host.SERVICE_MAGIC[host.CPU_GRANT], 1, 96, 9,
                                 0, 0, 0, 0, 0, 0, bytes(32))
    plain, active, inactive = produced(), produced(active_grant), produced(inactive_grant)
    grant_offset = host.session_decode(active)['offsets'][host.CPU_GRANT]
    cases = [('without-CPU-grant', plain, True), ('active-CPU-grant', active, True),
             ('inactive-CPU-grant', inactive, True)]

    def altered(name, raw, offset, fmt, value):
        bad = bytearray(raw)
        struct.pack_into(fmt, bad, offset, value)
        cases.append((name, bytes(bad), False))

    for name, offset, fmt, value in (
            ('stale-board-epoch', 24, '<Q', 17), ('wrong-board-generation', 32, '<Q', 38),
            ('zero-board-lease', 40, '<Q', 0), ('bad-board-magic', 0, '<Q', 0),
            ('bad-board-version', 8, '<Q', 3), ('bad-board-bytes', 16, '<Q', 600),
            ('bad-typed-magic', 592, '<Q', 0), ('bad-typed-version', 600, '<Q', 2),
            ('bad-typed-bytes', 608, '<I', 4096), ('unknown-flags', 620, '<I', 2),
            ('too-many-extents', 612, '<I', 0xffffffff), ('invalid-type', 656, '<I', 32),
            ('huge-payload', 660, '<I', 0xffffffff), ('truncated-payload', 660, '<I', 4096)):
        altered(name, plain, offset, fmt, value)
    for name, offset, fmt, value in (
            ('stale-CPU-epoch', 32, '<Q', 17), ('wrong-CPU-generation', 40, '<Q', 38),
            ('bad-CPU-magic', 0, '<Q', 0), ('bad-CPU-version', 8, '<Q', 2),
            ('bad-CPU-record-bytes', 16, '<Q', 88), ('zero-CPU-owner', 24, '<Q', 0)):
        altered(name, active, grant_offset + offset, fmt, value)
    altered('short-CPU-payload', active, grant_offset - 4, '<I', 88)
    altered('inactive-CPU-generation', inactive, grant_offset + 40, '<Q', GENERATION)
    altered('inactive-CPU-epoch', inactive, grant_offset + 32, '<Q', 1)
    altered('duplicate-type', plain, 656 + 8 + host.SERVICE_BYTES[host.THERMAL], '<I', host.THERMAL)
    # One-byte optional record exercises padded bounds independently of services.
    padded = bytearray(plain + struct.pack('<2I', 31, 1) + b'x' + bytes(7))
    struct.pack_into('<2I', padded, 608, len(padded), struct.unpack_from('<I', padded, 612)[0] + 1)
    cases.append(('zero-padding', bytes(padded), True))
    altered('nonzero-padding', padded, len(padded) - 1, '<B', 1)
    tail = bytearray(padded[:-1])
    struct.pack_into('<I', tail, 608, len(tail))
    cases.append(('truncated-padding', bytes(tail), False))
    trailing = bytearray(plain + bytes(8))
    struct.pack_into('<I', trailing, 608, len(trailing))
    cases.append(('unconsumed-bytes', bytes(trailing), False))
    for size in (0, 591, 592, 655, 4097):
        cases.append((f'length-{size}', bytes(size), False))

    command = [str(CLANG), '-std=c11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
               '-I', str(ROOT / 'include'), str(ROOT / 'checks/stage2-session.c'),
               '-o', str(args.output / 'validator')]
    subprocess.run(command, check=True, env=environment())
    image = Image(args.build / 'a733-stage2.elf', ('a733_smp_contract',))
    contract_layout = image.layouts['a733_smp_contract']
    results = []
    for name, raw, expected in cases:
        path = args.output / (name + '.bin')
        path.write_bytes(raw)
        subprocess.run([str(args.output / 'validator'), str(path), str(GENERATION), str(int(expected))],
                       check=True, env=environment())
        # Invalid lengths refuse before the linked entry's minimal header read.
        if len(raw) < 656 or len(raw) > 4096:
            results.append(dict(name=name, accepted=expected, linked=False))
            continue
        result = native_entry(image, raw)
        if expected:
            assert result['terminal'] == 'entered', name
            assert [address for address, _ in result['writes']] == [0x02050018, 0x02050010, 0x02050014, 0x0205000c]
            epoch = struct.unpack_from('<Q', result['session'], 24)[0]
            assert 24000000 < epoch <= result['ticks']
            stamped = bytearray(raw)
            struct.pack_into('<Q', stamped, 24, epoch)
            assert result['session'] == stamped
            contract = result['contract']
            assert struct.unpack_from('<3Q', contract) == (0x415337334f524331, 2, len(contract))
            assert struct.unpack_from('<2Q', contract, 24) == (epoch, GENERATION)
            reserved = contract_layout['reserved']
            assert struct.unpack_from('<2Q', contract, reserved) == (SESSION, len(raw))
            assert contract[reserved + 16:reserved + 48] == hashlib.sha256(stamped).digest()
        else:
            assert result['terminal'] == 'refused', name
            assert not any(0x02050000 <= address < 0x02051000 for address, _ in result['writes']), name
            assert result['session'] == raw and result['contract'] == bytes(len(result['contract'])), name
            assert result['result'] == struct.pack('<Q', 0x415337334f525231) + bytes([0xa5]) * 120
            detail = 1 if struct.unpack_from('<I', raw, 608)[0] != len(raw) else 2
            assert struct.unpack_from('<2Q', result['report'], 24) == (7, detail), name
        results.append(dict(name=name, accepted=expected, linked=True))
    result = dict(passed=True, physical_access=False, cases=results,
                  image_sha256=hashlib.sha256((args.build / 'a733-stage2.elf').read_bytes()).hexdigest(),
                  sources={path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in
                           ('boot/a733/stage2/session.h', 'boot/a733/stage2/loader.c', 'checks/stage2-session.c',
                            'checks/stage2-session.py', 'apps/os/host.py', 'tools/boot-slots.py')},
                  limits=['SD loading, electrical state and OS service policy are not modeled.'])
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS cold stage-2 session: {len(results)} sanitizer cases and linked pre-arm checks')


if __name__ == '__main__':
    main()
