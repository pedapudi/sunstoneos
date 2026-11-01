#!/usr/bin/env python3
"""Prepare and run one returning A733 image for development.

No queue claim, review record, continuity chain or sealed manifest is read or
written. The run keeps the safety mechanisms of the existing transport
(tools/a733-returning-host.py): the per-port board lock, one launch, the
host-armed WDT0, the breadcrumb decoder and the instance quarantine on an
unresolved failure.

Kinds:
  cpu-return  one CPU-return application image, by default the hello-compute
              control image, through apps/os/cpu-return-host.py
  gpu-return  refused: the GPU ladder images are pre-split recognition images
              (see GPU_REFUSAL)
  npu         refused: see npu_check

A CPU-return image is recognised by its watchdog marker and its result record
(apps/os/host.py). Preparation builds the typed session from the image's
manifest: the board prefix, thermal record and CPU clock grant come from a
reviewed session (--records), with fresh identities and with the thermal clock
fields derived from a read-only FEL capture of the target; the INPUTS extent
describes the inputs given with --input, in manifest order. The SRAM launcher
admits the image's WDT0 mode (apps/os/prepare.py). Before launch, the capture
is compared with the initial-state predicates the image itself enforces
(apps/os/thermal.c, drivers/thermal/a733_thermal.c, apps/os/cpu-return-entry.c),
so a board whose state the image would refuse is reported without a launch.

The application's decoder (--decoder, or apps/<name>/host.py for an
application in this tree) is a Python module with:
  decode(payload)   the result payload as a JSON value; required
  expected(inputs)  the value decode() must return for these inputs; optional
  inputs()          the inputs used when no --input is given; optional
A run passes when the image returns normally (RETURN_ROM breadcrumb, WDT0
mode 0) and, when the decoder has expected(), the decoded result equals it.
Without a decoder the runner prints the raw payload.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
EVIDENCE = CACHE / 'evidence'
FEL = CACHE / 'build/sunxi-a733/sunxi-fel'
CLANG, LLD = Path('/usr/bin/clang-21'), CACHE / 'host/usr/bin/ld.lld-21'
# The hello-compute CPU-return image of this tree:
#   cmake ... -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=hello-compute -DSUNSTONE_A733_OS_CPU_RETURN=ON
#             -DA733_A76_ADMITTED=ON -DSUNSTONE_A733_OS_CPU_CLOCK=ON
#   cmake --build ... --target sunstone-a733-os-hello
CONTROL_IMAGE = CACHE / 'build/mimus-split-1031/control-hello/sunstone-a733-os-hello.elf'
# The reviewed session of the CPU-return control bundle: board prefix,
# thermal record and CPU clock grant (904-byte layout before typed sessions).
RECORDS = EVIDENCE / 'wdt-breadcrumbs-1031/control/offline-proof/prepared/board/session-template.bin'
RAIL_HELPER = EVIDENCE / 'wdt-breadcrumbs-1031/control/offline-proof/rails-800/probe.bin'
RAIL_HELPER_SHA256 = 'f48a6be72b336c8990a124fd608bba5fddb5c8617ec62eac5642b2d50fb27c75'
GPU_REFUSAL = ('gpu-return is refused: the GPU ladder images are pre-split mimus recognition images that need '
               'the fixed-layout session template and the three recognition inputs, which this runner no longer '
               'prepares, and the generic return path does not copy the GPU retirement report. Run them with '
               'the pre-split runner, tools/dev-run.py at revision 6a714bcf.')
# The read-only words of the existing CPU/thermal state capture
# (sol-resume-20261031/port-3-3/cpu-thermal-state): CCU, R_CCU, SID, CPU PLLs, RTC.
WORDS = (0x2002000, 0x20020a0, 0x20020a8, 0x20020ac, 0x2002518, 0x2002fc0, 0x2002fe4, 0x2003908,
         0x2003988, 0x701000c, 0x7010010, 0x701019c, 0x701020c, 0x3006244, 0x3006248, 0x300624c,
         *(0x8871000 + 0x1000 * d + o for d in range(3) for o in (0, 4, 8, 0x18, 0x1c)), 0x7090160)
# apps/os/cpu-return-entry.c: boot CPU-L, CPU-B and DSU PLL and mux words.
BOOT_PLL, BOOT_MUX = (0xf8802600, 0xf8802700, 0xf8802600), (0x03010000, 0x03000000, 0x03010105)
# Thermal record fields set from the live board (apps/os/os.h): APB1 word and
# rate, and whether the thermal owner configures GPADC24M.
THERMAL_APB1, THERMAL_CONFIGURE = 168, 176
# CPU clock grant fields (include/a733_cpu_clock.h): epoch and generation, clusters.
GRANT_SESSION, GRANT_CLUSTERS = 32, 48
STORAGE = 1   # the USB storage device bit (include/sunstone_app.h)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


# ---- live state ---------------------------------------------------------------

def capture(fel):
    """One read-only FEL request for WORDS, under the port lock; released after."""
    access = load('dev_access', ROOT / 'tools/usb-access.py')
    with access.board_lock() as node:
        access.require_session(node)
        access.require_rom(node)
        arguments = [x for a in WORDS for x in ('readl', hex(a))]
        result = subprocess.run([str(fel), '-d', f'{int(node.parent.name)}:{int(node.name)}', *arguments],
                                capture_output=True, text=True, timeout=15)
        require(result.returncode == 0, 'FEL state read failed: ' + result.stderr.strip())
        values = [int(x, 16) for x in result.stdout.split()]
        require(len(values) == len(WORDS), 'FEL state read returned the wrong word count.')
        return {'node': str(node), 'identity': access.identity(node), 'port': access.target(node).name,
                'time': time.strftime('%Y-%m-%dT%H:%M:%S'), 'words': {hex(a): v for a, v in zip(WORDS, values)}}


def speed_bin(sid_word):
    """The CPU clock driver's column for a SID DVFS word: 1-8, 9 for code 0, None if refused."""
    text = (ROOT / 'drivers/clock/a733_cpu_clock.c').read_text()
    codes = [int(v, 16) for v in re.search(r'codes\[\]=\{([^}]*)\}', text)[1].split(',')]
    code = (sid_word >> 16) & 255 or (sid_word >> 8) & 255
    return 9 if not code else codes.index(code) + 1 if code in codes else None


