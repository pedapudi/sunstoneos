#!/usr/bin/env python3
"""Prepare and run one returning A733 image for development.

No queue claim, review record, continuity chain or sealed manifest is read or
written. The run keeps the safety mechanisms of the existing transport
(tools/a733-returning-host.py): the per-port board lock, one launch, the
host-armed WDT0, the breadcrumb decoder and the instance quarantine on an
unresolved failure.

Kinds:
  cpu-return  one recognition image through apps/os/cpu-return-host.py
  gpu-return  one GPU bisection ladder image through the same host, loaded
              from the ladder's source tree (its GPU report decoder)
  npu         an NPU returning image; refused unless the image admits the
              host-armed watchdog (see npu_check)

Preparation builds the board bundle from the image ELF, a session template
and the three recognition inputs with the existing prepare.py functions. It
replaces the session identities with fresh ones and derives the thermal clock
fields from a read-only FEL capture of the target. Before launch, that capture
is compared with the initial-state predicates the image itself enforces
(apps/os/thermal.c, drivers/thermal/a733_thermal.c, apps/os/cpu-return-entry.c),
so a board whose state the image would refuse is reported without a launch.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
EVIDENCE = CACHE / 'evidence'
FEL = CACHE / 'build/sunxi-a733/sunxi-fel'
CLANG, LLD = Path('/usr/bin/clang-21'), CACHE / 'host/usr/bin/ld.lld-21'
CONTROL_IMAGE = EVIDENCE / 'wdt-breadcrumbs-1031/control/package/native/image.elf'
CONTROL = EVIDENCE / 'wdt-breadcrumbs-1031/control/offline-proof/prepared'
GPU_TREE = Path.home() / 'git/sunstoneos-worktrees/gpu-bisect-1031-crumbs'
GPU_LADDER = CACHE / 'build/gpu-bisect-1031/crumbs/ladder-wdt2'
GPU_TEMPLATE = EVIDENCE / 'sol-gpu-completion-1031/prepared'
RAIL_HELPER = EVIDENCE / 'wdt-breadcrumbs-1031/control/offline-proof/rails-800/probe.bin'
RAIL_HELPER_SHA256 = 'f48a6be72b336c8990a124fd608bba5fddb5c8617ec62eac5642b2d50fb27c75'
# The read-only words of the existing CPU/thermal state capture
# (sol-resume-20261031/port-3-3/cpu-thermal-state): CCU, R_CCU, SID, CPU PLLs, RTC.
WORDS = (0x2002000, 0x20020a0, 0x20020a8, 0x20020ac, 0x2002518, 0x2002fc0, 0x2002fe4, 0x2003908,
         0x2003988, 0x701000c, 0x7010010, 0x701019c, 0x701020c, 0x3006244, 0x3006248, 0x300624c,
         *(0x8871000 + 0x1000 * d + o for d in range(3) for o in (0, 4, 8, 0x18, 0x1c)), 0x7090160)
# apps/os/cpu-return-entry.c: boot CPU-L, CPU-B and DSU PLL and mux words.
BOOT_PLL, BOOT_MUX = (0xf8802600, 0xf8802700, 0xf8802600), (0x03010000, 0x03000000, 0x03010105)


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


def symbol(elf, name):
    """A defined symbol by exact name, including C++ internal-linkage names."""
    found = [s for s in elf.symbols if s['shndx'] != 0 and (s['name'] == name or s['name'].endswith(
        str(len(name)) + name))]
    return found[0] if len(found) == 1 else None


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


def state_check(words, os_image):
    """The image's own initial-state predicates on FEL-readable words.

    Returns (findings, derived session thermal fields). An empty findings list
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
    if os_image.bin_column(w[0x300624c]) != 4:
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

