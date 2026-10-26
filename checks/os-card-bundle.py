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


def gpu_records(out, image_hash, stage2_hash, config, combined):
    source=out/'gpu-source.json';save(source,{'fixture':'MODEL ONLY; no physical authority'})
    reference={'path':str(source.resolve()),'sha256':card.sha(source.read_bytes())}
    result={'configuration':config,'conditions':{}}
    conditions=card.GPU_CONDITIONS | ({card.GPU_NPU_CLOCK} if combined else set())
    for condition in sorted(conditions):
        fact={'schema':'a733-os-card-gpu-condition-v1','condition':condition,'reviewed':True,
              'identity':'model-board','sid_authority':0x434f4c44,'image_sha256':image_hash,
              'stage2_sha256':stage2_hash,'evidence_class':'model-fixture','modeled':True,
              'physical_authority':False,'evidence':[reference]}
        if condition==card.GPU_RESET:
            fact.update(after_uncertain_loss=True,before_stage2_reload=True,
                        before_reserved_memory_reuse=True,firmware_and_bus_transactions_retired=True,
                        pll_gpu0_disabled=True,gpu_gates_closed=True,gpu_bus_reset_asserted=True,initial_supply_uv=800000)
        if condition==card.GPU_NPU_CLOCK:
            fact.update(schema='a733-os-card-gpu-npu-clock-v1',scope='simultaneous-powered-cold-lifetime',
                        gpu={'core_hz':config['core_hz'],'supply_uv':config['supply_uv']},
                        npu={'mhz':852,'supply_uv':800000},npu_pll_enable_with_gpu_active=True,
                        gpu_pll_start_stop_with_npu_active=True,cpu_ddr_clocks_remain_admitted=True)
        path=out/('gpu-'+condition+'.json');save(path,fact)
        result['conditions'][condition]={'path':str(path.resolve()),'sha256':card.sha(path.read_bytes())}
    return result


def npu_records(out, raw, refs, image_hash, gpu=None):
    rules = card.host.module('card_fixture_npu_rules', ROOT/'apps/npu-board/prepare.py')
    evidence = out/'npu-condition.json';save(evidence, {'fixture':'MODEL ONLY; no hardware evidence'})
    ref = {'path':str(evidence.resolve()), 'sha256':card.sha(evidence.read_bytes())}
    conditions = {k:set(v) for k,v in rules.CONDITIONS.items()}
    conditions['clock'].remove('supervised-cross-pll-disturbance-risk')
    conditions['clock'].add('cold-boot-cross-pll-disturbance-qualified')
    if gpu:conditions['clock'].add(card.GPU_NPU_CLOCK)
    conditions['supervision'] = {'watchdog-reset-retires-npu-and-memory', 'cold-entry-excludes-remote-writers',
                               'retain-until-qualified-cold-reset', 'stop-after-first-failure'}
    for name in ('ddr','admission','clock','power','supervision'):
        fact = {'schema':'a733-os-card-npu-'+name+'-v1','identity':'model-board','image_sha256':image_hash,
                'sid_authority':0x434f4c44,'evidence_class':'model-fixture','reviewed':True}
        if name=='ddr':
            fact.update(schema='a733-ddr-reservation-v1',ranges=[card.host.NPU_RANGE],exclusive=True,
                        published_cache_disabled=True,remote_writers_excluded=True,evidence=[ref])
        else: fact['conditions']={key:(gpu['conditions'][key] if key==card.GPU_NPU_CLOCK else ref) for key in sorted(conditions[name])}
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


