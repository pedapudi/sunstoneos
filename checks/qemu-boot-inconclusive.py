#!/usr/bin/env python3
"""A QEMU guest that fails in its boot stage or is killed by the host proves nothing, and a real failure or hang still does not pass.

The guests of platform/qemu-smp and platform/qemu-os give each boot wait one second of their own counter. On a loaded host the secondary
cores can take longer, and the guest then prints "FAIL boot-timeout <wait>" before its application runs. checks/harness.py (run_qemu,
until_booted) runs the guest again and reports the claim as inconclusive when every attempt ends in boot; the guest's bound is unchanged.
Cases, with a fake guest that prints the logs of real runs:
  boot-then-pass    a boot-stage failure followed by a passing run returns the passing run (two attempts);
  boot-always       a boot-stage failure in every attempt raises BootInconclusive, which is an AssertionError and names the wait;
  model-failure     a failing model assertion (the line of the broken-ping model, FAIL src/apps/latency/model.c:0x34) is returned at the
                    first attempt, so the check that called it still fails;
  application-text  "FAIL boot-timeout" inside a line of application output is not a boot-stage failure;
  kill-then-pass    a run the wall-clock bound kills (a loaded host) is run once more and the second run is the result;
  hang-guard        a threaded host model whose own wall-clock guard fires exits 75: it is run once more, and twice is inconclusive;
  kill-always       a guest that hangs is killed twice and is inconclusive (harness.KillInconclusive), never a pass, and the process exits
                    with harness.INCONCLUSIVE_EXIT, which the gate reports as INCONCLUSIVE and not as FAIL;
  platforms         each boot-wait platform prints the marker for its three waits and no longer asserts on the counter;
  sites             every check and tool that runs a QEMU guest with more than one core runs it through the harness (device-memory-capacity
                    runs images that boot from the ROM, which has no boot wait).
Usage: qemu-boot-inconclusive.py
"""
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (enforces assertions before any decision)

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['checks/harness.py', 'platform/qemu-smp/platform.c', 'platform/qemu-os/platform.c']
BOOT = 'FAIL boot-timeout secondary-cpus-online\n'
MODEL = 'broken ping\nFAIL src/apps/latency/model.c:0x0000000000000034\n'
GUEST = r'''
import json
import os
import time
from pathlib import Path
state, outputs = Path(os.environ['GUEST_STATE']), json.loads(os.environ['GUEST_OUTPUTS'])
attempt = int(state.read_text()) if state.exists() else 0
state.write_text(str(attempt + 1))
text = outputs[min(attempt, len(outputs) - 1)]
if text == 'HANG':
    time.sleep(120)
print(text, end='')
raise SystemExit(75 if text.startswith('INCONCLUSIVE') else 1 if 'FAIL' in text else 0)
'''
# Checks that run no guest through the harness for a stated reason.
OUTSIDE = {'checks/device-memory-capacity.py': 'its images boot from the ROM; the platform has no boot wait',
           'checks/harness.py': 'defines the helpers', 'checks/qemu-boot-inconclusive.py': 'this check',
           'checks/check-index.py': 'names checks', 'checks/gate-slots.py': 'names checks',
           'checks/qemu-icount-audit.py': 'names checks', 'checks/affected-checks.py': 'names checks',
           'tools/integration-gate.py': 'names checks', 'checks/integration-gate.py': 'names checks'}


def run(directory, *outputs, timeout=30):
    state = directory / 'attempts'
    state.unlink(missing_ok=True)
    guest = directory / 'guest.py'
    guest.write_text(GUEST)
    environment = {**os.environ, 'GUEST_STATE': str(state), 'GUEST_OUTPUTS': json.dumps(outputs)}
    done = harness.run_qemu([sys.executable, str(guest)], timeout, env=environment)
    return done, int(state.read_text())


