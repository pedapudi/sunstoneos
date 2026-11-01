#!/usr/bin/env python3
"""Check tools/dev-run.py without a board.

  dev-run.py --out DIR

Preparation rebuilds the reviewed control bundle's launcher and segments from
its image with fresh session identities. The state check derives the thermal
clock mode and refuses states the image's guards refuse. A modeled FEL then
drives the shared transport: a watchdog reset is decoded, a state change since
capture stops the run before any write, and a failure after a normal return
quarantines the instance without holding the process. No USB node, queue or
FEL executable is opened.
"""
import argparse
import contextlib
import importlib.util
import itertools
import json
from pathlib import Path
import shutil
import tempfile
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('checked_dev_run', ROOT / 'tools/dev-run.py')
dev = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dev)
CAPTURE = dev.EVIDENCE / 'sol-resume-20261031/port-3-3/cpu-thermal-state/session.json'
ROM_ENTRY = {0x20: 0xeb00008f, 0x24: 0xe3a00911, 0x28: 0xe3a0193d, 0x3c: 0xe3a0605c, 0x40: 0xea00004d}
rows = []


def captured_state():
    words = {}
    for t in json.loads(CAPTURE.read_text())['transactions']:
        c = t['command']
        if 'readl' in c:
            addresses = [int(c[i + 1], 16) for i, x in enumerate(c) if x == 'readl']
            words.update({hex(a): int(v, 16) for a, v in zip(addresses, t['stdout'].split())})
    assert set(words) == {hex(a) for a in dev.WORDS}, 'Capture covers exactly the state words.'
    return {'node': '/dev/bus/usb/003/056', 'identity': [1, 56, 3, 4, 5], 'port': '3-3', 'words': words}


def preparation_cases(host, out):
    live = captured_state()
    meta = dev.prepare(host, dev.CONTROL_IMAGE, dev.CONTROL, out / 'bundle', live)
    reviewed, built = dev.CONTROL / 'board', out / 'bundle/board'
    for name in ['launch.bin'] + [n for n in meta['files'] if n.startswith('segment-')]:
        assert (reviewed / name).read_bytes() == (built / name).read_bytes(), name
    old, new = (reviewed / 'session.bin').read_bytes(), (built / 'session.bin').read_bytes()
    changed = {i for i in range(len(old)) if old[i] != new[i]}
    assert changed <= set(range(24, 48)) | set(range(len(old) - 96 + 32, len(old) - 96 + 48)), sorted(changed)
    assert meta['state_findings'] == [] and meta['derived_session']['configure_adc_clock'] == 0
    rows.append({'name': 'prepare-matches-reviewed-transport', 'passed': True})
    def state(changes):
        words = dict(live['words'])
        words.update({hex(a): v for a, v in changes.items()})
        return dev.state_check(words, host.os_image)
    findings, derived = state({0x2002fc0: 0})
    assert not findings and derived['configure_adc_clock'] == 1
    findings, derived = state({0x2002fc0: 0x80000003})
    assert derived['configure_adc_clock'] is None and 'GPADC24M' in findings[0]
    findings, _ = state({0x8872000: 0xf8805300})
    assert len(findings) == 1 and 'CPU-B' in findings[0]
    # After a cold DRAM start boot0's board setup leaves CPU-L and the DSU here.
    findings, _ = state({0x8871000: 0xf8802700, 0x887101c: 0x03000000, 0x8873000: 0xf8801e00, 0x887301c: 0x03000105})
    assert len(findings) == 2 and findings[0].startswith('CPU-L') and findings[1].startswith('DSU'), findings
    findings, _ = state({0x300624c: 0x00000195})
    assert len(findings) == 1 and 'speed bin 4' in findings[0]
    rows.append({'name': 'state-check-derives-and-refuses', 'passed': True})
    return live