def gpu_cases(args, raw, manifest, model):
    image=args.image.read_bytes();monitor=args.stage2.read_bytes()
    original=json.loads(args.policy.read_text());out=args.policy.parent
    cases=['actual-cold-GPU-bundle','four-extents','bound-generation','explicit-model-only']
    def refused(name, action, reason=None):
        try: action()
        except (ValueError,AssertionError) as error:
            assert reason is None or reason in str(error),(name,str(error))
            cases.append(name);return
        raise AssertionError('Expected refusal: '+name)
    def check(name, change, reason):
        policy=json.loads(json.dumps(original));change(policy)
        path=out/('gpu-bad-'+name+'.json');save(path,policy)
        bound=bytearray(raw);bound[-32:]=hashlib.sha256(path.read_bytes()).digest()
        refused(name,lambda:card.policy_check(path,bound,image,manifest,monitor,True),reason)
    check('missing-GPU',lambda p:p.pop('gpu'),'Cold NPU condition set differs')
    check('linked-GPU-configuration',lambda p:p['gpu']['configuration'].__setitem__('core_hz',1),'Cold GPU configuration differs')
    check('missing-reset-condition',lambda p:p['gpu']['conditions'].pop(card.GPU_RESET),'Cold GPU condition set differs')
    check('canonical-session',lambda p:p.__setitem__('canonical_session_sha256','00'*32),'Cold policy binding differs')
    check('monitor-identity',lambda p:p.__setitem__('stage2_sha256','00'*32),'Cold policy binding differs')
    def fact_change(name, condition, change, reason):
        def mutate(policy):
            ref=policy['gpu']['conditions'][condition]
            fact=json.loads(Path(ref['path']).read_text());change(fact)
            path=out/('gpu-fact-'+name+'.json');save(path,fact)
            policy['gpu']['conditions'][condition]={'path':str(path.resolve()),'sha256':card.sha(path.read_bytes())}
        check(name,mutate,reason)
    for field in ('after_uncertain_loss','before_stage2_reload','before_reserved_memory_reuse','firmware_and_bus_transactions_retired',
                  'pll_gpu0_disabled','gpu_gates_closed','gpu_bus_reset_asserted'):
        fact_change('reset-'+field,card.GPU_RESET,lambda f,k=field:f.__setitem__(k,False),'Cold GPU reset must retire')
    fact_change('reset-supply',card.GPU_RESET,lambda f:f.__setitem__('initial_supply_uv',900000),'Cold GPU reset must retire')
    for field,value in [('identity','another-board'),('sid_authority',1),('image_sha256','00'*32),('stage2_sha256','00'*32)]:
        fact_change('record-'+field,card.GPU_RESET,lambda f,k=field,v=value:f.__setitem__(k,v),'Cold GPU condition is not bound')
    fact_change('unreviewed',card.GPU_RESET,lambda f:f.__setitem__('reviewed',False),'Cold GPU condition is not bound')
    fact_change('model-claims-physical',card.GPU_RESET,lambda f:f.__setitem__('physical_authority',True),'explicitly nondeployable')
    fact_change('missing-source',card.GPU_RESET,lambda f:f.__setitem__('evidence',[]),'source evidence is missing')
    fact_change('source-hash',card.GPU_RESET,lambda f:f['evidence'][0].__setitem__('sha256','00'*32),'Cold condition evidence changed')
    fact_change('shared-clock-different-bytes',card.GPU_NPU_CLOCK,lambda f:f.__setitem__('note','different'),'must reference the same evidence')
    fact_change('combined-clock-rate',card.GPU_NPU_CLOCK,lambda f:f['gpu'].__setitem__('core_hz',1),'combined GPU/NPU clock authority differs')
    fact_change('combined-clock-scope',card.GPU_NPU_CLOCK,lambda f:f.__setitem__('scope','supervised'),'combined GPU/NPU clock authority differs')
    refused('model-is-not-board-qualification',lambda:card.policy_check(args.policy,raw,image,manifest,monitor,False),
            'explicit --model-fixture')
    return cases + gpu_owner_cases(args, raw, manifest, model)


