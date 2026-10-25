#!/usr/bin/env python3
"""Check the host fault-report reader of tools/fault-report.py against a modeled device.

The modeled device applies the operation 5 rules that it parses from
apps/mimus-board/usb0.c: a 16-byte request whose op is FAULT_OP and whose
flag, count, offset and generation fields are zero gets the 1,160-byte report
with no frame header; any other request stops the device, which then answers
nothing. Each read returns no more than it requests, in fragments of 1, 29 or
512 bytes. The cases cover the byte-for-byte result, the request bytes and
response size, the refusals and the module's static form. No image, device or
USB library is used.
"""
import argparse
import array
import ast
import importlib.util
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
FRAGMENTS = (1, 29, 512)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


tool = load('fault_report_tool', ROOT / 'tools/fault-report.py')
decoder = load('fault_report_decoder', ROOT / 'checks/fault-decode.py')
host = load('fault_report_capture', ROOT / 'apps/mimus-board/host.py')


def device_rules():
    """FAULT_OP and the request fields that operation 5 requires to be zero, from usb0.c."""
    text = (ROOT / 'apps/mimus-board/usb0.c').read_text()
    op = int(re.search(r'#define FAULT_OP (\d+)u', text)[1])
    condition = re.search(r'if\(op==FAULT_OP\)\{\s*if\(([^)]*)\)stop\(\);', text)[1]
    # The NPU report (operation 6) refuses the same fields, so Device serves both.
    assert re.search(r'if\(op==OS_NPU_REPORT_OP\)\{\s*if\(([^)]*)\)stop\(\);', text)[1] == condition
    # Where usb0.c assembles each named field from the request bytes.
    assert 'request[4+i]<<(8*i)' in text and 'request[2]|(unsigned)request[3]<<8' in text
    assert 'request[8+i]<<(8*i)' in text and 'op=request[0]' in text
    places = {'request[1]': range(1, 2), 'bytes': range(2, 4), 'offset': range(4, 8), 'expected': range(8, 16)}
    names = condition.split('||')
    assert sorted(names) == sorted(places), names
    zero = sorted(i for name in names for i in places[name])
    assert zero == list(range(1, 16)), zero
    return op, zero


FAULT_OP, ZERO_BYTES = device_rules()


def report():
    """A non-empty report: a vector fault on core 2 and a deliberate exit on core 5."""
    slots = []
    for core, kind, counter in ((2, 1, 7000), (5, 2, 9000)):
        name = struct.unpack('<2Q', b'fault-%d\0' % core + bytes(16 - len(b'fault-%d\0' % core)))
        words = [decoder.SLOT_MAGIC, 0, kind | 3 << 8 | core << 16, 1, 0x55, counter, 9,
                 0x96000045, 0x41012340, 0x10, 0x3c5, 0x41300000, *name, 0, 0]
        words[1] = decoder.slot_crc(words)
        slots.append((core, struct.pack('<16Q', *words)))
    header = decoder.REPORT_MAGIC | (1 << 2 | 1 << 5) << 32 | 1 << 48
    data = bytearray(struct.pack('<Q', header) + struct.pack('<16Q', *range(1, 17)) + bytes(8 * decoder.SLOT_BYTES))
    for core, slot in slots:
        data[136 + core * decoder.SLOT_BYTES:136 + (core + 1) * decoder.SLOT_BYTES] = slot
    return bytes(data)


class Stopped(OSError):
    """What the transport raises when a stopped device never answers."""


class Device:
    """Operation 5 of usb0.c, served in bounded fragments, with a transfer log.

    response is the operation 5 report, or a map from operation (5 or 6, whose
    request rules are the same) to the report that operation returns."""

    def __init__(self, response, fragment, *, after=b'', change_at=None, stop=False):
        self.queue = bytearray()
        self.responses = response if isinstance(response, dict) else {FAULT_OP: response}
        self.after, self.fragment = after, fragment
        self.stop = stop
        self.writes, self.reads, self.transfers = [], [], 0
        self.change_at = change_at

    def write(self, endpoint, packet, timeout):
        assert endpoint == 0x02 and 1 <= timeout <= 500
        self.transfers += 1
        self.writes.append(bytes(packet))
        ok = len(packet) == 16 and packet[0] in self.responses and not any(packet[i] for i in ZERO_BYTES)
        if ok and not self.stop:
            self.queue += self.responses[packet[0]] + self.after
        else:
            self.stop = True
        return len(packet)

    def read(self, endpoint, size, timeout):
        assert endpoint == 0x81 and 1 <= timeout <= 500
        self.transfers += 1
        self.reads.append(size)
        if self.stop or not self.queue:
            raise Stopped('modeled transfer timeout')
        part = self.queue[:min(size, self.fragment)]
        del self.queue[:len(part)]
        return array.array('B', part)


