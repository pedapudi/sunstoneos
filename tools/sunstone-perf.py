#!/usr/bin/env python3
"""Switch a running SunstoneOS image between the high and low performance modes.

  sunstone-perf.py --port 3-3 --manifest BUILD/os-image/link-check.json status
  sunstone-perf.py --port 3-3 --manifest ... low [--block cpu-l,cpu-b,npu]
  sunstone-perf.py --port 3-3 --manifest ... high

High is the default mode: every block at the vendor's top operating point
for the chip's speed bin, with the vendor's voltage. Low is the fastest
vendor point at the lowest voltage. The tool talks to the running OS over
USB0 operation 8 (apps/os/perf.h) and prints each block's target point and
its rate and voltage read back from the hardware
(docs/performance-modes.md).

The image manifest is the OS build's link-check record. An image without the
performance-mode service stops USB0 on operation 8, so the tool refuses such
an image before it opens the device. The command takes the board lock of the
port and refuses a device that is not the running OS image. The functions
below also serve tools/check-a733-os-supervised.py, which holds the device
during a supervised run.
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OP = 8
REPORT, READ, SWITCH = 0, 1, 2
REPORT_BYTES = 192
MAGIC = 0x4d465250
ANSWERS = ('report', 'accepted', 'busy', 'stale-epoch')
BLOCKS = ('cpu-l', 'cpu-b', 'dsu', 'gpu', 'npu')
SELECTABLE = {'cpu-l': 1, 'cpu-b': 2, 'cpu': 3, 'gpu': 8, 'npu': 16}
MODES = {'high': 1, 'low': 2}
MODE_NAMES = ('unknown', 'high', 'low', 'mixed')
RESULTS = ('none', 'running', 'done', 'refused', 'stopped')
REASONS = ('ok', 'another command is running', 'no table entry for this speed bin', 'invalid request',
           'block not owned by this image', 'thermal level is not normal', 'an owner is not ready',
           'the CPU grant has too few changes left', 'the DSU rule refuses the plan',
           'NPU session still open after 1.5 s', 'step refused', 'step expired', 'step failed',
           'a block could not be read back')
COMMANDS = ('none', 'read back', 'switch')
FIELDS = ('magic', 'version', 'bytes', 'sequence', 'epoch', 'generation', 'answer', 'number', 'accepted',
          'completed', 'state', 'command', 'result', 'reason', 'failed_block', 'boot_mode', 'mode', 'bin',
          'changes_left', 'owned', 'read_ok', 'stopped')
LAYOUT = '<4I2Q16I'
TIMEOUT = 5.0


def request(command, selection=0, epoch=0):
    """The 16-byte operation 8 request."""
    if command not in (REPORT, READ, SWITCH) or (command != SWITCH and (selection or epoch)) or selection & ~0x31f:
        raise ValueError('Invalid performance-mode request.')
    return struct.pack('<BBHIQ', OP, command, 0, selection, epoch)


def decode(raw):
    """The report's fields; raises ValueError for another layout."""
    raw = bytes(raw)
    if len(raw) != REPORT_BYTES:
        raise ValueError('Performance-mode report size differs.')
    r = dict(zip(FIELDS, struct.unpack_from(LAYOUT, raw)))
    if (r['magic'], r['version'], r['bytes']) != (MAGIC, 1, REPORT_BYTES):
        raise ValueError('Not a performance-mode report.')
    r['blocks'] = {}
    for i, name in enumerate(BLOCKS):
        target_mhz, target_mv, mhz, mv = struct.unpack_from('<4I', raw, 96 + 16 * i)
        r['blocks'][name] = {'target_mhz': target_mhz, 'target_mv': target_mv, 'mhz': mhz, 'mv': mv,
                             'owned': bool(r['owned'] >> i & 1), 'read': bool(r['read_ok'] >> i & 1),
                             'stopped': bool(r['stopped'] >> i & 1)}
    r['selection'], r['mode_requested'], r['elapsed_us'] = struct.unpack_from('<3I', raw, 176)
    if r['answer'] >= len(ANSWERS) or r['mode'] >= len(MODE_NAMES) or r['result'] >= len(RESULTS) or \
            r['reason'] >= len(REASONS):
        raise ValueError('Invalid performance-mode report field.')
    return r


def selection(mode, blocks):
    """The switch's selection word: the mode in bits 9:8, the blocks in bits 4:0."""
    return MODES[mode] << 8 | blocks


def parse_blocks(text):
    mask = 0
    for name in text.split(','):
        if name not in SELECTABLE:
            raise ValueError('Unknown block %r; use cpu-l, cpu-b, cpu, gpu or npu.' % name)
        mask |= SELECTABLE[name]
    return mask


def manifest_check(manifest):
    """The image must link the performance-mode service."""
    if manifest.get('passed') is not True or manifest.get('target') != 'sunstone-a733-os':
        raise ValueError('Not a passed integrated OS link check.')
    if manifest.get('perf_modes') is not True or manifest.get('perf_operation') != OP:
        raise ValueError('This image has no performance-mode service; operation 8 would stop its USB0.')
    return manifest['perf_boot_mode']


