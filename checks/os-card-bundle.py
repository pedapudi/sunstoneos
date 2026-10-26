#!/usr/bin/env python3
"""Exercise the real cold-card producer using explicit ordinary-file model evidence."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import shlex
from types import SimpleNamespace
from harness import CLANG

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('card_os', ROOT / 'apps/os/card.py')
card = importlib.util.module_from_spec(spec); spec.loader.exec_module(card)


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def npu_records(out, raw, refs, image_hash):
    rules = card.host.module('card_fixture_npu_rules', ROOT/'apps/npu-board/prepare.py')
    evidence = out/'npu-condition.json';save(evidence, {'fixture':'MODEL ONLY; no hardware evidence'})
    ref = {'path':str(evidence.resolve()), 'sha256':card.sha(evidence.read_bytes())}
    conditions = {k:set(v) for k,v in rules.CONDITIONS.items()}
    conditions['clock'].remove('supervised-cross-pll-disturbance-risk')
    conditions['clock'].add('cold-boot-cross-pll-disturbance-qualified')
    conditions['supervision'] = {'watchdog-reset-retires-npu-and-memory', 'cold-entry-excludes-remote-writers',
                               'retain-until-qualified-cold-reset', 'stop-after-first-failure'}
    for name in ('ddr','admission','clock','power','supervision'):
        fact = {'schema':'a733-os-card-npu-'+name+'-v1','identity':'model-board','image_sha256':image_hash,
                'sid_authority':0x434f4c44,'evidence_class':'model-fixture','reviewed':True}
        if name=='ddr':
            fact.update(schema='a733-ddr-reservation-v1',ranges=[card.host.NPU_RANGE],exclusive=True,
                        published_cache_disabled=True,remote_writers_excluded=True,evidence=[ref])
        else: fact['conditions']={key:ref for key in sorted(conditions[name])}
        if name=='admission':fact['ddr_sha256']=refs['npu_ddr']['sha256']
        if name=='power':fact['supply_uv']=800000
        if name=='clock':fact.update(mhz=852,sid_dvfs_word=0x400)
        if name=='supervision':fact.update(supervisor=21,work_us=60000000,one_attempt_per_boot=True)
        path=out/('npu_'+name+'.json');save(path,fact);digest=card.sha(path.read_bytes())
        refs['npu_'+name]={'path':str(path.resolve()),'sha256':digest}
        offset=card.evidence_offsets(raw)['npu_'+name];raw[offset:offset+32]=bytes.fromhex(digest)


def npu_cases(card, args, raw, manifest, refused):
    policy=json.loads(args.policy.read_text())
    for key in ('jobs','client_model','owner','work_us','mhz','supervisor'):
        changed=json.loads(json.dumps(policy));changed['npu'][key]+=1
        path=args.policy.parent/('bad-npu-'+key+'.json');save(path,changed)
        bound = bytearray(raw)
        bound[-32:] = hashlib.sha256(path.read_bytes()).digest()
        refused('cold-npu-'+key,lambda p=path,r=bound:card.policy_check(p,r,args.image.read_bytes(),manifest,args.stage2.read_bytes(),True),
                'Cold NPU owner/catalogue/jobs/pool/operating point differs.')
    original=args.image.read_bytes();elf=card.host.os_link.Elf(original)
    card.linked_npu_assets(elf)
    table=next(s for s in elf.symbols if s['name']=='os_npu_models' and s['shndx']!=0)
    pointer,size=struct.unpack('<2Q',elf.read(table['value'],16))
    section=next(s for s in elf.sections if s[1]!=8 and s[3]<=pointer<s[3]+s[5])
    bad=bytearray(original);bad[section[4]+pointer-section[3]]^=1
    refused('actual-linked-npu-model-bytes',lambda:card.linked_npu_assets(card.host.os_link.Elf(bytes(bad))))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'base-model', 'overlay-model', 'wav', 'boot0', 'packer', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args(); out = a.output; out.mkdir(parents=True, exist_ok=False)
    image = a.build / 'sunstone-a733-os.elf'
    manifest = json.loads((a.build / 'os-image/link-check.json').read_text())
    npu = manifest.get('npu', False)
    _, build_id = card.flat_runtime(image.read_bytes())
    model = out / 'native-admission'
    command = [str(CLANG), '-std=c11', '-D_GNU_SOURCE', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Iinclude', '-Iplatform/a733-smp-cached', '-Iapps/os',
               '-DSUNSTONE_BOARD_SUPERVISED=1', '-DSUNSTONE_BOARD_USB2_DUAL=1', '-DSUNSTONE_BOARD_RESIDENT=1',
               '-DA733_A76_ADMITTED=1', '-DSUNSTONE_A733_OS_CPU_CLOCK=1', '-DA733_SMP_CONTRACT_MAGIC=0x415337334f524331ULL',
               '-DOS_MODEL_BUILD_ID=' + ','.join(str(v) for v in build_id), 'checks/os-image-model.c',
               'drivers/storage/storage_digest.c', 'drivers/clock/a733_cpu_clock.c', '-o', str(model)]
    if npu:
        commands = json.loads((a.build/'compile_commands.json').read_text())
        native = next(row for row in commands if row['file'] == str(ROOT/'apps/os/admission.c'))
        includes = [arg for arg in shlex.split(native['command']) if arg.startswith('-I') and
                    ('FreeRTOS-Kernel' in arg or 'freertos-a733' in arg or arg.endswith('/apps/board'))]
        command += ['-DSUNSTONE_A733_OS_NPU=1', *includes, str(a.build/'os-image/npu-models.c')]
    subprocess.run(command, cwd=ROOT, check=True); save(out / 'model-command.json', command)
    subprocess.run([model, '--write-staged-session', out / 'session.bin', '1'], check=True)
    raw = bytearray((out / 'session.bin').read_bytes())
    grant = len(raw) - 96
    struct.pack_into('<2Q', raw, 24, 0, 0)
    struct.pack_into('<Q', raw, 312, 0x434f4c44)
    sid = [0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00]
    raw[320:352] = hashlib.sha256(struct.pack('<4I', *sid)).digest()
    refs = {}
    for name, offset in card.evidence_offsets(raw).items():
        path = out / (name + '.json')
        save(path, {'fixture': 'MODEL ONLY; no physical fact established', 'subject': name})
        digest = card.sha(path.read_bytes()); raw[offset:offset+32] = bytes.fromhex(digest)
        refs[name] = {'path': str(path.resolve()), 'sha256': digest}
    if npu:
        npu_records(out, raw, refs, card.sha(image.read_bytes()))
    raw[80:112] = raw[280:312]; refs['qualification'] = refs['recovery']
    b1 = out / 'cpu-b1.json'
    save(b1, {'schema': card.host.B1_SCHEMA, 'fixture': 'MODEL ONLY', 'identity': 'model-board',
              'passed': True, 'grant_clusters': 0, 'pattern_lfm_clear': True, 'dcdc3_mv': 800, 'dcdc5_mv': 800, 'sid_word': 0})
    refs['cpu_b1'] = {'path': str(b1.resolve()), 'sha256': card.sha(b1.read_bytes())}
    maxima = [card.host.supported(c, 9)[-1] for c in ('cpu-l', 'cpu-b')]
    struct.pack_into('<2Q4I', raw, grant + 32, 0, 0, 3, *maxima, 8)
    manifest = json.loads((a.build / 'os-image/link-check.json').read_text())
    policy = {'schema': card.POLICY, 'scope': 'resident-recorded-cold-boot', 'identity': 'model-board',
              'evidence_class': 'model-fixture', 'sid_authority': 0x434f4c44, 'sid_words': sid,
              'image_sha256': card.sha(image.read_bytes()), 'stage2_sha256': card.sha((a.build/'a733-stage2.bin').read_bytes()),
              'inference': manifest['inference'], 'canonical_session_sha256': card.sha(card.canonical_session(raw)),
              'cpu': {'clusters': 3, 'max_mhz': maxima, 'changes_per_boot': 8}, 'evidence': refs}
    if npu: policy['npu'] = card.npu_summary(raw)
    save(out / 'policy.json', policy); raw[-32:] = hashlib.sha256((out/'policy.json').read_bytes()).digest()
    (out / 'session.bin').write_bytes(raw)
    backup = out / 'original-backup.img'
    with backup.open('xb') as stream:
        stream.truncate(128 << 20)
        stream.seek(12 << 20); stream.write(b'PRESERVE-STOCK-BACKUP' * 1024)
    allocation = out / 'allocation.json'
    card.slots.allocate(SimpleNamespace(backup=backup, slot_a='40960:49152', slot_b='90112:49152',
                        training_record=None, training_record_a=None, training_record_b=None, output=allocation))
    save(out/'card.json', {'kind': 1, 'host': 0, 'cid': [1,2,3,4], 'sectors': (128 << 20)//512,
                          'target': {'port': 'MODEL-NO-PORT', 'sysfs': 'MODEL-NO-SYSFS'}})
    working = out/'working.img'; shutil.copyfile(backup, working)
    args = SimpleNamespace(image=image, manifest=a.build/'os-image/link-check.json', stage2=a.build/'a733-stage2.bin',
                           stage2_elf=a.build/'a733-stage2.elf', session=out/'session.bin', policy=out/'policy.json',
                           base_model=a.base_model, overlay_model=a.overlay_model, wav=a.wav, allocation=allocation,
                           backup=backup, card_image=working, card=out/'card.json', boot0=a.boot0, packer=a.packer,
                           output=out/'bundle', model_fixture=True)
    class Once:
        def __init__(self,path): self.path,self.calls=path,0
        def read_bytes(self):
            self.calls+=1
            assert self.calls==1, 'Input reread: '+str(self.path)
            return self.path.read_bytes()
    snapshots = {name:Once(getattr(args,name)) for name in ('image','manifest','stage2','stage2_elf','session',
                                                          'base_model','overlay_model','wav','allocation','card')}
    captured = SimpleNamespace(**vars(args));vars(captured).update(snapshots)
    result = card.prepare(captured)
    assert all(p.calls==1 for p in snapshots.values())
    assert result['inference']['workers'] == manifest['inference']['workers'] and result['cpu_vendor_top_policy'] and not result['physical_qualified']
    plan_path = args.output/'slot/plan.json'; plan = json.loads(plan_path.read_text())
    bound = (args.output/'slot/os-session.bin').read_bytes()
    assert struct.unpack_from('<2Q', bound, 24) == struct.unpack_from('<2Q', bound, grant + 32) == (0,1)
    assert card.canonical_session(bound) == card.canonical_session(raw) and len(plan['extents']) == 4
    before_hash = card.sha(backup.read_bytes())
    card.slots.apply(SimpleNamespace(plan=plan_path, card_image=working, stop_after=None))
    fd, sectors = card.slots.open_image(working)
    try:
        selected = card.slots.inspect(fd, sectors)
        assert selected['selected'] == 'A' and selected['slots']['A']['load'] == 'done'
    finally:
        card.os.close(fd)
    original, installed = backup.read_bytes(), working.read_bytes()
    cursor = 0
    for item in sorted(plan['writes'], key=lambda w:w['first_sector']):
        begin, end = item['first_sector']*512, (item['first_sector']+item['sectors'])*512
        assert original[cursor:begin] == installed[cursor:begin]
        cursor = end
    assert original[cursor:] == installed[cursor:] and card.sha(backup.read_bytes()) == before_hash
    second = card.slots.plan(SimpleNamespace(allocation=allocation, card_image=working,
                   image=str(args.output/'runtime.bin')+'@0x41000000', entry=0x41000000,
                   os_session=args.output/'session-template.bin', asset=[str(args.output/'models.bin')+'@0x44000000',
                   str(args.output/'recorded.wav')+'@0x45000000'], container=None, output=out/'second-plan'))
    second_session = (out/'second-plan/os-session.bin').read_bytes()
    assert second['slot'] == 'B' and second['generation'] == 2
    assert struct.unpack_from('<2Q', second_session, 24) == struct.unpack_from('<2Q', second_session, grant + 32) == (0,2)
    cases = ['mutable-inputs-read-once', 'actual-linked-worker-bundle', 'four-extents-selected', 'both-generation-one', 'standby-generation-two',
             'canonical-preserved', 'backup-and-non-target-bytes-preserved']
    def refused(name, action, reason=None):
        try: action()
        except (ValueError, AssertionError) as error:
            assert reason is None or reason in str(error), (name, str(error))
            cases.append(name); return
        raise AssertionError('Expected refusal: ' + name)
    for name, offset, fmt, value in [('persisted-board-epoch',24,'Q',7), ('assigned-template',32,'Q',7),
                                    ('persisted-grant-epoch',grant+32,'Q',7), ('assigned-grant',grant+40,'Q',7),
                                    ('other-workload',400,'I',4), ('one-cluster',grant+48,'I',1), ('missing-board-owner',456,'Q',0),
                                    ('missing-memory-authority',200,'Q',0), ('unexpected-radio',352,'Q',1)]:
        bad = bytearray(raw); struct.pack_into('<'+fmt, bad, offset, value)
        refused(name, lambda:card.template_check(bad))
    refused('unknown-size', lambda:card.template_check(raw+bytes(8)))
    refused('unassigned-binder-rejects-finalized', lambda:card.slots.os_session_generation(bound, 2))
    refused('model-not-production', lambda:card.policy_check(args.policy, raw, image.read_bytes(), manifest,
                                                           args.stage2.read_bytes(), False))
    for name, offset in [('sid-mismatch',320), ('owner-canonical',456), ('thermal-canonical',772), ('approval-mismatch',grant+64)]:
        bad = bytearray(raw); bad[offset] ^= 1
        refused(name, lambda:card.policy_check(args.policy, bad, image.read_bytes(), manifest, args.stage2.read_bytes(), True))
    refused('monitor-bytes-mismatch', lambda:card.monitor_check(args.stage2_elf.read_bytes(), args.stage2.read_bytes()+b'\0'))
    if npu:
        subprocess.run([model, '--npu-health'], check=True)
        cases.append('resident-npu-health')
        npu_cases(card, args, raw, manifest, refused)
    result = {'passed': True, 'physical_access': False, 'cases': cases, 'bundle_sha256': card.sha((args.output/'bundle.json').read_bytes()),
              'sources': {p:card.sha((ROOT/p).read_bytes()) for p in ('apps/os/card.py','tools/boot-slots.py',
                         'checks/os-card-bundle.py','checks/os-image-model.c','apps/os/services.c')}}
    save(out/'result.json', result); print('PASS', len(cases), 'cold card bundle cases')

if __name__ == '__main__': main()
