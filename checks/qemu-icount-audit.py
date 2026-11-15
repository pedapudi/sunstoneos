#!/usr/bin/env python3
"""Every check that starts a QEMU guest runs it on guest time that follows the instructions, or says why not.

A guest that runs without -icount counts a tick, a timeout and a clock reading in the host's time, so a verdict that reads them can change
with the speed and the load of the host: beside 24 busy processes, six of the seven cases of checks/a733-radio-session.py and one of five of
radio-network-dhcp-dns failed, and the console output of three DHCP/SNTP cases differed in every run. The radio checks now pass
harness.QEMU_DETERMINISTIC (-accel tcg,thread=single -icount shift=0,sleep=off) and give identical output in every run.

This check reads the sources of checks/*.py and tools/check-*.py. A file that starts a guest (it names -semihosting-config and QEMU) must
carry "-icount" in its command line, or be in ALLOWED with the reason that its verdict does not read guest time; an allowed file that has gained icount, or
that no longer starts a guest, is out of date and fails.
Usage: qemu-icount-audit.py --output DIR
"""
import argparse
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
ALLOWED = {
    'checks/smp-ticket-lock.py': 'multi-threaded TCG on purpose: the cores must run in parallel to exercise the ticket lock\'s atomics; the guests assert no time and the host timeout is a hang bound',
    'checks/latency-cause-model.py': 'multi-threaded TCG on purpose: the cause ping is a handshake between cores that run in parallel on hardware, and on one TCG thread each exchange waits for the peer\'s turn and the ping\'s 250 ms guest-time deadline runs out after about 27 of 256 exchanges (docs/validation.md, "The cause ping"); the verdict reads the model\'s PASS line and a broken ping\'s failed assertion, and the host timeout is a hang bound',
    'checks/latency-npu.py': 'multi-threaded TCG on purpose, as checks/latency-cause-model.py runs the same model target: its cause ping is a handshake between cores that run in parallel; the verdict reads the PASS lines and decodes the printed result, whose times are functional evidence only, and the host timeout is a hang bound',
    'tools/check-smp.py': 'multi-threaded TCG on purpose (the same atomics, the eight-core soak); the guests assert no time and the host timeout is a hang bound',
}


def fixed_icount(text):
    """The guest's command line carries -icount (a string literal) or the shared harness flags."""
    return bool(re.search(r"""['\"]-icount['\"]""", text)) or 'QEMU_DETERMINISTIC' in text


def starts_a_guest(text):
    return 'semihosting-config' in text and 'qemu' in text.lower()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    found, problems = {}, []
    for path in sorted([*(ROOT / 'checks').glob('*.py'), *(ROOT / 'tools').glob('check-*.py')]):
        relative = path.relative_to(ROOT).as_posix()
        text = path.read_text(errors='replace')
        if relative == 'checks/qemu-icount-audit.py' or not starts_a_guest(text):
            continue
        deterministic = fixed_icount(text)
        found[relative] = 'icount' if deterministic else 'allowed' if relative in ALLOWED else 'MISSING'
        if not deterministic and relative not in ALLOWED:
            problems.append(f'{relative}: starts a QEMU guest without -icount; use harness.QEMU_DETERMINISTIC, or list it in ALLOWED with the reason')
    for relative, reason in ALLOWED.items():
        text = (ROOT / relative).read_text(errors='replace') if (ROOT / relative).is_file() else ''
        if not starts_a_guest(text):
            problems.append(f'{relative}: allowed to run without icount but starts no QEMU guest; remove it from ALLOWED')
        elif fixed_icount(text):
            problems.append(f'{relative}: allowed to run without icount but now uses it; remove it from ALLOWED')
    (args.output / 'audit.json').write_text(json.dumps({'files': found, 'problems': problems}, indent=1) + '\n')
    assert not problems, '\n'.join(problems)
    print('PASS: %d files start a QEMU guest: %d on a fixed icount, %d allowed with a reason' % (
        len(found), sum(v == 'icount' for v in found.values()), sum(v == 'allowed' for v in found.values())))


if __name__ == '__main__':
    main()
