"""Bind the primary cache diagnostic to the existing returning FEL runner."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('primary_cache_prepare', ROOT/'tools/prepare-primary-cache-probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
require, digest = probe.require, probe.digest
PROFILE = 'primary-cache-return-v1'
HOST_SOURCES = (*probe.primary.HOST_SOURCES, 'tools/primary-cache-host.py')
MODEL_SOURCES = ('checks/primary-cache-probe.py', *probe.primary.MODEL_SOURCES)
CASES = ('complete-return', 'transport-failure-retained', 'missing-return-retained',
         'invalid-report-restored', 'changed-input-refused', 'wrong-epoch-restored')
EL1_CASES = CASES + ('returned-local-refusal', 'incomplete-el1-restored', 'changed-entry-patch-refused')


def load(build, image, manifest):
    require(manifest.get('schema') == 'a733-primary-cache-probe-v1' and
            manifest.get('physical_access') is False and manifest.get('native_sha256') == probe.NATIVE,
            'Wrong primary cache profile.')
    el1 = manifest.get('el1', False)
    require(type(el1) is bool, 'Invalid EL1 selection.')
    report = probe.report_spec(manifest)
    require(manifest['sources'] == {n: digest(ROOT/n) for n in probe.sources(el1)}, 'Cache probe sources changed.')
    parent_path = Path(manifest['primary_manifest']['path'])
    require(parent_path.is_absolute() and digest(parent_path) == manifest['primary_manifest']['sha256'],
            'Primary preparation manifest changed.')
    parent = json.loads(parent_path.read_text())
    frozen, dram = probe.primary.load(parent_path.parent, image, parent)
    require(manifest['parent_hooks'] == parent['hooks'],
            'Primary preparation bindings changed.')
    require(manifest['report'] == {'address': report[0], 'bytes': report[1]},
            'Cache report reservation differs.')
    dram = [(name, *(report if name == 'primary-report' else (address, size)))
            for name, address, size in dram]
    require(all(report[0]+report[1] <= address or address+size <= report[0]
                for name, address, size in dram if name != 'primary-report'), 'Cache report overlaps native RAM.')
    with image.open('rb') as stream:
        symbols = {s.name: s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()}
    require(manifest['native_symbols'] == {name: symbols[target] for name, target in
            probe.native_aliases(el1).items()}, 'Production cache callees differ.')
    with (build/'cache.elf').open('rb') as stream:
        hooks = {s.name: s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                 if s.name.startswith(probe.HOOK_PREFIXES)}
    require(manifest['hooks'] == hooks and hooks['primary_cache_probe'] == probe.EXTENSION,
            'Cache extension entry differs.')
    address = parent['hooks']['primary_publication_after_prepare']
    offset = address-probe.primary.probe.OVERLAY[0]
    original = struct.unpack_from('<I', frozen['overlay.bin'], offset)[0]
    replacement = 0x14000000 | (((probe.EXTENSION-address)//4) & 0x3ffffff)
    require(manifest['patch'] == {'address': address, 'original': original, 'replacement': replacement},
            'Cache call patch differs.')
    expected = bytearray(frozen['overlay.bin'])
    struct.pack_into('<I', expected, offset, replacement)
    require(len(expected) <= probe.EXTENSION-probe.primary.probe.OVERLAY[0], 'Parent overlaps extension.')
    expected += bytes(probe.EXTENSION-probe.primary.probe.OVERLAY[0]-len(expected))
    expected += (build/'cache.bin').read_bytes()
    actual = (build/'overlay.bin').read_bytes()
    require(actual == expected and 0 < len(actual) <= probe.primary.probe.OVERLAY[1] and
            manifest['overlay'] == {**parent['overlay'], 'sha256': hashlib.sha256(actual).hexdigest()},
            'Cache overlay differs from the reviewed extension.')
    payload_data, payloads, patches = probe.patch_payloads(frozen, parent['payloads'],
                                                         probe.patch_points(symbols) if el1 else (), hooks)
    require(manifest['payloads'] == payloads and manifest['native_patches'] == patches,
            'EL1 checkpoint patches differ.')
    for name, data in payload_data.items():
        require((build/name).read_bytes() == data, 'Native payload differs beyond reviewed patches.')
    frozen.update(payload_data)
    frozen['overlay.bin'] = actual
    return frozen, dram


def validate_software(software, manifest_hash):
    require(software.get('schema') == 'a733-primary-cache-software-v1' and
            software.get('physical_access') is False and software.get('manifest_sha256') == manifest_hash,
            'Cache host evidence covers different inputs.')
    require(software.get('host_sources') == {n: digest(ROOT/n) for n in HOST_SOURCES}, 'Host implementation changed.')
    require(software.get('host_cases') == dict.fromkeys(EL1_CASES if software.get('el1', False) else CASES, True), 'Cache host cases are incomplete.')
    item = software['instruction_evidence']
    path = Path(item['path'])
    require(path.is_absolute() and digest(path) == item['sha256'], 'Cache instruction evidence changed.')
    model = json.loads(path.read_text())
    require(model.get('physical_access') is False and model.get('native_sha256') == probe.NATIVE and
            model.get('manifest_sha256') == manifest_hash and
            model.get('sources') == {n: digest(ROOT/n) for n in MODEL_SOURCES}, 'Cache instruction inputs changed.')
    require(model.get('el1', False) == software.get('el1', False), 'Model covers another CPU interval.')
    expected = ('success', 'baseline-refusal', 'group-refusal', 'wrong-el1-core',
                'cleanup-readback', 'cache-restoration-failure') if model.get('el1', False) else (
                'primary-production-cache-adoption-restored-return',
                'cache-restoration-failure-retains-without-reset')
    require({c['case']: c['pass'] for c in model['cases']} == dict.fromkeys(expected, True),
            'Cache instruction cases incomplete.')


def initial_report(nonce, manifest=None):
    require(len(nonce) == 16, 'Invalid nonce.')
    result = bytearray(probe.report_spec(manifest or {})[1])
    result[32:48] = nonce
    return bytes(result)
