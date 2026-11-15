#!/usr/bin/env python3
"""Check the radio record's RSN classifier against the pinned wpa_supplicant parser and the PSK candidate test."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
GATE_RUNS = [['--upstream', '{cache}/sources/radio/stacks/wpa_supplicant-2.12', '--evidence', '{output}']]
ARCHIVE_SHA256 = '08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6'  # as checks/aic8800-supplicant.py

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--upstream', type=pathlib.Path, required=True)
parser.add_argument('--evidence', type=pathlib.Path, required=True)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
upstream = args.upstream.resolve()
args.evidence.mkdir(parents=True, exist_ok=False)
assert hashlib.sha256((upstream.parent/(upstream.name + '.tar.gz')).read_bytes()).hexdigest() == ARCHIVE_SHA256
compiler = shutil.which('clang-21') or shutil.which('clang')
assert compiler
flags = [compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections',
         '-include', str(root/'include/aic8800_supplicant_port.h'), '-I' + str(root/'include'),
         '-I' + str(upstream/'src'), '-I' + str(upstream/'src/utils')]
sources = [root/'checks/radio-rsn-summary.c', root/'drivers/net/a733_radio_result.c',
           upstream/'src/common/wpa_common.c', upstream/'src/utils/common.c']
commands, objects = [], []
for source in sources:
    obj = args.evidence/(source.stem + '.o')
    commands.append(flags + ['-c', str(source), '-o', str(obj)])
    objects.append(obj)
binary = args.evidence/'radio-rsn-summary'
# The parser's file also holds key derivation; unreferenced sections and their crypto references are discarded.
commands.append([compiler, '-fuse-ld=lld', '-Wl,--gc-sections', *map(str, objects), '-o', str(binary)])
commands.append([str(binary)])
(args.evidence/'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
for command in commands:
    result = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    with (args.evidence/'run.log').open('a') as log:
        log.write(result.stdout)
    print(result.stdout, end='')
    if result.returncode:
        raise SystemExit(result.returncode)
