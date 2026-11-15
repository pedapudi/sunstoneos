#!/usr/bin/env python3
"""Check the NSI port 6 counter guard on the GPU power path: the restore before the GPU_TOP OFF command, refusal while
it is off, use after ON, and the restore at the next power-on of a bank refused during a call. Mutations that move the
restore after the OFF command, drop the gate test, drop the power-on restore or drop the holder's restore must fail."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
POWER = 'drivers/gpu/a733_gpu_power.c'
GUARD = 'drivers/dram/a733_nsi_counters_guard.c'
SOURCES = ['checks/nsi-counters-gpu-power.c', 'drivers/power/a733_pmic.c', 'drivers/power/a733_rail_leases.c',
           'drivers/clock/a733_clock_leases.c', 'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c',
           'drivers/pinctrl/a733_pin_tables.c', 'drivers/power/a733_power_domains.c',
           'drivers/power/a733_pmic_consumer.c', POWER, 'drivers/gpu/a733_gpu_platform.c',
           'drivers/dram/a733_nsi_counters.c', GUARD]
HOOK = '    if (!on) os_nsi_counters_power_off(0);   /* before every OFF step: GPU_TOP is still on */\n'
DOMAIN_DONE = '            s->requested = 0;\n'
OFF_DONE = '    if (on) os_nsi_counters_power_on(0);   /* after every ON step: GPU_TOP is on */\n'
GATE = '!(atomic_load(&gated) >> b & 1u) &&'
POWER_ON_TAKE = '    if (take()) release();\n}'
CALL_RELEASE = '    release();\n    return r;'


def replaced(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f'mutation anchor not found once: {old!r}')
    return text.replace(old, new)


# Each mutation names the file it changes and the change; the mutated build must fail.
MUTATIONS = {
    # The restore right after the TOP5 OFF step has completed.
    'restore-after-domain-off': (POWER, lambda t: replaced(replaced(t, HOOK, ''), DOMAIN_DONE,
        DOMAIN_DONE + '            if (!on) os_nsi_counters_power_off(0);\n')),
    # The restore at the end of the whole OFF.
    'restore-after-off': (POWER, lambda t: replaced(replaced(t, HOOK, ''), OFF_DONE,
        OFF_DONE + '    if (!on) os_nsi_counters_power_off(0);\n')),
    # The guard without its gate test: accesses while GPU_TOP is off reach the bus.
    'no-gate-test': (GUARD, lambda t: replaced(t, GATE, '')),
    # The power-on clears the gate but restores nothing.
    'no-power-on-restore': (GUARD, lambda t: replaced(t, POWER_ON_TAKE, '}')),
    # A holder gives the owner up without restoring a stale bank.
    'no-holder-restore': (GUARD, lambda t: replaced(t, CALL_RELEASE, '    give();\n    return r;')),
}


def build(output, name, sources):
    binary = output / name
    command = ['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DSUNSTONE_ACCEL_NSI_COUNTERS=1',
               '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'checks'), *map(str, sources), '-o', str(binary)]
    return command, [str(binary)]


def run(output, index, command):
    result = subprocess.run(command, capture_output=True, text=True,
                            env={**harness.environment(), 'ASAN_OPTIONS': 'detect_leaks=0'})
    (output / f'{index}.log').write_text(result.stdout + result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    steps = []
    index = 0

    def step(command, expect_pass):
        nonlocal index
        result = run(args.output, index, command)
        index += 1
        steps.append({'command': command, 'exit': result.returncode, 'expect_pass': expect_pass})
        harness.save(args.output / 'result.json', {'physical_access': False, 'steps': steps})
        return result

    compile_command, run_command = build(args.output, 'nsi-counters-gpu-power', [ROOT / s for s in SOURCES])
    step(compile_command, True).check_returncode()
    result = step(run_command, True)
    print(result.stdout + result.stderr, end='')
    result.check_returncode()
    for name, (path, mutate) in MUTATIONS.items():
        mutated = args.output / name / Path(path).name
        mutated.parent.mkdir(exist_ok=True)
        mutated.write_text(mutate((ROOT / path).read_text()))
        sources = [mutated if s == path else ROOT / s for s in SOURCES]
        compile_command, run_command = build(args.output / name, 'binary', sources)
        step(compile_command, True).check_returncode()
        result = step(run_command, False)
        if result.returncode == 0:
            raise SystemExit(f'FAIL mutation {name} passed: {result.stdout.strip()}')
        print(f'mutation {name} fails as required: {(result.stdout + result.stderr).strip().splitlines()[-1]}')


if __name__ == '__main__':
    main()
