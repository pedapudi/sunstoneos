#!/usr/bin/env python3
"""The retained failure record keeps the first failure, except that a device failure after an application failure replaces it (level 0xc).

The EL1 vector's first-failure call (SVC #0x5c0e, `.Lfail_record` of apps/os/vectors.patch) stores a failure's site, value and caller in the
RTC words that a watchdog reset preserves. An application's failure is the first record of every failure return, and a device that then
fails (the NPU, the GPU) would be invisible. The handler now lets one device failure replace an application's record, FAULT's level
becoming 0xc (SUNSTONE_CRUMB_FAIL_LEVEL_SECOND), and the decoder (tools/a733-crumbs.py) says so.

The block is cut out of the patch, assembled for AArch64 and run in Unicorn with the RTC page and the sealed lifetime word mapped, on five
seeded records: an application's record replaced by a device's (site, value and caller are the device's, FAULT's level is 0xc), an
application's record kept against another application failure, a second record kept against a third failure, a device's record kept against
another device, and a fault record kept. Two mutants must fail: the replacement removed, and the replacement without the test that the first
record is an application's. The decoder must name the replaced record.
"""
import argparse
import importlib.util
import re
import struct
import subprocess
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_SP, UC_ARM64_REG_CPACR_EL1

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/vectors.patch', 'apps/os/sealed-lifetime.h']
RTC, CODE, STACK = 0x07090000, 0x100000, 0x300000
if not __debug__:
    raise RuntimeError('checks require assertions')


def block():
    lines = (ROOT / 'apps/os/vectors.patch').read_text().split('\n')
    i = next(k for k, l in enumerate(lines) if l.startswith('+.Lfail_record:'))
    j = next(k for k in range(i, len(lines)) if lines[k].startswith('+.Lqualification:'))
    return '\n'.join(l[1:] for l in lines[i:j])


