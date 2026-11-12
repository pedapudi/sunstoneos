#!/usr/bin/env python3
"""Exercise the ordinary-file card producer with actual OS and stage-2 artifacts.

All board identity and evidence in this check are explicit model fixtures.
Optional prepacked assets carry the real linked application's inputs. The
128 MiB ordinary card has two 20 MiB slots and a preserved 64 MiB partition.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/card.py', 'tools/card-boot.py', 'tools/boot-slots.py', 'tools/storage-update.py',
                'tools/prepare-microsd-boot.py', 'tools/sunstone-image-link.py', 'drivers/clock/a733_cpu_clock.c']
GATE_RUNS = [[
    '--image', '{elf:card-profile}',
    '--link-check', '{build:card-profile}/os-image/sunstone-a733-os-hello/link-check.json',
    '--cpu-image', '{elf:card-profile}', '--diagnostic-image', '{elf:resident-stop}',
    '--stage2', '{build:stage2}/a733-stage2.bin', '--stage2-elf', '{elf:stage2}',
    '--boot0', '{cache}/sources/board-boot/boot0_sdcard_orangepizero3w.fex', '--output', '{output}',
]]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


card = module('check_card_producer', 'apps/os/card.py')
slots = module('check_card_slots', 'tools/boot-slots.py')
session_tool = module('check_card_session', 'tools/session.py')
checker = module('check_card_link', 'tools/sunstone-image-link.py')
rows = []


def passed(name):
    rows.append({'name': name, 'passed': True})
    print('PASS', name)


def write(path, data):
    path.write_bytes(data)
    return path


def record(path, data):
    return write(path, (json.dumps(data, indent=2) + '\n').encode())


def fixture(root, args, image=None, link_check=None, asset_values=None):
    root.mkdir()
    image = image or args.image
    linked = checker.check(image)
    if link_check is None:
        link_check = args.link_check if image == args.image else record(root/'link.json', dict(linked, passed=True))
    manifest = card.host.image_manifest(image)['manifest']
    assets = [(load, path.read_bytes()) for path, load in (slots.parse_load(value) for value in
              (args.asset if asset_values is None else asset_values))]
    declared = card.host.regions(manifest, (card.host.manifest_tool.KIND['input'],))
    inputs = []
    if not assets:
        for index, region in enumerate(declared):
            assert not any(bytes.fromhex(region['sha256'])), 'Pinned input needs --asset.'
            data = ('ordinary card input %d\n' % index).encode()
            path = write(root/('asset-%d.bin' % index), data)
            assets.append((region['address'], data))
    for index, region in enumerate(declared):
        pinned = any(bytes.fromhex(region['sha256']))
        value = next(data[region['address']-load:region['address']-load+region['bytes']] if pinned else
                     data[region['address']-load:] for load, data in assets
                     if load <= region['address'] < load + len(data))
        inputs.append(write(root/('input-%d.bin' % index), value))
    asset_files = [write(root/('packed-%d.bin' % index), data) for index, (_, data) in enumerate(assets)]
    session_tool.build(image, root/'session-build', inputs=inputs, clock_grant=bool(manifest['clock_use'] & 1))
    raw = bytearray((root/'session-build/template/session.bin').read_bytes())
    decoded = card.host.session_decode(raw)
    struct.pack_into('<2Q', raw, 24, 0, 0)
    words = [1, 2, 3, 4]
    struct.pack_into('<Q', raw, 312, 99)
    raw[320:352] = hashlib.sha256(struct.pack('<4I', *words)).digest()
    evidence = {}
    offsets = dict(card.EVIDENCE)
    offsets.update({name: decoded['offsets'][card.host.THERMAL]+offset for name, offset in card.THERMAL_EVIDENCE.items()})
    for name, offset in offsets.items():
        data = ('MODEL cold reset evidence\n' if name in ('qualification', 'recovery') else 'MODEL '+name+'\n').encode()
        path = write(root/(name+'.evidence'), data)
        raw[offset:offset+32] = hashlib.sha256(data).digest()
        evidence[name] = {'path': path.name, 'sha256': card.sha(data)}
    policy = {'schema': card.POLICY, 'scope': card.SCOPE, 'evidence_class': 'model-fixture',
              'identity': 'model-card-fixture', 'target': {'port': '3-1', 'sysfs': '/MODEL/3-1'},
              'sid_words': words, 'sid_authority': 99, 'image_sha256': card.sha(image.read_bytes()),
              'stage2_sha256': card.sha(args.stage2.read_bytes()), 'manifest_sha256': decoded['manifest_sha256'],
              'workers': manifest['workers'], 'evidence': evidence}
    if card.host.CPU_GRANT in decoded['offsets']:
        offset = decoded['offsets'][card.host.CPU_GRANT]
        raw[offset+32:offset+48] = bytes(16)
        raw[offset+64:offset+96] = bytes(32)
        b1 = {'schema': 'a733-os-cpu-clock-b1-v1', 'passed': True, 'identity': policy['identity'], 'port': '3-1',
              'grant_clusters': 0, 'pattern_lfm_clear': True, 'sid_word': 0x0400,
              'dcdc3_mv': 800, 'dcdc5_mv': 800, 'modeled': True, 'physical_authority': False}
        b1_path = record(root/'cpu-b1.json', b1)
        evidence['cpu_b1'] = {'path': b1_path.name, 'sha256': card.sha(b1_path.read_bytes())}
        vendor = card.vendor_cpu_points(b1['sid_word'])
        clusters, little, big, changes = struct.unpack_from('<4I', raw, offset+48)
        policy['cpu'] = {'clusters': clusters, 'max_mhz': [little, big], 'vendor_mv': [p['mv'] for p in vendor['points']],
                         'bin_column': vendor['bin_column'], 'changes_per_boot': changes}
    policy['canonical_session_sha256'] = card.sha(card.canonical_session(raw))
    raw_path = write(root/'cold-session.bin', raw)
    policy_path = record(root/'policy.json', policy)
    backup = root/'backup.img'
    with backup.open('xb') as source:
        source.seek((128 << 20)-1)
        source.write(b'\0')
        source.seek(0)
        mbr = bytearray(512)
        struct.pack_into('<B3xII', mbr, 450, 0x0c, 131072, 131072)
        mbr[510:512] = b'\x55\xaa'
        source.write(mbr)
        source.seek(24576*512)
        source.write(b'preserved stock backup package')
        source.seek((64 << 20)+512)
        source.write(b'preserved existing partition')
    allocation = root/'allocation.json'
    slots.allocate(SimpleNamespace(backup=backup, slot_a='40960:40960', slot_b='81920:40960',
                   training_record=None, training_record_a=None, training_record_b=None, output=allocation))
    current = root/'current.img'
    shutil.copyfile(backup, current)
    identified = record(root/'card.json', {'kind': 1, 'host': 0, 'cid': [11, 22, 33, 44],
                                         'sectors': (128 << 20)//512, 'target': policy['target']})
    return SimpleNamespace(image=image, link_check=link_check, session=raw_path, policy=policy_path,
                           stage2=args.stage2, stage2_elf=args.stage2_elf, boot0=args.boot0, packer=None,
                           allocation=allocation, backup=backup, card_image=current, card=identified,
                           asset=[str(path)+'@'+hex(load) for path, (load, _) in zip(asset_files, assets)],
                           output=root/'bundle', model_fixture=True)


def refusal(base, root, name, changes=None, session_edit=None, policy_edit=None, report_edit=None):
    args = copy.copy(base)
    args.output = root/('refuse-'+name)
    if session_edit:
        raw = bytearray(base.session.read_bytes())
        session_edit(raw)
        args.session = write(root/(name+'.session'), raw)
    if policy_edit:
        policy = json.loads(base.policy.read_text())
        policy_edit(policy)
        # Keep relative references rooted beside the model's evidence files.
        args.policy = record(base.policy.parent/(name+'.policy.json'), policy)
    if report_edit:
        report = json.loads(base.link_check.read_text())
        report_edit(report)
        args.link_check = record(root/(name+'.link.json'), report)
    for key, value in (changes or {}).items():
        setattr(args, key, value)
    try:
        card.prepare(args)
    except (ValueError, AssertionError, OSError):
        assert not os.path.lexists(args.output), 'A refusal published the requested bundle directory.'
        passed('refuses-'+name)
    else:
        raise AssertionError('Producer accepted '+name)


def end_to_end(base, root):
    result = card.prepare(base)
    assert result['physical_qualified'] is False and result['evidence_class'] == 'model-fixture'
    assert len(result['extents']) == 2 + len(base.asset)
    assert result['card_cid'] == [11, 22, 33, 44] and result['target']['port'] == '3-1'
    assert result['original_backup'] == json.loads(base.allocation.read_text())['backup']
    assert result['cpu_authority'] == (card.host.CPU_GRANT in card.host.session_decode(base.session.read_bytes())['extents'])
    assert set(result['evidence_files']) == set(json.loads(base.policy.read_text())['evidence'])
    for role, name in result['evidence_files'].items():
        assert card.sha((base.output/name).read_bytes()) == json.loads(base.policy.read_text())['evidence'][role]['sha256']
    bound = (base.output/'slot/os-session.bin').read_bytes()
    assert struct.unpack_from('<2Q', bound, 24) == (0, result['generation'])
    assert card.canonical_session(bound) == card.canonical_session(base.session.read_bytes())
    for path in base.output.rglob('*'):
        if path.is_file():assert path.stat().st_mode & 0o222 == 0
    with base.card_image.open('rb') as source:
        source.seek(24576*512); stock = source.read(8192*512)
        source.seek(64 << 20); partition = hashlib.sha256(source.read(64 << 20)).digest()
    slots.apply(SimpleNamespace(plan=base.output/'slot/plan.json', card_image=base.card_image, stop_after=None))
    first = slots.verify(SimpleNamespace(card_image=base.card_image))
    assert first['selected'] == result['slot'] == 'A'
    with base.card_image.open('rb') as source:
        source.seek(24576*512); assert source.read(8192*512) == stock
        source.seek(64 << 20); assert hashlib.sha256(source.read(64 << 20)).digest() == partition
        source.seek(40960*512); active = source.read(40960*512)
        source.seek(8192*512); header = source.read(512)
    next_args = copy.copy(base); next_args.output = root/'next-bundle'
    next_result = card.prepare(next_args)
    assert next_result['slot'] == 'B' and next_result['generation'] == 2
    slots.apply(SimpleNamespace(plan=next_args.output/'slot/plan.json', card_image=base.card_image, stop_after=None))
    assert slots.verify(SimpleNamespace(card_image=base.card_image))['selected'] == 'B'
    with base.card_image.open('rb') as source:
        source.seek(40960*512); assert source.read(40960*512) == active
        source.seek(8192*512); assert source.read(512) == header
        source.seek(24576*512); assert source.read(8192*512) == stock
        source.seek(64 << 20); assert hashlib.sha256(source.read(64 << 20)).digest() == partition
    # Planning never changes the original full backup.
    package = card.module('check_card_package', 'tools/prepare-microsd-boot.py')
    original = card.OrdinaryCard(base.backup, package)
    try:assert original.sha256 == result['original_backup']['sha256']
    finally:original.close()
    passed('end-to-end-%d-extents-two-generations-and-preservation' % len(result['extents']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'link-check', 'stage2', 'stage2-elf', 'boot0', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--asset', action='append', default=[])
    parser.add_argument('--cpu-image', type=Path, help='Optional actual image whose manifest declares CPU clock grants.')
    parser.add_argument('--diagnostic-image', type=Path, help='Actual stop-on-finish ELF for the production-image refusal.')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = fixture(args.output/'fixture', args)
    runtime = base.image.read_bytes()
    for name, offset, fmt, value in (('machine',18,'H',0),('type',16,'H',3),('entry',24,'Q',0x41000004)):
        changed=bytearray(runtime);struct.pack_into('<'+fmt,changed,offset,value)
        try:card.flat_runtime(bytes(changed))
        except ValueError:pass
        else:raise AssertionError('Invalid runtime ELF '+name+' accepted')
    changed=bytearray(runtime)
    phoff=struct.unpack_from('<Q',changed,32)[0]
    stride,count=struct.unpack_from('<2H',changed,54)
    for i in range(count):
        at=phoff+i*stride
        kind,flags,_,load,_,size=struct.unpack_from('<II4Q',changed,at)
        if kind==1 and load<=0x41000000<load+size:
            struct.pack_into('<I',changed,at+4,flags&~1)
    try:card.flat_runtime(bytes(changed))
    except ValueError:pass
    else:raise AssertionError('Uninitialized or nonexecutable entry accepted')
    package=card.module('monitor_entry_package','tools/prepare-microsd-boot.py')
    changed=bytearray(args.stage2_elf.read_bytes());struct.pack_into('<Q',changed,24,0x47000004)
    try:card.monitor_check(bytes(changed),args.stage2.read_bytes(),package)
    except ValueError:pass
    else:raise AssertionError('Wrong stage-2 ELF entry accepted')
    passed('runtime-and-stage2-entry-contract-refusals')
    original = base.session.read_bytes()
    linked = checker.check(base.image)
    thermal_offset = card.host.session_decode(original)['offsets'][card.host.THERMAL]
    invalid = [(name, offset, fmt, value) for name, offset, fmt, value in (
        ('minimum-reload',112,'Q',1),('maximum-recovery',120,'Q',1),('guard',128,'Q',1),('reset-by',136,'Q',1),
        ('RTC-control',144,'I',1),('RTC-divider',148,'I',1),('RTC-source',152,'I',1),('board-reserved',156,'I',1),
        ('deadline',160,'Q',1),('session-start',168,'Q',1),('reload-after',176,'Q',1),
        ('topology',184,'Q',0),('cold-master',192,'Q',0),('memory-authority',200,'Q',0),('recovery-authority',208,'Q',0),
        ('host-authority',408,'Q',1),('radio-authority',352,'Q',1),('prefix-reserved',582,'B',1),
        ('USB-supply',512,'I',1),('fabric-rate',500,'I',0))]
    invalid += [('thermal-'+name, thermal_offset+offset, fmt, value) for name,offset,fmt,value in (
        ('owner',24,'Q',0),('authority',32,'Q',0),('APB-parent',168,'I',1<<24),('APB-rate',172,'I',1),
        ('ADC-config',176,'B',2),('alarm-mask',177,'B',0),('critical-mask',178,'B',0),
        ('reserved',179,'B',1),('low-trip',180,'H',0),('high-trip',182,'H',4096),('hysteresis',184,'H',0),
        ('tail-reserved',210,'H',1),('CPU-critical',212,'I',110001))]
    for name, offset, fmt, value in invalid:
        changed = bytearray(original);struct.pack_into('<'+fmt,changed,offset,value)
        try:card.cold_profile(bytes(changed),base.image,linked)
        except ValueError:pass
        else:raise AssertionError('Native session invariant accepted: '+name)
    passed('cold-native-prefix-and-thermal-invariants-%d-refusals' % len(invalid))
    # Refusals run before the model applies any plan.
    refusal(base,args.output,'model-without-explicit-flag',{'model_fixture':False})
    refusal(base,args.output,'bad-link-report',report_edit=lambda d:d.update(build_id='0'*64))
    if args.diagnostic_image:
        diagnostic = dict(checker.check(args.diagnostic_image),passed=True)
        refusal(base,args.output,'stop-on-finish-production-image',{
            'image':args.diagnostic_image,'link_check':record(args.output/'diagnostic-link.json',diagnostic)})
    refusal(base,args.output,'bad-policy-SID',policy_edit=lambda d:d['sid_words'].__setitem__(0,99))
    refusal(base,args.output,'bad-policy-target',policy_edit=lambda d:d['target'].update(port='3-3'))
    refusal(base,args.output,'bad-evidence-hash',policy_edit=lambda d:d['evidence']['memory'].update(sha256='0'*64))
    refusal(base,args.output,'bad-evidence-binding',session_edit=lambda r:r.__setitem__(slice(248,280),bytes(32)))
    refusal(base,args.output,'qualification-recovery-mismatch',session_edit=lambda r:r.__setitem__(slice(80,112),bytes(32)))
    refusal(base,args.output,'stale-board-epoch',session_edit=lambda r:struct.pack_into('<Q',r,24,1))
    refusal(base,args.output,'persisted-board-generation',session_edit=lambda r:struct.pack_into('<Q',r,32,1))
    refusal(base,args.output,'unsupported-policy-schema',policy_edit=lambda d:d.update(schema='a733-os-card-policy-v1'))
    refusal(base,args.output,'wrong-policy-workers',policy_edit=lambda d:d.update(workers=d['workers']+1))
    card_record = json.loads(base.card.read_text()); card_record['sectors'] -= 1
    refusal(base,args.output,'wrong-card-capacity',{'card':record(args.output/'bad-card.json',card_record)})
    allocation = json.loads(base.allocation.read_text()); allocation['backup']['sha256'] = '0'*64
    refusal(base,args.output,'wrong-original-backup',{'allocation':record(args.output/'bad-allocation.json',allocation)})
    first, load = slots.parse_load(base.asset[0])
    damaged = bytearray(first.read_bytes()); damaged[0] ^= 1
    bad_asset = write(args.output/'bad-asset.bin',damaged)
    refusal(base,args.output,'bad-asset-slice',{'asset':[str(bad_asset)+'@'+hex(load)]+base.asset[1:]})
    padded = write(args.output/'bad-padding.bin',first.read_bytes()+b'\x01')
    refusal(base,args.output,'nonzero-asset-padding',{'asset':[str(padded)+'@'+hex(load)]+base.asset[1:]})
    facts=card.host.session_decode(base.session.read_bytes())['extents'][card.host.INPUTS]
    spans=[]
    for i,region in enumerate(card.host.regions(card.host.image_manifest(base.image)['manifest'],(card.host.manifest_tool.KIND['input'],))):
        length,_=card.host.INPUT.unpack_from(facts,i*card.host.INPUT.size)
        if load<=region['address'] and region['address']+length<=load+len(first.read_bytes()):
            spans.append((region['address']-load,region['address']-load+length))
    for before,after in zip(sorted(spans),sorted(spans)[1:]):
        if before[1]<after[0]:
            changed=bytearray(first.read_bytes());changed[before[1]]=1
            gap=write(args.output/'bad-gap.bin',changed)
            refusal(base,args.output,'nonzero-internal-asset-gap',{'asset':[str(gap)+'@'+hex(load)]+base.asset[1:]})
            break
    refusal(base,args.output,'missing-asset',{'asset':base.asset[1:]})
    monitor = bytearray(base.stage2.read_bytes()); monitor[-1] ^= 1
    refusal(base,args.output,'stage2-binary-mismatch',{'stage2':write(args.output/'bad-monitor.bin',monitor)})
    boot0 = bytearray(base.boot0.read_bytes()); boot0[-1] ^= 1
    refusal(base,args.output,'boot0-mismatch',{'boot0':write(args.output/'bad-boot0.bin',boot0)})
    if args.cpu_image:
        cpu = fixture(args.output/'cpu-fixture',args,image=args.cpu_image,asset_values=[])
    else:
        cpu = base
    raw = cpu.session.read_bytes(); decoded = card.host.session_decode(raw)
    if card.host.CPU_GRANT in decoded['offsets']:
        offset = decoded['offsets'][card.host.CPU_GRANT]
        canonical = card.canonical_session(raw)
        for name, relative in (('perf',64),('reserved',68)):
            changed = bytearray(raw);changed[offset+relative]=1
            assert card.canonical_session(changed) != canonical
            refusal(cpu,args.output,'CPU-'+name,session_edit=lambda r,p=offset+relative:r.__setitem__(p,1))
        for relative in (32,40):
            changed = bytearray(raw);struct.pack_into('<Q',changed,offset+relative,7)
            assert card.canonical_session(changed) == canonical
            refusal(cpu,args.output,'stale-CPU-'+str(relative),session_edit=lambda r,p=offset+relative:struct.pack_into('<Q',r,p,7))
        refusal(cpu,args.output,'CPU-B1-wrong-identity',policy_edit=lambda d:d.update(identity='another-model-board'))
        refusal(cpu,args.output,'CPU-wrong-vendor-maximum',policy_edit=lambda d:d['cpu']['max_mhz'].__setitem__(0,416))
        result = card.prepare(cpu) if cpu is not base else None
        if result:assert result['physical_qualified'] is False
        passed('CPU-canonical-hash-binds-perf-reserved-and-vendor-policy')
    else:
        raise AssertionError('Provide --cpu-image to cover the CPU grant ABI.')
    # The stream guard detects truncation and same-size writes before helper reuse.
    package = card.module('guard_package', 'tools/prepare-microsd-boot.py')
    with patch.object(card.os,'pread',return_value=b''):
        try:card.OrdinaryCard(base.backup,package)
        except ValueError:pass
        else:raise AssertionError('short full-card read accepted')
    guarded = card.OrdinaryCard(base.card_image,package)
    try:
        with base.card_image.open('r+b') as source:
            source.seek(1024);source.write(b'changed')
        try:guarded.check()
        except ValueError:pass
        else:raise AssertionError('changed source accepted')
    finally:guarded.close()
    # Restore the ordinary working model from the unchanged full backup.
    shutil.copyfile(base.backup,base.card_image)
    passed('full-card-short-read-and-source-mutation-refused')
    # Mutation by a reused planner must fail before publication too.
    load_module=card.module
    def mutating_module(name,path):
        loaded=load_module(name,path)
        if str(path)=='tools/boot-slots.py':
            plan=loaded.plan
            def mutate(values):
                result=plan(values)
                with Path(values.card_image).open('r+b') as source:
                    source.seek(1024);source.write(b'changed by planner')
                return result
            loaded.plan=mutate
        return loaded
    changed_args=copy.copy(base);changed_args.output=args.output/'changed-during-planning'
    with patch.object(card,'module',side_effect=mutating_module):
        try:card.prepare(changed_args)
        except ValueError:pass
        else:raise AssertionError('Planner source mutation was accepted')
    assert not os.path.lexists(changed_args.output)
    shutil.copyfile(base.backup,base.card_image)
    passed('source-mutation-across-helper-reuse-refused-before-publication')
    end_to_end(base,args.output)
    staging=args.output/'.publish-test.partial';staging.mkdir()
    destination=args.output/'existing-output';destination.mkdir()
    record(destination/'keep.json',{'keep':True})
    try:card.publish(staging,destination)
    except OSError:pass
    else:raise AssertionError('Atomic publication replaced an existing destination')
    assert json.loads((destination/'keep.json').read_text())=={'keep':True} and staging.exists()
    empty=args.output/'existing-empty';empty.mkdir()
    try:card.publish(staging,empty)
    except OSError:pass
    else:raise AssertionError('Atomic publication replaced an empty destination')
    assert empty.is_dir() and not list(empty.iterdir()) and staging.exists()
    failed=copy.copy(base);failed.output=args.output/'failed-publication'
    with patch.object(card,'publish',side_effect=OSError('injected publication failure')):
        try:card.prepare(failed)
        except OSError:pass
        else:raise AssertionError('Publication failure was ignored')
    assert not os.path.lexists(failed.output)
    assert any((path/'bundle.json').exists() for path in args.output.glob('.failed-publication.partial-*'))
    passed('atomic-publication-refuses-existing-destination')
    record(args.output/'results.json', {'physical_access':False,'model_fixture':True,'cases':rows})
    print('%d checks passed; no device access' % len(rows))


if __name__ == '__main__':
    main()