def prepare(host, image, template, output, live):
    """Board bundle from the ELF, a template bundle's session template and assets."""
    o = host.os_image
    data = Path(image).read_bytes()
    elf = o.os_link.Elf(data)
    require(symbol(elf, 'os_cpu_return_output') is not None, 'Image is not a CPU-return image.')
    watchdog = o.watchdog_marker(elf)
    require(watchdog is not None, 'Image carries no host-armed watchdog marker; a hang would strand the board.')
    raw = bytearray((Path(template) / 'board/session-template.bin').read_bytes())
    # Fresh identities: no record of an earlier run (DRAM result, report)
    # can match this session. The grant record names the same session.
    epoch, generation, lease = (secrets.randbits(62) | 1 for _ in range(3))
    struct.pack_into('<3Q', raw, 24, epoch, generation, lease)
    grant = o.grant_fields(raw)
    if grant and grant['clusters']:
        struct.pack_into('<2Q', raw, len(raw) - o.GRANT_BYTES + 32, epoch, generation)
    findings, derived = state_check(live['words'], o)
    if derived['apb1_hz']:
        struct.pack_into('<2I', raw, 760, derived['apb1'], derived['apb1_hz'])
    if derived['configure_adc_clock'] is not None:
        raw[768] = derived['configure_adc_clock']
    o.session_check(raw, False)
    output.mkdir(parents=True)
    board = output / 'board'
    board.mkdir()
    (board / 'image.elf').write_bytes(data)
    segments, address, identity = o.base.image_parts(board / 'image.elf')
    raw[48:80] = identity
    o.build_launcher(board, len(raw), str(CLANG), str(LLD), watchdog['mode'])
    uploads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            (board / f'segment-{index}.bin').write_bytes(segment['data'])
            uploads.append({'name': f'segment-{index}.bin', 'address': segment['address']})
    (board / 'session.bin').write_bytes(raw)
    uploads.append({'name': 'session.bin', 'address': o.SESSION_ADDRESS})
    for name, value in zip(('request.bin', 'contract.bin'), o.request_bytes(address, identity, raw)):
        (board / name).write_bytes(value)
    (board / 'result-zero.bin').write_bytes(bytes(16384))
    (board / 'report-zero.bin').write_bytes(bytes(128))
    shutil.copytree(Path(template) / 'assets', output / 'assets')
    assets = o.host.validate_assets(output / 'assets')['assets']
    linked = symbol(elf, 'mimus_asset_sha256')
    if linked is not None:
        require(elf.read(linked['value'], 96) == b''.join(bytes.fromhex(a['sha256']) for a in assets),
                'Recognition inputs differ from the hashes linked into the image.')
    names = ['launch.bin', 'session.bin', 'request.bin', 'contract.bin', 'result-zero.bin', 'report-zero.bin',
             'image.elf'] + [u['name'] for u in uploads if u['name'].startswith('segment-')]
    meta = {'schema': 'sunstoneos-dev-bundle-v1', 'image_sha256': hashlib.sha256(data).hexdigest(),
            'build_id': identity.hex(), 'epoch': epoch, 'generation': generation, 'lease': lease,
            'workload': struct.unpack_from('<I', raw, 400)[0], 'segments': uploads,
            'files': {n: digest(board / n) for n in names}, 'template': str(Path(template).resolve()),
            'derived_session': derived, 'state_findings': findings, 'watchdog': watchdog}
    save(output / 'dev-bundle.json', meta)
    return meta


def validator(host):
    cached = {}
    def validate(bundle):
        bundle = Path(bundle)
        meta = json.loads((bundle / 'dev-bundle.json').read_text())
        board = bundle / 'board'
        files = {n: digest(board / n) for n in meta['files']}
        require(files == meta['files'], 'Development bundle changed after preparation.')
        if meta['image_sha256'] not in cached:
            cached[meta['image_sha256']] = host.geometry(board / 'image.elf')
        p = {k: meta[k] for k in ('image_sha256', 'build_id', 'epoch', 'generation', 'lease', 'workload', 'segments')}
        p.update(files=files, _sizes={n: (board / n).stat().st_size for n in files},
                 qualification=cached[meta['image_sha256']],
                 _assets=host.os_image.host.validate_assets(bundle / 'assets')['assets'])
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
        print('Run failed after launch; lock released; the run record states whether the instance was quarantined.', file=sys.stderr)


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


def restore_rails(args, host, evidence):
    """The existing rail helper (board bundle 33) through the existing kernel runner."""
    require(digest(args.rail_helper) == RAIL_HELPER_SHA256, 'Rail helper binary differs from the approved one.')
    followup = load('dev_followup', host.ROOT / 'apps/os/cpu-return-followup.py')
    out = evidence / 'rail-helper'
    code = host.cpu.kernel.main(['--fel', str(args.fel), '--image', str(args.rail_helper), '--evidence', str(out),
                                 '--mode', 'kernel', '--expect', followup.COMPLETE, '--return-timeout', '10'])
    require(code in (None, 0), 'Rail helper runner failed.')
    return followup.helper_result(json.loads((out / 'session.json').read_text()), (out / 'kernel.log').read_text())


def npu_check(image):
    """Bringup's NPU launcher and monitor refuse an enabled WDT0 (launch.c ADMIT,
    monitor.c finish(1)); armed, the launcher parks and the watchdog resets the
    board 16 s later with no breadcrumbs. Only an image with the watchdog
    marker admits the armed mode."""
    os_link = load('dev_os_link', ROOT / 'checks/os-image-link.py')
    marker = symbol(os_link.Elf(Path(image).read_bytes()), 'os_cpu_return_watchdog')
    require(marker is not None, 'NPU image does not admit the host-armed watchdog (no os_cpu_return_watchdog '
            'marker); rebuild it on work/wdt-breadcrumbs-1031 before a development run.')
    require(False, 'NPU development preparation is not implemented for watchdog-admitting images yet.')