def state_check(words):
    """The image's own initial-state predicates on FEL-readable words.

    Returns (findings, derived thermal record fields). An empty findings list
    means the readable part of the native guards admits this board state.
    The PMIC rails (DCDC3 and DCDC5 must read 800 mV) are behind the PMIC
    controller and cannot be read here; see rails_note()."""
    w = {int(k, 16): v for k, v in words.items()}
    findings = []
    osc, ref = w[0x7090160], w[0x2002000]
    dcxo = (24000000, 19200000, 26000000, 24000000)[(osc >> 14) & 3]
    n, m = ((ref >> 8) & 255) + 1, ((ref >> 16) & 127) + 1
    if not ((osc & 0x10000002) == 0x10000002 and ref & 0xf8000000 == 0xf8000000 and
            80 <= n <= 116 and dcxo * n == 24000000 * m):
        findings.append('normalized 24 MHz PLL_REF invalid (thermal clocks, CPU guard oscillator bits)')
    if dcxo != 26000000:
        findings.append('DCXO is not 26 MHz (CPU entry guard)')
    apb1 = w[0x2002518] & 0x0300001f
    parent, div = apb1 >> 24, (apb1 & 31) + 1
    rate = {0: 24000000, 3: 600000000}.get(parent)
    if not rate or rate % div:
        findings.append(f'APB1 {apb1:#x} is not SYS24M or PERI0/600 MHz with an exact divider (thermal clocks)')
    if parent == 3 and not (w[0x20020a8] >> 31 == 0 and w[0x20020ac] & 0x8e000000 == 0 and
                            w[0x2003908] & 0x0a000a00 == 0x0a000a00 and w[0x2003988] & 0x0a000000 == 0x0a000000):
        findings.append('PERI0 pattern, LFM or output gates differ (thermal clocks with PERI0 APB1)')
    gpadc = w[0x2002fc0] & 0x8700001f
    if gpadc == 0x80000000 or (dcxo == 24000000 and gpadc == 0x81000000):
        configure = 0   # adopt the enabled normalized clock
    elif not gpadc >> 31:
        configure = 1   # the thermal owner configures the disabled clock
    else:
        configure = None
        findings.append(f'GPADC24M {w[0x2002fc0]:#x} is enabled with another source or divider (thermal clocks)')
    for d, name in enumerate(('CPU-L', 'CPU-B', 'DSU')):
        base = 0x8871000 + 0x1000 * d
        if w[base] != BOOT_PLL[d] or w[base + 0x1c] != BOOT_MUX[d]:
            findings.append(f'{name} PLL/mux {w[base]:#x}/{w[base + 0x1c]:#x} differ from the boot point '
                            f'{BOOT_PLL[d]:#x}/{BOOT_MUX[d]:#x} (CPU entry guard)')
        if w[base + 4] & (3 << 29) or w[base + 8] >> 31 or w[base + 0x18] >> 31:
            findings.append(f'{name} pattern or LFM enabled (CPU entry guard)')
    if speed_bin(w[0x300624c]) != 4:
        findings.append(f'SID DVFS word {w[0x300624c]:#x} does not select speed bin 4 (CPU entry guard)')
    derived = {'apb1': apb1, 'apb1_hz': rate // div if rate and not rate % div else None,
               'configure_adc_clock': configure}
    return findings, derived


