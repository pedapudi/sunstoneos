#!/usr/bin/env python3
"""Check tools/dev-run.py without a board.

  dev-run.py --out DIR

Preparation builds the hello-compute CPU-return control image's bundle: the
launcher admits the image's WDT0 mode, and the typed session follows the
image's manifest, takes the reviewed board prefix, thermal record and CPU
clock grant with fresh identities and the live thermal clock fields, and is
admitted by the linked image's own contract check. The state check derives the
thermal clock mode and refuses states the image's guards refuse; such a state
and the refused kinds stop before any launch, and an image without the
watchdog marker or inputs that differ from the manifest stop before the board
is read. A modeled FEL then drives the shared transport through the runner's
command line. Its memory reads back what was written, and every other address
reads a pattern of its own address. A normal return whose Adler-32 matches
passes from a development bundle that has no return-review.json; a wrong or
malformed result fails; every returned run leaves all DRAM as it was before
the run. A result of another session or a bad return report fails after the confirmed
normal return without quarantining the instance or holding the process.
A watchdog reset is decoded, a state
change since capture stops the run before any write, and an external decoder
module decodes the result and may report it failed.

The rail helper runs after a normal return whose read-only capture shows
the boot clock words, and not after a watchdog reset, a quarantine, any other
failure, other clock words or --no-restore-rails. Its report
accepts only a normal return with one 800 mV VOLT readback per rail above
800 mV at entry, DCDC3 first; its REFUSED and STOPPED returns are
controlled outcomes of the actual kernel runner. Before a launch the helper
runs by default for an image that links the CPU-return entry guard, ahead of
the capture, and a helper run that does not complete stops before the
capture. The launched run's record is saved before the helper runs after it,
so an interrupted helper does not lose it. No USB node, queue or FEL
executable is opened.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import itertools
import json
from pathlib import Path
import shutil
import struct
import tempfile
import types
from unittest.mock import patch
import zlib
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


dev = load('checked_dev_run', ROOT / 'tools/dev-run.py')
crumbs = load('checked_dev_crumbs', ROOT / 'tools/a733-crumbs.py')
hello = load('checked_hello_host', ROOT / 'apps/hello-compute/host.py')
CAPTURE = dev.EVIDENCE / 'sol-resume-20261031/port-3-3/cpu-thermal-state/session.json'
rows = []
# Addresses the modeled application overwrites. hello-compute has no WORK or
# OUTPUT region, and an application must not write its INPUT regions, so only
# image RAM changes besides the host's uploads.
DIRTIED = (0x41000000,)


def passed(name, **extra):
    rows.append({'name': name, 'passed': True, **extra})


class Memory:
    """Board memory: written bytes read back, and an address never written
    reads a pattern derived from its page number."""
    PAGE = 4096

    def __init__(self):
        self.pages = {}

    @classmethod
    def pattern(cls, number):
        return hashlib.sha256(number.to_bytes(8, 'little')).digest() * (cls.PAGE // 32)

    def page(self, number):
        return self.pages.setdefault(number, bytearray(self.pattern(number)))

    def write(self, address, data):
        while data:
            number, offset = divmod(address, self.PAGE)
            n = min(len(data), self.PAGE - offset)
            self.page(number)[offset:offset + n] = data[:n]
            address, data = address + n, data[n:]

    def read(self, address, size):
        out = bytearray()
        while size:
            number, offset = divmod(address, self.PAGE)
            n = min(size, self.PAGE - offset)
            page = self.pages.get(number)
            out += (page if page is not None else self.pattern(number))[offset:offset + n]
            address, size = address + n, size - n
        return bytes(out)

    def changed_dram(self):
        """Start addresses of DRAM pages that differ from their pattern."""
        return [n * self.PAGE for n, page in self.pages.items()
                if n * self.PAGE >= 0x40000000 and page != self.pattern(n)]


def captured_state():
    words = {}
    for t in json.loads(CAPTURE.read_text())['transactions']:
        c = t['command']
        if 'readl' in c:
            addresses = [int(c[i + 1], 16) for i, x in enumerate(c) if x == 'readl']
            words.update({hex(a): int(v, 16) for a, v in zip(addresses, t['stdout'].split())})
    assert set(words) == {hex(a) for a in dev.WORDS}, 'Capture covers exactly the state words.'
    return {'node': '/dev/bus/usb/003/056', 'identity': [1, 56, 3, 4, 5], 'port': '3-3', 'words': words}


def native_admits(launcher, image, session, contract):
    """The linked image's a733_smp_contract_valid on this session and launch contract."""
    native = load('checked_dev_native', ROOT / 'checks/a733-storage-maintenance.py').Native(image)
    native.sys.update(cntpct_el0=24000000, cntfrq_el0=24000000, daif=0, tpidr_el1=1)
    contract = bytearray(contract)
    # The launcher's contract words: the OS magic, then armed at tick 100,
    # one startup second and the 60 s supervised lifetime (a733_board_application.h).
    struct.pack_into('<Q', contract, 0, launcher.CONTRACT_MAGIC)
    struct.pack_into('<4Q', contract, 80, 100, 100 + 24000000, 100 + 60 * 24000000, 100 + 60 * 24000000)
    contract[192:224] = hashlib.sha256(session).digest()
    native.u.mem_write(0x41f00000, bytes(session))
    native.u.mem_write(0x42004000, bytes(contract))
    native.stop = None
    native.u.reg_write(UC_ARM64_REG_SP, 0x41dff000)
    native.u.reg_write(UC_ARM64_REG_LR, 0x43000000)
    native.u.reg_write(UC_ARM64_REG_X0, 0x42004000)
    native.u.reg_write(UC_ARM64_REG_X0 + 1, 101)
    native.u.emu_start(native.symbols['a733_smp_contract_valid'], 0x43000004, count=4000000)
    assert native.stop == 'return', 'contract check did not return'
    return native.u.reg_read(UC_ARM64_REG_X0)