def device_exchange(device, same, deadline, retain=lambda raw: None, now=time.monotonic):
    """One request and its 192-byte answer on a claimed device, as
    tools/fault-report.py fetch() exchanges a fixed report. same() runs before
    every transfer; retain(raw) may save each answer."""
    def exchange(packet):
        def timeout():
            remaining = deadline - now()
            if remaining <= 0:
                raise ValueError('Performance-mode deadline expired.')
            return min(500, max(1, int(remaining * 1000)))
        same()
        if device.write(0x02, packet, timeout=timeout()) != len(packet):
            raise ValueError('Incomplete performance-mode request.')
        raw = bytearray()
        while len(raw) < REPORT_BYTES:
            same()
            part = device.read(0x81, 512, timeout=timeout())
            if not part:
                raise ValueError('Empty performance-mode fragment.')
            raw += part
            if len(raw) > REPORT_BYTES:
                raise ValueError('Performance-mode report too large.')
        retain(bytes(raw))
        return decode(raw)
    return exchange


def run(exchange, command, mode=None, blocks=None, *, now=time.monotonic, pause=time.sleep, timeout=TIMEOUT):
    """Post one command and wait for its completion; returns the final report.

    A busy answer is retried until the timeout; a stale epoch or a refusal is
    reported in the returned report, never retried."""
    end = now() + timeout
    first = exchange(request(REPORT))
    if command == READ:
        packet = request(READ)
    else:
        if blocks is None:
            blocks = first['owned'] & 0x1b
        packet = request(SWITCH, selection(mode, blocks), first['epoch'])
    while True:
        answer = exchange(packet)
        if answer['answer'] == 1:
            break
        if answer['answer'] == 3:
            raise ValueError('The OS refused the switch: the session epoch changed.')
        if now() >= end:
            raise ValueError('The OS stayed busy for %.1f s.' % timeout)
        pause(0.01)
    number = answer['number']
    while True:
        report = exchange(request(REPORT))
        if report['completed'] == number and not report['state']:
            return report
        if now() >= end:
            raise ValueError('Command %d did not finish within %.1f s.' % (number, timeout))
        pause(0.005)


def describe(report):
    """Readable lines for one report."""
    lines = ['mode %s (boot mode %s), last command: %s, %s%s' % (
        MODE_NAMES[report['mode']], MODE_NAMES[report['boot_mode']], COMMANDS[report['command']],
        RESULTS[report['result']],
        '' if not report['reason'] else ': %s%s' % (REASONS[report['reason']], '' if report['failed_block'] >= 5
                                                   else ' at ' + BLOCKS[report['failed_block']])),
        'speed bin vf%04x, CPU grant changes left %d' % (report['bin'], report['changes_left']),
        '%-6s %-17s %-19s %s' % ('block', 'target', 'read back', '')]
    for name, b in report['blocks'].items():
        target = '%4d MHz %4d mV' % (b['target_mhz'], b['target_mv']) if b['owned'] else 'not owned'
        read = '%4d MHz %4d mV' % (b['mhz'], b['mv']) if b['read'] else 'not read'
        lines.append('%-6s %-17s %-19s %s' % (name, target, read, 'clock stopped' if b['stopped'] else ''))
    return lines


def open_device(port):
    """The running OS image on the port, under its board lock."""
    import importlib.util
    import usb.core
    import usb.util
    spec = importlib.util.spec_from_file_location('sunstone_perf_usb_access', ROOT / 'tools/usb-access.py')
    access = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(access)
    node = access.discover(port)
    lock = access.board_lock(node)
    node = lock.__enter__()
    try:
        sysfs = access.target(node)
        if ((sysfs / 'bcdDevice').read_text().strip(), (sysfs / 'product').read_text().strip(),
                (sysfs / 'serial').read_text().strip()) != ('0100', 'sunstoneos native board', 'sunstoneos-a733-board'):
            raise ValueError('The device on this port is not the running OS image.')
        identity = access.identity(node)
        device = usb.core.find(idVendor=0x1f3a, idProduct=0xefe8, bus=int(node.parent.name), address=int(node.name))
        if device is None:
            raise ValueError('The OS device disappeared.')
        usb.util.claim_interface(device, 0)

        def same():
            if access.identity(node) != identity:
                raise ValueError('The USB device changed; stop.')
        return device, same, lock
    except BaseException:
        lock.__exit__(None, None, None)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('high', 'low', 'status'))
    parser.add_argument('--port', required=True, help='physical USB port of the board, for example 3-3')
    parser.add_argument('--manifest', type=Path, required=True, help="the image's os-image/link-check.json")
    parser.add_argument('--block', help='comma-separated blocks to switch: cpu-l, cpu-b, cpu, gpu, npu')
    parser.add_argument('--json', action='store_true', help='print the decoded report as JSON')
    args = parser.parse_args(argv)
    manifest_check(json.loads(args.manifest.read_text()))
    if args.command == 'status' and args.block:
        parser.error('--block applies to high and low')
    blocks = parse_blocks(args.block) if args.block else None
    device, same, lock = open_device(args.port)
    try:
        exchange = device_exchange(device, same, time.monotonic() + TIMEOUT + 1)
        report = run(exchange, READ) if args.command == 'status' else run(exchange, SWITCH, args.command, blocks)
    finally:
        lock.__exit__(None, None, None)
    print(json.dumps(report, indent=2) if args.json else '\n'.join(describe(report)))
    return 0 if report['result'] == 2 and not report['reason'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        sys.exit('Stopped: %s' % error)