def rails_note(record):
    if record.get('complete'):
        return ('CPU-B was lowered to 1014 MHz and DCDC3 to 800 mV before return; DCDC5 stays at 1000 mV. '
                'The next CPU-return image refuses (detail 0x736) until the rail helper restores it.')
    return ('Unknown after a failed run: a reset restores boot PLLs but not PMIC rails. If the run raised '
            'them, the next CPU-return entry guard refuses (detail 0x736) and the rail helper refuses '
            'a DCDC3 other than 800 mV; only USB power removal resets both rails.')


# ---- preparation --------------------------------------------------------------

def session_records(path, host):
    """The board prefix, thermal record and CPU clock grant of a reviewed session."""
    raw = Path(path).read_bytes()
    size = host.PREFIX_BYTES
    if len(raw) > size + 8 and struct.unpack_from('<Q', raw, size)[0] == host.SESSION_MAGIC:
        extents = host.session_decode(raw)['extents']
        thermal, grant = extents[host.THERMAL], extents.get(host.CPU_GRANT)
    else:
        # The fixed layout: prefix, thermal record, CPU clock grant.
        thermal = raw[size:size + host.SERVICE_BYTES[host.THERMAL]]
        grant = raw[size + len(thermal):]
        require(struct.unpack_from('<Q', thermal)[0] == host.SERVICE_MAGIC[host.THERMAL],
                'Reviewed session has no thermal record.')
    require(grant is not None and len(grant) == host.SERVICE_BYTES[host.CPU_GRANT] and
            struct.unpack_from('<Q', grant)[0] == host.SERVICE_MAGIC[host.CPU_GRANT],
            'Reviewed session has no CPU clock grant; the CPU-return profile requires one.')
    return raw[:size], thermal, grant


def kit():
    """The OS CPU-return host (apps/os/cpu-return-host.py) and launcher (apps/os/prepare.py)."""
    os.environ.setdefault('SUNSTONEOS_DIR', str(ROOT))
    return (load('dev_cpu_return_host', ROOT / 'apps/os/cpu-return-host.py'),
            load('dev_os_launcher', ROOT / 'apps/os/prepare.py'))