def preparation_cases(host, launcher, out):
    live = captured_state()
    inputs = hello.inputs()
    meta = dev.prepare(host, launcher, dev.CONTROL_IMAGE, dev.RECORDS, inputs, out / 'bundle', live)
    board = out / 'bundle/board'
    assert meta['application'] == 'hello-compute' and meta['state_findings'] == []
    assert meta['derived_session']['configure_adc_clock'] == 0 and meta['watchdog']['mode'] == 0xb1
    armed = '(rd(0x02050014)&0xffff)==0xb1 && (rd(0x02050010)&0xffff)==1 && (rd(0x02050018)&0xffff)==2 &&'
    assert armed in (board / 'board-launch-base.c').read_text(), 'launcher admits the image WDT0 mode'
    segments, address, identity = host.base.image_parts(board / 'image.elf')
    for index, segment in enumerate(segments):
        if segment['data']:
            assert (board / f'segment-{index}.bin').read_bytes() == segment['data']
    raw = (board / 'session.bin').read_bytes()
    assert ((board / 'request.bin').read_bytes(), (board / 'contract.bin').read_bytes()) == \
        launcher.request_bytes(address, identity, raw)
    dev.validator(host)(out / 'bundle')
    passed('prepare-hello-compute-control-image', session_bytes=len(raw))
    # The session follows the manifest; only the identities and the live
    # thermal clock fields differ from the reviewed records.
    os_host = host.host
    info = os_host.image_manifest(board / 'image.elf')
    decoded = os_host.session_decode(raw, info['manifest'], info['sha256'])
    assert (decoded['devices'], decoded['flags']) == (0, os_host.BIND_SID)
    assert set(decoded['extents']) == {os_host.THERMAL, os_host.CPU_GRANT, os_host.INPUTS}
    assert decoded['extents'][os_host.INPUTS] == struct.pack('<Q32s', len(inputs[0]), hashlib.sha256(inputs[0]).digest())
    assert [i['address'] for i in meta['inputs']] == [r['address'] for r in os_host.regions(info['manifest'], (1,))]
    prefix, thermal, grant = dev.session_records(dev.RECORDS, os_host)
    def changed(old, new):
        return {i for i in range(len(old)) if old[i] != new[i]}
    assert changed(prefix, raw[:os_host.PREFIX_BYTES]) <= set(range(24, 80))
    assert struct.unpack_from('<3Q', raw, 24) == (meta['epoch'], meta['generation'], meta['lease'])
    assert raw[48:80] == identity
    new_thermal = decoded['extents'][os_host.THERMAL]
    assert changed(thermal, new_thermal) <= set(range(16, 24)) | set(range(dev.THERMAL_APB1, dev.THERMAL_CONFIGURE + 1))
    assert struct.unpack_from('<2IB', new_thermal, dev.THERMAL_APB1) == (meta['derived_session']['apb1'],
                                                                          meta['derived_session']['apb1_hz'], 0)
    new_grant = decoded['extents'][os_host.CPU_GRANT]
    assert changed(grant, new_grant) <= set(range(32, 48))
    assert struct.unpack_from('<2Q', new_grant, 32) == (meta['epoch'], meta['generation'])
    passed('session-matches-manifest-and-reviewed-records')
    contract = (board / 'contract.bin').read_bytes()
    assert native_admits(launcher, board / 'image.elf', raw, contract) == 1
    wrong_device = bytearray(raw)
    wrong_device[os_host.PREFIX_BYTES + 24] |= 0x10   # a GPU grant the image does not admit
    long_input = bytearray(raw)
    struct.pack_into('<Q', long_input, len(raw) - 40, hello.INPUT_BYTES + 1)
    wrong_grant = bytearray(raw)
    grant_offset = decoded['offsets'][os_host.CPU_GRANT]
    struct.pack_into('<Q', wrong_grant, grant_offset + 40, meta['generation'] ^ 1)
    for session in (wrong_device, long_input, wrong_grant):
        assert native_admits(launcher, board / 'image.elf', session, contract) == 0
    passed('linked-image-admits-session-and-refuses-changes', refusals=3)

    def state(changes):
        words = dict(live['words'])
        words.update({hex(a): v for a, v in changes.items()})
        return dev.state_check(words)
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
    passed('state-check-derives-and-refuses')
    return live