def gpu_owner_cases(args, raw, manifest, model):
    original=json.loads(args.policy.read_text());out=args.policy.parent;cases=[]
    frozen={name:Path(ref['path']).read_bytes() for name,ref in original['evidence'].items()}
    for name,offset,fmt,value in [('cpu-owner',len(raw)-72,'Q',0x4f53475055464d31),
                                  ('npu-owner',832,'Q',0x4f534731),('npu-client',856,'I',7),
                                  ('thermal-owner',616,'Q',0x4f534731)]:
        bad=bytearray(raw);struct.pack_into('<'+fmt,bad,offset,value)
        try: card.gpu_policy(original,manifest['gpu_profile'],bad,out,dict(frozen))
        except ValueError as error: assert 'aliases' in str(error),(name,str(error))
        else: raise AssertionError('Expected refusal: '+name)
        cases.append(name)
    bound=bytearray((args.output/'slot/os-session.bin').read_bytes())
    struct.pack_into('<Q',bound,24,1);struct.pack_into('<Q',bound,len(bound)-64,1)
    path=out/'native-bound-session.bin';path.write_bytes(bound)
    subprocess.run([model,'--check-session',path],check=True)
    cases.append('actual-native-session-admission')
    return cases


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'base-model', 'overlay-model', 'wav', 'boot0', 'packer', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--stage2-build', type=Path, help='Reuse an accepted monitor build without rebuilding unchanged stage2')
    p.add_argument('--gpu-only', action='store_true', help='Only changed cold GPU authority and actual bundle cases')
    p.add_argument('--prepare-only', action='store_true', help='Bind a final integration image without replaying mutation cases')
    a = p.parse_args(); out = a.output; out.mkdir(parents=True, exist_ok=False)
    image = a.build / 'sunstone-a733-os.elf'
    manifest = json.loads((a.build / 'os-image/link-check.json').read_text())
    npu = manifest.get('npu', False)
    gpu = manifest.get('gpu_linear', False)
    stage2 = a.stage2_build or a.build
    _, build_id = card.flat_runtime(image.read_bytes())
    model = out / 'native-admission'
    command = [str(CLANG), '-std=c11', '-D_GNU_SOURCE', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Iinclude', '-Iplatform/a733-smp-cached', '-Iapps/os',
               '-DSUNSTONE_BOARD_SUPERVISED=1', '-DSUNSTONE_BOARD_USB2_DUAL=1', '-DSUNSTONE_BOARD_RESIDENT=1',
               '-DA733_A76_ADMITTED=1', '-DSUNSTONE_A733_OS_CPU_CLOCK=1', '-DA733_SMP_CONTRACT_MAGIC=0x415337334f524331ULL',
               '-DOS_MODEL_BUILD_ID=' + ','.join(str(v) for v in build_id), 'checks/os-image-model.c',
               'drivers/storage/storage_digest.c', 'drivers/clock/a733_cpu_clock.c', '-o', str(model)]
    if gpu:
        command += ['-DSUNSTONE_A733_OS_GPU_LINEAR=1',
                    '-DOS_RECOGNITION_CORE=' + str(manifest['inference']['owner_core'])]
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
    gpu_facts = gpu_records(out, card.sha(image.read_bytes()), card.sha((stage2/'a733-stage2.bin').read_bytes()),
                            manifest['gpu_profile'], npu) if gpu else None
    if npu:
        npu_records(out, raw, refs, card.sha(image.read_bytes()), gpu_facts)
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
              'image_sha256': card.sha(image.read_bytes()), 'stage2_sha256': card.sha((stage2/'a733-stage2.bin').read_bytes()),
              'inference': manifest['inference'], 'canonical_session_sha256': card.sha(card.canonical_session(raw)),
              'cpu': {'clusters': 3, 'max_mhz': maxima, 'changes_per_boot': 8}, 'evidence': refs}
    if npu: policy['npu'] = card.npu_summary(raw)
    if gpu: policy['gpu'] = gpu_facts
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
    args = SimpleNamespace(image=image, manifest=a.build/'os-image/link-check.json', stage2=stage2/'a733-stage2.bin',
                           stage2_elf=stage2/'a733-stage2.elf', session=out/'session.bin', policy=out/'policy.json',
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
    if a.gpu_only:
        assert gpu and npu and result['gpu']==manifest['gpu_profile']
        cases=gpu_cases(args,raw,manifest,model)
        save(out/'result.json',{'passed':True,'physical_access':False,'cases':cases,
             'workers':manifest['inference']['workers'],'bundle_sha256':card.sha((args.output/'bundle.json').read_bytes())})
        print('PASS cold GPU card producer:',len(cases),'focused cases');return
    if a.prepare_only:
        save(out/'result.json', {'passed':True, 'physical_access':False,
             'scope':'Actual producer and linked layout; mutation cases reused from component evidence.',
             'workers':manifest['inference']['workers'], 'bundle_sha256':card.sha((args.output/'bundle.json').read_bytes())})
        print('PASS final card producer:',manifest['inference']['workers'],'workers')
        return
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