def prepare(host, launcher, image, records, inputs, output, live):
    """Board bundle from a CPU-return image, a reviewed session's records and the inputs."""
    os_host = host.host
    data = Path(image).read_bytes()
    watchdog = os_host.watchdog_marker(data)
    require(watchdog is not None, 'Image carries no host-armed watchdog marker; a hang would strand the board.')
    symbols = host.linked_symbols(image)
    require('ss_result_record' in symbols, 'Image has no result record; it is not a CPU-return application image.')
    require('os_gpu_configuration' not in symbols, 'Image links the GPU return; the generic return path does not '
            'copy its GPU retirement report.')
    info = os_host.image_manifest(data)
    require(info['manifest'] is not None, 'Image has no application manifest.')
    manifest = info['manifest']
    devices = manifest['devices_required']
    require(not devices & STORAGE, 'The CPU return admits only sessions without USB storage (workload 2).')
    findings, derived = state_check(live['words'])
    prefix, thermal, grant = session_records(records, os_host)
    prefix, thermal, grant = bytearray(prefix), bytearray(thermal), bytearray(grant)
    require(struct.unpack_from('<I', prefix, 400)[0] == 2, 'Reviewed board prefix is not workload 2.')
    launcher.board().session_check(bytes(prefix), usb_reference_modes=(1,))
    # Fresh identities: no record of an earlier run (DRAM result, report)
    # can match this session. The grant names the same session.
    epoch, generation, lease = (secrets.randbits(62) | 1 for _ in range(3))
    struct.pack_into('<3Q', prefix, 24, epoch, generation, lease)
    output.mkdir(parents=True)
    board = output / 'board'
    board.mkdir()
    (board / 'image.elf').write_bytes(data)
    segments, address, identity = host.base.image_parts(board / 'image.elf')
    prefix[48:80] = identity
    # The record's size field names the complete fixed-layout session.
    struct.pack_into('<Q', thermal, 16, os_host.PREFIX_BYTES + len(thermal) + len(grant))
    if derived['apb1_hz']:
        struct.pack_into('<2I', thermal, THERMAL_APB1, derived['apb1'], derived['apb1_hz'])
    if derived['configure_adc_clock'] is not None:
        thermal[THERMAL_CONFIGURE] = derived['configure_adc_clock']
    if struct.unpack_from('<I', grant, GRANT_CLUSTERS)[0]:
        struct.pack_into('<2Q', grant, GRANT_SESSION, epoch, generation)
    values = [bytes(v) for v in inputs]
    raw = os_host.session_bytes(bytes(prefix), {os_host.THERMAL: bytes(thermal), os_host.CPU_GRANT: bytes(grant)},
                                manifest, info['sha256'], devices, os_host.BIND_SID, values)
    launcher.build_launcher(board, len(raw), str(CLANG), str(LLD), watchdog['mode'])
    uploads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            (board / f'segment-{index}.bin').write_bytes(segment['data'])
            uploads.append({'name': f'segment-{index}.bin', 'address': segment['address']})
    (board / 'session.bin').write_bytes(raw)
    uploads.append({'name': 'session.bin', 'address': launcher.SESSION_ADDRESS})
    for name, value in zip(('request.bin', 'contract.bin'), launcher.request_bytes(address, identity, raw)):
        (board / name).write_bytes(value)
    (board / 'result-zero.bin').write_bytes(bytes(16384))
    (board / 'report-zero.bin').write_bytes(bytes(128))
    _, planned = os_host.input_facts(manifest, values)
    (output / 'inputs').mkdir()
    for item, value in zip(planned, values):
        item['name'] = 'input-%d.bin' % item.pop('index')
        (output / 'inputs' / item['name']).write_bytes(value)
    names = ['launch.bin', 'session.bin', 'request.bin', 'contract.bin', 'result-zero.bin', 'report-zero.bin',
             'image.elf'] + [u['name'] for u in uploads if u['name'].startswith('segment-')]
    meta = {'schema': 'sunstoneos-dev-bundle-v2', 'image_sha256': hashlib.sha256(data).hexdigest(),
            'application': manifest['name'], 'build_id': identity.hex(), 'epoch': epoch,
            'generation': generation, 'lease': lease, 'devices': devices,
            'workload': struct.unpack_from('<I', raw, 400)[0], 'segments': uploads, 'inputs': planned,
            'files': {n: digest(board / n) for n in names},
            'records': {'path': str(Path(records).resolve()), 'sha256': digest(records)},
            'derived_session': derived, 'state_findings': findings, 'watchdog': watchdog}
    save(output / 'dev-bundle.json', meta)
    return meta


