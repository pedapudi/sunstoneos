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
a = parser.parse_args(); a.output=prep.finite.unique_directory(a.output)
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def dump(name, value):
    path = a.output/name; path.write_text(json.dumps(value, indent=2)+'\n')
    return dict(path=str(path.resolve()), sha256=sha(path))
image = a.build/'build/checkpoint.elf'; manifest = a.build/'manifest.json'
parts, address, build, geometry = prep.image_profile(image, json.loads(manifest.read_bytes()))
h = sha(image)
raw = bytearray((Path.home()/'.cache/sunstoneos/evidence/gpu-single-core-20261018/provider-valid-fixture/fixtures/session-template.bin').read_bytes())
struct.pack_into('<Q',raw,192,200)
template = a.output/'session-template.bin'; template.write_bytes(raw)
launcher = a.output/'launcher'; launcher.mkdir()
clang = Path('/usr/lib/llvm-21/bin/clang'); lld = Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21'
prep.original.build_launcher(launcher, clang, lld)
admission_path=Path(dump('ram-policy.json',{'ram_disposition':'discard'})['path'])
bundle=a.output/'bundle'
with patch.object(prep.finite.secrets,'randbits',side_effect=(100,1,2,3)):
    prepared=prep.prepare(image,None,admission_path,template,bundle,clang,lld)
assert not prepared['entry_evidence'] and 'host_sources' not in prepared
results={'bundle-without-manifest-review-or-external-evidence':True}
archive=a.output/'archived-inputs';archive.mkdir()
for name in ('launch.c','build.log','manifest.json','session-template.bin'):
    path=bundle/name
    if not path.exists():path.write_text('Archived development input.\n')
    metadata=json.loads((bundle/'prepared.json').read_bytes())
    metadata['files'][name]=prep.sha(path.read_bytes());(bundle/'prepared.json').write_text(json.dumps(metadata))
    path.write_text('Changed archived development input.\n')
    accepted=prep.validate_bundle(bundle)
    assert name not in accepted['_frozen'] and name not in accepted['files']
    path.rename(archive/name);prep.validate_bundle(bundle)
    results['archived-'+name+'-edit-and-removal-accepted']=True

def refused(name,function):
    try:function()
    except (ValueError,KeyError):results[name]=True
    else:raise AssertionError(name)
refused('normal-producer-refuses-checkpoint',lambda:prep.finite.image_profile(image,{}))
for name,change in [('owner-conflict',lambda b:b.__setitem__(slice(464,472),b[456:464])),
                    ('expanded-session',lambda b:b.extend(bytes(40)))]:
    bad=bytearray(raw);change(bad)
    refused(name,lambda:prep.validate_admission({},h,bad,build))
second=prep.prepare(image,None,admission_path,template,bundle,clang,lld)
assert Path(second['output']).parent==bundle
results['existing-bundle-preserved']=True
for name in ('session.bin','contract.bin','launch.bin'):
    path=bundle/name;saved=path.read_bytes();path.write_bytes(saved+b'x')
    refused('changed-'+name,lambda:prep.validate_bundle(bundle));path.write_bytes(saved)
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
with patch.object(host.base, 'preload', lambda *args: None), patch.object(host.base,'verify_dram',lambda *args:None):
    result = host.run_once(transaction, lambda:None, lambda:None, bundle, prepared, run, record,
                           lambda:journalled.append(copy.deepcopy(record)))
before=len(transactions)
def unavailable(*args):raise ValueError('modeled unresponsive DRAM')
with patch.object(host.base,'verify_dram',unavailable):
    refused('unresponsive-DRAM-refused-before-upload',lambda:host.run_once(transaction,lambda:None,lambda:None,bundle,prepared,run,{},lambda:None))
assert len(transactions)==before
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
