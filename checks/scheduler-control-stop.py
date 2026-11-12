#!/usr/bin/env python3
"""Run the actual control recorder with modeled architectural boundaries.

The constructed counter values test phase arithmetic, stop ownership and final
publication. They do not model board interrupt latency. The timer mechanism is
documented in docs/designs/scheduler-partitioned.md under the control-interrupt
heading. Negative controls restore the former stale-IRQ, stop and arithmetic
defects and must fail the corresponding cases.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['runtime/control_irq.c', 'runtime/control_ring.h', 'include/sunstone_control.h',
                'apps/latency/control.c', 'apps/latency/control.h',
                'checks/scheduler-control-stop.c', 'checks/control-stop-config']
GATE_RUNS = [['--output', '{output}']]
CASES = ('arithmetic', 'overflow-only', 'task-stop', 'unarmed-stop', 'last-task-write',
         'nested-control-stop', 'coalesced-notification', 'consumer-stop', 'start-validation',
         'unselected-stale', 'empty-stop', 'qualification-idle', 'qualification-busy')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--skip-negative', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    compiler = shutil.which('clang-21')
    if not compiler:
        raise SystemExit('clang-21 is required')
    env = dict(os.environ, TMPDIR=str(output))
    base = [compiler, '-std=c11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I' + str(ROOT / 'checks/control-stop-config'), '-I' + str(ROOT / 'include'),
            '-I' + str(ROOT / 'runtime'), '-I' + str(ROOT / 'apps/latency')]
    sources = [str(ROOT / 'checks/scheduler-control-stop.c'), str(ROOT / 'apps/latency/control.c')]
    records, commands = [], []

    def build(name, definitions=()):
        binary = output / name
        command = [*base, *definitions, *sources, '-o', str(binary)]
        result = subprocess.run(command, capture_output=True, text=True, env=env)
        (output / (name + '-compile.log')).write_text(result.stdout + result.stderr)
        commands.append(command)
        if result.returncode:
            raise RuntimeError(result.stderr)
        return binary

    def run(binary, case, failure=None, timeout=20):
        command = [str(binary), case]
        try:
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)
            text, code = result.stdout + result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            text = ((error.stdout or b'') + (error.stderr or b'')).decode(errors='replace')
            code = 124
        expected = (code == 0 and 'PASS control stop and qualification model' in text) if failure is None else \
            (code == 124 if failure == 'timeout' else code != 0 and failure in text)
        log = output / (binary.name + '-' + case + '.log')
        log.write_text(text)
        records.append({'command': command, 'exit_code': code, 'expected_failure': failure,
                        'passed': expected, 'log': str(log)})
        if not expected:
            raise RuntimeError(f'{binary.name} {case}: unexpected result {code}\n{text[-2000:]}')

    binary = build('model')
    for case in args.case or CASES:
        run(binary, case)
    disabled = build('no-task-state', ['-DINCLUDE_eTaskGetState=0'])
    run(disabled, 'suspended-query-disabled')
    diagnostic = ['-DSUNSTONE_LATENCY=1', '-DconfigSS_PARTITIONED_SCHEDULER=1']
    acknowledged = build('saved-frame-ack', diagnostic)
    run(acknowledged, 'suspension-frame-ack')

    if not args.skip_negative:
        source = (ROOT / 'runtime/control_irq.c').read_text()
        saved_frame = '    return ss_latency_task_suspended(task);'
        assert source.count(saved_frame) == 1
        old_state = output / 'logical-state-only.c'
        old_state.write_text(source.replace(saved_frame, '    return eTaskGetState(task) == eSuspended;'))
        bad = build('logical-state-only', [*diagnostic, '-DSS_CONTROL_SOURCE="' + str(old_state) + '"'])
        run(bad, 'suspension-frame-ack', '!ulSunstoneControlTasksSuspended()')
        task_start = source.index('static void handle_task_path(')
        guard = '    if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) { stop_local(s, 1); return; }\n'
        assert source[task_start:].count(guard) == 1
        old_task = source[:task_start] + source[task_start:].replace(guard, '', 1)
        variants = [('old-task-stop', old_task, 'task-stop', 'state[1].task_irq_count == before', 20)]
        early_ack = source.replace('if (stop) { stop_local(s, 0); return; }',
                                   'if (stop) { stop_local(s, 1); return; }')
        assert early_ack != source
        variants.append(('early-control-ack', early_ack, 'nested-control-stop',
                         '!__atomic_load_n(&state[1].isr_stopped, __ATOMIC_ACQUIRE)', 20))
        start = source.index('void __wrap_vApplicationIRQHandler(')
        end = source.index('#ifdef SUNSTONE_CONTROL_TASK_EL1', start)
        old_wrapper = '''void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
    const unsigned core = this_core();
    if (core >= FIRST_CORE && core < CORES) {
        struct core_state *s = &state[core];
        if (interrupt == INTID && s->armed) { handle(s); return; }
        if (interrupt == TASK_INTID && s->armed) { handle_task_path(s); return; }
        if (__atomic_load_n(&s->requested, __ATOMIC_ACQUIRE) && !s->armed && !s->stop) arm(s);
    }
    __real_vApplicationIRQHandler(interrupt);
}

'''
        variants.append(('old-stale-ownership', source[:start] + old_wrapper + source[end:],
                         'task-stop', 'native_irqs[TASK_INTID] + native_irqs[INTID] == native_before', 20))
        start = source.index('static int advance(')
        end = source.index('static int serve(', start)
        old_advance = '''static int advance(struct path *p, uint64_t now, uint64_t *served, unsigned *missed)
{
    uint64_t next = p->deadline + p->period;
    *served = p->deadline; *missed = 0;
    while (next <= now) { next += p->period; ++*missed; }
    p->deadline = next;
    return 1;
}
'''
        variants.append(('old-deadline-loop', source[:start] + old_advance + source[end:],
                         'overflow-only', 'timeout', 2))
        start = source.index('static uint32_t ticks_to_ns(')
        end = source.index('static uint64_t ticks_to_us(', start)
        old_ns = '''static uint32_t ticks_to_ns(unsigned core, uint32_t ticks)
{ (void)core; return ticks * 1000u / 24u; }
'''
        variants.append(('old-ns-overflow', source[:start] + old_ns + source[end:],
                         'arithmetic', 'ticks_to_ns(1, 24000000) == 1000000000', 20))
        for name, changed, case, failure, timeout in variants:
            path = output / (name + '.c')
            path.write_text(changed)
            bad = build(name, ['-DSS_CONTROL_SOURCE="' + str(path) + '"'])
            run(bad, case, failure, timeout)

    files = [ROOT / 'runtime/control_irq.c', ROOT / 'runtime/control_ring.h',
             ROOT / 'include/sunstone_control.h', ROOT / 'include/sunstone_latency.h', ROOT / 'apps/latency/control.c',
             ROOT / 'apps/latency/control.h', ROOT / 'checks/scheduler-control-stop.c',
             ROOT / 'checks/control-stop-config/FreeRTOS.h', ROOT / 'checks/control-stop-config/task.h']
    result = {'passed': True, 'physical_access': False, 'constructed_time': True,
              'reference': 'docs/designs/scheduler-partitioned.md#control-interrupts-above-the-api-mask-do-not-wait-for-kernel-spans',
              'inputs': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
              'build_commands': commands, 'cases': records}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'cases': len(records), 'physical_access': False}))


if __name__ == '__main__':
    main()