def validator(host):
    cached = {}
    def validate(bundle):
        bundle = Path(bundle)
        meta = json.loads((bundle / 'dev-bundle.json').read_text())
        require(meta.get('schema') == 'sunstoneos-dev-bundle-v2', 'Unsupported development bundle.')
        board = bundle / 'board'
        files = {n: digest(board / n) for n in meta['files']}
        require(files == meta['files'], 'Development bundle changed after preparation.')
        inputs = [dict(item, path=str(bundle / 'inputs' / item['name'])) for item in meta['inputs']]
        require(all(digest(item['path']) == item['sha256'] for item in inputs), 'Development input changed.')
        if meta['image_sha256'] not in cached:
            cached[meta['image_sha256']] = host.geometry(board / 'image.elf')
        info = host.host.image_manifest(board / 'image.elf')
        decoded = host.host.session_decode((board / 'session.bin').read_bytes(), info['manifest'], info['sha256'])
        facts, planned = host.host.input_facts(info['manifest'], [Path(i['path']).read_bytes() for i in inputs])
        require(decoded['extents'].get(host.host.INPUTS, b'') == facts and
                [(p['address'], p['bytes']) for p in planned] == [(i['address'], i['bytes']) for i in inputs],
                'Session inputs differ from the development inputs.')
        p = {k: meta[k] for k in ('image_sha256', 'build_id', 'epoch', 'generation', 'lease', 'workload', 'segments')}
        p.update(files=files, _sizes={n: (board / n).stat().st_size for n in files},
                 qualification=cached[meta['image_sha256']], _inputs=inputs)
        return p
    return validate


# ---- runs -----------------------------------------------------------------------

class NoHold:
    """The transport's base module without the indefinite ownership hold.

    With WDT0 armed the board cannot stay in a running image past the
    watchdog interval, and an unresolved instance is still quarantined; there
    is no queue claim to keep, so the process exits after recording."""
    def __init__(self, base):
        self._base = base
    def __getattr__(self, name):
        return getattr(self._base, name)
    @staticmethod
    def hold_ownership():
        print('Run failed after launch; instance quarantined; lock released.', file=sys.stderr)


def run_return(args, host, bundle, live, out, access=None, guard=None):
    """access and guard are the transport's injection points for modeled checks."""
    validate = validator(host)
    p = validate(bundle)
    watchdog = p['qualification'].get('watchdog')
    require(watchdog, 'Image geometry has no watchdog mode.')
    def execute(tx, launch, ready, prepared, record, journal):
        require(record['initial_identity'] == live['identity'] and record['port'] == live['port'],
                'Target changed since the state capture.')
        again = [int(x, 16) for x in tx(*[x for a in WORDS for x in ('readl', hex(a))]).split()]
        require({hex(a): v for a, v in zip(WORDS, again)} == live['words'],
                'Board state changed since the state capture.')
        return host.run_once(tx, launch, ready, bundle, prepared, out, record, journal)
    ns = argparse.Namespace(bundle=bundle, evidence=out, fel=args.fel, root_owned_claim='development run')
    return host.cpu.returning.run(ns, base=NoHold(host.base), profile=host.PROFILE, validate_bundle=validate,
                                  admit=lambda *a: None, execute=execute, description='development return run',
                                  return_timeout=90, watchdog=watchdog, access=access, guard=guard)


def judge(decoder, payload, inputs, session):
    """(outcome, detail, decoded) of a run that returned with this payload."""
    w = session.get('watchdog', {})
    last = w.get('breadcrumbs', {}).get('last', {}).get('name')
    if w.get('mode_after_return') != 0 or last != 'RETURN_ROM':
        return 'failed', f'return not normal: WDT0 mode {w.get("mode_after_return")}, last breadcrumb {last}', None
    if decoder is None:
        shown = payload[:64].hex() + ('...' if len(payload) > 64 else '')
        return 'returned', f'raw result {len(payload)} bytes {shown}', None
    try:
        decoded = decoder.decode(payload)
    except (ValueError, struct.error) as error:
        return 'result-invalid', f'decoder refused the result: {error}', None
    if not hasattr(decoder, 'expected'):
        return 'returned', 'result ' + json.dumps(decoded, sort_keys=True), decoded
    expected = decoder.expected(inputs)
    if decoded != expected:
        return 'result-mismatch', 'result %s, expected %s' % (json.dumps(decoded, sort_keys=True),
                                                             json.dumps(expected, sort_keys=True)), decoded
    return 'passed', 'result ' + json.dumps(decoded, sort_keys=True) + ' as expected', decoded


