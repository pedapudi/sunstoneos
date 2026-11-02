#!/usr/bin/env python3
"""Check notification transactions and native bounded atomic code generation."""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(command, env):
    return subprocess.run(command, check=True, text=True, capture_output=True, env=env).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path.home() / '.cache/sunstoneos/build/scheduler-notification')
    parser.add_argument('--cc', default='clang-21')
    parser.add_argument('--objdump', default='llvm-objdump-21')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(build))
    common = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror']
    notification = str(ROOT / 'kernel/scheduler/notification.c')
    binary = build / 'scheduler-notification'
    run(common + ['-O1', '-g', '-pthread', '-fsanitize=address,undefined',
                  str(ROOT / 'kernel/scheduler/owner.c'), notification,
                  str(ROOT / 'checks/scheduler-notification.c'), '-o', str(binary)], env)
    print(run([str(binary)], env), end='')
    obj = build / 'scheduler-notification-aarch64.o'
    run(common + ['--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding',
                  '-O2', '-c', notification, '-o', str(obj)], env)
    disassembly = run([args.objdump, '-d', str(obj)], env)
    (build / 'scheduler-notification-aarch64.disassembly').write_text(disassembly)
    assert not re.search(r'\b(?:ldaxr|stlxr|ldxr|stxr)\b', disassembly), 'LL/SC is forbidden'
    apply = disassembly.split('<ss_notification_apply>:', 1)[1].split('\n\n', 1)[0]
    assert len(re.findall(r'\bcasal\b', apply)) == 1, 'one full-word CAS required'
    for instruction in ('ldaddal', 'ldclr', 'ldset', 'ldaddl', 'stlr'):
        assert re.search(r'\b' + instruction + r'\b', apply), instruction
    # A failed CAS must lead to registration, not another CAS attempt. The only
    # intentional wait cycle is ticket serving through ss_owner_wait (WFE).
    cas = re.search(r'^\s*([0-9a-f]+):[^\n]*\bcasal\b', apply, re.M)
    after = apply[cas.end():]
    failure = re.search(r'\bb\.ne\s+0x([0-9a-f]+)', after)
    assert failure
    destination = int(failure[1], 16)
    lines = apply.splitlines()
    target = next(i for i, line in enumerate(lines) if re.match(r'\s*' + format(destination, 'x') + ':', line))
    assert any('ldaddal' in line for line in lines[target:target+4]), 'CAS conflict must register FIFO'
    print('scheduler notification: native one-CAS conflict fallback and LSE payload/count operations pass')


if __name__ == '__main__':
    main()