def run_main(root, live, argv, launched=False):
    """dev-run's main() with captured state; returns (exit code, dev-run.json)."""
    (root / 'state.json').write_text(json.dumps(live))
    code = dev.main(['cpu-return', '--state', str(root / 'state.json'), '--evidence', str(root / 'evidence'),
                     '--fel', str(root / 'fel'), *argv])
    record = json.loads((root / 'evidence/dev-run.json').read_text())
    assert record['launched'] is launched
    return code, record


def refusal_cases(out, live):
    root = out / 'refused-state'; root.mkdir()
    changed = dict(live, words=dict(live['words'], **{hex(0x8872000): 0xf8805300}))
    code, record = run_main(root, changed, [])
    assert code == 1 and record['outcome'] == 'refused-before-launch' and 'CPU-B' in record['summary']
    assert not (root / 'evidence/run').exists()
    root = out / 'refused-image'; root.mkdir()
    data = bytearray(dev.CONTROL_IMAGE.read_bytes())
    name = b'\0os_cpu_return_watchdog\0'
    assert name in data
    data = data.replace(name, b'\0Os_cpu_return_watchdog\0')   # rename the marker symbol
    (root / 'image.elf').write_bytes(data)
    (root / 'state.json').write_text(json.dumps(live))
    (root / 'input.bin').write_bytes(hello.inputs()[0])
    def unread(fel):
        raise AssertionError('board read before an image refusal')
    for argv, text in ((['--image', str(root / 'image.elf')], 'watchdog marker'),
                       (['--input', str(root / 'input.bin')] * 2, 'Input count differs')):
        evidence = root / 'evidence'
        if evidence.exists():
            shutil.move(str(evidence), str(root / ('evidence-' + text.split()[0])))
        with patch.object(dev, 'capture', unread):
            code = dev.main(['cpu-return', '--port', '3-3', '--evidence', str(evidence), '--fel', str(root / 'fel'),
                             *argv])
        record = json.loads((evidence / 'dev-run.json').read_text())
        assert code == 1 and record['outcome'] == 'refused-before-launch' and text in record['summary'], record
        assert not record['launched'] and not (evidence / 'state.json').exists()
    for kind, text in (('gpu-return', 'GPU-capable'), ('npu', 'npu is refused')):
        stream = io.StringIO()
        with contextlib.redirect_stderr(stream):
            try:
                dev.main([kind, '--image', str(dev.CONTROL_IMAGE), '--state', str(root / 'state.json')])
            except SystemExit as stop:
                assert stop.code == 2, (kind, stop.code)
        assert text in stream.getvalue(), (kind, stream.getvalue())
    passed('refused-before-launch', cases=['state', 'image-without-watchdog', 'input-count', 'gpu-return', 'npu'])


def gpu_preparation_cases(host, launcher, out, live, images):
    """Actual CLI preflight for each frozen GPU image, without board access."""
    for index, image in enumerate(images):
        case = out / ('gpu-preflight-%d' % index)
        case.mkdir()
        state = case / 'state.json'
        state.write_text(json.dumps(live))
        inputs = sorted((image.parent / 'template').glob('input-*.bin'))
        argv = ['gpu-return', '--image', str(image), '--state', str(state),
                '--check-only', '--evidence', str(case / 'evidence')]
        for value in inputs:
            argv.extend(['--input', str(value)])
        with patch.object(dev, 'capture', side_effect=AssertionError('offline preflight read board')):
            code = dev.main(argv)
        record = json.loads((case / 'evidence/dev-run.json').read_text())
        assert code == 0 and record['outcome'] == 'checked' and not record['launched'], record
        meta = json.loads((case / 'evidence/bundle/dev-bundle.json').read_text())
        assert meta['devices'] & 16 and len(meta['inputs']) == len(inputs)
        board = case / 'evidence/bundle/board'
        assert native_admits(launcher, image, (board / 'session.bin').read_bytes(),
                             (board / 'contract.bin').read_bytes()) == 1
        passed('gpu-preflight-' + image.parent.name, image_sha256=dev.digest(image))


def return_report(host, epoch):
    """A normal-return report of the monitor (apps/os/cpu-return-host.py decode_report)."""
    cpu = host.cpu
    raw = bytearray(cpu.REPORT_BYTES)
    struct.pack_into('<8Q', raw, 0, cpu.MAGIC, 3, cpu.REPORT_BYTES, epoch, 1, 4, 0, 0)
    for i in range(8):
        offset = cpu.CPU_OFFSET + i * cpu.CPU_BYTES
        handshake = epoch if i else 0
        struct.pack_into('<7Q', raw, offset, epoch, epoch, epoch, epoch, handshake, handshake, 0)
        struct.pack_into('<Q', raw, offset + 104, epoch)
        struct.pack_into('<Q', raw, offset + 768, handshake)
    return bytes(raw)