def assemble(text, directory, clang='clang-21', lld=None):
    lld = lld or Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / 'handler.S'
    source.write_text('#include "sealed-lifetime.h"\n.text\n.global entry\nentry:\n'
                      '    stp x0, x1, [sp, #-16]!\n    b .Lfail_record\n' + text.replace('eret', 'b done') +
                      '\ndone:\n    nop\n.global finish\nfinish:\n    nop\n.balign 4096\n.global os_sealed\nos_sealed:\n    .space 8192\n')
    obj, image = directory / 'handler.o', directory / 'handler.bin'
    subprocess.run([clang, '--target=aarch64-none-elf', '-I', str(ROOT / 'apps/os'),
                    '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run([str(lld), '-Ttext=%#x' % CODE, '--image-base=0', '-e', 'entry', '--oformat=binary', '-o', str(image), str(obj)], check=True,
                   capture_output=True)
    return image.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    text = block()
    assert text.count('cbnz w4, .Lfail_second') == 1
    cases = [
        # (name, FAULT, ESR, new site, expected replaced)
        ('an application failure replaced by a device failure', 0xfa0b0000, 0x80004ff0, 0x80008e5a, True),
        ('a registered storage fault names its owner', 0xfa0b0000, 0x80004ff0, 0x8106c901, True),
        ('an application failure kept against another application failure', 0xfa0b0000, 0x80004ff0, 0x80004100, False),
        ('a second record kept against a third failure', 0xfa0c0000, 0x80008e5a, 0x80009001, False),
        ('a device failure kept against another device', 0xfa0b0000, 0x80008e5a, 0x80009001, False),
        ('a fault record kept', 0xfa010400, 0x80004ff0, 0x80008e5a, False),
    ]

    def check(text, label, expect_all=True):
        code = assemble(text, args.output / label)
        symbols = subprocess.run(['llvm-nm-21', str(args.output / label / 'handler.o')], capture_output=True, text=True).stdout
        sealed = int(re.search(r'^([0-9a-f]+) T os_sealed', symbols, re.M).group(1), 16)
        finish = int(re.search(r'^([0-9a-f]+) T finish', symbols, re.M).group(1), 16)
        done = CODE + finish
        wrong = []
        for name, fault, esr, site, replaced in cases:
            uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            uc.mem_map(CODE, 0x100000); uc.mem_write(CODE, code)
            uc.mem_map(STACK, 0x10000)
            uc.mem_map(RTC, 0x1000)
            uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
            for offset, v in {0x110: fault, 0x114: esr, 0x118: 0x11111111, 0x11c: 0x22222222}.items():
                uc.mem_write(RTC + offset, struct.pack('<I', v))
            uc.mem_write(CODE + sealed + 4100, struct.pack('<I', 3))
            uc.reg_write(UC_ARM64_REG_SP, STACK + 0x8000)
            uc.reg_write(UC_ARM64_REG_X0, site)
            uc.reg_write(UC_ARM64_REG_X1, 0x5555)
            uc.reg_write(UC_ARM64_REG_X2, 0xabcd1234)
            uc.emu_start(CODE, done, count=1000)
            got = struct.unpack('<4I', bytes(uc.mem_read(RTC + 0x110, 16)))
            want = (0xfa0c0000, site, 0xabcd1234, 0x5555) if replaced else (fault, esr, 0x11111111, 0x22222222)
            if got != want:
                wrong.append('%s: %s, wanted %s' % (name, [hex(x) for x in got], [hex(x) for x in want]))
        return wrong
    problem = check(text, 'real')
    assert not problem, problem
    for name, old, new in (('the replacement removed', 'cbnz w4, .Lfail_second', 'cbnz w4, 1f'),
                           ('the replacement without the test that the first record is an application\'s', 'b.ne 1b\n    ubfx w5, w0, #12, #4',
                            'nop\n    ubfx w5, w0, #12, #4')):
        assert text.count(old) == 1, name
        assert check(text.replace(old, new), 'mutant-' + str(abs(hash(name)) % 1000)), 'the check did not notice ' + name
    spec = importlib.util.spec_from_file_location('crumbs', ROOT / 'tools/a733-crumbs.py')
    crumbs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(crumbs)
    assert crumbs.FAIL_LEVEL_SECOND == 0xc
    defines, _ = crumbs.header()
    owners = {v: k[len('QUIESCE_'):].lower().removesuffix('_owner')
              for k, v in defines.items() if k.startswith('QUIESCE_')}
    assert len(owners) == 13
    assert defines['QUIESCE_USB0'] == 12 and defines['FAIL_USB0'] != defines['FAIL_QUIESCE']
    usb0_site = (0x80000000 | defines['FAIL_USB0'] << 24 | defines['QUIESCE_USB0'] << 16 |
                 defines['FAIL_QUIESCE'] << 12 | 24)
    usb0 = crumbs.board_failure({'fault': 0xfa0c0000, 'esr': usb0_site,
                                 'elr': 0x41001234, 'far': 1}, defines, None)
    assert usb0['owner'] == 'usb0' and usb0['source_group'] == 'usb0' and usb0['code'] == 24
    for owner, name in owners.items():
        site = 0x8100c901 | owner << 16
        record = crumbs.board_failure({'fault': 0xfa0c0000, 'esr': site,
                                       'elr': 0x41001234, 'far': 0x902}, defines, None)
        assert record['owner'] == name and record['code'] == 0x901
        assert record['source_group'] == 'board' and record['value'] == '0x902'
    for phase, name in enumerate(('unverified', 'fat1', 'fat2', 'directory', 'fsinfo',
                                  'dirty1', 'dirty2', 'clean1', 'clean2', 'raw', 'failed', 'protocol')):
        site = 0x8100c921 | defines['QUIESCE_STORAGE'] << 16
        site += phase
        words = dict.fromkeys(crumbs.WORDS, 0)
        words.update(tag=0x5c0d0001, fault=0xfa0c0000, esr=site,
                     elr=0x41001234, far=0xfedcba98)
        decoded = crumbs.decode(words, 1)
        record = decoded['board_failure']
        assert record['session'] == 0xfedcba98 and record['storage_phase'] == name, record
        assert 'SESSION 0xfedcba98, phase ' + name in decoded['summary'], decoded['summary']
    print('PASS second record: a device failure replaces an application failure in the retained words (level 0xc) and nothing else replaces a record; '
          '6 seeded records, 13 owner decodes, 2 mutants fail')


if __name__ == '__main__':
    main()
