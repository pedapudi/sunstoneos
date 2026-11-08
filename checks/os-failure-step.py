#!/usr/bin/env python3
"""A failure with the NPU queue open returns cleanly when the application's NPU owner closes it, and ends at the watchdog otherwise.

docs/designs/failure-return.md: a failure marks the application finished (ss_live() is -1, board.fatal stays clear); the application's NPU
owner closes its queue when it sees that; the NPU client publishes its report; the supervisor waits for that for 3 s. The supervisor does
not stop the NPU itself. Run 3's test image showed why: the supervisor's own stop (6eb9a01e, os_npu_thermal from the supervisor) took the NPU
client's retained path, a device failure that sets board.fatal, and bounded() ended the return at EL3 with 0x736.

1. The source of failure_claim holds no task-level API and no NPU stop, and the supervisor holds none either.
2. checks/os-failure-step.c drives the step functions the way the supervisor and bounded() drive them against a model of the owner and
   of the client: an owner that closes within 100 ms returns cleanly within 300 ms at 40 different instants; one that never closes ends at
   the 3 s limit (0x742); a device failure ends at the bounded() stop with the NPU's source line in the detail (0x800 | line); a normal
   return that finds the NPU open stops at once (0x741).

Four mutants must fail: the supervisor stopping the NPU from outside (which reproduces the 0x736 stop of that run: the model's client takes
the retained path), the old stop at once with 0x741 during a failure return, bounded() that ignores board.fatal, and the stop put into
failure_claim.
"""
import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(text, name):
    start = text.index(name + '(')
    start = text.rindex('\n', 0, start) + 1
    depth, i = 0, text.index('{', start)
    begin = i
    while True:
        depth += text[i] == '{'
        depth -= text[i] == '}'
        i += 1
        if depth == 0:
            return text[begin:i]


def source_problem(text):
    claim = function(text, 'static int failure_claim')
    if re.search(r'xTaskNotify|vTaskNotify|ulTaskNotify|os_npu_thermal|xTaskResume|vTaskResume', claim):
        return 'failure_claim calls a task-level API'
    if 'os_npu_thermal' in function(text, 'void os_cpu_return_supervise'):
        return 'the supervisor stops the NPU from outside'
    return None


def build(out, step_text, name):
    step = out / name
    step.mkdir(parents=True, exist_ok=True)
    (step / 'cpu-return-step.h').write_text(step_text)
    binary = step / 'model'
    subprocess.run(['clang-21', '-std=c11', '-O1', '-g', '-w' if name != 'real' else '-Wall', '-fsanitize=address,undefined', '-I' + str(step),
                    str(ROOT / 'checks/os-failure-step.c'), '-o', str(binary)], check=True, capture_output=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    text = (ROOT / 'apps/os/cpu-return.c').read_text()
    assert source_problem(text) is None, source_problem(text)
    step = (ROOT / 'apps/os/cpu-return-step.h').read_text()
    real = build(args.output, step, 'real')
    assert real.returncode == 0, real.stderr
    print(real.stdout.strip())
    claim = function(text, 'static int failure_claim')
    mutated = text.replace(claim, claim.replace('atomic_store_explicit(&failing,1,memory_order_release);',
                                                'atomic_store_explicit(&failing,1,memory_order_release);\n    os_npu_thermal(0,1,0,0);', 1), 1)
    assert source_problem(mutated) == 'failure_claim calls a task-level API'
    sup = function(text, 'void os_cpu_return_supervise')
    assert source_problem(text.replace(sup, sup.replace('case OS_FAILURE_STEP_WAIT:return;', 'case OS_FAILURE_STEP_WAIT:os_npu_thermal(0,1,0,0);return;', 1), 1)) \
        == 'the supervisor stops the NPU from outside'
    wait = 'return failing?OS_FAILURE_STEP_WAIT:OS_FAILURE_STEP_DEVICES;'
    assert step.count(wait) == 1
    outside = build(args.output, step.replace(wait, 'return failing?OS_FAILURE_STEP_STOP_NPU:OS_FAILURE_STEP_DEVICES;'), 'mutant-stops-from-outside')
    assert outside.returncode != 0, 'the check did not notice a supervisor that stops the NPU from outside'
    print('the supervisor stopping the NPU from outside reproduces the stop:', outside.stderr.strip().splitlines()[0])
    at_once = build(args.output, step.replace(wait, 'return OS_FAILURE_STEP_DEVICES;'), 'mutant-stops-at-el3')
    assert at_once.returncode != 0, 'the check did not notice a failure return that stops at EL3'
    # the thermal owner exiting at once, as before: reproduces the stop 0x744 of the NPU failure test
    thermal = 'return failing?OS_THERMAL_STEP_WAIT:OS_THERMAL_STEP_EXIT;'
    assert step.count(thermal) == 1
    exits = build(args.output, step.replace(thermal, 'return OS_THERMAL_STEP_EXIT;'), 'mutant-thermal-exits')
    assert exits.returncode != 0 and '0x744' in exits.stderr, 'the check did not reproduce the stop of the NPU failure test: ' + exits.stderr
    print('the thermal owner exiting at once reproduces the stop:', exits.stderr.strip().splitlines()[0])
    # the clock owner re-running its return branch after the lowering: reproduces the stop 0xf8e5 / second record cpu.c:235 of the fifth test
    clock = 'static inline int os_cpu_return_clock_pass(int lowering_done) { return !lowering_done; }'
    assert step.count(clock) == 1
    reruns = build(args.output, step.replace(clock, 'static inline int os_cpu_return_clock_pass(int lowering_done) { (void)lowering_done; return 1; }'), 'mutant-clock-reruns')
    assert reruns.returncode != 0 and '0x744' in reruns.stderr, 'the check did not notice a clock owner that re-runs its return while the thermal owner waits'
    print('the clock owner re-running its return while the thermal owner waits reproduces the stop:', reruns.stderr.strip().splitlines()[0])
    fatal = 'if(fatal)return npu_line?OS_BOUNDED_FATAL_NPU|(npu_line&0x3ffu):OS_BOUNDED_FATAL;'
    assert step.count(fatal) == 1
    blind = build(args.output, step.replace(fatal, ''), 'mutant-ignores-fatal')
    assert blind.returncode != 0, 'the check did not notice a bounded() that ignores board.fatal'
    print('PASS failure with the NPU open: an owner that closes returns cleanly at 40 instants, one that never closes ends at the 3 s limit, '
          'a device failure stops with the NPU line, the thermal owner waits for the retirement, the supervisor does not stop the NPU from outside; 6 mutants fail')


if __name__ == '__main__':
    main()
