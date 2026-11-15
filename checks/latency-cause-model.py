#!/usr/bin/env python3
"""The latency cause model's functional result: its cause ping completes, and a broken ping fails the model.

apps/latency/model.c runs the cacheline ping of apps/latency/cause-ping.c between CPU0 and CPU1 and between CPU0 and CPU6 (256 exchanges
each, a 250 ms deadline) and asserts that every exchange completed (result.ping.status), then prints "PASS latency cause model". Nothing
required that line: checks/tick-footprint-budget.py runs the same image to trace the busy ticks and judges only the trace. When that step
moved to one TCG thread with a fixed icount, the ping began to time out after about 30 exchanges (docs/validation.md, "The cause ping"),
and the step went on passing.

The ping is a handshake between cores that run in parallel on hardware. On one TCG thread the cores take turns, each exchange waits for
the peer's turn, and the 250 ms of guest time runs out; on QEMU's multi-threaded TCG, where the cores run in parallel as on the board,
it completes in about 2 s of host time. So this check runs the image multi-threaded, as an instrumented hardware-like run:
  passes   the unmodified image prints "PASS latency cause model" and exits 0;
  breaks   the image rebuilt with its peer reporting a sequence error after the fourth exchange (the source of apps/latency/cause-ping.c
           changed in a copy, compiled with the image's own flags and linked with its own link command) must exit non-zero with the failed
           assertion of the ping status (the line of "configASSERT(result.ping.status == LATENCY_CAUSE_PING_OK)" in model.c).
Each image runs once. The check holds the timing lock (timing_checks, tools/gate-slots.json), and the image's own bounds cover the
host it gets there: 193 runs of the cause model made four at once under the timing lock on 2026-11-15, at load averages 11 to 75,
all passed, with the ping at most 84.9 ms of its 250 ms deadline and the run at most 2.2 s of its 12 s budget. A run that misses
either bound fails the check.
Usage: latency-cause-model.py --build DIR --output DIR
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build', '{build:latency-cause-model}', '--output', '{output}'],
             ['--build', '{build:latency-wake-model}', '--output', '{output}'],
             ['--build', '{build:latency-local-model}', '--output', '{output}']]
TARGET = 'latency-partitioned-pmu-model'
CACHE = harness.CACHE
MUTATION = ("            status = wait_turn(2u * i + 1, deadline);\n            if (status != LATENCY_CAUSE_PING_OK) break;",
            "            status = i == 3 ? LATENCY_CAUSE_PING_SEQUENCE : wait_turn(2u * i + 1, deadline);\n"
            "            if (status != LATENCY_CAUSE_PING_OK) break;")


def run_guest(qemu, image, seconds):
    """The image on QEMU's multi-threaded TCG: the cores run in parallel, as on the board."""
    command = [str(qemu), '-machine', 'virt,gic-version=3,secure=off,virtualization=off', '-accel', 'tcg,thread=multi', '-cpu', 'cortex-a55',
               '-smp', '8', '-m', '128M', '-nographic', '-monitor', 'none', '-semihosting-config', 'enable=on,target=native',
               '-device', f'loader,file={image},cpu-num=0']
    done = harness.run_qemu(command, seconds)
    return done.returncode, done.stdout + done.stderr


# The peer's report of the planted defect: LATENCY_CAUSE_PING_SEQUENCE is status 5 (apps/latency/cause-ping.h).
PEER_FAILED = re.compile(r'^latency cause ping failed: pair=[0-9] status=5$', re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    args = parser.parse_args()
    build, out = args.build.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    seconds = harness.qemu_kill_seconds('latency-cause-model')
    image = build / (TARGET + '.elf')
    code, log = run_guest(args.qemu, image, seconds)
    (out / 'qemu-pass.log').write_text(log)
    assert code == 0 and 'PASS latency cause model' in log and 'FAIL' not in log, ('the cause model did not pass', out / 'qemu-pass.log')
    print('PASS cause ping completes: the model printed its PASS line on the multi-threaded guest')

    model = (ROOT / 'apps/latency/model.c').read_text()
    needle = 'configASSERT(result.ping.status == LATENCY_CAUSE_PING_OK);'
    assert model.count(needle) == 1, 'the ping assertion of model.c moved'
    expected = 'FAIL src/apps/latency/model.c:0x%016x' % (model[:model.index(needle)].count('\n') + 1)
    source = (ROOT / 'apps/latency/cause-ping.c').read_text()
    assert source.count(MUTATION[0]) == 1, 'the peer loop of cause-ping.c changed; update the mutation'
    mutated = out / 'cause-ping-mutated.c'
    mutated.write_text(source.replace(*MUTATION, 1))
    directory = build / 'CMakeFiles' / (TARGET + '.dir')
    flags = dict(re.findall(r'^(C_(?:DEFINES|INCLUDES|FLAGS)) = (.*)$', (directory / 'flags.make').read_text(), re.M))
    compile_flags = shlex.split(' '.join(flags[key] for key in ('C_DEFINES', 'C_INCLUDES', 'C_FLAGS')))
    link = shlex.split((directory / 'link.txt').read_text())
    original = 'CMakeFiles/%s.dir/apps/latency/cause-ping.c.obj' % TARGET
    assert original in link, 'the image does not link apps/latency/cause-ping.c'
    obj = out / 'cause-ping-mutated.o'
    compiler = [link[0], '--target=aarch64-none-elf']
    done = subprocess.run([*compiler, *compile_flags, '-c', str(mutated), '-o', str(obj)], cwd=build, capture_output=True, text=True)
    assert done.returncode == 0, done.stderr[-2000:]
    broken = out / 'latency-cause-model-broken.elf'
    command = [str(obj) if word == original else word for word in link]
    command[command.index('-o') + 1] = str(broken)
    command = [word.replace(str(build / (TARGET + '.map')), str(out / 'broken.map')) for word in command]
    done = subprocess.run(command, cwd=build, capture_output=True, text=True)
    assert done.returncode == 0, done.stderr[-2000:]
    # The planted defect is detected when the model exits non-zero without its PASS line and either the ping assertion fails or the
    # peer that met the planted sequence error reports it (apps/latency/workload.c, ss_latency_cause_ping_failed).
    code, log = run_guest(args.qemu, broken, seconds)
    (out / 'qemu-broken.log').write_text(log)
    detected = expected in log or PEER_FAILED.search(log)
    assert code != 0 and detected and 'PASS latency cause model' not in log, ('the broken ping did not fail the model', expected, out / 'qemu-broken.log')
    (out / 'result.json').write_text(json.dumps({'passed': True, 'expected_failure': expected, 'ping_assertion': expected in log}, indent=1) + '\n')
    print('PASS broken ping fails the model: ' + ('the ping assertion ' + expected if expected in log else 'the peer reported the planted sequence error'))


if __name__ == '__main__':
    main()
