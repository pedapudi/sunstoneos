#!/usr/bin/env python3
"""Run checks/a733-iommu-owner.c: the OS-image IOMMU owner with G2D and the decoder in one session, and a mutant that installs a second directory.

The owner (drivers/iommu/a733_iommu_owner.c) must pass every case. The mutant is the same source with a later attach
that allocates a new directory and points both banks' TTB at it; the test must fail it, since G2D's pages no longer
walk from bank 1.

Usage: a733-iommu-owner.py --evidence DIR"""
import argparse
import pathlib
import shutil
import subprocess
import sys
GATE_RUNS = [['--evidence', '{output}']]

root = pathlib.Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument('--evidence', type=pathlib.Path, required=True)
a = ap.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
cc = shutil.which('clang-21') or shutil.which('clang') or shutil.which('gcc')
owner = root / 'drivers/iommu/a733_iommu_owner.c'
later = '''    if(o->state == A733_IOMMU_OWNER_ACTIVE) {
'''
second = later + '''        uint32_t *c2; uint64_t p2;
        if(o->ops.table(o->context, A733_IOMMU_OWNER_DIRECTORY_BYTES + ALIAS_BYTES, A733_IOMMU_OWNER_DIRECTORY_BYTES,
                        &c2, &p2) != 1) return -1;
        memset(c2, 0, A733_IOMMU_OWNER_DIRECTORY_BYTES + ALIAS_BYTES);
        o->directory = c2; o->directory_physical = p2;
        if(both(o, TTB, (uint32_t)(p2 >> 14)) != 1) return -1;
'''
text = owner.read_text()
if text.count(later) != 1:
    sys.exit('the mutant no longer applies: the later-attach branch of a733_iommu_owner_attach changed')
mutant = a.evidence / 'a733_iommu_owner_second_directory.c'
mutant.write_text(text.replace(later, second))


def build(source, name):
    binary = a.evidence / name
    command = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-Iinclude', '-Idrivers/iommu', str(source), 'drivers/iommu/a733_iommu.c',
               'checks/a733-iommu-owner.c', '-o', str(binary)]
    done = subprocess.run(command, cwd=root, capture_output=True, text=True)
    if done.returncode:
        sys.exit(done.stderr)
    return subprocess.run([str(binary)], cwd=root, capture_output=True, text=True)


done = build(owner, 'owner')
print(done.stdout, end='')
if done.returncode:
    sys.exit(done.stderr or 'the owner failed the host test')
done = build(mutant, 'second-directory')
(a.evidence / 'second-directory.txt').write_text(done.stdout + done.stderr)
if done.returncode != 1 or 'FAIL' not in done.stdout:
    sys.exit('the second-directory mutant was not detected:\n' + done.stdout + done.stderr)
print('mutant (second directory) detected: %d failed conditions' % done.stdout.count('FAIL '))