def result_record(host, epoch, generation, payload, capacity):
    os_host = host.host
    raw = os_host.RESULT.pack(os_host.RESULT_MAGIC, os_host.RESULT_VERSION, os_host.RESULT.size + capacity, epoch,
                              generation, os_host.RESULT_FINISHED, len(payload)) + payload
    return raw + bytes(os_host.RESULT.size + capacity - len(raw))


def transport_case(host, launcher, root, live, variant, argv=(), payload=None):
    returning = host.cpu.returning
    old, new = Path('/dev/bus/usb/003/056'), Path('/dev/bus/usb/003/057')
    state = {'locked': False, 'guard': False, 'node': old, 'launched': False, 'quarantined': 0,
             'calls': [], 'mode': 0, 'rtc': {}, 'memory': Memory(), 'writes_after_launch': 0, 'rails': 0}
    words = {int(k, 16): v for k, v in live['words'].items()}
    geometry = host.geometry(dev.CONTROL_IMAGE)
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
        if command[0] != str(fel):   # the launcher build during preparation
            return build(command, **kwargs)
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
                out.append(hex(host.ROM_ENTRY.get(address, 0) if value is None else value))
            elif op == 'write':
                data = Path(arguments[i + 2]).read_bytes(); state['memory'].write(int(arguments[i + 1], 0), data); i += 3
                state['writes_after_launch'] += state['launched']
            elif op == 'read':
                address, size = int(arguments[i + 1], 0), int(arguments[i + 2])
                Path(arguments[i + 3]).write_bytes(state['memory'].read(address, size)); i += 4
            elif op == 'exe':
                assert state['guard'] and state['mode'] == 0xb1, 'exe inside the guard with WDT0 armed'
                i += 2; state['launched'] = True; state['node'] = new
                # The application overwrites part of its image RAM.
                for region in DIRTIED:
                    state['memory'].write(region, b'\xa5' * 65536)
                meta = json.loads((root / 'evidence/bundle/dev-bundle.json').read_text())
                epoch, generation = meta['epoch'], meta['generation']
                state['rtc'][0x100] = 0x5c0d0000 | epoch & 0xffff
                if variant == 'fired':   # fail(0x736): EL3 stop shortens WDT0 to 0.5 s and parks
                    state['mode'] = 0x01
                    state['rtc'].update({0x104: 0x0111 << 16 | 0x6736, 0x10c: 0x4abfea6, 0x110: 0xfa010836,
                                         0x114: 0x736, 0x118: 0x4101a1c4, 0x11c: 0xfff4f7ff})
                else:                    # normal ROM return: disarmed, RETURN_ROM 0
                    state['mode'] = 0; state['rtc'][0x104] = 0x0503 << 16
                    if variant != 'bad-report':
                        state['memory'].write(geometry['report_address'], return_report(host, epoch))
                    if payload is not None:
                        record_epoch = epoch ^ (variant == 'foreign-result')
                        state['memory'].write(geometry['output_address'], result_record(
                            host, record_epoch, generation, payload, geometry['output_bytes'] - host.host.RESULT.size))
            else:
                raise AssertionError(op)
        return types.SimpleNamespace(returncode=0, stdout='\n'.join(out), stderr='')
    fel = root / 'fel'; fel.write_bytes(b'modeled, never executed')
    build = returning.subprocess.run
    clock = itertools.count(0, 0.5)
    original = dev.run_return
    def modeled(args, host_module, bundle, live_state, out):
        return original(args, host_module, bundle, live_state, out, access=Access(), guard=Guard())
    def helper(args, host_module, out):
        state['rails'] += 1
        assert out == root / 'evidence/rail-helper'
        return {'modeled': True, 'complete': True, 'supply': ['SUPPLY modeled'], 'volt': [], 'rail_state': [],
                'outcome': [dev.RAIL_COMPLETE]}
    def after(fel):
        """The read-only capture after a normal return: result-invalid leaves CPU-B off its boot word."""
        assert variant in ('passed', 'result-invalid', 'external-decoder'), 'board read after a failed run'
        words = dict(live['words'])
        if variant == 'result-invalid':
            words[hex(0x8872000)] = 0xf8805300
        return dict(live, words=words)
    with patch.object(host.base, 'digest', side_effect=lambda p: host.base.FEL_HASH if Path(p) == fel else dev.digest(p)), \
            patch.object(returning.subprocess, 'run', process), patch.object(returning.os, 'access', return_value=True), \
            patch.object(returning.time, 'monotonic', lambda: next(clock)), patch.object(returning.time, 'sleep', lambda s: None), \
            patch.object(dev, 'run_return', modeled), patch.object(dev, 'run_rail_helper', helper), \
            patch.object(dev, 'capture', after), \
            patch.object(dev, 'kit', lambda: (host, launcher)):
        code, record = run_main(root, live, list(argv), launched=True)
    return state, code, record