def restore_rails(args, host, evidence):
    """The existing rail helper (board bundle 33) through the existing kernel runner."""
    require(digest(args.rail_helper) == RAIL_HELPER_SHA256, 'Rail helper binary differs from the approved one.')
    followup = load('dev_followup', ROOT / 'apps/os/cpu-return-followup.py')
    out = evidence / 'rail-helper'
    code = host.cpu.kernel.main(['--fel', str(args.fel), '--image', str(args.rail_helper), '--evidence', str(out),
                                 '--mode', 'kernel', '--expect', followup.COMPLETE, '--return-timeout', '10'])
    require(code in (None, 0), 'Rail helper runner failed.')
    return followup.helper_result(json.loads((out / 'session.json').read_text()), (out / 'kernel.log').read_text())


def npu_check(image, host):
    """Bringup's NPU launcher and monitor refuse an enabled WDT0 (launch.c ADMIT,
    monitor.c finish(1)); armed, the launcher parks and the watchdog resets the
    board 16 s later with no breadcrumbs. Only an image with the watchdog
    marker admits the armed mode, and the NPU session's reviewed authority
    records have no development producer."""
    require(host.watchdog_marker(image) is not None, 'npu is refused: the image does not admit the host-armed '
            'watchdog (no os_cpu_return_watchdog marker); rebuild it on work/wdt-breadcrumbs-1031.')
    require(False, 'npu is refused: development preparation of an NPU session, whose NPU extent cites reviewed '
            'admission, clock, power, supervision and DDR records, is not implemented.')


