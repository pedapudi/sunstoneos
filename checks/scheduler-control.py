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
    # Control code runs on any task's stack, so no frame may exceed the smallest
    # task stack. A whole-control compound literal once needed 42 KiB.
    words = int(re.search(r'#define configMINIMAL_STACK_SIZE\s+(\d+)',
                          (ROOT / 'include/FreeRTOSConfig.h').read_text())[1])
    run(common + ['--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding', '-O2',
                  f'-Wframe-larger-than={words * 8}', '-c', sources[0], '-o', obj])
    dis = run(['llvm-objdump-21', '-d', obj])
    (build / 'control-aarch64.disassembly').write_text(dis)
    assert not re.search(r'\b(?:ldaxr|stlxr|ldxr|stxr)\b', dis)
    print(f'scheduler control: AArch64 LSE atomics contain no LL/SC; no frame exceeds {words * 8} bytes')
    event_sources = [sources[0], sources[1], ROOT / 'checks/scheduler-control-events.c']
    event_flags = common + ['-O1', '-g', '-fsanitize=address,undefined', '-I', ROOT / 'kernel/scheduler']
    events = build / 'scheduler-control-events'
    run(event_flags + event_sources + ['-o', events])
    print(run([events]), end='')
    code = sources[0].read_text()
    old_budget = build / 'control-old-epoch-budget.c'
    marker = 'unsigned limit = visited_count >= SS_OWNER_BATCH ? 0 : SS_OWNER_BATCH - visited_count;'
    assert code.count(marker) == 1
    old_budget.write_text(code.replace(marker, '(void)visited_count; unsigned limit = SS_OWNER_BATCH;'))
    old_budget_binary = build / 'control-old-epoch-budget'
    run(common + ['-O1', '-g', '-pthread', '-fsanitize=address,undefined', '-I', ROOT / 'kernel/scheduler',
        old_budget, sources[1], ROOT / 'checks/scheduler-control.c', '-o', old_budget_binary])
    result = subprocess.run([str(old_budget_binary)], text=True, capture_output=True, env=env)
    (build / 'control-old-epoch-budget.log').write_text(result.stdout + result.stderr)
    assert result.returncode and 'one maintenance epoch has one total batch budget' in result.stderr
    print('scheduler control: resetting the budget within an epoch fails its negative control')
    negatives = (
        ('unlock', '    atomic_fetch_add_explicit(&s->serving, 1, memory_order_release);\n    ss_owner_event(c->backend);',
         '    atomic_fetch_add_explicit(&s->serving, 1, memory_order_release);\n    /* negative: release without its event */',
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
    atomic_sources = [sources[0], sources[1], ROOT / 'kernel/scheduler/notification.c',
                      ROOT / 'checks/scheduler-control-atomic.c']
    atomic_flags = event_flags + ['-DSS_CONTROL_PARK_TEST=1']
    atomic = build / 'control-atomic'
    run(atomic_flags + atomic_sources + ['-o', atomic])
    print(run([atomic]), end='')
    for name, flag, assertion in (
        ('fifo-publication', 'SS_CONTROL_OLD_PUBLICATION_NEGATIVE', 'interrupted publisher cannot release this FIFO'),
        ('record-revisit', 'SS_CONTROL_REVISIT_NEGATIVE', 'same record must wait for a new maintenance epoch'),
    ):
        negative = build / ('old-' + name)
        run(atomic_flags + ['-D' + flag + '=1', *atomic_sources, '-o', negative])
        result = subprocess.run([str(negative)], text=True, capture_output=True, env=env)
        (build / ('old-' + name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode and assertion in result.stderr, name
    print('scheduler control atomic: old FIFO publication and repeated-visit controls fail')
    park_begin = code.index('void ss_control_park_on(')
    bit_begin = code.index('    atomic_fetch_or_explicit(&c->parked[cpu].word', park_begin)
    bit_end = code.index('\n', bit_begin) + 1
    cookie_begin = code.index('    atomic_store_explicit(&c->parked_on[cpu][slot]', bit_end)
    cookie_end = code.index('\n', cookie_begin) + 1
    old_park = build / 'control-old-park-order.c'
    old_park.write_text(code[:bit_begin] + code[cookie_begin:cookie_end] +
                        code[bit_end:cookie_begin] + code[bit_begin:bit_end] + code[cookie_end:])
    old_park_binary = build / 'control-old-park-order'
    run(atomic_flags + [old_park, *atomic_sources[1:], '-o', old_park_binary])
    result = subprocess.run([str(old_park_binary)], text=True, capture_output=True, env=env)
    (build / 'control-old-park-order.log').write_text(result.stdout + result.stderr)
    assert result.returncode and 'release during repark must not strand its parked bit' in result.stderr
    print('scheduler control parking: old cookie-before-bit order strands a renewed waiter')
    traced = build / 'control-atomic-traced'
    run(atomic_flags + ['-DSUNSTONE_LATENCY=1', '-DSUNSTONE_SCHED_LATENCY_TRACE=1',
        '-DSS_SCHED_TRACE_HOST_TEST=1', '-DSS_SCHED_TRACE_WAITS=1',
        '-DSS_SCHED_TRACE_TICKS=1', '-DSS_SCHED_TRACE_HISTORY=1', *atomic_sources, '-o', traced])
    print(run([traced]), end='')
    print('scheduler control tracing: an actual failed FIFO try retains its attempt and parking costs')
    omitted = build / 'control-no-busy-record.c'
    marker = '        ss_sched_trace_record(cpu, &trace_record);\n#endif\n        return;\n    }\n#ifdef SUNSTONE_SCHED_LATENCY_TRACE\n    trace_record.stamp[1]'
    assert code.count(marker) == 1
    omitted.write_text(code.replace(marker, marker.replace('        ss_sched_trace_record(cpu, &trace_record);',
                                                          '        /* old: omit failed FIFO attempt */')))
    no_busy = build / 'control-no-busy-record'
    run(atomic_flags + ['-DSUNSTONE_LATENCY=1', '-DSUNSTONE_SCHED_LATENCY_TRACE=1',
        '-DSS_SCHED_TRACE_HOST_TEST=1', '-DSS_SCHED_TRACE_WAITS=1',
        '-DSS_SCHED_TRACE_TICKS=1', '-DSS_SCHED_TRACE_HISTORY=1', omitted, *atomic_sources[1:], '-o', no_busy])
    result = subprocess.run([str(no_busy)], text=True, capture_output=True, env=env)
    (build / 'control-no-busy-record.log').write_text(result.stdout + result.stderr)
    assert result.returncode and 'busy_records' in result.stderr
    print('scheduler control tracing: omitting failed FIFO attempts fails its negative control')
if __name__ == '__main__':
    main()