def transport_cases(host, launcher, out, live):
    data = hello.inputs()[0]
    good = struct.pack('<IIQ', zlib.adler32(data), 0, len(data))
    decoder = out / 'external-decoder.py'
    decoder.write_text('def decode(payload):\n    return {"first_word": int.from_bytes(payload[:4], "little")}\n')
    (out / 'input.bin').write_bytes(b'external application input')
    small = b'external application input'
    cases = [('passed', good, (), 0, 'passed'),
             ('result-mismatch', struct.pack('<IIQ', zlib.adler32(data) ^ 1, 0, len(data)), ('--no-restore-rails',),
              1, 'result-mismatch'),
             ('result-invalid', struct.pack('<IIQ', zlib.adler32(data), 7, len(data)), (), 1, 'result-invalid'),
             ('foreign-result', good, (), 1, 'failed'),
             ('bad-report', good, (), 1, 'failed'),
             ('fired', None, (), 1, 'watchdog-reset'),
             ('state-changed', None, (), 1, 'failed'),
             ('external-decoder', struct.pack('<IIQ', zlib.adler32(small), 0, len(small)),
              ('--decoder', str(decoder), '--input', str(out / 'input.bin')), 0, 'returned')]
    for variant, payload, argv, exit_code, outcome in cases:
        root = out / variant; root.mkdir()
        state, code, record = transport_case(host, launcher, root, live, variant, argv, payload)
        executed = [c for c in state['calls'] if 'exe' in c]
        run = root / 'evidence/run'
        assert (code, record['outcome']) == (exit_code, outcome), (variant, code, record)
        if variant == 'state-changed':
            assert 'Board state changed' in record['error'] and not executed and not state['quarantined']
            assert not any('write' in c for c in state['calls']), 'no write before the state comparison'
        else:
            assert len(executed) == 1, variant
        session = json.loads((run / 'session.json').read_text())
        if variant == 'fired':
            report = json.loads((run / 'watchdog-report.json').read_text())
            assert not state['quarantined'] and report['stop_shortened']
            assert report['breadcrumbs']['fault']['detail'] == '0x736', report
        elif variant in ('foreign-result', 'bad-report'):
            text = {'foreign-result': 'belongs to another session', 'bad-report': 'No successful CPU return report'}
            assert text[variant] in record['error'] and not state['quarantined'], (variant, record)
            assert session['host_failed_after_normal_return'] and session['instance_quarantined'] is False
            assert session['normal_return']['first_request_succeeded'] and not session.get('fel_request_raised')
            assert not session['complete'] and not session['dram_restored'] and state['writes_after_launch'] == 0
        elif variant != 'state-changed':
            # A returned run restores every backed-up DRAM range, whatever the result.
            assert session['complete'] and session['dram_restored'] and not state['quarantined']
            assert state['writes_after_launch'] == len(session['dram_backups'])
            assert not state['memory'].changed_dram(), (variant, 'DRAM differs after restoration')
            assert session['watchdog']['mode_after_return'] == 0
            assert session['watchdog']['breadcrumbs']['last']['name'] == 'RETURN_ROM'
            assert (run / 'result.bin').read_bytes() == payload
        # The rail helper runs after a normal return only: a watchdog reset leaves DRAM
        # uninitialized, and --no-restore-rails skips it. Normalization before the launch
        # never runs with --state.
        restored = variant in ('passed', 'external-decoder')
        assert (root / 'evidence/state-after.json').exists() == (variant in ('passed', 'result-invalid',
                                                                           'external-decoder')), variant
        assert state['rails'] == restored, (variant, state['rails'], record.get('rail_helper_reason'))
        assert record['normalize_rails'].startswith('not run: --state'), record['normalize_rails']
        assert 'rails: ' in record['summary']
        if restored:
            assert record['rail_helper']['modeled'] and 'at 800 mV after the rail helper' in record['rails']
            assert record['rail_helper_reason'] == 'normal return, CPU clocks at their boot words', record
        else:
            assert record['rail_helper'] is None if variant != 'result-mismatch' else 'rail_helper' not in record
            reason = {'result-mismatch': 'not run: --no-restore-rails',
                      'result-invalid': 'not run: CPU-B not at the boot PLL and mux words after the return'}.get(
                          variant, 'not run: run cold DDR first; the next launch lowers the rails')
            assert record['rail_helper_reason'] == reason, (variant, record['rail_helper_reason'])
        if variant == 'passed':
            # The development bundle has no return-review.json, and the normal return passes
            # without it: the host neither reads nor writes one.
            assert not any(p.name == 'return-review.json' for p in (root / 'evidence').rglob('*'))
            passed('normal-return-without-return-review')
            assert record['decoded'] == {'adler32': zlib.adler32(data), 'bytes': len(data)}
            launch = next(i for i, c in enumerate(state['calls']) if 'exe' in c)
            uploads = [c for c in state['calls'][:launch] if c[:2] == ['write', hex(0x44000000)]]
            assert len(uploads) == 1, 'the manifest input is uploaded once at its region before launch'
        if variant == 'external-decoder':
            assert record['decoded'] == {'first_word': zlib.adler32(small)}
            assert record['decoder'] == str(decoder.resolve())
        rows.append({'name': 'run-' + variant, 'passed': True, 'outcome': record['outcome'],
                     'fel_requests': len(state['calls'])})
    # Without a decoder the runner reports the raw payload.
    outcome, detail, decoded = dev.judge(None, good, [data], {'watchdog': {
        'mode_after_return': 0, 'breadcrumbs': {'last': {'name': 'RETURN_ROM'}}}})
    assert outcome == 'returned' and good.hex() in detail and decoded is None
    outcome, detail, _ = dev.judge(hello, good, [data], {'watchdog': {
        'mode_after_return': 0, 'breadcrumbs': {'last': {'name': 'RECOGNITION_START'}}}})
    assert outcome == 'failed' and 'return not normal' in detail
    # A decoder's failures() turns a decoded result into a failure.
    reporting = types.SimpleNamespace(decode=lambda payload: {'lines': ['FAIL one']},
                                      failures=lambda decoded: decoded['lines'])
    outcome, detail, decoded = dev.judge(reporting, good, [data], {'watchdog': {
        'mode_after_return': 0, 'breadcrumbs': {'last': {'name': 'RETURN_ROM'}}}})
    assert outcome == 'result-failed' and 'FAIL one' in detail and decoded == {'lines': ['FAIL one']}
    passed('judge-raw-result-failures-and-abnormal-return')


