#!/usr/bin/env python3
"""Freeze a CPU-entry checkpoint bundle; it grants no GPU or PMIC operation."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import types
from elftools.elf.elffile import ELFFile
ROOT = Path(__file__).resolve().parents[2]

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value

finite = module('checkpoint_finite_serializer', ROOT / 'apps/gpu-finite/prepare.py')
original = finite.original; base = finite.base; require = finite.require
PROFILE = 'gpu-entry-memory-checkpoint-v1'
SCHEMA = 'a733-gpu-entry-checkpoint-bundle-v1'
HOST_SOURCES = tuple(n for n in finite.HOST_SOURCES if n not in ('apps/gpu-finite/host.py', 'apps/gpu-finite/report.py')) + (
    'apps/gpu-entry-checkpoint/prepare.py', 'apps/gpu-entry-checkpoint/host.py', 'apps/gpu-entry-checkpoint/report.py')
PLATFORM_CONDITIONS = {'primary-cache-transition-and-secondary-off', 'cold-cache-and-dirty-alias-exclusion',
    'dual-pas-dram', 'exclusive-cpus-memory-dma', 'remote-writers-excluded', 'physical-code-publication',
    'physical-counter-continuity', 'usb-clock-power-reset-ownership', 'usb-supplies-stable-and-writers-excluded'}
PASSIVE_CONDITIONS = {'gpu-ccu-gates-off', 'pck-command-and-status-zero', 'gpu-int95-disabled-inactive-clear',
                      'no-device-or-remote-writes-through-return'}
REVIEW_CONDITIONS = {'architecture-reset-reviewed', 'never-open-empty-resource-proof-reviewed',
                     'private-publication-and-EL1h-return-reviewed', 'failure-retention-reviewed'}

def sha(data): return hashlib.sha256(data).hexdigest()

def image_profile(path, manifest):
    data = path.read_bytes(); parts, address, build = original.image_parts(path)
    require(manifest.get('target') == 'sunstone-a733-gpu-entry-checkpoint' and manifest.get('execution_profile') == PROFILE
            and manifest.get('workload') == 'entry-memory-ready-no-providers' and manifest.get('final_elf') == sha(data)
            and manifest.get('build_id') == build.hex(), 'Distinct checkpoint image required.')
    elf = ELFFile(io.BytesIO(data)); symbols = {s.name: (int(s['st_value']), int(s['st_size']))
                                               for s in elf.get_section_by_name('.symtab').iter_symbols()}
    def at(name):
        require(name in symbols and symbols[name][0], 'Missing checkpoint symbol: ' + name); return symbols[name][0]
    for name in ('gpu_entry_checkpoint', 'gpu_checkpoint_ready', 'gpu_return_finish', 'gpu_return_reset',
                 'gpu_finite_smc', 'gpu_return_allocations', 'gpu_return_device_quiet'): at(name)
    require(at('sd_single_core_entry') == 0x41000000 and not any(n in symbols for n in
        ('a733_smp_diagnostic_start', 'a733_native_usb_init', 'a733_pmic_poll', 'pvr_gpu_service_init',
         'a733_cpu_start_init', 'a733_cpu_primary_continue', 'FreeRTOS_SGI_Handler')), 'Provider or multicore entry linked.')
    lo, hi = at('__gpu_private_start'), (at('__gpu_private_end') + 63) & ~63
    receipt, log = at('gpu_return_receipt'), at('__gpu_platform_log')
    require(lo == 0x41200000 and lo < hi <= 0x41f00000 and
            at('__gpu_retained_start') <= receipt < receipt + 104 <= at('__gpu_retained_end') and
            symbols['gpu_return_receipt'][1] == 104 and lo <= log < log + 16384 <= hi, 'Checkpoint layout differs.')
    for name in ('gpu_checkpoint', 'app', 'boot_pool', 'uncached_pool', 'a733_cached_runtime',
                 'gpu_runtime_contract', 'sealed_session'):
        start, size = symbols[name]
        require(size and lo <= start < start + size <= hi, 'Unpublished checkpoint state: ' + name)
    return parts, address, build, dict(clean_start=lo, clean_end=hi, receipt=receipt, log=log,
                                     wb_start=0, wb_end=0, packet=0x42000100)

def validate_admission(record, image_hash, template, build, cache=None, frozen=False):
    cache = {} if cache is None else cache
    def evidence(ref):
        require(isinstance(ref, dict) and set(ref) == {'path', 'sha256'} and Path(ref['path']).is_absolute(), 'Invalid checkpoint evidence reference.')
        name = ref['path']
        if name not in cache:
            require(not frozen, 'Missing frozen checkpoint evidence.'); cache[name] = Path(name).read_bytes()
        require(sha(cache[name]) == ref['sha256'], 'Changed checkpoint evidence.'); return cache[name]
    require(len(template) == 592, 'Checkpoint requires exactly 592 session bytes.')
    original.session_check(template)
    require(record.get('schema') == 'a733-gpu-entry-checkpoint-admission-v1' and record.get('execution_profile') == PROFILE
            and record.get('image_sha256') == image_hash and record.get('session_template_sha256') == sha(template)
            and type(record.get('reservation')) is int and 0 < record['reservation'] < 2**64, 'Distinct checkpoint admission required.')
    disposition = record.get('ram_disposition', 'restore')
    require(disposition in ('restore', 'discard'), 'Unknown checkpoint RAM disposition.')
    identity, port, supervisor = (record.get(k) for k in ('identity', 'port', 'supervisor'))
    require(identity and port and supervisor, 'Checkpoint target and supervisor required.')
    def bound(value):
        return value.get('identity') == identity and value.get('port') == port and value.get('image_sha256') == image_hash
    for key, ranges in [('ddr', [[0x41000000, 0x42200000]]), ('gpu_ddr', [[0x48000000, 0x79000000]])]:
        value = json.loads(evidence(record[key]))
        require(value.get('schema') == 'a733-ddr-reservation-v1' and bound(value) and value.get('ranges') == ranges
                and all(value.get(k) is True for k in ('exclusive', 'published_cache_disabled', 'remote_writers_excluded'))
                and value.get('evidence'), 'Full checkpoint DDR ownership required.')
        if disposition == 'discard': require(value.get('contents_discardable') is True, 'Both checkpoint ranges must be discardable.')
        for ref in value['evidence']: evidence(ref)
    platform = json.loads(evidence(record['platform']))
    require(platform.get('schema') == 'a733-gpu-entry-checkpoint-platform-v1' and bound(platform)
            and platform.get('entry_conditions_established') is True
            and set(platform.get('conditions', {})) == PLATFORM_CONDITIONS, 'Checkpoint CPU entry facts differ.')
    for ref in platform['conditions'].values(): evidence(ref)
    # The shared 592-byte wire format calls this binding gpu/cold_master. Here
    # it authenticates passive isolation and exclusion only, never an OPEN grant.
    passive = json.loads(evidence(record['gpu']))
    require(passive.get('schema') == 'a733-gpu-entry-checkpoint-passive-v1' and bound(passive)
            and passive.get('reservation') == record['reservation'] and passive.get('entry_conditions_established') is True
            and passive.get('provider_access_authorized') is False and type(passive.get('cold_master')) is int
            and 0 < passive['cold_master'] < 2**64 and set(passive.get('conditions', {})) == PASSIVE_CONDITIONS,
            'Checkpoint passive entry facts differ.')
    for ref in passive['conditions'].values(): evidence(ref)
    require(template[80:112] == bytes.fromhex(record['gpu']['sha256']) and
            struct.unpack_from('<Q', template, 192)[0] == passive['cold_master'] and not record.get('payloads')
            and record.get('radio_ddr') is None, 'Checkpoint wire binding differs.')
    review = json.loads(evidence(record['root_review']))
    require(review.get('schema') == 'a733-gpu-entry-checkpoint-review-v1' and bound(review) and review.get('reviewed') is True
            and review.get('execution_profile') == PROFILE and review.get('build_id') == build.hex()
            and review.get('supervisor') == supervisor and review.get('ram_disposition', 'restore') == disposition
            and set(review.get('conditions', {})) == REVIEW_CONDITIONS, 'Exact checkpoint return review required.')
    for ref in review['conditions'].values(): evidence(ref)
    require(review.get('host_sources') == {n: sha((ROOT/n).read_bytes()) for n in HOST_SOURCES}, 'Checkpoint host sources changed.')
    required = {'never-open-empty-resources-and-ROM-retirement', 'failure-retain-until-whole-power-removal',
                'DRAM-restore-only-after-validated-normal-return', 'no-old-SRAM-restore',
                'manual-whole-board-power-removal', 'stop-after-first-failure'}
    if disposition == 'discard': required = (required - {'DRAM-restore-only-after-validated-normal-return'}) | {'discardable-DRAM-release-only-after-validated-normal-return'}
    require(record.get('one_attempt') is True and set(record.get('supervision', {})) == required, 'Explicit checkpoint supervision required.')
    for ref in record['supervision'].values(): evidence(ref)
    return cache

# Reuse exactly the finite bundle serializer and its byte-for-byte validator.
# Only these named profile policies differ; no imported module is mutated.
_binding = {**vars(finite), 'ROOT': ROOT, 'PROFILE': PROFILE, 'SCHEMA': SCHEMA, 'HOST_SOURCES': HOST_SOURCES,
            'image_profile': image_profile, 'validate_admission': validate_admission}
validate_bundle = types.FunctionType(finite.validate_bundle.__code__, _binding)
_binding['validate_bundle'] = validate_bundle
prepare = types.FunctionType(finite.prepare.__code__, _binding)
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'manifest', 'admission', 'session-template', 'output', 'clang', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    a = parser.parse_args(); prepare(a.image, a.manifest, a.admission, a.session_template, a.output, a.clang, a.lld)
