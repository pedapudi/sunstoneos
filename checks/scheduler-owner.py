#!/usr/bin/env python3
"""Compile and exercise the reusable owner mechanism without board access."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

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
    # The threaded race model stops with the inconclusive status when the host does not schedule its owner thread (checks/scheduler-owner.c);
    # it is run once more, and a second stop is inconclusive (harness.KillInconclusive). Its claim is stated in the owner's own passes.
    done = harness.until_booted(lambda: subprocess.run([str(binary)], text=True, capture_output=True, env=env,
                                                       timeout=harness.host_kill_seconds('scheduler-owner')),
                                lambda finished: finished.stdout + finished.stderr, lambda finished: finished.returncode)
    assert done.returncode == 0, (done.returncode, done.stdout + done.stderr)
    print(done.stdout, end='')
    # A hint the owner loses (one slot in one round, which the owner never inspects) must fail the race model by the owner's own idle passes,
    # in seconds, with its message; the host-time guard would end the run inconclusive (status 75) and is not the claim.
    lost = build / 'scheduler-owner-lost-hint'
    run(common + ['-DSS_OWNER_TEST_STATS', '-DSS_RACE_LOSE_ONE', '-O1', '-g', '-pthread', '-fsanitize=address,undefined', *sources, '-o', str(lost)], env)
    broken = subprocess.run([str(lost)], text=True, capture_output=True, env=env, timeout=harness.host_kill_seconds('scheduler-owner'))
    assert broken.returncode not in (0, harness.INCONCLUSIVE_EXIT) and 'slot 7 never inspected, with' in broken.stderr, \
        ('the lost-hint model was not failed by the owner passes', broken.returncode, broken.stderr[-400:])
    print('scheduler owner: a lost hint fails the race model by the owner\'s idle passes: ' + broken.stderr.strip().splitlines()[-1])
    obj = build / 'scheduler-owner-aarch64.o'
    run(common + ['--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding',
                  '-O2', '-c', sources[0], '-o', str(obj)], env)
    disassembly = run([objdump, '-d', str(obj)], env)
    (build / 'scheduler-owner-aarch64.disassembly').write_text(disassembly)
    assert 'ss_owner_test_scan' not in disassembly
    assert not re.search(r'\b(?:ldaxr|stlxr|ldxr|stxr)\b', disassembly), 'LL/SC is forbidden'
    producer = disassembly.split('<ss_owner_publish>:', 1)[1].split('\n\n', 1)[0]
    # The outbox path stores the entry and releases the count; only the spill
    # path uses read-modify-writes. No store-to-load barrier: the owner's
    # dispatch state is never read.
    assert len(re.findall(r'\bstlrh\b', producer)) == 1
    assert len(re.findall(r'\bldsetl\b', producer)) == 1
    assert len(re.findall(r'\bldaddl\b', producer)) == 1
    assert not re.search(r'\bdmb\b', producer), 'publication needs no DMB'
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