def run(device, *, deadline=1000.0, clock=None, same=None):
    retained = []
    clock = clock or (lambda: 0.0)
    result = tool.fetch(device, same or (lambda: None), deadline, retained.append, now=clock)
    return result, retained


def refused(action, *errors):
    try:
        action()
    except errors or (ValueError,) as error:
        return str(error)
    raise AssertionError('refusal expected')


def static_form():
    """The module opens no device, starts no program and has no command line."""
    path = ROOT / 'tools/fault-report.py'
    text = path.read_text()
    tree = ast.parse(text)
    imported = {alias.name.split('.')[0] for node in ast.walk(tree) if isinstance(node, ast.Import)
                for alias in node.names}
    imported |= {node.module.split('.')[0] for node in ast.walk(tree)
                 if isinstance(node, ast.ImportFrom) and node.module}
    forbidden = {'usb', 'usb1', 'libusb', 'serial', 'subprocess', 'argparse'}
    assert imported == {'struct', 'time'} and not imported & forbidden, imported
    for word in ('/dev/' + 'bus/usb', 'usb' + '-access'):
        assert word not in text, word
    mains = [node for node in ast.walk(tree) if isinstance(node, ast.Compare) and
             isinstance(node.left, ast.Name) and node.left.id == '__name__']
    assert not mains, 'a __main__ block'
    names = {node.id for node in ast.walk(tree) if isinstance(node, ast.Name)}
    names |= {node.attr for node in ast.walk(tree) if isinstance(node, ast.Attribute)}
    assert not names & {'ArgumentParser', 'argv', 'Popen', 'system'}, names
    functions = [node.name for node in tree.body if isinstance(node, ast.FunctionDef)]
    assert functions == ['fetch'], functions
    return sorted(imported)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    assert ROOT not in [out, *out.parents]
    out.mkdir(parents=True, exist_ok=False)
    cases, metrics = {}, {}
    data = report()
    decoded = decoder.decode_report(data)
    assert [s['core'] for s in decoded['slots']] == [2, 5] and all(s['state'] == 'valid' for s in decoded['slots'])

    # Request bytes and response size against the device and the decoder.
    assert tool.FAULT_OP == FAULT_OP and tool.REQUEST[0] == FAULT_OP and len(tool.REQUEST) == 16
    assert not any(tool.REQUEST[i] for i in ZERO_BYTES)
    assert tool.REPORT_BYTES == decoder.REPORT_BYTES == len(data)
    cases['request-matches-usb0-rule-and-size-matches-decoder'] = True

    # Byte for byte, one retain() call with the same bytes, at every fragment size.
    for fragment in FRAGMENTS:
        device = Device(data, fragment)
        result, retained = run(device)
        assert bytes(result) == data and len(retained) == 1 and retained[0] is result
        assert isinstance(result, memoryview) and result.readonly and isinstance(result.obj, bytearray)
        assert device.writes == [tool.REQUEST] and not device.queue
        assert all(size % 512 == 0 and size - 511 <= tool.REPORT_BYTES for size in device.reads)
        assert decoder.decode_report(result) == decoded
        metrics[f'fragment_{fragment}'] = {'requests': len(device.writes), 'reads': len(device.reads),
                                           'read_sizes': sorted(set(device.reads)), 'retries': 0}
        cases[f'returns-report-byte-for-byte:{fragment}-byte-fragments'] = True
    assert metrics['fragment_512']['reads'] == 3 and metrics['fragment_512']['read_sizes'] == [512, 1024, 1536]
    # The view's buffer is the one the fragments were appended to: one host copy.
    metrics['host_copies_per_byte'] = 1
    cases['three-reads-at-512-bytes-and-one-buffer'] = True

    # A refused request: the device stops and answers nothing; one write, no retry.
    for name, packet in (('flag', 1), ('count', 2), ('offset', 4), ('generation', 8)):
        bad = bytearray(tool.REQUEST)
        bad[packet] = 1
        device = Device(data, 512)
        device.write(0x02, bytes(bad), 500)
        assert device.stop, name
    device = Device(data, 512, stop=True)
    retained = []
    refused(lambda: tool.fetch(device, lambda: None, 1000.0, retained.append, now=lambda: 0.0), Stopped)
    assert device.writes == [tool.REQUEST] and len(device.reads) == 1 and not retained
    cases['refuses-stopped-device-with-one-write'] = True

    # An extra byte in the same transfer as the report's end.
    device = Device(data, 512, after=b'\x5a')
    message = refused(lambda: run(device))
    assert 'too large' in message and len(device.writes) == 1
    cases['refuses-extra-byte-in-final-transfer'] = True

    # An extra byte that arrives in a later transfer stays on the endpoint; the
    # capture exchange's frame checks then refuse its first response.
    epoch, generation = 0x1122334455667788, 3
    status = struct.pack('<8I', 0x424d494d, 1, 2, 0, 0, 0, 1, 0) + struct.pack('<QQ', epoch, generation)
    status += bytes(64 - len(status))
    for fragment in FRAGMENTS[:2]:
        for stray in (b'', b'\x5a'):
            device = Device(data, fragment, after=stray + status)
            result, _ = run(device)
            assert bytes(result) == data and len(device.queue) == len(stray) + 64

            # The capture's own requests are accepted; only the queued bytes answer them.
            device.write = lambda endpoint, packet, timeout: len(packet)

            def capture():
                exchange = host.device_exchange(device, lambda: None, 1000.0, lambda frame: None,
                                                now=lambda: 0.0)
                host.capture(exchange, epoch, generation, 1000.0, now=lambda: 0.0, pause=lambda s: None)
            message = refused(capture)
            if stray:
                assert message in ('Coalesced response refused.', 'Response identity differs.',
                                   'Response too large.', 'Response extent differs.'), message
            else:
                assert message == 'Native combined workload refused result.', message
        cases[f'later-extra-byte-refused-by-capture-frame-checks:{fragment}-byte-fragments'] = True

    # An empty fragment.
    class Empty(Device):
        def read(self, endpoint, size, timeout):
            part = super().read(endpoint, size, timeout)
            return array.array('B') if len(self.reads) == 2 else part
    device = Empty(data, 512)
    assert 'Empty' in refused(lambda: run(device)) and len(device.writes) == 1
    cases['refuses-empty-fragment'] = True

    # An identity change seen before the write, before a read, or after the
    # last read: same() runs once before each transfer and once before retain().
    calls = []
    run(Device(data, 512), same=lambda: calls.append(1))
    assert len(calls) == 5
    for change in range(len(calls)):
        device, retained, seen = Device(data, 512), [], []

        def same():
            seen.append(1)
            if len(seen) > change:
                raise ValueError('Observer instance changed; stop.')
        assert 'changed' in refused(lambda: tool.fetch(device, same, 1000.0, retained.append, now=lambda: 0.0))
        assert not retained and len(device.writes) == min(change, 1) and len(device.reads) == max(change - 1, 0)
    cases['refuses-identity-change-before-each-transfer-and-retain'] = True

    # Deadlines: expired before the write, and during the reads.
    device = Device(data, 512)
    assert 'expired' in refused(lambda: run(device, deadline=0.0)) and not device.writes
    ticks = iter(range(100))
    device = Device(data, 29)
    assert 'expired' in refused(lambda: run(device, deadline=5.0, clock=lambda: float(next(ticks))))
    assert len(device.writes) == 1 and 0 < len(device.reads) < 40
    # The deadline passes after the last read: the report is not retained.
    times = iter([0.0] * 4 + [10.0])
    device, retained = Device(data, 512), []
    assert 'expired' in refused(lambda: tool.fetch(device, lambda: None, 5.0, retained.append,
                                                   now=lambda: next(times)))
    assert len(device.reads) == 3 and not device.queue and not retained
    cases['refuses-deadline-before-write'] = True
    cases['refuses-deadline-after-last-read'] = True
    cases['refuses-deadline-during-reads'] = True

    metrics['imports'] = static_form()
    cases['module-imports-no-device-library-and-has-no-command-line'] = True
    result = {'passed': True, 'physical_access': False, 'metrics': metrics, 'cases': cases,
              'scope': 'Modeled operation 5 device parsed from usb0.c; no image, device or USB library.'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(metrics, indent=2))
    print(f'PASS {len(cases)} cases')


if __name__ == '__main__':
    main()