def transport_case(host, root, live, variant):
    returning = host.cpu.returning
    crumbs = load_crumbs()
    old, new = Path('/dev/bus/usb/003/056'), Path('/dev/bus/usb/003/057')
    state = {'locked': False, 'guard': False, 'node': old, 'launched': False, 'quarantined': 0,
             'calls': [], 'mode': 0, 'rtc': {}, 'memory': {}}
    words = {int(k, 16): v for k, v in live['words'].items()}
    class Access:
        @contextlib.contextmanager
        def board_lock(self):
            state['locked'] = True
            try: yield old
            finally: state['locked'] = False
        def require_session(self, node): assert state['locked']
        def require_rom(self, node): assert state['locked']
        def identity(self, node): return [1, 57 if node == new else 56, 3, 4, 5]
        def target(self, node): return Path('/sys/bus/usb/devices/3-3')
        def session_node(self, config): return state['node']
        def quarantine_instance(self, *values): state['quarantined'] += 1
    class Guard:
        def check_health(self): pass
        @contextlib.contextmanager
        def reenumeration(self, *values):
            state['guard'] = True
            try: yield
            finally: state['guard'] = False
    def process(command, **kwargs):
        arguments, out, i = command[3:], [], 0
        state['calls'].append(arguments)
        while i < len(arguments):
            op = arguments[i]
            if op == 'version':
                out.append('soc=00001903(A733)'); i += 1
            elif op == 'writel':
                address, value = int(arguments[i + 1], 0), int(arguments[i + 2], 0); i += 3
                if address == crumbs.WDT_MODE: state['mode'] = value & 0xffff
                elif crumbs.RTC <= address < crumbs.RTC + 0x200: state['rtc'][address - crumbs.RTC] = value
            elif op == 'readl':
                address = int(arguments[i + 1], 0); i += 2
                value = {crumbs.WDT_MODE: state['mode'], crumbs.WDT_CONFIG: 1, crumbs.WDT_OUTPUT: 2}.get(address)
                if value is None and address in words:
                    # The state-changed board differs from its capture in one GPADC24M bit.
                    value = words[address] ^ (variant == 'state-changed' and address == 0x2002fc0)
                if value is None and crumbs.RTC <= address < crumbs.RTC + 0x200:
                    value = state['rtc'].get(address - crumbs.RTC, 0)
                out.append(hex(ROM_ENTRY.get(address, 0) if value is None else value))
            elif op == 'write':
                data = Path(arguments[i + 2]).read_bytes(); state['memory'][int(arguments[i + 1], 0)] = data; i += 3
            elif op == 'read':
                address, size = int(arguments[i + 1], 0), int(arguments[i + 2])
                data = state['memory'].get(address, b'')
                Path(arguments[i + 3]).write_bytes(data if len(data) == size else bytes(size)); i += 4
            elif op == 'exe':
                assert state['guard'] and state['mode'] == 0xb1, 'exe inside the guard with WDT0 armed'
                i += 2; state['launched'] = True; state['node'] = new; state['memory'].clear()
                epoch = json.loads((root / 'bundle/dev-bundle.json').read_text())['epoch']
                state['rtc'][0x100] = 0x5c0d0000 | epoch & 0xffff
                if variant == 'fired':   # fail(0x736): EL3 stop shortens WDT0 to 0.5 s and parks
                    state['mode'] = 0x01
                    state['rtc'].update({0x104: 0x0111 << 16 | 0x6736, 0x10c: 0x4abfea6, 0x110: 0xfa010836,
                                         0x114: 0x736, 0x118: 0x4101a1c4, 0x11c: 0xfff4f7ff})
                else:                    # normal ROM return (disarmed, RETURN_ROM 0) with a zero report
                    state['mode'] = 0; state['rtc'][0x104] = 0x0503 << 16
            else:
                raise AssertionError(op)
        return types.SimpleNamespace(returncode=0, stdout='\n'.join(out), stderr='')
    fel = root / 'fel'; fel.write_bytes(b'modeled, never executed')
    args = types.SimpleNamespace(fel=fel)
    clock = itertools.count(0, 0.5)
    error = None
    with patch.object(host.base, 'digest', side_effect=lambda p: host.base.FEL_HASH if Path(p) == fel else dev.digest(p)), \
            patch.object(returning.subprocess, 'run', process), patch.object(returning.os, 'access', return_value=True), \
            patch.object(returning.time, 'monotonic', lambda: next(clock)), patch.object(returning.time, 'sleep', lambda s: None):
        try:
            dev.run_return(args, host, root / 'bundle', live, root / 'run', access=Access(), guard=Guard())
        except Exception as failure:
            error = failure
    return state, error


def load_crumbs():
    spec = importlib.util.spec_from_file_location('checked_dev_crumbs', ROOT / 'tools/a733-crumbs.py')
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value


def transport_cases(host, out, live):
    for variant in ('fired', 'state-changed', 'returned-bad-report'):
        root = out / variant; root.mkdir()
        shutil.copytree(out / 'bundle', root / 'bundle')
        state, error = transport_case(host, root, live, variant)
        executed = [c for c in state['calls'] if 'exe' in c]
        if variant == 'fired':
            report = json.loads((root / 'run/watchdog-report.json').read_text())
            assert type(error).__name__ == 'WatchdogReset' and len(executed) == 1 and not state['quarantined']
            assert report['breadcrumbs']['fault']['detail'] == '0x736' and report['stop_shortened'], report
        elif variant == 'state-changed':
            assert 'Board state changed' in str(error) and not executed and not state['quarantined']
            assert not any('write' in c for c in state['calls']), 'no write before the state comparison'
        else:
            assert 'No successful CPU return report' in str(error), error
            assert len(executed) == 1 and state['quarantined'] == 1, 'quarantined, process not held'
        rows.append({'name': 'transport-' + variant, 'passed': True, 'fel_requests': len(state['calls'])})


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True)
    out = parser.parse_args().out
    out.mkdir(parents=True, exist_ok=False)
    host = dev.load('checked_dev_host', ROOT / 'apps/os/cpu-return-host.py')
    with tempfile.TemporaryDirectory(prefix='dev-run-', dir=out) as temporary:
        live = preparation_cases(host, Path(temporary))
        transport_cases(host, Path(temporary), live)
    result = {'passed': True, 'physical_access': False, 'cases': rows}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
