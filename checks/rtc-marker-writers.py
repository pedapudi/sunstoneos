#!/usr/bin/env python3
"""Fail when code outside the card gate and the runner can write the cold-boot marker; no device access.

The first-stage gate boots the card only while RTC IC_CHARA_REG (0x070901f0,
A733 manual 9.1.7.20) reads zero, which means the RTC supply was lost
(docs/designs/card-fel-recovery.md). A writer that clears or sets that
register outside the gate's cold path and the runner's first FEL contact
would change which resets boot the card. This check finds every tracked
source that names the register, by absolute address in any spelling, by its
manual name, or as RTC base plus offset 0x1f0, and requires each to be one of
the reviewed writers or their checks. The search covers literal spellings
only. The RTC owners that write by a caller's offset (apps/os/rtc.c,
apps/rtc/initialize.c, apps/rtc/native.c, apps/rtc/runtime.c and
apps/rtc/service-port.c) admit fixed offsets at or below 0x30, which exclude
0x1f0; an offset computed elsewhere is outside this check.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps', 'arch', 'boot', 'cmake', 'drivers', 'include', 'kernel', 'lib', 'network', 'patches', 'platform',
                'runtime', 'tools', 'checks',
                'boot/a733/card-fel-gate.S', 'include/a733_card_fel_gate.h', 'tools/board.py', 'tools/a733-crumbs.py',
                'tools/card_recovery.py']
# The gate's cold path writes the marker; the runner sets it at first contact.
WRITERS = {'boot/a733/card-fel-gate.S', 'include/a733_card_fel_gate.h', 'tools/board.py', 'tools/a733-crumbs.py',
           'tools/card_recovery.py'}
# Checks that model or execute those writers.
# checks/stage2_marks.py names the marker only to refuse it: the stage-2 models fail on any stage-2 write to it.
CHECKS = {'checks/card-fel-recovery.py', 'checks/board-runner.py', 'checks/rtc-marker-writers.py', 'checks/stage2_marks.py'}
SUFFIXES = {'.c', '.h', '.S', '.s', '.py', '.patch', '.cmake', '.ld', '.txt', '.json', '.sh', '.cc', '.cpp'}
PATTERNS = [
    re.compile(r'0x0*70901f0\b', re.I),                       # absolute address
    re.compile(r'\b117965296\b'),                             # the same address in decimal
    re.compile(r'IC_?CHARA', re.I),                           # the manual's register name
    re.compile(r'(\bRTC\w*|0x0*7090000)\s*\+\s*0x0*1f0\b', re.I),  # base plus offset
    re.compile(r'\bA733_CARD_FEL_RTC_MARKER\b|\bRTC_MARKER\b'),
]


def tracked():
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z'], check=True, capture_output=True).stdout
    return [name for name in out.decode().split('\0') if name and not name.startswith('docs/')]


def hits(name, text):
    return [(n, line.strip()) for n, line in enumerate(text.splitlines(), 1)
            if any(p.search(line) for p in PATTERNS)]


def self_test():
    """Each spelling is found; unrelated 0x1f0 offsets are not."""
    found = ['x = 0x070901f0;', 'ldr r4,=0x70901F0', 'w(RTC + 0x1f0, v)', 'w(0x07090000+0x01f0,v)',
             'IC_CHARA_REG', 'v=117965296', 'ldr r4,=A733_CARD_FEL_RTC_MARKER']
    missed = ['patch(p,0x198,0x1f0,12u<<4);', 'word(&p, LCD + 0x1f0, 1, 3);', '#define A733_PINS_PB_MASK UINT32_C(0x1f0)']
    assert all(hits('x', line) for line in found)
    assert not any(hits('x', line) for line in missed)


def main():
    self_test()
    found = {}
    for name in tracked():
        path = ROOT / name
        if path.suffix not in SUFFIXES and path.name != 'CMakeLists.txt' or not path.is_file():
            continue
        text = path.read_text(errors='replace')
        lines = hits(name, text)
        if lines:
            found[name] = lines
    outside = {name: lines for name, lines in found.items() if name not in WRITERS | CHECKS}
    for name, lines in sorted(outside.items()):
        for n, line in lines:
            print(f'{name}:{n}: names the cold-boot marker outside its reviewed writers: {line}', file=sys.stderr)
    if outside:
        sys.exit(1)
    missing = {'boot/a733/card-fel-gate.S', 'include/a733_card_fel_gate.h', 'tools/a733-crumbs.py'} - set(found)
    if missing:
        sys.exit('The reviewed writers no longer name the marker: ' + ', '.join(sorted(missing)))
    print('PASS cold-boot marker writers: %d reviewed files, no other source names RTC 0x1f0' % len(found))


if __name__ == '__main__':
    main()