def main():
    with tempfile.TemporaryDirectory(prefix='boot-inconclusive-') as name:
        directory = Path(name)

        done, attempts = run(directory, BOOT, 'PASS model\n')
        assert attempts == 2 and done.returncode == 0 and 'PASS model' in done.stdout, ('boot-then-pass', attempts, done)
        print('PASS a boot-stage failure is run again and the passing run is the result')

        try:
            run(directory, BOOT)
        except harness.BootInconclusive as error:
            assert isinstance(error, AssertionError) and 'inconclusive' in str(error) and 'secondary-cpus-online' in str(error), str(error)
            assert (directory / 'attempts').read_text() == str(harness.BOOT_ATTEMPTS)
        else:
            raise AssertionError('boot-always: a guest that never boots was not reported inconclusive')
        print('PASS a guest that fails in boot every time is inconclusive, naming the wait')

        done, attempts = run(directory, MODEL, 'PASS model\n')
        assert attempts == 1 and done.returncode != 0 and 'FAIL src/apps/latency/model.c:0x0000000000000034' in done.stdout, \
            ('model-failure', attempts, done)
        print('PASS a model failure is returned at the first attempt and fails')

        done, attempts = run(directory, 'HANG', 'PASS model\n', timeout=2)
        assert attempts == 2 and done.returncode == 0 and 'PASS model' in done.stdout, ('kill-then-pass', attempts, done)
        print('PASS a run the host killed is run once more and the second run is the result')

        try:
            run(directory, 'HANG', timeout=2)
        except harness.KillInconclusive as error:
            assert isinstance(error, harness.Inconclusive) and isinstance(error, AssertionError) and 'wall-clock bound' in str(error) and 'stopped the run' in str(error), str(error)
            assert (directory / 'attempts').read_text() == str(harness.KILL_ATTEMPTS)
        else:
            raise AssertionError('kill-always: a guest that hangs was not reported inconclusive')
        # a threaded model's own hang guard: it exits with the inconclusive status, which is run again and then inconclusive
        done, attempts = run(directory, 'INCONCLUSIVE: no thread progressed\n', 'PASS model\n')
        assert attempts == 2 and done.returncode == 0 and 'PASS model' in done.stdout, ('guard-then-pass', attempts, done)
        try:
            run(directory, 'INCONCLUSIVE: no thread progressed\n')
        except harness.KillInconclusive as error:
            assert 'its own hang guard' in str(error) and (directory / 'attempts').read_text() == str(harness.KILL_ATTEMPTS), str(error)
        else:
            raise AssertionError('guard-always: a model that stops on its hang guard in every attempt was not reported inconclusive')
        # the exit status the gate reads: a check that does not catch the exception ends with INCONCLUSIVE_EXIT
        ended = subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, %r); import harness; raise harness.KillInconclusive("x")'
                                % str(Path(__file__).resolve().parent)], capture_output=True, text=True)
        assert ended.returncode == harness.INCONCLUSIVE_EXIT == 75 and 'INCONCLUSIVE: x' in ended.stderr, ended
        failed = subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, %r); import harness; assert False, "model"'
                                 % str(Path(__file__).resolve().parent)], capture_output=True, text=True)
        assert failed.returncode == 1 and 'AssertionError' in failed.stderr, failed
        print('PASS a guest that hangs is inconclusive, exits with the inconclusive status, and a failed assertion still exits 1')

        assert harness.boot_stage_failure('output FAIL boot-timeout primary-init-flag\nPASS\n') is None
        assert harness.boot_stage_failure('x\nFAIL boot-timeout register-clear\n') == 'register-clear'
        print('PASS application text that mentions the marker is not a boot-stage failure')

    for path in ('platform/qemu-smp/platform.c', 'platform/qemu-os/platform.c'):
        text = (ROOT / path).read_text()
        assert not re.search(r'configASSERT\((raw_)?counter\(\) < end\)', text), (path, 'a boot wait asserts on the counter')
        for wait in ('register-clear', 'secondary-cpus-online', 'primary-init-flag'):
            assert 'boot_timeout("%s")' % wait in text, (path, wait)
    print('PASS the boot-wait platforms print the boot-stage marker for their waits')

    unrouted = []
    for path in sorted([*(ROOT / 'checks').glob('*.py'), *(ROOT / 'tools').glob('*.py')]):
        relative = path.relative_to(ROOT).as_posix()
        text = path.read_text()
        if relative in OUTSIDE or 'qemu' not in text:
            continue
        # a '-smp' option whose count is not the single core
        launches = [m for m in re.finditer(r"""['"]-smp['"],\s*(?:str\([^)]*\)|['"]([0-9]+)['"])""", text) if m.group(1) != '1']
        if launches and not ('run_qemu(' in text or 'until_booted(' in text):
            unrouted.append(relative)
    assert not unrouted, ('these run a multi-core QEMU guest without the harness (run_qemu or until_booted)', unrouted)
    print('PASS every multi-core QEMU launch in checks/ and tools/ runs through the harness')

    # A kill bound read from tools/gate-slots.json is a hang guard: its users run the guest or binary through the harness, which runs it
    # again after a kill. A user that passes it to subprocess.run directly turns host load into a failure.
    bounded = []
    for path in sorted([*(ROOT / 'checks').glob('*.py'), *(ROOT / 'tools').glob('*.py')]):
        relative = path.relative_to(ROOT).as_posix()
        text = path.read_text()
        if relative in OUTSIDE or not re.search(r'(?:qemu|host)_kill_seconds\(|QEMU_KILL_SECONDS', text):
            continue
        if not ('run_qemu(' in text or 'until_booted(' in text):
            bounded.append(relative)
    assert not bounded, ('these use a measured kill bound without the harness retry', bounded)
    print('PASS every user of a measured kill bound runs through the harness')


if __name__ == '__main__':
    main()
