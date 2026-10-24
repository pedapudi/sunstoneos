#!/usr/bin/env python3
"""Check checkpoint bundle production and capture with explicit software-only facts."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unittest.mock import patch
if not __debug__: raise SystemExit('Checkpoint checks require assertions enabled.')
ROOT = Path(__file__).resolve().parents[2]
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value
prep = module('entry_checkpoint_prepare_check', ROOT/'apps/gpu-entry-checkpoint/prepare.py')
host = module('entry_checkpoint_host_check', ROOT/'apps/gpu-entry-checkpoint/host.py')
parser = argparse.ArgumentParser(description=__doc__)
for name in ('build', 'native', 'output'): parser.add_argument('--'+name, type=Path, required=True)
a = parser.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def dump(name, value):
    path = a.output/name; path.write_text(json.dumps(value, indent=2)+'\n')
    return dict(path=str(path.resolve()), sha256=sha(path))
image = a.build/'build/checkpoint.elf'; manifest = a.build/'manifest.json'
parts, address, build, geometry = prep.image_profile(image, json.loads(manifest.read_bytes()))
h = sha(image)
model = a.output/'MODEL-ONLY.txt'; model.write_text('Synthetic fixture only; no board admission or authority.\n')
ref = dict(path=str(model.resolve()), sha256=sha(model))
def fact(schema, **fields):
    return dict(schema=schema, identity='software-only-model', port='no-physical-port', image_sha256=h, **fields)
ddr = dump('ddr.json', fact('a733-ddr-reservation-v1', exclusive=True, published_cache_disabled=True,
    remote_writers_excluded=True, contents_discardable=True, ranges=[[0x41000000,0x42200000]], evidence=[ref]))
gpu_ddr = dump('pool-ddr.json', fact('a733-ddr-reservation-v1', exclusive=True, published_cache_disabled=True,
    remote_writers_excluded=True, contents_discardable=True, ranges=[[0x48000000,0x79000000]], evidence=[ref]))
platform = dump('platform.json', fact('a733-gpu-entry-checkpoint-platform-v1',
    entry_conditions_established=True, conditions={n:ref for n in prep.PLATFORM_CONDITIONS}))
passive = dump('passive.json', fact('a733-gpu-entry-checkpoint-passive-v2', reservation=100, cold_master=200,
    entry_conditions_established=True, provider_access_authorized=False, conditions={n:ref for n in prep.PASSIVE_CONDITIONS}))
raw = bytearray((Path.home()/'.cache/sunstoneos/evidence/gpu-single-core-20261018/provider-valid-fixture/fixtures/session-template.bin').read_bytes())
raw[80:112] = bytes.fromhex(passive['sha256']); struct.pack_into('<Q', raw, 192, 200)
template = a.output/'session-template.bin'; template.write_bytes(raw)
launcher = a.output/'launcher'; launcher.mkdir()
clang = Path('/usr/lib/llvm-21/bin/clang'); lld = Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21'
prep.original.build_launcher(launcher, clang, lld)
review = dump('review-MODELED.json', fact('a733-gpu-entry-checkpoint-review-v2', reviewed=True,
    execution_profile=prep.PROFILE, build_id=build.hex(), supervisor='modeled-supervisor', ram_disposition='discard',
    conditions={n:ref for n in prep.REVIEW_CONDITIONS}, host_sources={n:sha(ROOT/n) for n in prep.HOST_SOURCES},
    launcher_sha256=sha(launcher/'launch.bin')))
admission = fact('a733-gpu-entry-checkpoint-admission-v2', execution_profile=prep.PROFILE,
    supervisor='modeled-supervisor', session_template_sha256=sha(template), reservation=100, ram_disposition='discard',
    ddr=ddr, gpu_ddr=gpu_ddr, platform=platform, gpu=passive, root_review=review, one_attempt=True,
    supervision={n:ref for n in ('never-open-empty-resources-and-ROM-retirement', 'failure-retain-until-whole-power-removal',
        'discardable-DRAM-release-only-after-validated-normal-return', 'no-old-SRAM-restore',
        'manual-whole-board-power-removal', 'stop-after-first-failure')})
admission_path = Path(dump('admission-MODELED.json', admission)['path']); bundle = a.output/'bundle'
with patch.object(prep.finite.secrets, 'randbits', side_effect=(1,2,3)):
    prepared = prep.prepare(image, manifest, admission_path, template, bundle, clang, lld)
results = {'bundle-round-trip': True}
def refused(name, function):
    try: function()
    except (ValueError, KeyError): results[name] = True
    else: raise AssertionError(name)
refused('normal-image-refused', lambda: prep.image_profile(Path.home()/'.cache/sunstoneos/evidence/gpu-returning-repeat-fix-20261019/build/return.elf', json.loads((Path.home()/'.cache/sunstoneos/evidence/gpu-returning-repeat-fix-20261019/manifest.json').read_bytes())))
refused('normal-producer-refuses-checkpoint', lambda: prep.finite.image_profile(image, json.loads(manifest.read_bytes())))
bad = copy.deepcopy(admission); bad['schema'] = 'a733-gpu-finite-entry-v1'
refused('normal-admission-refused', lambda: prep.validate_admission(bad,h,bytes(raw),build))
for name, field, change in [('provider-authority-refused','gpu',lambda v:v.update(provider_access_authorized=True)),
                          ('partial-pool-reservation-refused','gpu_ddr',lambda v:v.update(ranges=[[0x48000000,0x48001000]])),
                          ('missing-passive-condition-refused','gpu',lambda v:v['conditions'].pop('no-device-or-remote-writes-through-return'))]:
    bad = copy.deepcopy(admission); value = json.loads(Path(bad[field]['path']).read_bytes()); change(value)
    bad[field] = dump(name+'.json', value)
    refused(name, lambda: prep.validate_admission(bad,h,bytes(raw),build))
# Exercise the real local run_once adapter. Shared preload, USB discovery and
# FEL transaction mechanics reuse existing evidence and are modeled here.
record = {}; journalled = []; transactions = []
prepared['_backup_hashes'] = {}
report_at = {0x42000000:'monitor.bin', geometry['log']:'platform.bin', geometry['receipt']:'receipt.bin', 0x42000100:'packet.bin'}
def transaction(*values, **kwargs):
    transactions.append([str(v) for v in values])
    if values == ('exe','0x52000'): return ''
    if values == ('version',): return 'soc=00001903(A733)'
    if values == ('readl','0x02050014'): return '0'
    if len(values) == 10 and values[::2] == ('readl',)*5:
        assert values[1::2] == ('0x20','0x24','0x28','0x3c','0x40')
        return 'eb00008f e3a00911 e3a0193d e3a0605c ea00004d'
    if values[0] == 'read':
        _, address, size, dest = values; data = (a.native/report_at[int(address,16)]).read_bytes()
        assert len(data) == size; dest.write_bytes(data); return ''
    raise AssertionError(values)
run = a.output/'run'; run.mkdir()
with patch.object(host.base, 'preload', lambda *args: None):
    result = host.run_once(transaction, lambda:None, lambda:None, bundle, prepared, run, record,
                           lambda:journalled.append(copy.deepcopy(record)))
assert result['checkpoint'] == 'ENTRY_MEMORY_READY' and result['gpu_jobs'] == 0 and not result['gpu_functional_success']
assert record['complete'] and record['ram_disposition'] == 'discarded'
assert not any('gpu_result' in value or value.get('checkpoint_result',{}).get('gpu_functional_success') for value in journalled)
assert not any('0x03000024' in tx for tx in transactions)
results['actual-adapter-checkpoint-journals'] = True
# Actual native packet bytes also prove receipt status independently of magic.
reports = [(a.native/n).read_bytes() for n in ('monitor.bin','platform.bin','receipt.bin','packet.bin')]
changed = list(reports); value = bytearray(changed[2]); struct.pack_into('<Q',value,32,0); changed[2] = bytes(value)
refused('normal-receipt-status-refused', lambda: host.report.validate_reports(*changed,prepared))
old_profile = copy.deepcopy(prepared); old_profile['execution_profile'] = 'gpu-entry-memory-checkpoint-v1'
refused('v1-profile-refused', lambda: host.report.validate_reports(*reports,old_profile))
dump('result.json', dict(passed=True, physical_access=False, physical_admission_created=False,
    synthetic_fixtures_only=True, image_sha256=h, launcher_sha256=sha(launcher/'launch.bin'), cases=results,
    scope='Actual checkpoint producer/validator and local run_once with captured modeled native reports; shared transport/preload modeled.'))
dump('adapter-journal.json', journalled); dump('adapter-transactions.json', transactions)
print('PASS checkpoint host:',len(results),'focused cases; no physical access.')