def summary_of(kind, outcome, detail):
    return f'{kind}: {outcome}: {detail}'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('kind', choices=('cpu-return', 'gpu-return', 'npu'))
    parser.add_argument('--image', type=Path, help='Image ELF (default: the control image or ladder stage)')
    parser.add_argument('--stage', type=int, help='gpu-return: ladder stage number (1-9)')
    parser.add_argument('--ladder', type=Path, default=GPU_LADDER, help='gpu-return: ladder directory')
    parser.add_argument('--template', type=Path, help='Bundle supplying session-template.bin and assets/')
    parser.add_argument('--host-tree', type=Path, help='Source tree whose host modules run the image')
    parser.add_argument('--port', help='Physical USB port, for example 3-1 (else SUNSTONE_USB_PORT)')
    parser.add_argument('--evidence', type=Path, help='New evidence directory')
    parser.add_argument('--state', type=Path, help='Use this captured state.json instead of reading the board')
    parser.add_argument('--check-only', action='store_true', help='Prepare and check; no launch')
    parser.add_argument('--force', action='store_true', help='Launch despite state findings')
    parser.add_argument('--restore-rails', action='store_true', help='Run the rail helper after a normal return')
    parser.add_argument('--rail-helper', type=Path, default=RAIL_HELPER)
    parser.add_argument('--fel', type=Path, default=FEL)
    args = parser.parse_args(argv)
    if args.port:
        os.environ['SUNSTONE_USB_PORT'] = args.port
    port = os.environ.get('SUNSTONE_USB_PORT')
    require(port or args.state, 'Select the board with --port or SUNSTONE_USB_PORT.')
    if args.kind == 'npu':
        require(args.image, 'npu requires --image.')
        npu_check(args.image)
    if args.kind == 'gpu-return':
        tree = args.host_tree or GPU_TREE
        if args.image is None:
            require(args.stage, 'gpu-return requires --image or --stage.')
            stages = sorted(args.ladder.glob(f'{args.stage}-*/sunstone-a733-os.elf'))
            require(len(stages) == 1, f'No single ladder stage {args.stage} image in {args.ladder}.')
            args.image = stages[0]
        template = args.template or GPU_TEMPLATE
    else:
        tree = args.host_tree or ROOT
        args.image = args.image or CONTROL_IMAGE
        template = args.template or CONTROL
    evidence = args.evidence or EVIDENCE / 'dev-runs-1031' / f"{port or 'offline'}-{args.kind}-{time.strftime('%H%M%S')}"
    evidence.mkdir(parents=True, exist_ok=False)
    os.environ.setdefault('PYTHONDONTWRITEBYTECODE', '1')
    sys.dont_write_bytecode = True
    host = load('dev_cpu_return_host', tree / 'apps/os/cpu-return-host.py')
    live = json.loads(args.state.read_text()) if args.state else capture(args.fel)
    save(evidence / 'state.json', live)
    meta = prepare(host, args.image, template, evidence / 'bundle', live)
    record = {'kind': args.kind, 'port': live['port'], 'identity': live['identity'], 'host_tree': str(tree),
              'image': str(args.image.resolve()), 'image_sha256': meta['image_sha256'],
              'bundle': str(evidence / 'bundle'), 'derived_session': meta['derived_session'],
              'state_findings': meta['state_findings'], 'launched': False}
    if meta['state_findings'] and not args.force:
        record.update(outcome='refused-before-launch', summary=summary_of(
            args.kind, 'not launched', '; '.join(meta['state_findings'])))
    elif args.check_only:
        validator(host)(evidence / 'bundle')
        record.update(outcome='checked', summary=summary_of(args.kind, 'prepared and checked',
            f"image {meta['image_sha256'][:12]}, WDT0 mode {meta['watchdog']['mode']:#x}, session {meta['watchdog']['session_seconds']} s, "
            f"GPADC {'adopt' if meta['derived_session']['configure_adc_clock'] == 0 else 'configure'}"))
    else:
        out = evidence / 'run'
        record['launched'] = True
        try:
            result = run_return(args, host, evidence / 'bundle', live, out)
            record.update(outcome='returned', result=result)
            gpu = result.get('gpu')
            transcript = out / 'transcript.txt'
            require(transcript.is_file(), 'Normal return wrote no transcript.')
            lines = [x for x in transcript.read_text().splitlines() if x.startswith(('TEXT ', 'PASS ', 'FAIL '))]
            record.update(transcript=str(transcript), transcript_result=lines)
            record['summary'] = summary_of(args.kind, 'returned', 'secondaries off %d%s; %s; transcript %s' % (
                result['secondaries_off'], f', GPU report {gpu}' if gpu else '', ' | '.join(lines), transcript))
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
        if args.restore_rails and record['outcome'] == 'returned':
            try:
                record['rail_helper'] = restore_rails(args, host, evidence)
                record['rails'] = 'DCDC3 and DCDC5 restored to 800 mV by the rail helper.'
            except Exception as error:
                record['rail_helper_error'] = str(error)
    save(evidence / 'dev-run.json', record)
    print(record['summary'])
    print('evidence:', evidence / 'dev-run.json')
    return 0 if record['outcome'] in ('returned', 'checked') else 1


if __name__ == '__main__':
    sys.exit(main())
