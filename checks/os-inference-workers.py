#!/usr/bin/env python3
"""Check linked OS worker capabilities and exact entry binding without hardware."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

prep = load('os_workers_preparation', ROOT / 'apps/os/prepare.py')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--eight-build', type=Path, required=True)
parser.add_argument('--legacy-build', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
raw = (args.eight_build / 'sunstone-a733-os.elf').read_bytes()
manifest = json.loads((args.eight_build / 'os-image/link-check.json').read_text())
legacy = (args.legacy_build / 'sunstone-a733-os.elf').read_bytes()
legacy_manifest = json.loads((args.legacy_build / 'os-image/link-check.json').read_text())
results = {}
def refused(name, call):
    try:
        call()
    except (AssertionError, ValueError, SystemExit) as error:
        results[name] = {'refused': True, 'reason': str(error)}
    else:
        raise AssertionError(name + ' was accepted')

config = prep.image_inference(manifest, raw)
assert config == dict(version=1, workers=8, owner_core=6, core_mask=255,
                      owner_priority=3, a55_priority=2, a76_priority=3)
assert prep.image_manifest_check(manifest, raw) is False
prep.inference_admission({'inference': config}, config)
results['actual-eight-image-and-entry'] = True
assert prep.image_inference(legacy_manifest, legacy) is None
assert prep.image_manifest_check(legacy_manifest, legacy) is False
prep.inference_admission({}, None)
results['actual-legacy-pair'] = True
for field in config:
    changed = copy.deepcopy(manifest)
    changed['inference'][field] ^= 1
    refused('manifest-' + field, lambda: prep.image_inference(changed, raw))
for value in (True, 1.0):
    changed = copy.deepcopy(manifest)
    changed['inference']['version'] = value
    refused('noninteger-version-' + str(value), lambda: prep.image_inference(changed, raw))
changed = copy.deepcopy(manifest)
del changed['inference']
refused('missing-manifest-record', lambda: prep.image_inference(changed, raw))
for field, value in [('lifetime', 'resident'), ('audio', True), ('radio', True), ('headset', True), ('gpu_linear', True)]:
    changed = dict(manifest, **{field: value})
    refused('unsupported-' + field, lambda: prep.image_inference(changed, raw))
for name, record in [('missing', {}), ('pair', {'inference': dict(config, workers=2)}),
                     ('priority', {'inference': dict(config, a55_priority=3)}),
                     ('mapping', {'inference': dict(config, core_mask=0xc0)})]:
    # The actual fresh/frozen entry validator rejects before consulting any
    # unrelated platform authority, which is deliberately absent here.
    record['schema'] = prep.ENTRY_SCHEMA
    for frozen in (False, True):
        refused('entry-' + name + ('-frozen' if frozen else '-fresh'),
                lambda: prep.validate_admission(record, hashlib.sha256(raw).hexdigest(), '', b'', False,
                                                {}, frozen, inference=config))
elf = prep.os_link.Elf(raw)
symbol = next(s for s in elf.symbols if s['name'] == 'mimus_parallel_configuration')
section = elf.sections[symbol['shndx']]
offset = section[4] + symbol['value'] - section[3]
for index, value in [(0, 2), (1, 4), (2, 2), (3, 0xc0), (5, 3)]:
    changed = bytearray(raw)
    struct.pack_into('<I', changed, offset + 4 * index, value)
    refused('linked-word-' + str(index), lambda: prep.image_inference(manifest, changed))
# Strip only the new symbol name: the real adapter still reports eight, so it
# must not gain the legacy two-worker interpretation.
changed = raw.replace(b'mimus_parallel_configuration\0', b'mimus_parallel_configuratioX\0')
refused('missing-linked-record-cannot-downgrade', lambda: prep.image_inference({}, changed))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps({'passed': True, 'physical_access': False,
    'image_sha256': hashlib.sha256(raw).hexdigest(), 'legacy_image_sha256': hashlib.sha256(legacy).hexdigest(),
    'checker_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'cases': results}, indent=2) + '\n')
print('PASS OS inference capabilities:', len(results), 'cases')
