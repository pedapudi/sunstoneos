#!/usr/bin/env python3
"""Finish one reviewed CPU-return claim through cache/survey and rail restoration.

Only a successful receipt's exact successor ROM is admitted. This command reads
queue state; it never submits, reviews, claims or finishes a queue item. Helper
RAM is explicitly retired, not restored. Ordinary survey admission is unchanged.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


cpu = module('compound_cpu_host', 'apps/os/cpu-return-host.py')
survey = module('compound_survey', 'tools/run-launch-survey.py')
base, require, digest = cpu.base, cpu.require, cpu.digest
SCHEMA = 'a733-os-cpu-return-compound-followup-v1'
PREREQUISITE = 'cpu-return-compound-followup'
CACHE_SHA = '1417834e1f273ead64cf151fc0843baf7ded9c6287a521c7b5c7f82e0acddc4a'
COMPLETE = ('COMPLETE board bundle 33: DCDC5 restored to 800 mV with DCDC3 '
            'already at 800 mV and all boot clock words')
PHASES = ('parity', 'cache-off', 'survey', 'rail-helper', 'final-proof')
SOURCES = ('apps/os/cpu-return-followup.py', 'apps/os/cpu-return-host.py', 'apps/os/prepare.py',
           'apps/os/cpu-return-clock-review.py', 'tools/run-launch-survey.py',
           'tools/check-a733-kernel.py', 'tools/check-a733-smp.py', 'tools/usb-access.py',
           'checks/mimus-parity.py', 'boot/a733/launch-survey.S', 'apps/os/gpu-return.py',
           'checks/os-image-link.py', 'apps/cpu-runtime-qualification/host.py',
           'apps/os/cpu-return-review.py')


def evidence(item):
    require(isinstance(item, dict) and set(item) == {'path', 'sha256'} and
            Path(item['path']).is_absolute(), 'Invalid compound artifact reference.')
    path = Path(item['path'])
    require(digest(path) == item['sha256'], 'Compound artifact changed: ' + str(path))
    return path


def claim(args, expected=None):
    """Read the actual existing claim; never change a row or recovery identity."""
    db = sqlite3.connect(args.database.resolve().as_uri() + '?mode=ro', uri=True)
    db.row_factory = sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],
                'Compound queue is halted.')
        row = db.execute('SELECT * FROM tests WHERE id=?', (args.claim,)).fetchone()
        require(row is not None and row['state'] == 'claimed' and row['owner'] == args.owner,
                'The original CPU claim must remain claimed by its owner.')
        port = db.execute("SELECT value FROM settings WHERE key='port'").fetchone()[0]
        submission, review = json.loads(row['submission']), json.loads(row['review'])
        require(PREREQUISITE in submission['prerequisites'] and PREREQUISITE in review['prerequisites'],
                'Queue did not preapprove the CPU compound continuation.')
        item = review['prerequisites'][PREREQUISITE]
        require(evidence(item).resolve() == args.review.resolve(), 'Queue covers another compound review.')
        snapshot = {'submission': hashlib.sha256(row['submission'].encode()).hexdigest(),
                    'review': hashlib.sha256(row['review'].encode()).hexdigest(), 'port': port,
                    'authority': item['sha256']}
        require(expected is None or snapshot == expected, 'Claim submission/review changed during continuation.')
        return snapshot
    finally:
        db.close()


def gpu_authority(a, p):
    """Opt in only to the exact GPU capability already validated by CPU host."""
    geometry = p['qualification'].get('gpu')
    if geometry is None:
        require('gpu_return' not in a, 'GPU authority supplied for a CPU-only profile.')
        return None
    value = a.get('gpu_return')
    require(isinstance(value, dict), 'CPU-only profile review does not authorize GPU continuation.')
    manifest = json.loads((Path(a['cpu_bundle']) / 'board/image-input-manifest.json').read_text())
    config = manifest.get('gpu_profile')
    require(set(value) == {'configuration', 'retirement_review_sha256', 'report'} and
            isinstance(config, dict) and config.get('return') == geometry and
            json.dumps(value['configuration'], sort_keys=True) == json.dumps(config, sort_keys=True) and
            value['retirement_review_sha256'] == a['cpu_return_review']['sha256'] and
            Path(value['report']).is_absolute() and
            value['report'] == str(Path(a['return_session']).parent / 'gpu-retirement.bin'),
            'Exact GPU continuation configuration, review and capture path required.')
    return geometry


def gpu_returned(a, p, r):
    geometry = gpu_authority(a, p)
    if geometry is None:
        return None
    raw_path = Path(a['gpu_return']['report'])
    require(raw_path.is_file() and digest(raw_path) == r.get('gpu_report_sha256'),
            'Copied GPU retirement report changed.')
    decoded = cpu.os_image.gpu_return_rules.decode(raw_path.read_bytes(), p['epoch'], p['generation'], require)
    # The CPU host already compared and restored every backup before recording
    # dram_restored. Bind this GPU's ranges to those exact recorded chunks;
    # no reread of retired device RAM or new backup operation occurs here.
    rows = r.get('dram_backups')
    require(isinstance(rows, list), 'GPU pool backup coverage is absent.')
    require(all(isinstance(row, dict) and type(row.get('address')) is int and
                type(row.get('bytes')) is int and row['bytes'] > 0 for row in rows),
            'Invalid GPU backup extent.')
    rows = sorted(rows, key=lambda item: item['address'])
    for low, high in (geometry['write_back_pool'], geometry['uncached_pool']):
        require(0x41000000 <= low < high <= 0x42200000, 'GPU pool is outside retired private RAM.')
        cursor = low
        for row in rows:
            start, size = row['address'], row['bytes']
            end = start + size
            if start <= cursor < end:
                require(0x41000000 <= start < end <= 0x42200000 and size <= cpu.CHUNK,
                        'GPU backup extent exceeds the original private reservation.')
                cursor = min(high, end)
            if cursor == high:
                break
        require(cursor == high, 'GPU pool backup coverage is incomplete.')
    return decoded


def authority(args, snapshot):
    require(digest(args.fel) == base.FEL_HASH, 'FEL executable changed.')
    a = json.loads(args.review.read_text())
    require(a.get('schema') == SCHEMA and a.get('reviewed') is True and
            a.get('execution_profile') == cpu.PROFILE and a.get('claim') == args.claim and
            a.get('owner') == args.owner and a.get('database') == str(args.database.resolve()) and
            a.get('port') == snapshot['port'] and
            a.get('phases') == list(PHASES) and a.get('one_attempt') is True and
            a.get('failure_policy') == 'retain-without-retry' and
            a.get('successor_authority') == 'validated-cpu-return-receipt-only' and
            a.get('helper_ram') == {'range': [0x41000000, 0x42200000], 'exclusive': True,
                                   'disposition': 'retired-overwritten-no-preserved-bytes'} and
            a.get('no_unrelated_successor') is True,
            'Exact bounded compound authority is absent.')
    paths = {str(evidence(item).resolve()): item['sha256'] for item in a.get('sources', [])}
    require(all(str((ROOT / name).resolve()) in paths for name in SOURCES),
            'Compound executable/source closure is not reviewed.')
    p = cpu.validate_bundle(Path(a['cpu_bundle']))
    gpu_authority(a, p)
    require(evidence(a['cpu_prepared']).resolve() == (Path(a['cpu_bundle']) / 'prepared.json').resolve() and
            p['image_sha256'] == a['image_sha256'] and p['build_id'] == a['build_id'] and
            all(p[k] == a[k] for k in ('epoch', 'generation', 'lease')) and
            p['_review']['identity'] == a['initial_identity'] and p['_review']['port'] == a['port'],
            'Compound authority covers another original CPU bundle/session.')
    require(evidence(a['cpu_return_review']).resolve() ==
            (Path(a['cpu_bundle']) / 'return-review.json').resolve(), 'Original CPU return review differs.')
    entry = json.loads((Path(a['cpu_bundle']) / 'board/admission.json').read_text())
    clock_ref = entry['cpu_clock_approval']
    clock = json.loads((Path(a['cpu_bundle']) / 'board' /
                       p['entry_evidence'][clock_ref['path']]['name']).read_text())
    require(clock.get('root_owned_claim') == args.claim and
            clock.get('schema') in cpu.os_image.cpu_return_clock.UNDER_TEST_SCHEMAS,
            'Original clock approval names another claim or profile.')
    require(Path(a['return_session']).is_absolute() and
            Path(a['outputs']['root']).resolve() == args.evidence.resolve(), 'Compound fixed paths differ.')
    cache = evidence(a['cache_off'])
    require(cache.stat().st_size == 40 and digest(cache) == CACHE_SHA, 'Wrong accepted cache-off helper.')
    sm = json.loads(evidence(a['survey']['manifest']).read_text())
    binary, software = evidence(a['survey']['binary']), evidence(a['survey']['software'])
    require(sm.get('schema') == 'a733-launch-survey-v1' and sm.get('physical_access') is False and
            sm.get('source_sha256') == digest(ROOT / 'boot/a733/launch-survey.S') and
            sm.get('host_sha256') == digest(Path(survey.__file__)) and
            sm['files']['launch-survey.bin'] == digest(binary) and 0 < binary.stat().st_size <= 0x400,
            'Accepted survey build/source differs.')
    for name, sha in sm['files'].items():
        require(Path(name).name == name and digest(binary.parent / name) == sha,
                'Frozen survey build bytes changed.')
    sw = json.loads(software.read_text())
    require(sw.get('schema') == 'a733-launch-survey-software-v1' and
            sw.get('binary_sha256') == digest(binary) and sw.get('return_preserved') is True and
            sw.get('physical_access') is False, 'Survey return software evidence differs.')
    require(set(a['helper']) == {'image', 'binary', 'manifest'}, 'Helper fields differ.')
    for item in a['helper'].values():
        evidence(item)
    helper_elf = evidence(a['helper']['image']).read_bytes()
    require(helper_elf[:7] == b'\x7fELF\x02\x01\x01' and
            struct.unpack_from('<Q', helper_elf, 24)[0] == 0x41000000, 'Helper entry differs.')
    phoff = struct.unpack_from('<Q', helper_elf, 32)[0]
    phsize, count = struct.unpack_from('<2H', helper_elf, 54)
    require(phsize == 56 and 0 < count < 64, 'Helper program headers differ.')
    for i in range(count):
        kind, flags, off, va, pa, size, memory, align = struct.unpack_from('<IIQQQQQQ', helper_elf, phoff + i*56)
        if kind == 1:
            require(0x41000000 <= va <= va+memory <= 0x42200000 and pa == va and
                    size <= memory and off+size <= len(helper_elf), 'Helper load lies outside retired private RAM.')
    ddr = evidence(a['ddr']).read_bytes()
    original_survey = evidence(a['initial_survey']).read_bytes()
    warm = tuple(evidence(item).read_bytes() for item in a['warm_returns'])
    cpu.cpu.kernel.validate_platform_admission((ddr, original_survey), Path(a['initial_node']),
        a['initial_identity'], a['port'], base.FEL_HASH, base.FEL_HASH, warm)
    reference = json.loads(evidence(a['reference']).read_text())
    require(reference.get('exit') == 0 and reference.get('paced', False) is False,
            'Exact unpaced reference required.')
    return a, p


def returned(a, p):
    path = Path(a['return_session'])
    r = json.loads(path.read_text())
    require(r.get('execution_profile') == cpu.PROFILE and 'mode' not in r and
            all(r.get(k) is True for k in ('complete', 'launch_attempted', 'physical_access',
                                         'rom_usable', 'dram_restored', 'cpu_rail_restoration_pending')) and
            r.get('transport_failed') is False and r.get('dirty') is False and
            r.get('automatic_access') is True and r.get('tool_sha256') == base.FEL_HASH and
            r.get('ownership_release_permitted') is False and
            r.get('root_owned_claim') == a['claim'] and r.get('port') == a['port'] and
            r.get('initial_node') == a['initial_node'] and r.get('initial_identity') == a['initial_identity'] and
            r.get('image_sha256') == p['image_sha256'] and
            r.get('return_review_sha256') == digest(Path(a['cpu_bundle']) / 'return-review.json') and
            r.get('host_source_sha256') == digest(Path(cpu.__file__)) and
            r.get('cpu_clock_entry_status') == 'under-test', 'No matching successful retained CPU return.')
    identity, node = r.get('returned_identity'), r.get('returned_node')
    require(isinstance(identity, list) and len(identity) == 5 and all(type(v) is int for v in identity) and
            identity != a['initial_identity'] and isinstance(node, str) and
            re.fullmatch(r'/dev/bus/usb/\d+/\d+', node) and node != a['initial_node'],
            'CPU receipt does not identify a unique successor ROM.')
    report, output = path.parent / 'return-report.bin', path.parent / 'recorded-output.bin'
    require(digest(report) == r['report_sha256'] and digest(output) == r['output_sha256'],
            'Copied CPU report/output changed.')
    decoded = cpu.decode_report(report.read_bytes(), p['epoch'])
    gpu = gpu_returned(a, p, r)
    if gpu is not None:
        decoded['gpu'] = gpu
    require(decoded['secondaries_off'] == 7 and r.get('result') == decoded,
            'CPU secondary retirement or receipt summary differs.')
    text = cpu.decode_output(output.read_bytes(), p['epoch'], p['generation'])
    require((path.parent / 'transcript.txt').read_text() == text, 'Copied transcript differs.')
    cpu.rail_followup_check(r, evidence(a['helper']['image']), evidence(a['helper']['binary']),
                           json.loads(evidence(a['helper']['manifest']).read_text()),
                           a['helper']['manifest']['sha256'])
    return r, text


def parity(a, p, r, text, out):
    reference = json.loads(evidence(a['reference']).read_text())
    assets = [(Path(a['cpu_bundle']) / 'assets' / x['name']).read_bytes() for x in p['_assets']]
    packed = bytearray(128)
    struct.pack_into('<8sQQQIII', packed, 0, b'SUNMIM01', *map(len, assets), *(zlib.crc32(x) for x in assets))
    for data in assets:
        packed.extend(data); packed.extend(bytes(-len(packed) % 64))
    require(hashlib.sha256(packed).hexdigest() == reference['bundle_sha256'] and
            hashlib.sha256(assets[2]).hexdigest() == reference['audio_sha256'], 'Reference input differs.')
    out.mkdir()
    (out / 'kernel.log').write_text(text)
    base.save_json(out / 'candidate.json', {'complete': True, 'image_sha256': p['image_sha256'],
                   'ram_input_sha256': reference['bundle_sha256'], 'paced': False,
                   'return_session_sha256': digest(Path(a['return_session']))})
    command = [sys.executable, str(ROOT / 'checks/mimus-parity.py'), '--reference', a['reference']['path'],
               '--candidate', str(out / 'candidate.json'), '--output', str(out / 'parity.json')]
    result = subprocess.run(command, capture_output=True, text=True)
    (out / 'compare.log').write_text(result.stdout + result.stderr)
    require(result.returncode == 0 and json.loads((out / 'parity.json').read_text()).get('passed') is True,
            'Actual copied recognition differs from the reference.')


def cache_capture(tx, binary, out):
    out.mkdir(); before = out / 'before.bin'
    tx('read', '0x73000', 40, before)
    require(before.stat().st_size == 40, 'Short cache-off SRAM backup.')
    transfer(tx, 0x73000, binary, out / 'uploaded.bin')
    tx('exe', '0x73000')
    transfer(tx, 0x73000, before.read_bytes(), out / 'restored.bin')
    return {'complete': True, 'rom_usable': True, 'sram_restored': True, 'helper_sha256': CACHE_SHA}


def transfer(tx, address, data, readback):
    with base.sealed_source(data) as (path, fd):
        tx('write', hex(address), path, 'read', hex(address), len(data), readback, pass_fds=(fd,))
    require(readback.read_bytes() == data, 'Compound SRAM transfer comparison failed.')


def survey_capture(tx, binary, out):
    """Same bounded SRAM extents, nonce, decoder and restore as the survey tool."""
    out.mkdir(); backups = {}
    for name, address, size in survey.REGIONS:
        path = out / (name + '-before.bin'); tx('read', hex(address), size, path)
        backups[name] = path.read_bytes(); require(len(backups[name]) == size, 'Short survey SRAM backup.')
    nonce = secrets.token_bytes(8)
    transfer(tx, 0x85000, binary, out / 'code-upload.bin')
    transfer(tx, 0x86000, bytes(120) + nonce, out / 'report-upload.bin')
    tx('exe', '0x85000'); tx('read', '0x86000', 128, out / 'report.bin')
    raw = (out / 'report.bin').read_bytes(); error = None
    try:
        result = survey.decode(raw, nonce)
    except (ValueError, struct.error) as e:
        error = e
    for name, address, size in survey.REGIONS:
        transfer(tx, address, backups[name], out / (name + '-restored.bin'))
    if error:
        raise error
    require(all(result['launcher_gates'].values()), 'Returned ROM survey launcher gate refused.')
    return {'physical_access': True, 'complete': True, 'rom_usable': True, 'sram_restored': True,
            'nonce': nonce.hex(), 'result': result}


def helper_result(record, log):
    require(record.get('complete') is True and record.get('transport_failed') is False and
            record.get('automatic_access') is True and record.get('mode') == 'kernel' and
            record.get('result', {}).get('magic') == 0x534b4552 and
            record['result'].get('stage') == 5 and record['result'].get('status') == 0 and
            'controlled' not in record and log.splitlines().count(COMPLETE) == 1 and
            not re.search(r'^(?:FAIL|REFUSED|STOPPED)\b', log, re.M), 'Rail helper did not complete normally.')
    supply = re.findall(r'^SUPPLY (.+)$', log, re.M)
    require(len(supply) == 1, 'Helper entry supplies absent or ambiguous.')
    fields = dict(x.split('=') for x in supply[0].split())
    require(int(fields['dcdc3_uv'], 0) == 800000 and int(fields['dcdc5_uv'], 0) == 1000000 and
            int(fields['bin'], 0) == 4 and int(fields['dcxo_hz'], 0) == 26000000,
            'Helper entry supply/bin/reference differs.')
    volt = re.findall(r'^VOLT (.+)$', log, re.M)
    require(len(volt) == 1, 'Helper must have exactly one rail operation.')
    fields = dict(x.split('=') for x in volt[0].split())
    require(set(fields) == {'dcdc5_selector', 'dcdc5_uv'} and int(fields['dcdc5_uv'], 0) == 800000 and
            int(fields['dcdc5_selector'], 0) & 127 == 30, 'Helper rail readback differs.')
    for name, pll, mux, mhz in [('CPU-L', 0xf8802600, 0x03010000, 494),
                              ('CPU-B', 0xf8802700, 0x03000000, 1014),
                              ('DSU', 0xf8802600, 0x03010105, 494)]:
        for phase in ('before', 'after'):
            rows = re.findall(r'^' + name + ' ' + phase + r' (.+)$', log, re.M)
            require(len(rows) == 1, 'Helper clock inspection absent or ambiguous.')
            f = {k: int(v, 0) for k, v in (x.split('=') for x in rows[0].split())}
            require(f['pll'] == pll and f['mux'] == mux and f['mhz'] == mhz and
                    not (f['pat0'] & (3 << 29) or f['pat1'] >> 31 or f['lfm'] >> 31),
                    'Helper boot clock or pattern/LFM differs.')
    return {'boot_clock_domains': 3, 'dcdc3_uv': 800000, 'dcdc5_uv': 800000,
            'proof_scope': 'Exact helper pre-return inspections/readback and status-zero completion; no post-ROM analog measurement.',
            'helper_ram': 'retired-overwritten', 'dram_restored': False}


def run(args):
    snapshot = claim(args); a, p = authority(args, snapshot)
    require(digest(args.fel) == base.FEL_HASH, 'FEL executable changed.')
    require(os.environ.get('SUNSTONE_USB_PORT') == a['port'], 'Compound must select its reviewed physical port.')
    args.evidence.mkdir(parents=True, exist_ok=False)
    state = {'schema': SCHEMA, 'complete': False, 'claim': args.claim, 'owner': args.owner,
             'port': a['port'], 'physical_access': False, 'phases': [], 'queue_snapshot': snapshot,
             'cpu_rail_restoration_pending': True, 'ownership_release_permitted': False,
             'cpu_return_path': a['return_session']}
    def save(): base.save_json(args.evidence / 'session.json', state)
    def ready():
        claim(args, snapshot); authority(args, snapshot); returned(a, p)
    def begin(name):
        require(name == PHASES[len(state['phases'])], 'Compound phase repeated or out of order.')
        ready(); state['phases'].append({'name': name, 'attempted': True, 'complete': False}); save()
    def end(): state['phases'][-1]['complete'] = True; save()
    def observe(name, binary, capture):
        access = module('compound_access_' + name, 'tools/usb-access.py')
        sys.path.insert(0, str(Path.home() / 'git/mimus/tools')); import usb_guard
        usb_guard.check_health()
        with access.board_lock() as node:
            access.require_session(node); access.require_rom(node)
            require(str(node) == r['returned_node'] and access.identity(node) == r['returned_identity'] and
                    access.target(node).name == a['port'], 'Successor differs from validated CPU receipt.')
            record = {'transactions': [], 'launch_attempted': False, 'dirty': False,
                      'device_access_attempted': False}; failed = False
            def journal_transport():
                base.save_json(args.evidence / (name + '-transport.json'), record)
            def tx(*arguments, pass_fds=()):
                nonlocal failed
                require(not failed, 'No requests after compound transport failure.')
                claim(args, snapshot)
                require(str(node) == r['returned_node'] and access.identity(node) == r['returned_identity'] and
                        access.target(node).name == a['port'], 'Successor changed before request.')
                access.require_rom(node)
                item = {'arguments': list(map(str, arguments))}; record['transactions'].append(item)
                record['device_access_attempted'] = True
                if 'write' in arguments: record['dirty'] = True
                if 'exe' in arguments: record['launch_attempted'] = True
                state['physical_access'] = True; save(); journal_transport()
                try:
                    x = subprocess.run([str(args.fel), '-d', f'{int(node.parent.name)}:{int(node.name)}',
                                        *map(str, arguments)], capture_output=True, text=True, timeout=15,
                                       pass_fds=pass_fds)
                    item.update(exit_code=x.returncode, stdout=x.stdout, stderr=x.stderr)
                    require(x.returncode == 0, 'Compound SRAM transaction failed.')
                except BaseException:
                    failed = True; record['transport_failed'] = True; journal_transport(); raise
                journal_transport(); return x.stdout
            try:
                require('soc=00001903(A733)' in tx('version'), 'Successor is not A733.')
                result = capture(tx, binary, args.evidence / name)
                result.update(node=str(node), identity=r['returned_identity'], port=a['port'],
                              claim=args.claim, owner=args.owner, transactions=record['transactions'])
                base.save_json(args.evidence / name / 'session.json', result)
                record.update(dirty=False, complete=True, sram_restored=True); journal_transport()
                state['physical_access'] = True; save()
            except BaseException as error:
                record.update(failure=str(error), transport_failed=failed)
                base.save_json(args.evidence / (name + '-failure.json'), record)
                base.stop_after_failure(record, journal_transport,
                    lambda: access.quarantine_instance(node, 'CPU compound follow-up stopped; no retry.',
                                                      r['returned_identity']), error)
                raise
    save()
    try:
        r, text = returned(a, p)
        state['cpu_return_sha256'] = digest(Path(a['return_session']))
        if 'gpu' in p['qualification']:
            state['gpu_return'] = r['result']['gpu']
            state['gpu_report_sha256'] = r['gpu_report_sha256']
        save()
        begin('parity'); parity(a, p, r, text, args.evidence / 'parity'); end()
        begin('cache-off'); observe('cache-off', evidence(a['cache_off']).read_bytes(), cache_capture); end()
        begin('survey'); observe('survey', evidence(a['survey']['binary']).read_bytes(), survey_capture); end()
        begin('rail-helper')
        helper = a['helper']; cpu.rail_followup_check(r, evidence(helper['image']), evidence(helper['binary']),
                    json.loads(evidence(helper['manifest']).read_text()), helper['manifest']['sha256'])
        argv = ['--fel', str(args.fel), '--image', helper['binary']['path'], '--evidence',
                str(args.evidence / 'rail-helper'), '--mode', 'kernel', '--expect', COMPLETE,
                '--return-timeout', '10', '--ddr-evidence', str(evidence(a['ddr'])),
                '--platform-survey', str(args.evidence / 'survey/session.json')]
        for item in a['warm_returns']:
            argv += ['--warm-return', str(evidence(item))]
        argv += ['--warm-return', a['return_session']]
        code = cpu.cpu.kernel.main(argv)
        require(code in (None, 0), 'Rail helper runner failed.')
        ready(); end()
        begin('final-proof')
        helper_path = args.evidence / 'rail-helper/session.json'
        hr = json.loads(helper_path.read_text())
        require(hr.get('initial_node') == r['returned_node'] and hr.get('initial_identity') == r['returned_identity'] and
                hr.get('port') == a['port'] and hr.get('image_sha256') == helper['binary']['sha256'],
                'Helper ran on another successor or image.')
        proof = helper_result(hr, (helper_path.parent / 'kernel.log').read_text())
        require(isinstance(hr.get('returned_node'), str) and hr['returned_node'] != hr['initial_node'] and
                isinstance(hr.get('returned_identity'), list) and len(hr['returned_identity']) == 5 and
                hr['returned_identity'] != hr['initial_identity'], 'Helper successor ROM is absent.')
        ready(); end(); state.update(complete=True, cpu_rail_restoration_pending=False, ownership_release_permitted=True,
            helper_session_sha256=digest(helper_path), helper_final_proof=proof,
            returned_node=hr['returned_node'], returned_identity=hr['returned_identity'],
            ram_continuity=True, helper_ram_retired=True, helper_dram_restored=False,
            release_scope='Root may finish this claim; every subsequent device needs its own authority.')
        save()
    except BaseException as error:
        state.update(complete=False, failure=str(error), cpu_rail_restoration_pending=True, ownership_release_permitted=False)
        save(); raise
    return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('database', 'review', 'fel', 'evidence'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--claim', required=True); parser.add_argument('--owner', required=True)
    parser.add_argument('--check-review-only', action='store_true',
                        help='Validate finalized input bytes before REVIEW/CLAIM; no queue or device action')
    args = parser.parse_args()
    if args.check_review_only:
        a = json.loads(args.review.read_text())
        authority(args, {'port': a.get('port')})
        print(json.dumps({'review_inputs_valid': True, 'queue_claim_checked': False, 'physical_access': False}))
    else:
        run(args)


if __name__ == '__main__':
    main()
