#!/usr/bin/env python3
"""Exercise actual control code and reject AArch64 LL/SC publication."""
import argparse
import os
from pathlib import Path
import re
import resource
import subprocess
ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build-dir', '{output}']]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path.home() / '.cache/sunstoneos/build/scheduler-control')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    temporary = build / 'tmp'
    temporary.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temporary))
    common = ['/usr/bin/clang-21', '-std=c11', '-Wall', '-Wextra', '-Werror']
    sources = [ROOT / 'kernel/scheduler/freertos_control.c', ROOT / 'kernel/scheduler/owner.c', ROOT / 'checks/scheduler-control.c']
    def run(command):
        result = subprocess.run(list(map(str, command)), check=True, text=True, capture_output=True, env=env)
        if result.stderr:
            print(result.stderr, end='')
        return result.stdout
    binary = build / 'scheduler-control'
    run(common + ['-O1', '-g', '-pthread', '-fsanitize=address,undefined', *sources, '-o', binary])
    print(run(['/usr/bin/time', '-f', 'peak RSS %M KiB', binary]), end='')
    obj = build / 'control-aarch64.o'
    run(common + ['--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding', '-O2', '-c', sources[0], '-o', obj])
    dis = run(['llvm-objdump-21', '-d', obj])
    (build / 'control-aarch64.disassembly').write_text(dis)
    assert not re.search(r'\b(?:ldaxr|stlxr|ldxr|stxr)\b', dis)
    print('scheduler control: AArch64 LSE atomics contain no LL/SC')
    event_sources = [sources[0], sources[1], ROOT / 'checks/scheduler-control-events.c']
    event_flags = common + ['-O1', '-g', '-fsanitize=address,undefined', '-I', ROOT / 'kernel/scheduler']
    events = build / 'scheduler-control-events'
    run(event_flags + event_sources + ['-o', events])
    print(run([events]), end='')
    code = sources[0].read_text()
    negatives = (
        ('unlock', '    ss_owner_event(c->backend);\n}\nstatic void hint',
         '    (void)c; /* negative: release without its event */\n}\nstatic void hint',
         'previous FIFO release must precede the next wait'),
        ('completion', '    if (put || retire) ss_owner_event(c->backend);',
         '    /* negative: omit the post-callback completion event */',
         'retirement callback must precede its completion event'),
    )
    for name, before, after, assertion in negatives:
        assert code.count(before) == 1, name
        source = build / ('control-no-' + name + '-event.c')
        source.write_text(code.replace(before, after))
        negative = build / ('no-' + name + '-event')
        run(event_flags + [source, *event_sources[1:], '-o', negative])
        result = subprocess.run([str(negative)], text=True, capture_output=True, env=env)
        (build / ('no-' + name + '-event.log')).write_text(result.stdout + result.stderr)
        assert result.returncode and assertion in result.stderr, name
    print('scheduler control events: both omitted-event negative controls fail')
if __name__ == '__main__':
    main()