HELPER_RECORD = {'complete': True, 'transport_failed': False, 'mode': 'kernel',
                 'result': {'magic': 0x534b4552, 'stage': 5, 'status': 0}}


def helper_log(dcdc5, dcdc3, volt=None, end=None):
    clocks = []
    for phase in ('before', 'after'):
        for name, pll, mux, mhz in dev.BOOT_ROWS:
            clocks.append(f'{name} {phase} pll={pll:#x} mux={mux:#x} mhz={mhz} pat0=0x0 pat1=0x30000000 lfm=0x40000001')
    if volt is None:
        volt = [f'VOLT {r}_selector=0x9e {r}_uv=800000' for r, mv in (('dcdc3', dcdc3), ('dcdc5', dcdc5)) if mv != 800]
    return '\n'.join(['BEGIN board bundle 33: ' + dev.RAIL_COMPLETE[len('COMPLETE board bundle 33: '):],
                      'TEMP set=0 level=0', *clocks[:3],
                      f'SUPPLY dcdc5_uv={dcdc5 * 1000} dcdc3_uv={dcdc3 * 1000} sid=0x496 bin=4 dcxo_hz=26000000',
                      *volt, *clocks[3:], end or dev.RAIL_COMPLETE]) + '\n'


def helper_cases():
    # Every REFUSED and STOPPED text of the helper source is a controlled outcome, and no other.
    import re
    source = (ROOT / 'apps/board-bundles-power/cpu-opp-step.c').read_text()
    texts = {('REFUSED', t) for t in re.findall(r'refuse\("([^"]+)"\)', source)} | \
        {('STOPPED', t) for t in re.findall(r'controlled\("STOPPED", "([^"]+)"\)', source)}
    assert texts == set(dev.RAIL_CONTROLLED) and len(texts) == len(dev.RAIL_CONTROLLED), texts ^ set(dev.RAIL_CONTROLLED)
    for dcdc5, dcdc3, written in ((1000, 1050, ['dcdc3', 'dcdc5']), (800, 1050, ['dcdc3']),
                                  (1000, 800, ['dcdc5']), (800, 800, [])):
        r = dev.helper_report(HELPER_RECORD, helper_log(dcdc5, dcdc3))
        assert r['complete'] and r['written'] == written and len(r['volt']) == len(written), r
        assert r['supply'] == [f'SUPPLY dcdc5_uv={dcdc5 * 1000} dcdc3_uv={dcdc3 * 1000} sid=0x496 bin=4 dcxo_hz=26000000']
    refused = {'complete': False, 'transport_failed': False, 'mode': 'kernel',
               'result': {'magic': 0x534b4552, 'stage': 5, 'status': 1}}
    for name, record, log, reason in (
            ('refused', refused, helper_log(1000, 1050, volt=[], end='REFUSED board bundle 33: boot clock words'),
             'did not return normally'),
            ('missing-volt', HELPER_RECORD, helper_log(1000, 1050, volt=['VOLT dcdc3_selector=0x9e dcdc3_uv=800000']),
             'VOLT lines'),
            ('dcdc5-first', HELPER_RECORD, helper_log(1000, 1050, volt=['VOLT dcdc5_selector=0x9e dcdc5_uv=800000',
                                                                       'VOLT dcdc3_selector=0x9e dcdc3_uv=800000']),
             'VOLT lines'),
            ('wrong-readback', HELPER_RECORD, helper_log(1000, 800, volt=['VOLT dcdc5_selector=0x9f dcdc5_uv=810000']),
             'VOLT lines'),
            ('clock-after', HELPER_RECORD, helper_log(1000, 1050).replace('CPU-B after pll=0xf8802700',
                                                                          'CPU-B after pll=0xf8805300'), 'CPU-B after'),
            # A log truncated after the VOLT word.
            ('truncated-volt', HELPER_RECORD, helper_log(1000, 1050, volt=[
                'VOLT ', 'VOLT dcdc5_selector=0x9e dcdc5_uv=800000']), 'VOLT lines')):
        r = dev.helper_report(record, log)
        assert not r['complete'] and reason in r['reason'], (name, r)
        assert r['supply'] and r['outcome'], (name, r)
    passed('rail-helper-report')


