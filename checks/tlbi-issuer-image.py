#!/usr/bin/env python3
"""Check the soak images of the invalidation task (apps/os/tlbi-issuer.c, docs/designs/application-memory-layout.md, step 7).

Takes the linked OS images built with no task (--none) and with SUNSTONE_TLBI_ISSUER = 0, 1, 2 and 3. The images with no task and with
form 0 hold no broadcast TLB invalidation (a TLBI whose operation ends in IS); each of the forms 1 to 3 holds exactly one, in the
task's function, of the form the build names, followed by DSB ISH; the sets of the other TLBI instructions are the same in all five.
The form 0 image differs from the image with no task by the task alone: its set of symbols adds the task and its statistics and
nothing of the control or task code of the soak.
"""
import argparse
import re
import subprocess
from pathlib import Path

OBJDUMP = '/usr/lib/llvm-21/bin/llvm-objdump'
NM = '/usr/lib/llvm-21/bin/llvm-nm'
FORMS = {1: 'vae1is', 2: 'aside1is', 3: 'vmalle1is'}


def disassemble(image, symbol=None):
    command = [OBJDUMP, '-d', '--no-show-raw-insn'] + (['--disassemble-symbols=' + symbol] if symbol else []) + [str(image)]
    return subprocess.run(command, capture_output=True, text=True, check=True).stdout


def tlbi(text):
    return [line.split('\t', 1)[1].strip() for line in text.splitlines() if '\ttlbi\t' in line]


def broadcasts(lines):
    return [line for line in lines if re.match(r'tlbi\s+\w+is\b', line)]


def symbols(image):
    out = subprocess.run([NM, str(image)], capture_output=True, text=True, check=True).stdout
    return {line.split()[-1] for line in out.splitlines() if len(line.split()) >= 3}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('none', 'f0', 'f1', 'f2', 'f3'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    base = tlbi(disassemble(args.none))
    assert not broadcasts(base), 'the image with no task issues a broadcast invalidation: %s' % broadcasts(base)
    assert sorted(tlbi(disassemble(args.f0))) == sorted(base), 'form 0 changes the TLBI instructions of the image'
    added = symbols(args.f0) - symbols(args.none)
    assert 'os_tlbi_stats' in added and 'os_tlbi_start' in added, added
    for form, name in FORMS.items():
        image = getattr(args, 'f%d' % form)
        whole = tlbi(disassemble(image))
        assert len(broadcasts(whole)) == 1 and re.match(r'tlbi\s+%s\b' % name, broadcasts(whole)[0]), (form, broadcasts(whole))
        assert sorted(whole) == sorted(base + broadcasts(whole)), 'form %d changes another TLBI of the image' % form
        task = disassemble(image, 'issuer')
        assert tlbi(task) == broadcasts(whole), 'form %d: the invalidation is not in the task' % form
        assert re.search(r'tlbi\s+%s[^\n]*\n(?:[^\n]*\n){0,3}[^\n]*dsb\s+ish\b' % name, task), 'form %d: no DSB ISH after it' % form
    print('PASS invalidation task: one broadcast invalidation per form in the task, none in the control or the image without it')


if __name__ == '__main__':
    main()