def summary_of(kind, outcome, detail):
    return f'{kind}: {outcome}: {detail}'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('kind', choices=('cpu-return', 'gpu-return', 'npu'))
    parser.add_argument('--image', type=Path, help='CPU-return image ELF, from this tree or an application '
                        'such as mimus (default: the hello-compute control image)')
    parser.add_argument('--input', type=Path, action='append', default=[],
                        help="An INPUT region's content, once per region in manifest order")
    parser.add_argument('--decoder', type=Path, help='Python module that decodes the result payload '
                        '(default: apps/<name>/host.py in this tree when present; otherwise the raw payload)')
    parser.add_argument('--records', type=Path, default=RECORDS,
                        help='Reviewed session supplying the board prefix, thermal record and CPU clock grant')
    parser.add_argument('--port', help='Physical USB port, for example 3-1 (else SUNSTONE_USB_PORT)')
    parser.add_argument('--evidence', type=Path, help='New evidence directory')
    parser.add_argument('--state', type=Path, help='Use this captured state.json instead of reading the board')
    parser.add_argument('--check-only', action='store_true', help='Prepare and check; no launch')
    parser.add_argument('--force', action='store_true', help='Launch despite state findings')
    parser.add_argument('--restore-rails', action='store_true', help='Run the rail helper after a normal return')
    parser.add_argument('--rail-helper', type=Path, default=RAIL_HELPER)
    parser.add_argument('--fel', type=Path, default=FEL)
    args = parser.parse_args(argv)
    os.environ.setdefault('PYTHONDONTWRITEBYTECODE', '1')
    sys.dont_write_bytecode = True
    if args.kind == 'gpu-return':
        parser.exit(2, GPU_REFUSAL + '\n')
    host, launcher = kit()
    if args.kind == 'npu':
        try:
            require(args.image, 'npu is refused: npu requires --image.')
            npu_check(args.image, host.host)
        except ValueError as error:
            parser.exit(2, f'{error}\n')
    if args.port:
        os.environ['SUNSTONE_USB_PORT'] = args.port
    port = os.environ.get('SUNSTONE_USB_PORT')
    require(port or args.state, 'Select the board with --port or SUNSTONE_USB_PORT.')
    image = args.image or CONTROL_IMAGE
    info = host.host.image_manifest(image)
    require(info['manifest'] is not None, 'Image has no application manifest.')
    name = info['manifest']['name']
    decoder_path = args.decoder or (ROOT / 'apps' / name / 'host.py')
    decoder = load('dev_decoder', decoder_path) if args.decoder or decoder_path.is_file() else None
    if args.input:
        inputs = [p.read_bytes() for p in args.input]
    else:
        require(decoder is not None and hasattr(decoder, 'inputs') or
                not host.host.regions(info['manifest'], (1,)), f'{name} declares inputs; give them with --input.')
        inputs = decoder.inputs() if decoder is not None and hasattr(decoder, 'inputs') else []
    evidence = args.evidence or EVIDENCE / 'dev-runs-1031' / f"{port or 'offline'}-{args.kind}-{time.strftime('%H%M%S')}"
    evidence.mkdir(parents=True, exist_ok=False)
    live = json.loads(args.state.read_text()) if args.state else capture(args.fel)
    save(evidence / 'state.json', live)
    record = {'kind': args.kind, 'port': live['port'], 'identity': live['identity'], 'image': str(Path(image).resolve()),
              'application': name, 'decoder': str(decoder_path.resolve()) if decoder else None,
              'bundle': str(evidence / 'bundle'), 'launched': False}
    try:
        meta = prepare(host, launcher, image, args.records, inputs, evidence / 'bundle', live)
    except ValueError as error:
        meta = None
        record.update(outcome='refused-before-launch', summary=summary_of(args.kind, 'not launched', str(error)))
    if meta:
        record.update(image_sha256=meta['image_sha256'], derived_session=meta['derived_session'],
                      state_findings=meta['state_findings'])
    if meta is None:
        pass   # the preparation refusal is recorded above
    elif meta['state_findings'] and not args.force:
        record.update(outcome='refused-before-launch', summary=summary_of(
            args.kind, 'not launched', '; '.join(meta['state_findings'])))
    elif args.check_only:
        validator(host)(evidence / 'bundle')
        record.update(outcome='checked', summary=summary_of(args.kind, 'prepared and checked',
            f"{name} image {meta['image_sha256'][:12]}, WDT0 mode {meta['watchdog']['mode']:#x}, "
            f"session {meta['watchdog']['session_seconds']} s, {len(inputs)} input(s), "
            f"GPADC {'adopt' if meta['derived_session']['configure_adc_clock'] == 0 else 'configure'}"))
    else:
        out = evidence / 'run'
        record['launched'] = True
        try:
            result = run_return(args, host, evidence / 'bundle', live, out)
            session = json.loads((out / 'session.json').read_text())
            payload = (out / 'result.bin').read_bytes()
            outcome, detail, decoded = judge(decoder, payload, inputs, session)
            record.update(outcome=outcome, result=result, payload=str(out / 'result.bin'), decoded=decoded)
            record['summary'] = summary_of(args.kind, outcome, 'secondaries off %d; %s' % (
                result['secondaries_off'], detail))
        except Exception as error:
            report = out / 'watchdog-report.json'
            if report.exists():
                record.update(outcome='watchdog-reset', watchdog=json.loads(report.read_text()))
                record['summary'] = summary_of(args.kind, 'watchdog reset', record['watchdog']['summary'] +
                                               '; run cold DDR before the next image')
            else:
                record.update(outcome='failed', error=str(error))
                record['summary'] = summary_of(args.kind, 'failed', str(error))
        session = out / 'session.json'
        run_record = json.loads(session.read_text()) if session.exists() else {}
        record['rails'] = rails_note(run_record)
        if args.restore_rails and run_record.get('complete'):
            try:
                record['rail_helper'] = restore_rails(args, host, evidence)
                record['rails'] = 'DCDC3 and DCDC5 restored to 800 mV by the rail helper.'
            except Exception as error:
                record['rail_helper_error'] = str(error)
    save(evidence / 'dev-run.json', record)
    print(record['summary'])
    print('evidence:', evidence / 'dev-run.json')
    return 0 if record['outcome'] in ('passed', 'returned', 'checked') else 1


if __name__ == '__main__':
    sys.exit(main())
