#!/usr/bin/env python3
"""Fail when an OS image keeps a device owner's memory static: a .bss or .data object larger than 4 KiB in an object of the
owners that take their memory from the unified ledger when the session's grant is made (apps/os/owner-memory.h,
docs/designs/unified-memory.md, Device owners).

The owners are the NPU runtime (apps/os/npu.c and apps/os/npu-queue.c), the capture buffer (apps/os/capture.c and the
generated ss_capture_buffer), the I2S stream (apps/os/audio.c), the headset (apps/os/headset.c and apps/bluetooth/) and the USB
service and class drivers (apps/os/usb-service.c and drivers/usb/host/). The build's linker map (os-image/image.map) names the
object of every input section, and the OS compiles with -fdata-sections, so each static object is its own section. Objects of
4 KiB or less (control words, pointers, the headset profile that EL3 copies before any ledger exists) stay in the image.

EXCEPTIONS lists each object that is above the bound and has not moved yet, with the reason; an exception that the image no
longer holds fails the check, so the list cannot go stale. Before reading the build, the check runs its parser on a map with a
reintroduced 64 KiB static of apps/os/npu.c and requires that to fail.
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build', '{build:sup}', '--output', '{output}'], ['--build', '{build:headset}', '--output', '{output}']]
BOUND = 4096
OWNERS = ('apps/os/npu.c', 'apps/os/npu-queue.c', 'apps/os/capture.c', 'apps/os/audio.c', 'apps/os/headset.c',
          'apps/bluetooth/', 'apps/os/usb-service.c', 'drivers/usb/host/')
# Generated per application (tools/sunstone-manifest.py); the buffer is taken from the ledger at admission.
GENERATED = ('ss_capture_buffer',)
# (object, section): reason. Each entry is still in the image.
EXCEPTIONS = {
    ('apps/os/npu-queue.c', '.bss.direct'):
        'the NPU direct queue (9,384 bytes) moves into the NPU extent with a board run of the NPU queue',
}
# One input section of an lld map: VMA, LMA, size, alignment, then object:(section).
LINE = re.compile(r'^\s*[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(\S+):\((\.(?:bss|data|tbss|tdata)\.[^)]*)\)\s*$')


def owner_of(path, section):
    """The owner source of an input section, or None."""
    for owner in OWNERS:
        if owner in path:
            return owner if not owner.endswith('/') else path[path.index(owner):].removesuffix('.obj').removesuffix('.o')
    name = section.split('.', 2)[-1]
    return 'generated ' + name if name in GENERATED else None


def statics(lines):
    """(owner source, section, bytes) of every owner object above the bound."""
    found = []
    for line in lines:
        m = LINE.match(line)
        if not m or int(m.group(1), 16) <= BOUND:
            continue
        owner = owner_of(m.group(2), m.group(3))
        if owner:
            found.append((owner, m.group(3), int(m.group(1), 16)))
    return found


def verdict(found):
    """The failures: objects above the bound that are not exceptions, and exceptions the image no longer holds."""
    held = {(owner, section) for owner, section, _ in found}
    failures = ['%s %s: %d bytes of static device-owner memory' % (o, s, n) for o, s, n in found if (o, s) not in EXCEPTIONS]
    failures += ['exception %s %s is no longer in the image; remove it from EXCEPTIONS' % key
                 for key in EXCEPTIONS if key not in held]
    return failures


def self_test():
    """A reintroduced static buffer of an owner fails; the same map without it passes."""
    direct = '        41400000         41400000     24a8    8         CMakeFiles/x.dir/apps/os/npu-queue.c.obj:(.bss.direct)'
    probe = '        41401000         41401000    10000    16         CMakeFiles/x.dir/apps/os/npu.c.obj:(.bss.npu)'
    small = '        41411000         41411000       90    16         CMakeFiles/x.dir/apps/os/npu.c.obj:(.bss.npu)'
    assert not verdict(statics([direct, small])), 'the parser refuses a map with no static owner memory'
    assert verdict(statics([direct, probe])) == ['apps/os/npu.c .bss.npu: 65536 bytes of static device-owner memory'], \
        'the parser misses a reintroduced static buffer'
    assert verdict(statics([small])), 'the parser misses a stale exception'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--build', type=Path, required=True, help='an OS build directory (its os-image/image.map is read)')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    self_test()
    found = statics((args.build / 'os-image/image.map').read_text().splitlines())
    failures = verdict(found)
    (args.output / 'result.json').write_text(json.dumps(
        {'passed': not failures, 'build': str(args.build), 'bound': BOUND, 'above_bound': found, 'failures': failures},
        indent=1) + '\n')
    for failure in failures:
        print('FAIL ' + failure)
    if failures:
        raise SystemExit(1)
    print('PASS no device owner keeps an object above %d bytes static (%d named exception)' % (BOUND, len(EXCEPTIONS)))


if __name__ == '__main__':
    main()