def controlled_cases(host, out):
    """The helper's REFUSED and STOPPED returns are controlled outcomes of the actual kernel runner, so a
    refusal is recorded without quarantine and reported as not complete; a FAIL inside a rail change is not
    one, and its RAIL line is kept."""
    runner = host.cpu.kernel
    calls = []
    def main(argv):
        calls.append(argv)
        outcomes = runner.controlled_outcomes(Path(argv[argv.index('--controlled') + 1]).read_bytes())
        evidence = Path(argv[argv.index('--evidence') + 1]); evidence.mkdir()
        log = helper_log(1000, 1050, volt=[], end=end)
        terminal = {'magic': 0x534b4552, 'stage': 5, 'status': 1}
        found = [[t for t in o['log_contains'] if t in log] for o in outcomes]
        matched = [o for o, f in zip(outcomes, found) if runner.controlled_match(o, terminal, [], f)]
        record = {'complete': bool(matched), 'transport_failed': False, 'mode': 'kernel', 'result': terminal}
        if matched:
            record['controlled'] = {'outcome': matched[0]}
        (evidence / 'session.json').write_text(json.dumps(record))
        (evidence / 'kernel.log').write_text(log)
        if not matched:
            raise RuntimeError('Unexpected kernel result')
        return 0
    helper = out / 'helper.bin'; helper.write_bytes(b'modeled helper')
    args = types.SimpleNamespace(fel=out / 'fel', rail_helper=helper)
    with patch.object(host.cpu.kernel, 'main', main), patch.object(dev, 'RAIL_HELPER_SHA256', dev.digest(helper)):
        for name, end, reason in (
                ('refused', 'REFUSED board bundle 33: boot clock words', 'refused-boot-clock-words, no rail written'),
                ('stopped', 'STOPPED board bundle 33: thermal limit', 'controlled outcome stopped-thermal-limit'),
                ('failed', 'RAIL rail=2 stage=2 byte_index=7 selector=0xb7 written=0x9e readback=0x9f\n'
                           'FAIL CPU rail write result', 'did not return normally')):
            r = dev.run_rail_helper(args, host, out / f'controlled-{name}')
            assert not r['complete'] and reason in r['reason'] and r['supply'], (name, r)
            assert name != 'failed' or (r['rail_state'] == [
                'RAIL rail=DCDC3 stage=WRITE byte_index=7 selector=0xb7 written=0x9e readback=0x9f'] and
                'runner_error' in r), r
    assert len(calls) == 3
    r = dev.run_rail_helper(types.SimpleNamespace(fel=out / 'fel', rail_helper=helper), host, out / 'controlled-hash')
    assert not r['complete'] and 'differs from the reviewed one' in r['reason'] and len(calls) == 3, r
    # Any other error while recording is a report, not an exception: a truncated log, an
    # unexpected decode error and a missing helper binary.
    def truncated(argv):
        evidence = Path(argv[argv.index('--evidence') + 1]); evidence.mkdir()
        (evidence / 'session.json').write_text(json.dumps(HELPER_RECORD))
        (evidence / 'kernel.log').write_text(helper_log(1000, 1050, volt=['VOLT ']))
    with patch.object(host.cpu.kernel, 'main', truncated), patch.object(dev, 'RAIL_HELPER_SHA256', dev.digest(helper)):
        r = dev.run_rail_helper(args, host, out / 'controlled-truncated')
        assert not r['complete'] and 'VOLT lines' in r['reason'] and r['volt'] == ['VOLT '], r
        with patch.object(dev, 'helper_report', side_effect=StopIteration):
            r = dev.run_rail_helper(args, host, out / 'controlled-decode-error')
        assert not r['complete'] and 'StopIteration' in r['reason'], r
    r = dev.run_rail_helper(types.SimpleNamespace(fel=out / 'fel', rail_helper=out / 'absent.bin'), host,
                            out / 'controlled-absent')
    assert not r['complete'] and 'FileNotFoundError' in r['reason'] and r['supply'] == [], r
    passed('rail-helper-controlled-outcomes')


