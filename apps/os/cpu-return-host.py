#!/usr/bin/env python3
"""Prepare or capture one staged CPU recognition through returned ROM.

Successful capture restores uploaded DRAM. CPU rail restoration remains a
separate coordinator-owned continuation; failures retain the existing claim.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


os_image = module('cpu_return_os', 'apps/os/prepare.py')
cpu = module('cpu_return_prior', 'apps/cpu-runtime-qualification/host.py')
base, require, digest = cpu.base, cpu.require, cpu.digest
PROFILE = 'os-cpu-return-v1'
CHUNK = 4 * 1024 * 1024
OUTPUT_MAGIC, OUTPUT_BYTES = 0x3152434F33333741, 48 + 16384


def geometry(image):
    elf = os_image.os_link.Elf(image.read_bytes())
    symbols = {}
    for s in elf.symbols:
        if s['shndx'] != 0 and s['bind'] == 1:
            require(s['name'] not in symbols, 'Ambiguous global CPU return symbol.')
            symbols[s['name']] = s
    def at(name):
        require(name in symbols, 'CPU return symbol missing: ' + name)
        return symbols[name]['value']
    monitor = [at('__monitor_bss_start'), at('__monitor_bss_end')]
    report, stacks = at('a733_qualification_report'), at('a733_qualification_stacks')
    require(0x42005000 <= monitor[0] < monitor[1] <= at('__a733_smp_boot_stack_top') <= 0x421f0000 and
            symbols['a733_qualification_report']['size'] == cpu.REPORT_BYTES and
            symbols['a733_qualification_stacks']['size'] == 32768 and
            monitor[0] <= report < report + cpu.REPORT_BYTES <= monitor[1] and
            monitor[0] <= stacks < stacks + 32768 <= monitor[1] and
            (report + cpu.REPORT_BYTES <= stacks or stacks + 32768 <= report),
            'CPU return Secure Device allocations differ.')
    require(0x41200000 <= at('__bss_start') <= at('__bss_end') <= at('__heap_end') <= 0x41c00000,
            'CPU return cached writable range differs.')
    result = {'report_address': report, 'report_bytes': cpu.REPORT_BYTES,
              'cleanup_stacks': [stacks, stacks + 32768], 'monitor': monitor,
              'cached_writable': [0x41200000, (at('__heap_end') + 63) & ~63],
              'recognizer_arena': [0x46000000, 0x47000000]}
    output = symbols['os_cpu_return_output']
    require(output['size'] == OUTPUT_BYTES and 0x41200000 <= output['value'] <
            output['value'] + OUTPUT_BYTES <= 0x42000000, 'Copied output geometry differs.')
    result['output_address'] = output['value']
    result['output_bytes'] = OUTPUT_BYTES
    config=os_image.os_link.gpu_configuration(elf)
    if config:
        require('return' in config, 'GPU return capability absent.')
        result['gpu']=config['return']
    return result


def review_check(review, prepared):
    require(review.get('schema') == 'a733-os-cpu-return-review-v1' and
            review.get('image_sha256') == prepared['image_sha256'] and
            all(review.get(k) == prepared[k] for k in ('epoch', 'generation', 'lease')) and
            review.get('build_id') == prepared['build_id'] and
            review.get('failure_policy') == 'retain-without-retry' and
            review.get('rail_followup') == 'verify-boot-clocks-and-restore-cpu-rails',
            'CPU return source review differs from this session.')
    require(review.get('sources'), 'CPU return source evidence is absent.')
    for item in review['sources']:
        require(isinstance(item, dict) and set(item) == {'path', 'sha256'} and
                Path(item['path']).is_absolute() and digest(item['path']) == item['sha256'],
                'CPU return source evidence changed.')


def prepare(args):
    admission = json.loads(args.admission.read_text())
    require(admission.get('normal_return_review') ==
            {'path': str(args.return_review.resolve()), 'sha256': digest(args.return_review)},
            'CPU return review is absent from the sealed entry authority.')
    os_image.prepare(args.image, args.manifest, args.admission, args.session_template,
                     [args.base, args.overlay, args.wav], args.output, args.clang, args.lld,
                     cpu_return=True)
    inner = os_image.validate_board(args.output / 'board', cpu_return=True)
    review = json.loads(args.return_review.read_text())
    review_check(review, inner)
    shutil.copy2(args.return_review, args.output / 'return-review.json')
    files = {}
    for i, item in enumerate(review['sources']):
        name = f'return-source-{i}.bin'
        shutil.copy2(item['path'], args.output / name)
        files[name] = item['sha256']
    return seal_preparation(args.output)


def seal_preparation(output):
    """Seal already copied inputs; also resumes a failed offline layout check."""
    output = Path(output)
    require(not (output / 'return.json').exists(), 'CPU return inputs are already sealed.')
    inner = os_image.validate_board(output / 'board', cpu_return=True)
    review = json.loads((output / 'return-review.json').read_text())
    review_check(review, inner)
    sources = {f'return-source-{i}.bin': item['sha256'] for i, item in enumerate(review['sources'])}
    require(all(digest(output / name) == expected for name, expected in sources.items()),
            'Copied CPU return evidence changed before sealing.')
    record = {'schema': PROFILE, 'physical_access': False,
              'prepared_sha256': digest(output / 'prepared.json'),
              'review_sha256': digest(output / 'return-review.json'),
              'image_sha256': inner['image_sha256'], 'geometry': geometry(output / 'board/image.elf'),
              'sources': sources}
    base.save_json(output / 'return.json', record)
    return validate_bundle(output)


def validate_bundle(bundle):
    bundle = Path(bundle)
    record = json.loads((bundle / 'return.json').read_text())
    require(record.get('schema') == PROFILE and record.get('physical_access') is False and
            digest(bundle / 'prepared.json') == record['prepared_sha256'] and
            digest(bundle / 'return-review.json') == record['review_sha256'], 'CPU return bundle changed.')
    os_image.validate_bundle(bundle, cpu_return=True)
    p = os_image.validate_board(bundle / 'board', cpu_return=True)
    review = json.loads((bundle / 'return-review.json').read_text())
    require(review['image_sha256'] == p['image_sha256'] == record['image_sha256'] and
            review.get('build_id') == p['build_id'] and
            all(review[k] == p[k] for k in ('epoch', 'generation', 'lease')),
            'CPU return session identity changed.')
    require(len(record['sources']) == len(review['sources']), 'CPU return evidence count differs.')
    for i, item in enumerate(review['sources']):
        name = f'return-source-{i}.bin'
        require(record['sources'].get(name) == item['sha256'] and digest(bundle / name) == item['sha256'],
                'Frozen CPU return evidence changed.')
    require(geometry(bundle / 'board/image.elf') == record['geometry'], 'CPU return linked layout changed.')
    require(p['workload'] == 2, 'CPU return admits staged workload 2 only.')
    p['qualification'] = record['geometry']
    if 'gpu' in p['qualification']:
        manifest=json.loads((bundle/'board/image-input-manifest.json').read_text())
        def frozen_evidence(item):
            row=p['entry_evidence'][item['path']]
            data=(bundle/'board'/row['name']).read_bytes()
            require(hashlib.sha256(data).hexdigest()==item['sha256'], 'GPU retirement source changed.')
            return data
        os_image.gpu_return_rules.validate(review,manifest['gpu_profile'],frozen_evidence,require)
    p['_review'] = review
    p['_assets'] = os_image.host.validate_assets(bundle / 'assets')['assets']
    entry = json.loads((bundle / 'board/admission.json').read_text())
    approved = entry['cpu_clock_approval']
    clock_review = json.loads((bundle / 'board' / p['entry_evidence'][approved['path']]['name']).read_text())
    if clock_review.get('schema') in os_image.cpu_return_clock.UNDER_TEST_SCHEMAS:
        p['_approved_rail_helper'] = {k: clock_review['rail_helper'][k]['sha256']
                                      for k in ('image', 'binary', 'manifest')}
    return p


def decode_report(raw, epoch):
    require(len(raw) == cpu.REPORT_BYTES, 'Incomplete CPU return report.')
    magic, version, size, actual, deadline, state, error, core = struct.unpack_from('<8Q', raw)
    require((magic, version, size, actual, state, error, core) ==
            (cpu.MAGIC, 3, cpu.REPORT_BYTES, epoch, 4, 0, 0) and deadline,
            'No successful CPU return report of this session.')
    for i in range(8):
        offset = cpu.CPU_OFFSET + i * cpu.CPU_BYTES
        seen, captured, cleaned, restored, request, ack, error = struct.unpack_from('<7Q', raw, offset)
        drained = struct.unpack_from('<Q', raw, offset + 104)[0]
        off = struct.unpack_from('<Q', raw, offset + 768)[0]
        require((seen, captured, drained, cleaned, restored) == (epoch,) * 5 and not error and
                (request, ack, off) == ((epoch,) * 3 if i else (0, 0, 0)),
                f'CPU {i} normal shutdown incomplete.')
    return {'epoch': epoch, 'state': state, 'secondaries_off': 7}


def decode_output(raw, epoch, generation):
    require(len(raw) == OUTPUT_BYTES, 'Incomplete recorded output.')
    magic, version, size, actual, actual_generation, used = struct.unpack_from('<6Q', raw)
    require((magic, version, size, actual, actual_generation) ==
            (OUTPUT_MAGIC, 1, OUTPUT_BYTES, epoch, generation) and 0 < used <= 16384,
            'Recorded output belongs to another session or is incomplete.')
    return raw[48:48 + used].decode('utf-8', errors='strict')


def rail_followup_check(returned, image, binary, manifest, manifest_sha256=None):
    """Admit only the existing helper's exact already-800mV DCDC3 variant.

    The coordinator keeps the original claim. The helper itself verifies all
    boot clock words before its sole DCDC5 write; this grants no successor.
    """
    require(returned.get('execution_profile') == PROFILE and returned.get('complete') is True and
            returned.get('physical_access') is True and returned.get('transport_failed') is False and
            returned.get('dram_restored') is True and returned.get('rom_usable') is True and
            returned.get('cpu_rail_restoration_pending') is True and
            returned.get('ownership_release_permitted') is False and
            returned.get('result', {}).get('secondaries_off') == 7 and
            isinstance(returned.get('root_owned_claim'), str) and returned['root_owned_claim'].strip(),
            'Rail follow-up requires the retained successful CPU return claim.')
    require(manifest.get('bundle') == 33 and manifest.get('execution_variant') == 'os-cpu-return-dcdc3-800-v1' and
            manifest.get('elf_sha256') == digest(image) and manifest.get('binary_sha256') == digest(binary),
            'Wrong CPU return rail-helper artifact or variant.')
    if returned.get('cpu_clock_entry_status') == 'under-test':
        require(returned.get('approved_rail_helper') ==
                {'image': digest(image), 'binary': digest(binary),
                 'manifest': manifest_sha256},
                'Rail helper differs from the reviewed first-functional approval.')
    elf = os_image.os_link.Elf(Path(image).read_bytes())
    markers = [s for s in elf.symbols if s['name'] == 'os_cpu_return_rail_profile' and s['shndx'] != 0]
    require(len(markers) == 1 and markers[0]['size'] == 32 and
            struct.unpack('<4Q', elf.read(markers[0]['value'], 32)) == (0x31524c4941525043, 1, 800000, 1000000),
            'CPU return rail-helper linked entry marker differs.')
    return {'execution_variant': manifest['execution_variant'], 'image_sha256': manifest['elf_sha256'],
            'binary_sha256': manifest['binary_sha256'], 'root_owned_claim': returned['root_owned_claim'],
            'entry_uv': {'dcdc3': 800000, 'dcdc5': 1000000},
            'obligation': 'Helper verifies all boot PLL/mux and pattern/LFM words; only DCDC5 may change.'}


def admit(args, prepared, node, identity, port):
    require(args.root_owned_claim.strip(), 'Explicit root-owned claim required.')
    validate_bundle(args.bundle)
    admission = json.loads((args.bundle / 'board/admission.json').read_text())
    require(admission['identity'] == identity and admission['port'] == port and
            prepared['_review'].get('identity') == identity and prepared['_review'].get('port') == port,
            'CPU return review names another live target.')
    clock_ref = admission['cpu_clock_approval']
    clock_copy = prepared['entry_evidence'][clock_ref['path']]['name']
    clock_review = json.loads((args.bundle / 'board' / clock_copy).read_text())
    if clock_review.get('schema') in os_image.cpu_return_clock.UNDER_TEST_SCHEMAS:
        require(clock_review.get('root_owned_claim') == args.root_owned_claim,
                'First-functional entry requires its reviewed root-owned claim.')
    paths = [args.ddr_evidence, args.platform_survey, *args.warm_return]
    raw = [p.read_bytes() for p in paths]
    cpu.kernel.validate_platform_admission(tuple(raw[:2]), node, identity, port,
                                          base.FEL_HASH, base.FEL_HASH, tuple(raw[2:]))
    cpu.caller_interval(json.loads(raw[1]))
    ddr = json.loads(cpu.supervised.artifact(admission['ddr']).read_text())
    covered = {(item['path'], item['sha256']) for item in ddr['evidence']}
    require(all((str(p.resolve()), hashlib.sha256(data).hexdigest()) in covered for p, data in zip(paths, raw)),
            'Current continuity inputs are absent from the image DDR reservation.')


def run_once(tx, wait, ready, bundle, p, evidence, record, journal):
    require(not record.get('launch_attempted'), 'One CPU return attempt only.')
    ready()
    # Back up every modified DRAM range before any upload. Returned ROM owns
    # SRAM; only these copied DRAM bytes may be restored after native return.
    extents = [(0x41000000, 0x1200000), (0x46000000, 0x1000000)]
    extents += [(item['address'], item['bytes']) for item in p['_assets']]
    backups = []
    record.update(dirty=True, dram_restored=False, cpu_rail_restoration_pending=True)
    if '_approved_rail_helper' in p:
        record.update(cpu_clock_entry_status='under-test', approved_rail_helper=p['_approved_rail_helper'])
    journal()
    for address, size in extents:
        for offset in range(0, size, CHUNK):
            n = min(CHUNK, size - offset)
            path = evidence / f'before-dram-{len(backups)}.bin'
            tx('read', hex(address + offset), n, path)
            require(path.stat().st_size == n, 'Incomplete CPU return RAM backup.')
            base.sync_file(path)
            backups.append((address + offset, n, path, digest(path)))
    record['dram_backups'] = [{'address': a, 'bytes': n, 'file': path.name, 'sha256': h}
                              for a, n, path, h in backups]
    journal()
    words = [int(x, 16) for x in tx(*[v for a in (0x20, 0x24, 0x28, 0x3c, 0x40)
                                                for v in ('readl', hex(a))]).split()]
    require(words == [0xeb00008f, 0xe3a00911, 0xe3a0193d, 0xe3a0605c, 0xea00004d],
            'Forced-FEL ROM entry differs.')
    base.preload(tx, bundle / 'board', p, evidence)
    for item in p['_assets']:
        data = (bundle / 'assets' / item['name']).read_bytes()
        readback = evidence / ('readback-' + item['name'])
        require(hashlib.sha256(data).hexdigest() == item['sha256'], 'Recognition input changed.')
        with base.sealed_source(data) as (source, descriptor):
            tx('write', hex(item['address']), source, 'read', hex(item['address']), len(data), readback,
               pass_fds=(descriptor,))
        require(readback.read_bytes() == data, 'Recognition upload readback differs.')
        base.sync_file(readback)
    ready()
    require(int(tx('readl', '0x02050014').strip(), 16) & 1 == 0, 'Watchdog enabled before native execution.')
    record.update(launch_attempted=True, backup_sha256=p['_backup_hashes'].copy())
    journal()
    tx('exe', '0x52000')
    wait()
    require('soc=00001903(A733)' in tx('version'), 'Returned service is not A733 ROM.')
    copied = {}
    captures = [('return-report', p['qualification']['report_address'], cpu.REPORT_BYTES),
                                ('recorded-output', p['qualification']['output_address'], OUTPUT_BYTES)]
    if 'gpu' in p['qualification']:
        captures.append(('gpu-retirement',p['qualification']['gpu']['report_address'],64))
    for name,address,size in captures:
        path = evidence / (name + '.bin')
        tx('read', hex(address), size, path)
        require(path.stat().st_size == size, 'Incomplete copied CPU return output.')
        base.sync_file(path)
        copied[name] = path.read_bytes()
    result = decode_report(copied['return-report'], p['epoch'])
    transcript = decode_output(copied['recorded-output'], p['epoch'], p['generation'])
    if 'gpu' in p['qualification']:
        result['gpu']=os_image.gpu_return_rules.decode(copied['gpu-retirement'],p['epoch'],p['generation'],require)
        record['gpu_report_sha256']=hashlib.sha256(copied['gpu-retirement']).hexdigest()
    record.update(report_sha256=hashlib.sha256(copied['return-report']).hexdigest(),
                  output_sha256=hashlib.sha256(copied['recorded-output']).hexdigest(),
                  return_review_sha256=digest(bundle / 'return-review.json'),
                  host_source_sha256=digest(__file__))
    journal()
    (evidence / 'transcript.txt').write_text(transcript)
    base.sync_file(evidence / 'transcript.txt')
    for _, n, path, expected in backups:
        require(path.stat().st_size == n and digest(path) == expected, 'Saved CPU return RAM changed.')
    for i, (address, n, path, expected) in enumerate(backups):
        data = path.read_bytes()
        require(len(data) == n and hashlib.sha256(data).hexdigest() == expected, 'Saved RAM changed before restore.')
        readback = evidence / f'restored-dram-{i}.bin'
        with base.sealed_source(data) as (source, descriptor):
            tx('write', hex(address), source, 'read', hex(address), n, readback, pass_fds=(descriptor,))
        require(readback.read_bytes() == data, 'DRAM restoration readback differs.')
        base.sync_file(readback)
    record.pop('mode', None)
    record.update(result=result, complete=True, physical_access=True, dirty=False,
                  dram_restored=True, rom_usable=True, ownership_release_permitted=False,
                  cpu_rail_restoration_pending=True,
                  result_limit='RAM continuity only; verify boot clocks and restore CPU rails before shared reuse.')
    journal()
    return result


def run(args):
    def execute(tx, wait, ready, p, record, journal):
        paths = [args.ddr_evidence, args.platform_survey, *args.warm_return]
        record['continuity_inputs'] = []
        for i, path in enumerate(paths):
            data = path.read_bytes()
            saved = args.evidence / f'continuity-{i}.json'
            saved.write_bytes(data)
            base.sync_file(saved)
            record['continuity_inputs'].append({'path': str(path.resolve()), 'file': saved.name,
                                               'sha256': hashlib.sha256(data).hexdigest()})
        journal()
        return run_once(tx, wait, ready, args.bundle, p, args.evidence, record, journal)
    return cpu.returning.run(args, base=base, profile=PROFILE, validate_bundle=validate_bundle,
                             admit=admit, execute=execute, description='One recorded CPU recognition and RAM return',
                             return_timeout=90)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='operation', required=True)
    prep = sub.add_parser('prepare')
    for name in ['image', 'manifest', 'admission', 'session-template', 'base', 'overlay', 'wav',
                 'return-review', 'output', 'clang', 'lld']:
        prep.add_argument('--' + name, type=Path, required=True)
    live = sub.add_parser('run')
    for name in ['bundle', 'fel', 'evidence', 'ddr-evidence', 'platform-survey']:
        live.add_argument('--' + name, type=Path, required=True)
    live.add_argument('--warm-return', type=Path, action='append', default=[])
    live.add_argument('--root-owned-claim', required=True)
    rails = sub.add_parser('check-rail-followup')
    for name in ['return-session', 'image', 'binary', 'manifest', 'output']:
        rails.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if args.operation == 'prepare':
        prepare(args)
    elif args.operation == 'run':
        run(args)
    else:
        result = rail_followup_check(json.loads(args.return_session.read_text()), args.image, args.binary,
                                     json.loads(args.manifest.read_text()), digest(args.manifest))
        base.save_json(args.output, result)


if __name__ == '__main__':
    main()
