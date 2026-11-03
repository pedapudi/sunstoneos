#!/usr/bin/env python3
"""Compile and exercise the reusable owner mechanism without board access."""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build-dir', '{output}']]


def run(command, env):
    return subprocess.run(command, check=True, text=True, capture_output=True, env=env).stdout


def build_and_check(build, compiler='clang-21', objdump='llvm-objdump-21'):
    build.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(build))
    binary = build / 'scheduler-owner'
    common = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror']
    sources = [str(ROOT / 'kernel/scheduler/owner.c'), str(ROOT / 'checks/scheduler-owner.c')]
    run(common + ['-DSS_OWNER_TEST_STATS', '-O1', '-g', '-pthread', '-fsanitize=address,undefined', *sources, '-o', str(binary)], env)
    print(run([str(binary)], env), end='')
    obj = build / 'scheduler-owner-aarch64.o'
    run(common + ['--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding',
                  '-O2', '-c', sources[0], '-o', str(obj)], env)
    disassembly = run([objdump, '-d', str(obj)], env)
    (build / 'scheduler-owner-aarch64.disassembly').write_text(disassembly)
    assert 'ss_owner_test_scan' not in disassembly
    assert not re.search(r'\b(?:ldaxr|stlxr|ldxr|stxr)\b', disassembly), 'LL/SC is forbidden'
    producer = disassembly.split('<ss_owner_publish>:', 1)[1].split('\n\n', 1)[0]
    assert len(re.findall(r'\bldsetl\b', producer)) == 2
    assert len(re.findall(r'\bswpal\b', producer)) == 1
    assert re.search(r'\bdmb\s+ish\b', producer)
    assert re.search(r'\bdsb\s+ishst\b', producer)
    last_put = disassembly.split('<ss_owner_put>:', 1)[1].split('\n\n', 1)[0]
    assert re.search(r'\bldaddl\b.*\bdmb\s+ish\b.*\bldar\b', last_put, re.S), \
        'last release must order reference decrement before admission load'
    # Reject cycles in direct producer control flow, allowing backwards branches
    # to shared epilogues. Indirect calls are the immutable architecture backend.
    instructions = {}
    for line in producer.splitlines():
        match = re.match(r'\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\S+)\s*(.*)', line)
        if match:
            instructions[int(match[1], 16)] = (match[2], match[3])
    addresses = list(instructions)
    edges = {}
    for i, address in enumerate(addresses):
        op, args = instructions[address]
        targets = []
        if op == 'b' or op.startswith(('b.', 'cb', 'tb')):
            target = re.search(r'0x([0-9a-f]+)', args)
            if target:
                targets.append(int(target[1], 16))
        if op not in ('b', 'br', 'ret') and i + 1 < len(addresses):
            targets.append(addresses[i + 1])
        edges[address] = [target for target in targets if target in instructions]
    active, done = set(), set()

    def visit(address):
        assert address not in active, 'producer contains a direct control-flow cycle'
        if address in done:
            return
        active.add(address)
        for target in edges[address]:
            visit(target)
        active.remove(address)
        done.add(address)

    visit(addresses[0])
    print(f'scheduler owner: producer has {len(instructions)} static instructions, '
          'excluding out-of-line backend callbacks')
    print('scheduler owner: AArch64 LSE publication has no direct retry cycle')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path.home() / '.cache/sunstoneos/build/scheduler-owner')
    parser.add_argument('--cc', default='clang-21')
    parser.add_argument('--objdump', default='llvm-objdump-21')
    args = parser.parse_args()
    build_and_check(args.build_dir.resolve(), args.cc, args.objdump)


if __name__ == '__main__':
    main()