def normalize_cases(host, out):
    """The default for an image that links the entry guard: the helper, then the capture and launch; after
    a failed run no second helper run; a failed helper stops before the capture."""
    assert dev.guard_requires_800(host, dev.CONTROL_IMAGE), 'the control image links the CPU-return entry guard'
    for name, argv, complete, expected in (
            ('default', [], True, ['helper', 'capture', 'launch']),
            ('helper-refused', [], False, ['helper']),
            ('skipped', ['--no-normalize-rails', '--no-restore-rails'], True, ['capture', 'launch'])):
        calls = []
        def helper(args, host, where, complete=complete):
            calls.append('helper')
            return {'complete': complete, 'supply': ['SUPPLY dcdc5_uv=1000000 dcdc3_uv=1050000'], 'volt': [],
                    'rail_state': [], 'outcome': [], 'reason': 'modeled refusal', 'evidence': str(where)}
        def capture(fel):
            calls.append('capture')
            return captured_state()
        def run(*values, **keywords):
            calls.append('launch')
            raise RuntimeError('modeled stop after admission')
        evidence = out / f'normalize-{name}'
        with patch.object(dev, 'run_rail_helper', helper), patch.object(dev, 'capture', capture), \
                patch.object(dev, 'run_return', run):
            code = dev.main(['cpu-return', '--port', '3-3', '--evidence', str(evidence), *argv])
        record = json.loads((evidence / 'dev-run.json').read_text())
        assert calls == expected and code == 1, (name, calls, record)
        if name == 'skipped':
            assert record['normalize_rails'] == 'not run: --no-normalize-rails', record
            assert record['rail_helper_reason'] == 'not run: --no-restore-rails', record
            continue
        assert record['normalize_rails']['supply'] == ['SUPPLY dcdc5_uv=1000000 dcdc3_uv=1050000'], record
        if complete:
            assert record['outcome'] == 'failed' and record['launched'], record
            assert record['rail_helper_reason'] == 'not run: run cold DDR first; the next launch lowers the rails'
            assert 'run cold DDR first' in record['summary'], record
        else:
            assert record['outcome'] == 'refused-before-launch' and not record['launched'], record
            assert 'modeled refusal' in record['summary'] and not (evidence / 'state.json').exists()
    # A helper interrupted after a normal return: the run's record is already saved.
    calls = []
    def helper(args, host, where):
        calls.append('helper')
        if where.name == 'rail-helper':
            raise KeyboardInterrupt
        return {'complete': True, 'supply': [], 'volt': [], 'rail_state': [], 'outcome': [], 'evidence': str(where)}
    def returned(args, host, bundle, live, run, **keywords):
        calls.append('launch')
        run.mkdir()
        (run / 'session.json').write_text(json.dumps({'complete': True, 'watchdog': {
            'mode_after_return': 0, 'breadcrumbs': {'last': {'name': 'RETURN_ROM'}}}}))
        (run / 'result.bin').write_bytes(b'')
        return {'secondaries_off': 7}
    def capture(fel):
        calls.append('capture')
        return captured_state()
    evidence = out / 'normalize-interrupted'
    with patch.object(dev, 'run_rail_helper', helper), patch.object(dev, 'capture', capture), \
            patch.object(dev, 'run_return', returned):
        try:
            dev.main(['cpu-return', '--port', '3-3', '--evidence', str(evidence)])
            raise AssertionError('the interrupt did not propagate')
        except KeyboardInterrupt:
            pass
    record = json.loads((evidence / 'dev-run.json').read_text())
    assert calls == ['helper', 'capture', 'launch', 'capture', 'helper'] and record['launched'], (calls, record)
    assert record['outcome'] == 'result-invalid' and record['rail_helper_reason'] == 'started after the run', record
    calls = []
    with patch.object(dev, 'run_rail_helper', lambda *a: calls.append('helper')):
        evidence = out / 'normalize-check-only'
        dev.main(['cpu-return', '--state', str(out / 'normalize-default/state.json'), '--check-only',
                  '--evidence', str(evidence)])
    record = json.loads((evidence / 'dev-run.json').read_text())
    assert not calls and record['outcome'] == 'checked' and record['normalize_rails'].startswith('not run'), record
    passed('normalize-and-restore-rails-order',
           cases=['default', 'helper-refused', 'skipped', 'interrupted-after-return', 'check-only'])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gpu-image', type=Path, action='append', default=[])
    args = parser.parse_args()
    out = args.out
    assert dev.CONTROL_IMAGE.is_file(), f'Build the control image first: {dev.CONTROL_IMAGE}'
    host, launcher = dev.kit()
    out.mkdir(parents=True, exist_ok=False)
    temporary = out / 'cases'
    temporary.mkdir()
    live = preparation_cases(host, launcher, temporary)
    refusal_cases(temporary, live)
    gpu_preparation_cases(host, launcher, temporary, live, args.gpu_image)
    transport_cases(host, launcher, temporary, live)
    helper_cases()
    controlled_cases(host, temporary)
    normalize_cases(host, temporary)
    result = {'passed': True, 'physical_access': False, 'control_image_sha256': dev.digest(dev.CONTROL_IMAGE),
              'cases': rows}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
