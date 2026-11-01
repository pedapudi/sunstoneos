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
PROFILE = 'gpu-entry-memory-checkpoint-v2'
SCHEMA = 'a733-gpu-entry-checkpoint-bundle-v2'
HOST_SOURCES = tuple(n for n in finite.HOST_SOURCES if n not in ('apps/gpu-finite/host.py', 'apps/gpu-finite/report.py')) + (
    'apps/gpu-entry-checkpoint/prepare.py', 'apps/gpu-entry-checkpoint/host.py', 'apps/gpu-entry-checkpoint/report.py')
PLATFORM_CONDITIONS = {'primary-cache-transition-and-secondary-off', 'cold-cache-and-dirty-alias-exclusion',
    'dual-pas-dram', 'exclusive-cpus-memory-dma', 'remote-writers-excluded', 'physical-code-publication',
    'physical-counter-continuity', 'usb-clock-power-reset-ownership', 'usb-supplies-stable-and-writers-excluded'}
PASSIVE_CONDITIONS = {'gpu-int95-disabled-inactive-clear', 'no-device-or-remote-writes-through-return'}
REVIEW_CONDITIONS = {'architecture-reset-reviewed', 'never-open-empty-resource-proof-reviewed',
                     'private-publication-and-EL1h-return-reviewed', 'failure-retention-reviewed'}

def sha(data): return hashlib.sha256(data).hexdigest()

def image_profile(path, manifest):
    data = path.read_bytes(); parts, address, build = original.image_parts(path)

    elf = ELFFile(io.BytesIO(data)); symbols = {s.name: (int(s['st_value']), int(s['st_size']))
                                               for s in elf.get_section_by_name('.symtab').iter_symbols()}
    def at(name):
        require(name in symbols and symbols[name][0], 'Missing checkpoint symbol: ' + name); return symbols[name][0]
    for name in ('gpu_entry_checkpoint', 'gpu_checkpoint_ready', 'gpu_return_finish', 'gpu_return_reset',
                 'gpu_finite_smc', 'gpu_return_allocations', 'gpu_checkpoint_interrupt_quiet'): at(name)
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
    return finite.validate_admission(record,image_hash,template,build,cache,frozen)

# Reuse exactly the finite bundle serializer and its byte-for-byte validator.
# Only these named profile policies differ; no imported module is mutated.
_binding = {**vars(finite), 'ROOT': ROOT, 'PROFILE': PROFILE, 'SCHEMA': SCHEMA, 'HOST_SOURCES': HOST_SOURCES,
            'image_profile': image_profile, 'validate_admission': validate_admission, 'unique_directory':finite.unique_directory}
validate_bundle = types.FunctionType(finite.validate_bundle.__code__, _binding)
_binding['validate_bundle'] = validate_bundle
prepare = types.FunctionType(finite.prepare.__code__, _binding)
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'session-template', 'output', 'clang', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--manifest',type=Path,help='Optional build metadata.')
    parser.add_argument('--admission',type=Path,help='Optional RAM disposition and reservation configuration.')
    a = parser.parse_args(); print(prepare(a.image, a.manifest, a.admission, a.session_template, a.output, a.clang, a.lld)['output'])
