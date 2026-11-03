#!/usr/bin/env python3
"""Check where the latency diagnostic issues broadcast TLB invalidations (docs/designs/application-memory-layout.md, step 7).

Takes the linked OS images of the latency diagnostic built with SUNSTONE_LATENCY_TLBI = 0, 1, 2 and 3. The image of the
control (0) holds no broadcast invalidation (a TLBI whose operation ends in IS) anywhere. Each of the others holds exactly
one, in ss_latency_tick_hook, of the form the build names: 1 TLBI VAE1IS, 2 TLBI ASIDE1IS, 3 TLBI VMALLE1IS, followed in the
same function by DSB ISH. Nothing else of the image differs in its set of TLBI instructions.
"""
import argparse
import re
import subprocess
from pathlib import Path

OBJDUMP = '/usr/lib/llvm-21/bin/llvm-objdump'
FORMS = {1: 'vae1is', 2: 'aside1is', 3: 'vmalle1is'}


def tlbi_lines(image, symbol=None):
    command = [OBJDUMP, '-d', '--no-show-raw-insn']
    if symbol:
        command.append('--disassemble-symbols=' + symbol)
    text = subprocess.run(command + [str(image)], capture_output=True, text=True, check=True).stdout
    return [line.split('\t', 1)[1].strip() for line in text.splitlines() if '\ttlbi\t' in line]


def broadcasts(lines):
    return [line for line in lines if re.match(r'tlbi\s+\w+is\b', line)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for form in (0, 1, 2, 3):
        parser.add_argument('--tlbi%d' % form, type=Path, required=True, help='image built with SUNSTONE_LATENCY_TLBI=%d' % form)
    args = parser.parse_args()
    control = tlbi_lines(args.tlbi0)
    assert not broadcasts(control), 'the control image issues a broadcast invalidation: %s' % broadcasts(control)
    assert not broadcasts(tlbi_lines(args.tlbi0, 'ss_latency_tick_hook')), 'the control tick hook invalidates'
    for form, name in FORMS.items():
        image = getattr(args, 'tlbi%d' % form)
        whole, hook = broadcasts(tlbi_lines(image)), tlbi_lines(image, 'ss_latency_tick_hook')
        assert len(whole) == 1 and re.match(r'tlbi\s+%s\b' % name, whole[0]), (form, whole)
        assert hook == whole, (form, hook, whole)
        assert sorted(tlbi_lines(image)) == sorted(control + whole), 'form %d changes another TLBI of the image' % form
        text = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '--disassemble-symbols=ss_latency_tick_hook', str(image)],
                              capture_output=True, text=True, check=True).stdout
        assert re.search(r'tlbi\s+%s[^\n]*\n(?:[^\n]*\n){0,3}[^\n]*dsb\s+ish\b' % name, text), 'form %d: no DSB ISH after the invalidation' % form
    print('PASS latency diagnostic: one broadcast invalidation per form, in the tick hook, none in the control')


if __name__ == '__main__':
    main()
