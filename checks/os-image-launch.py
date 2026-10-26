#!/usr/bin/env python3
"""Host check of the supervised integrated OS preparation and launcher.

It prepares a bundle for a linked OS image from synthetic reviewed records,
then checks that:

- the image's own admission (checks/os-image-model.c, compiled with the
  image's build identity) accepts the prepared session with the contract the
  SRAM launcher would complete, and the built launcher names the OS request,
  contract and session size;
- preparation refuses a missing or wrong reviewed record, changed evidence,
  a link check of another configuration and out-of-rule session fields;
- tools/check-a733-os-supervised.py freezes and revalidates the bundle, bounds
  the generations, and its read-only claim check accepts only the matching
  claim (through a modeled database connection);
- its --fault-report flag is refused before any launch step for an image
  without a733_fault_previous, and otherwise reads the report once through
  a modeled device before the capture, which a failed read never reaches;
- for a link check that records the NPU profile, it requests the NPU report
  (operation 6) once after a completed capture, saves it and records its
  decoding and classification; a report of another session is kept with
  its decoding error, a device that never answers ends the run, a failed
  capture, or one that leaves less than 100 ms of its window, sends no
  request, and an image without the profile never sends operation 6;
- in the CPU clock build (docs/designs/cpu-a76-dvfs.md), a grant that names
  a cluster is prepared only with an approval record bound to one board, the
  image, the session and one step, which cites that board's B1 record, and
  every changed binding is refused;
- in the performance-mode build (docs/designs/performance-modes.md), only a
  grant of both clusters at the bin's high points is prepared, bound to a
  performance-mode approval, and a single-cluster grant is refused;
- --perf-sequence is refused for an image without the performance-mode
  service and for a count other than the generations, and otherwise makes
  one switch before each recognition and one readback after the capture.

Every physical fact is a labeled synthetic record. No device, USB, queue
database or FEL program is opened; the launcher's transport is never called.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import shlex
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HZ = 24000000


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


tool = module('os_launch_tool', ROOT / 'tools/check-a733-os-supervised.py')
prep = tool.preparation


def refused(action):
    try:
        action()
    except ValueError:
        return True
    raise AssertionError('refusal expected')


def clock_cases(raw, record, put, target, identities, prepare, refused, out, cases, gpu=False, top_only=False):
    """A2 on a modeled board 3-3: CPU-B to 2,002 MHz (fuse word 0x00000496,
    column vf0400) under an approval bound to this board, image and session,
    which cites that board's read-only B1 record. Each refusal changes one
    binding."""
    tail = len(raw) - prep.GRANT_BYTES
    assert struct.unpack_from('<4I', raw, tail + 48) == (0, 0, 0, 0), 'the nominal grant names nothing'
    b1 = {**target, 'schema': prep.B1_SCHEMA, 'passed': True, 'grant_clusters': 0, 'sid_word': 0x496,
          'dcdc3_mv': 900, 'dcdc5_mv': 850,
          'pattern_lfm_clear': True, 'warning': 'Synthetic fixture only. Never physical authority.'}
    approval = {**target, **identities, 'schema': prep.APPROVAL_SCHEMA, 'step': 'A2', 'cluster': 'cpu-b',
                'highest_mhz': 2002, 'changes': 1, 'approval_text': 'MODEL: no user approval exists',
                'approval_date': '2026-10-24', 'b1': put('cpu-clock-b1', b1)}

    def granted(name, facts=approval, maxima=(0, 2002), clusters=2, changes=1, with_record=True):
        ref = put('cpu-clock-approval-' + name, facts)
        value = bytearray(raw)
        struct.pack_into('<2Q', value, tail + 32, identities['epoch'], identities['generation'])
        struct.pack_into('<4I', value, tail + 48, clusters, *maxima, changes)
        value[tail + 64:tail + 96] = bytes.fromhex(ref['sha256'])
        path = out / ('MODEL-session-clock-' + name + '.bin')
        path.write_bytes(value)
        entry = dict(record, session_template_sha256=hashlib.sha256(value).hexdigest())
        if with_record:
            entry['cpu_clock_approval'] = ref
        entry_path = out / ('MODEL-admission-clock-' + name + '.json')
        entry_path.write_text(json.dumps(entry))
        return {'session': path, 'entry': entry_path}
    if not gpu:
        bundle = prepare('MODEL-NOT-DEPLOYABLE-clock-a2', **granted('a2'))
        session = (out / 'MODEL-NOT-DEPLOYABLE-clock-a2/board/session.bin').read_bytes()
        assert bundle['audio'] is prep.session_layout(session)[0]
        assert prep.grant_fields(session)['max_mhz'] == (0, 2002)
        cases['prepared-a2-grant-bound-to-board-image-session-and-b1'] = True
    other_b1 = put('cpu-clock-b1-envelope', dict(b1, sid_word=0))
    failed_b1 = put('cpu-clock-b1-failed', dict(b1, passed=False))
    granted_b1 = put('cpu-clock-b1-granted', dict(b1, grant_clusters=2))
    refusals = {
        'approval-for-another-port': granted('port', dict(approval, port='OTHER-PORT')),
        'approval-for-another-session': granted('lease', dict(approval, lease=identities['lease'] + 1)),
        'b1-envelope-bin-tops-at-1508': granted('envelope', dict(approval, b1=other_b1)),
        'b1-run-not-passed': granted('b1-failed', dict(approval, b1=failed_b1)),
        'b1-with-a-grant': granted('b1-granted', dict(approval, b1=granted_b1)),
        'grant-above-approval': granted('above', maxima=(0, 1794)),
        'a1-at-2002': granted('a1', dict(approval, step='A1')),
        'grant-without-approval-record': granted('missing', with_record=False),
        'two-changes-for-one': granted('changes', changes=2),
        'little-cluster-for-cpu-b-step': granted('cluster', maxima=(2002, 0), clusters=1),
    }
    # TOP, the default path of docs/designs/cpu-top-opp.md: both clusters at
    # the top rows of vf0400, CPU-L 1794 and CPU-B 2002 MHz.
    top = dict(approval, step='TOP', cluster='both', highest_mhz={'cpu-l': 1794, 'cpu-b': 2002}, changes=4)
    prepare('MODEL-NOT-DEPLOYABLE-clock-top', **granted('top', top, maxima=(1794, 2002), clusters=3, changes=4))
    session = (out / 'MODEL-NOT-DEPLOYABLE-clock-top/board/session.bin').read_bytes()
    assert prep.grant_fields(session)['max_mhz'] == (1794, 2002) and prep.grant_fields(session)['clusters'] == 3
    cases['prepared-top-grant-both-clusters-at-the-bin-tops'] = True
    if top_only:
        return out / 'MODEL-NOT-DEPLOYABLE-clock-top'
    refusals.update({
        'top-with-one-cluster': granted('top-one', top, maxima=(0, 2002), clusters=2, changes=4),
        'top-below-the-bin-tops': granted('top-low', dict(top, highest_mhz={'cpu-l': 1404, 'cpu-b': 2002}),
                                          maxima=(1404, 2002), clusters=3, changes=4),
        'both-clusters-for-a2': granted('a2-both', maxima=(1794, 2002), clusters=3),
    })
    nothing = dict(record, cpu_clock_approval=approval['b1'])
    (out / 'MODEL-admission-clock-nothing.json').write_text(json.dumps(nothing))
    if gpu:
        refusals = {key: refusals[key] for key in ('approval-for-another-port', 'grant-without-approval-record', 'top-with-one-cluster')}
    for index, (name, files) in enumerate(refusals.items()):
        assert refused((lambda f, n: lambda: prepare(n, **f))(files, 'c%d' % index)), name
        cases['refuses-clock-' + name] = True
    assert refused(lambda: prepare('cn', entry=out / 'MODEL-admission-clock-nothing.json'))
    cases['refuses-clock-approval-without-grant'] = True
    assert not any((out / ('c%d' % i)).exists() for i in range(len(refusals))) and not (out / 'cn').exists()
    return out / 'MODEL-NOT-DEPLOYABLE-clock-top'


def perf_clock_cases(raw, record, put, target, identities, prepare, refused, out, cases):
    """The performance-mode grant on a modeled board 3-3 (fuse word 0x496,
    column vf0400): both clusters up to 1794 and 2002 MHz under one approval
    of the mode service, which cites the board's B1 record."""
    tail = len(raw) - prep.GRANT_BYTES
    b1 = {**target, 'schema': prep.B1_SCHEMA, 'passed': True, 'grant_clusters': 0, 'sid_word': 0x496,
          'dcdc3_mv': 800, 'dcdc5_mv': 800, 'pattern_lfm_clear': True,
          'warning': 'Synthetic fixture only. Never physical authority.'}
    approval = {**target, **identities, 'schema': prep.PERF_APPROVAL_SCHEMA,
                'high_mhz': {'cpu-l': 1794, 'cpu-b': 2002}, 'changes': 64,
                'approval_text': 'MODEL: no user approval exists', 'approval_date': '2026-10-25',
                'b1': put('perf-b1', b1)}

    def granted(name, facts=approval, maxima=(1794, 2002), clusters=3, changes=64):
        ref = put('perf-approval-' + name, facts)
        value = bytearray(raw)
        struct.pack_into('<2Q', value, tail + 32, identities['epoch'], identities['generation'])
        struct.pack_into('<4I', value, tail + 48, clusters, *maxima, changes)
        value[tail + 64:tail + 96] = bytes.fromhex(ref['sha256'])
        path = out / ('MODEL-session-perf-' + name + '.bin')
        path.write_bytes(value)
        entry = dict(record, session_template_sha256=hashlib.sha256(value).hexdigest(), cpu_clock_approval=ref)
        entry_path = out / ('MODEL-admission-perf-' + name + '.json')
        entry_path.write_text(json.dumps(entry))
        return {'session': path, 'entry': entry_path}
    prepare('MODEL-NOT-DEPLOYABLE-perf', **granted('both'))
    session = (out / 'MODEL-NOT-DEPLOYABLE-perf/board/session.bin').read_bytes()
    assert prep.grant_fields(session)['max_mhz'] == (1794, 2002) and prep.grant_fields(session)['clusters'] == 3
    cases['prepared-performance-mode-grant-of-both-clusters'] = True
    refusals = {
        'single-cluster-grant': granted('single', maxima=(0, 2002), clusters=2),
        'high-points-of-another-bin': granted('points', dict(approval, high_mhz={'cpu-l': 1794, 'cpu-b': 1976})),
        'grant-below-the-high-points': granted('below', maxima=(1794, 1950)),
        'approval-for-another-session': granted('lease', dict(approval, lease=identities['lease'] + 1)),
        'cpu-clock-step-approval': granted('step', dict(approval, schema=prep.APPROVAL_SCHEMA)),
        'changes-differ': granted('changes', changes=63),
    }
    for index, (name, files) in enumerate(refusals.items()):
        assert refused((lambda f, n: lambda: prepare(n, **f))(files, 'p%d' % index)), name
        cases['refuses-perf-' + name] = True
    assert not any((out / ('p%d' % i)).exists() for i in range(len(refusals)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'base', 'overlay', 'wav', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--staged', action='store_true', help='explicit USB0-only recorded workload 2')
    parser.add_argument('--gpu-performance-only', action='store_true', help='only GPU performance capability and frozen entry admission')
    parser.add_argument('--accelerators-only', action='store_true', help='only combined GPU/NPU session, authority and asset admission')
    parser.add_argument('--gpu-workers-only', action='store_true', help='only eight-worker GPU capability and combined admission')
    parser.add_argument('--gpu-cpu-only', action='store_true', help='only combined GPU and CPU-grant admission')
    parser.add_argument('--gpu-only', action='store_true', help='only supervised GPU artifact, entry and frozen-launch admission')
    parser.add_argument('--clang', type=Path, default=Path('/usr/bin/clang-21'))
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/bin/ld.lld-21')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = args.build / 'sunstone-a733-os.elf'
    link_check = args.build / 'os-image/link-check.json'
    manifest = json.loads(link_check.read_text())
    audio = manifest['audio']
    assert sum((args.gpu_only, args.gpu_cpu_only, args.accelerators_only, args.gpu_workers_only, args.gpu_performance_only)) <= 1
    npu = manifest.get('npu', False)
    if args.accelerators_only:
        assert npu and manifest.get('gpu_linear') and not audio
    if args.gpu_only or args.gpu_cpu_only or args.gpu_workers_only or args.gpu_performance_only:
        assert manifest.get('gpu_linear') and not audio
    # The CPU clock build appends the driver's 96-byte grant record.
    cpu_clock = 'SUNSTONE_A733_OS_CPU_CLOCK:BOOL=ON' in (args.build / 'CMakeCache.txt').read_text()
    if args.gpu_cpu_only:
        assert cpu_clock and not manifest.get('perf_modes')
    if args.gpu_workers_only:
        assert cpu_clock and manifest['inference']['workers'] == 8
    image_hash = hashlib.sha256(image.read_bytes()).hexdigest()
    cases = {}

    # The image's admission, compiled on the host with the image's identity.
    build_id = bytes.fromhex(manifest['build_id'])
    model = out / 'os-image-model'
    command = [str(args.clang), '-std=c11', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-Iinclude', '-Iplatform/a733-smp-cached', '-Iapps/os', '-DSUNSTONE_BOARD_SUPERVISED=1',
               '-DSUNSTONE_BOARD_USB2_DUAL=1', '-DA733_SMP_CONTRACT_MAGIC=0x415337334f534331ULL',
               '-DOS_MODEL_BUILD_ID=' + ','.join(str(b) for b in build_id),
               *(['-DSUNSTONE_A733_OS_AUDIO=1'] if audio else []),
               *(['-DSUNSTONE_A733_OS_PERF=1'] if manifest.get('perf_modes') else []),
               *(['-DA733_A76_ADMITTED=1', '-DSUNSTONE_A733_OS_CPU_CLOCK=1', '-ffunction-sections',
                  '-Wl,--gc-sections', 'drivers/clock/a733_cpu_clock.c'] if cpu_clock else []),
               'checks/os-image-model.c', 'drivers/storage/storage_digest.c', '-o', str(model)]
    if npu:
        commands = json.loads((args.build/'compile_commands.json').read_text())
        native = next(row for row in commands if row['file'] == str(ROOT/'apps/os/admission.c'))
        includes = [arg for arg in shlex.split(native['command']) if arg.startswith('-I') and
                    ('FreeRTOS-Kernel' in arg or 'freertos-a733' in arg or arg.endswith('/apps/board'))]
        command += ['-DSUNSTONE_A733_OS_NPU=1', *includes, str(args.build/'os-image/npu-models.c')]
        if manifest.get('gpu_linear'):
            command += ['-DSUNSTONE_A733_OS_GPU_LINEAR=1', '-DOS_RECOGNITION_CORE=6']
    (out/'native-command.json').write_text(json.dumps(command,indent=2)+'\n')
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(model), '--write-staged-session' if args.staged else '--write-session',
                    str(out / 'model-session.bin'), '9'], check=True)
    raw = bytearray((out / 'model-session.bin').read_bytes())
    assert len(raw) == (992 if audio else 1080 if npu else 808) + (prep.GRANT_BYTES if cpu_clock else 0)
    if args.staged:
        assert not audio and struct.unpack_from('<I', raw, 400)[0] == 2
        assert struct.unpack_from('<I', raw, 532)[0] == 1

    # Synthetic reviewed records, labeled as such, bound to this image.
    facts = out / 'MODEL-NOT-PHYSICAL'
    facts.mkdir()

    def put(name, value):
        data = json.dumps(value, sort_keys=True).encode()
        path = (facts / (name + '.json')).resolve()
        path.write_bytes(data)
        return {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest()}
    target = {'identity': 'OS-SOFTWARE-MODEL', 'port': 'NO-PHYSICAL-PORT', 'image_sha256': image_hash}
    marker = put('MODEL', {'physical_access': False, 'warning': 'Synthetic fixture only. Never physical authority.'})
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    identities = {'epoch': epoch, 'generation': generation, 'lease': lease}
    thermal_facts = {
        'initial': ['exclusive_ths_and_irq71', 'remote_writers_excluded', 'no_inherited_thermal_service',
                    'security_access_authorized', 'gic_route_owned', 'boot_lineage_reviewed'],
        'electrical': ['avcc_adc_digital_supplies_stable', 'sid_shadows_readable', 'device_mappings_verified',
                       'no_physical_aliases'],
        'clocks': ['stable_apb1_ref_osc', 'no_clock_reset_writers',
                   'gpadc_consumers_excluded' if raw[768] else 'stable_enabled_gpadc24m'],
        'supervision': ['whole_board_power_removal', 'permanent_retention', 'no_reuse', 'one_attempt']}
    record = {**target, **identities, 'schema': prep.ENTRY_SCHEMA, 'reservation': 100,
              'whole_board_power_removal': True, 'no_resource_reuse_before_power_removal': True,
              'recovery': {'mode': 'manual-usb-reconnect', 'demonstrated_fel': True,
                           'explicit_reconnect_before_resumption': True, 'preserve_microsd': True,
                           'one_attempt': True, 'evidence': [marker]}}
    if manifest.get('inference', {}).get('workers') == 8:
        record['inference'] = manifest['inference']
    if manifest.get('gpu_linear'):
        record['gpu'] = {'configuration': manifest['gpu_profile'],
                         'conditions': {key: marker for key in sorted(prep.GPU_CONDITIONS |
                             ({prep.GPU_PERF_CONDITION} if manifest.get('perf_modes') else set()))}}
    for key, offset in zip(thermal_facts, (632, 664, 696, 728)):
        ref = put('thermal-' + key, {**target, 'schema': 'a733-thermal-' + key + '-v1', 'reviewed': True,
                                     **dict.fromkeys(thermal_facts[key], True)})
        raw[offset:offset + 32] = bytes.fromhex(ref['sha256'])
        record[key] = ref
    conditions = ('source-backed-cpu-dsu-transition', 'cold-cache-and-dirty-alias-exclusion', 'dual-pas-dram',
                  'exclusive-cpus-memory-dma', 'remote-writers-excluded', 'usb-clock-power-reset-ownership',
                  'usb-device-mapping', 'physical-counter-continuity')
    record['platform'] = put('platform', {**target, 'schema': 'a733-smp-observer-platform-v1',
                                          'entry_conditions_established': True,
                                          'physical_coherence_result': 'under-test',
                                          'conditions': {k: marker for k in conditions}})
    ddr = {**target, 'schema': 'a733-ddr-reservation-v1', 'exclusive': True, 'published_cache_disabled': True,
           'remote_writers_excluded': True, 'evidence': [marker]}
    record['ddr'] = put('ddr', {**ddr, 'ranges': [[0x41000000, 0x42200000]]})
    record['mimus_ddr'] = put('mimus-ddr', {**ddr, 'ranges': prep.mimus.RANGES})
    if npu:
        rules = prep.module('combined_fixture_npu_rules', ROOT/'apps/npu-board/prepare.py')
        # Synthetic records exercise the parser's asserted fields. Their marker
        # identifies modeled evidence; these bundles are never deployable.
        record.update(physical_authority=True, modeled=False, warning='MODEL ONLY; no physical authority exists')
        refs = {}
        for name, offset in (('ddr',1048),('admission',920),('clock',952),('power',984),('supervision',1016)):
            fact = {**target, **identities, 'physical_authority':True, 'modeled':False, 'reviewed':True,
                    'warning':'MODEL ONLY; no physical authority exists', 'schema':'a733-npu-board-'+name+'-v1'}
            if name == 'ddr': fact.update(schema='a733-ddr-reservation-v1',ranges=[prep.NPU_RANGE],
                exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,evidence=[marker])
            else: fact['conditions']={key:marker for key in sorted(rules.CONDITIONS[name])}
            if name == 'admission': fact['ddr_sha256']=refs['ddr']['sha256']
            if name == 'power': fact['supply_uv']=800000
            if name == 'supervision': fact.update(supervisor=struct.unpack_from('<Q',raw,848)[0],one_attempt=True)
            refs[name]=put('npu-'+name,fact);raw[offset:offset+32]=bytes.fromhex(refs[name]['sha256'])
        record['npu']=refs
        if manifest.get('gpu_linear'):
            concurrent={**target,**identities,'schema':prep.GPU_NPU_SCHEMA,'scope':'simultaneous-powered-lifetime',
                'physical_authority':True,'modeled':False,'reviewed':True,'warning':'MODEL ONLY',
                'gpu':{'core_hz':manifest['gpu_profile']['core_hz'],'supply_uv':manifest['gpu_profile']['supply_uv']},
                'npu':{'mhz':852,'supply_uv':800000},'npu_pll_enable_with_gpu_active':True,
                'gpu_pll_start_stop_with_npu_active':True,'cpu_ddr_clocks_remain_admitted':True,'evidence':[marker]}
            record['gpu']['conditions'][prep.GPU_NPU_CONDITION]=put('combined-authority',concurrent)
    if audio:
        dma_sha256 = hashlib.sha256(raw[prep.AUDIO_DMA:prep.AUDIO_DMA + prep.AUDIO_DMA_BYTES]).hexdigest()
        audio_cold, samples = struct.unpack_from('<2I', raw, prep.AUDIO_TAIL)
        audio_facts = {**target, **identities, 'schema': prep.AUDIO_SCHEMA, 'audio_cold': audio_cold,
                       'capture_samples': samples, 'dma_sha256': dma_sha256,
                       'inherited_sha256': hashlib.sha256(raw[976:985]).hexdigest(),
                       'conditions': {k: marker for k in sorted(prep.AUDIO_CONDITIONS)}, 'one_attempt': True,
                       'whole_board_power_removal': True, 'no_resource_reuse_before_power_removal': True}
        record['audio'] = put('audio', audio_facts)
        raw[prep.AUDIO_EVIDENCE:prep.AUDIO_EVIDENCE + 32] = bytes.fromhex(record['audio']['sha256'])
        record['dma_nonsecure_access'] = put('dma-nonsecure', {
            **target, **identities, 'schema': 'a733-dma-nonsecure-access-v1', 'physical_authority': True,
            'modeled': False, 'reviewed': True, 'controller': 0x04601000, 'service_channels': 1,
            'nonsecure_channels': 1, 'dma_evidence': lease, 'dma_handoff': lease, 'configuration_sha256': dma_sha256,
            **dict.fromkeys(prep.security_authority.FACTS, True), 'evidence': [marker]})
    template = out / 'MODEL-session-template.bin'
    template.write_bytes(raw)
    record['session_template_sha256'] = hashlib.sha256(raw).hexdigest()
    admission = out / 'MODEL-admission.json'
    admission.write_text(json.dumps(record, indent=2) + '\n')
    assets = [args.base, args.overlay, args.wav]

    def prepare(name, session=template, entry=admission, check=link_check):
        return prep.prepare(image, check, entry, session, assets, out / name, str(args.clang), str(args.lld))
    bundle = out / 'MODEL-NOT-DEPLOYABLE-bundle'
    value = prepare(bundle.name)
    assert value['audio'] is audio
    inner = prep.validate_board(bundle / 'board')
    session = (bundle / 'board/session.bin').read_bytes()
    assert session[:48] == raw[:48] and session[48:80] == build_id and session[80:] == raw[80:]
    base_text = (bundle / 'board/board-launch-base.c').read_text()
    assert ('CONTRACT->reserved[1]==%du' % len(raw)) in base_text and '0x415337334f534c31' in base_text
    cases['prepared-bundle-and-launcher-for-session-size'] = True

    # The contract as the SRAM launcher completes it, judged by the image's admission.
    contract = bytearray((bundle / 'board/contract.bin').read_bytes()[:224])
    armed = 100 * HZ
    struct.pack_into('<Q', contract, 0, prep.CONTRACT_MAGIC)
    struct.pack_into('<4Q', contract, 80, armed, armed + HZ, armed + 60 * HZ, armed + 60 * HZ)
    (out / 'launched-contract.bin').write_bytes(contract)

    def admit(contract_path, now, session_path=bundle / 'board/session.bin'):
        run = subprocess.run([str(model), '--admit', str(contract_path), str(session_path), str(now)],
                             capture_output=True, text=True)
        return run.returncode == 0 and run.stdout.strip() == 'admitted'
    assert admit(out / 'launched-contract.bin', armed + HZ // 10)
    assert not admit(out / 'launched-contract.bin', armed + HZ)
    changed = bytearray(contract)
    struct.pack_into('<Q', changed, 184, 992 if len(raw) == 808 else 808)
    (out / 'changed-size-contract.bin').write_bytes(changed)
    assert not admit(out / 'changed-size-contract.bin', armed + HZ // 10)
    cases['image-admission-accepts-launched-contract'] = True
    if args.accelerators_only:
        from harness import refused as reject
        frozen = tool.freeze_bundle(bundle, 1)
        tool.entry_check(frozen, target['identity'], target['port'])
        cases['complete-combined-frozen-entry'] = True
        if cpu_clock:
            top = clock_cases(raw, record, put, target, identities, prepare, refused, out, cases, gpu=True, top_only=True)
            tool.entry_check(tool.freeze_bundle(top, 1), target['identity'], target['port'])
            top_session = top / 'board/session.bin'
            bound = bytearray(contract); bound[192:224] = hashlib.sha256(top_session.read_bytes()).digest()
            bound_path = out / 'MODEL-contract-top.bin'; bound_path.write_bytes(bound)
            assert admit(bound_path, armed + HZ // 10, top_session)
            cases['complete-combined-top-frozen-and-native-entry'] = True
        for name,offset,fmt,value in (('firmware-owner',832,'Q',0x4f53475055464d31),
                ('session-owner',832,'Q',0x4f534731),('pck-client',856,'I',7)):
            changed=bytearray(raw);struct.pack_into('<'+fmt,changed,offset,value)
            path=out/('MODEL-bad-'+name+'.bin');path.write_bytes(changed)
            entry=dict(record,session_template_sha256=hashlib.sha256(changed).hexdigest())
            entry_path=out/('MODEL-bad-'+name+'.json');entry_path.write_text(json.dumps(entry))
            reject(lambda:prepare('refused-'+name,session=path,entry=entry_path),'NPU owner or PCK client aliases the GPU.')
            bound=bytearray(contract);bound[192:224]=hashlib.sha256(changed).digest()
            bound_path=out/('MODEL-contract-'+name+'.bin');bound_path.write_bytes(bound)
            assert not admit(bound_path,armed+HZ//10,path)
            cases['host-and-native-'+name]=True
        for name,field,value in (('single-device-schema','schema','a733-npu-board-clock-v1'),
                ('other-scope','scope','single-device'),('other-generation','generation',generation+1),
                ('other-board','identity','another-board'),('modeled','modeled',True),
                ('wrong-point','npu',{'mhz':492,'supply_uv':800000}),
                ('missing-gpu-transition','gpu_pll_start_stop_with_npu_active',False)):
            fact={**concurrent,field:value};entry=copy.deepcopy(record)
            entry['gpu']['conditions'][prep.GPU_NPU_CONDITION]=put('bad-'+name,fact)
            path=out/('MODEL-bad-'+name+'.json');path.write_text(json.dumps(entry))
            reason='Combined GPU/NPU clock authority is incomplete.' if name in ('wrong-point','missing-gpu-transition') else 'Combined GPU/NPU authority names another scope or lifetime.'
            reject(lambda:prepare('refused-'+name,entry=path),reason);cases['refuses-'+name]=True
        elf=prep.os_link.Elf(image.read_bytes())
        table=next(s for s in elf.symbols if s['name']=='os_npu_inputs' and s['shndx']!=0)
        pointer,size=struct.unpack('<2Q',elf.read(table['value'],16))
        section=next(s for s in elf.sections if s[1]!=8 and s[3]<=pointer<s[3]+s[5])
        changed=bytearray(image.read_bytes());changed[section[4]+pointer-section[3]]^=1
        reject(lambda:prep.linked_npu_assets(prep.os_link.Elf(bytes(changed))),'NPU embedded asset differs')
        cases['actual-linked-npu-input-bytes']=True
        (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=image_hash,
            cases=cases,limits='Synthetic physical assertions exercise actual producers/validators; no board authority or transport.'),indent=2)+'\n')
        print('PASS combined GPU/NPU preparation:',len(cases),'focused cases');return
    if args.gpu_performance_only:
        from harness import refused as reject
        assert manifest['perf_modes'] and manifest['gpu_profile']['performance']['boot_mode']=='low'
        perf_clock_cases(raw, record, put, target, identities, prepare, refused, out, cases)
        frozen = tool.freeze_bundle(out / 'MODEL-NOT-DEPLOYABLE-perf', 1)
        tool.entry_check(frozen, target['identity'], target['port'])
        cases['frozen-performance-grant'] = True
        for label, change in (
                ('hidden-service', lambda v: v.update(perf_modes=False)),
                ('boot-mode', lambda v: v.update(perf_boot_mode='high')),
                ('points', lambda v: v['gpu_profile']['performance'].update(low_uv=960000))):
            value = copy.deepcopy(manifest);change(value)
            reject(lambda: prep.image_manifest_check(value, image.read_bytes()), 'GPU')
            cases['refuse-manifest-' + label] = True
        for label, change in (
                ('missing-runtime-authority', lambda v: v['gpu']['conditions'].pop(prep.GPU_PERF_CONDITION)),
                ('fixed-rate-entry', lambda v: v['gpu']['configuration'].pop('performance'))):
            value = copy.deepcopy(record);change(value)
            entry = out / ('MODEL-' + label + '.json');entry.write_text(json.dumps(value))
            reject(lambda: prepare('refuse-' + label, entry=entry), 'GPU')
            prepared = tool.freeze_bundle(bundle, 1)
            altered = dict(prepared['_combined_frozen']);altered['board/admission.json']=json.dumps(value).encode()
            reject(lambda: tool.entry_check(dict(prepared, _combined_frozen=altered), target['identity'], target['port']), 'GPU')
            cases['fresh-and-frozen-refuse-' + label] = True
        altered = image.read_bytes().replace(b'os_gpu_performance_configuration\0', b'os_gpu_performance_configuratioX\0')
        reject(lambda: prep.image_gpu(manifest, altered), 'incomplete')
        cases['missing-performance-record-refused'] = True
        altered = altered.replace(b'os_perf_boot_mode\0', b'os_perf_boot_modX\0')
        reject(lambda: prep.image_gpu(manifest, altered), 'incomplete')
        cases['missing-both-records-cannot-downgrade'] = True
        (out / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False,
            image_sha256=image_hash, build_id=manifest['build_id'], cases=cases,
            limits='Synthetic exact-image entry facts; no physical performance authority.'), indent=2) + '\n')
        print('PASS GPU performance admission:', len(cases), 'focused outcomes')
        return
    if args.gpu_workers_only:
        from harness import refused as reject
        top = clock_cases(raw, record, put, target, identities, prepare, refused, out, cases, gpu=True, top_only=True)
        for label, selected in (('read-only', bundle), ('top', top)):
            frozen = tool.freeze_bundle(selected, 1)
            tool.entry_check(frozen, target['identity'], target['port'])
            cases['frozen-eight-gpu-' + label] = True
        prepared = tool.freeze_bundle(bundle, 1)
        for label, change in (
                ('missing-inference', lambda v: v.pop('inference')),
                ('pair-inference', lambda v: v['inference'].update(workers=2)),
                ('service-priority', lambda v: v['inference'].update(a55_priority=3)),
                ('owner-core', lambda v: v['inference'].update(owner_core=2))):
            value = copy.deepcopy(record);change(value)
            entry = out / ('MODEL-' + label + '.json');entry.write_text(json.dumps(value))
            reject(lambda: prepare('refuse-' + label, entry=entry), 'exact eight-worker placement')
            frozen = dict(prepared['_combined_frozen'])
            frozen['board/admission.json'] = json.dumps(value).encode()
            reject(lambda: tool.entry_check(dict(prepared, _combined_frozen=frozen), target['identity'], target['port']),
                   'exact eight-worker placement')
            cases['fresh-and-frozen-refuse-' + label] = True
        for label, field, value in (('count', 'workers', 2), ('mask', 'core_mask', 192), ('priority', 'a55_priority', 3)):
            altered = copy.deepcopy(manifest);altered['inference'][field] = value
            reject(lambda: prep.image_manifest_check(altered, image.read_bytes()), 'differs from the linked adapter')
            cases['refuse-manifest-' + label] = True
        altered = image.read_bytes().replace(b'mimus_parallel_configuration\0', b'mimus_parallel_configuratioX\0')
        reject(lambda: prep.image_manifest_check(manifest, altered), 'Legacy image does not contain')
        cases['missing-native-record-cannot-downgrade'] = True
        (out / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False,
            image_sha256=image_hash, build_id=manifest['build_id'], cases=cases,
            limits='Synthetic exact-image GPU/CPU entry facts; no transport or physical authority.'), indent=2) + '\n')
        print('PASS eight-worker GPU admission:', len(cases), 'focused outcomes')
        return
    if args.gpu_cpu_only:
        # Reuse accepted GPU tampering checks; select only the new CPU-grant
        # composition, with actual preparation and frozen-entry admission.
        top_bundle = clock_cases(raw, record, put, target, identities, prepare, refused, out, cases, gpu=True)
        for label, selected in (('read-only', bundle), ('top', top_bundle)):
            frozen = tool.freeze_bundle(selected, 1)
            tool.entry_check(frozen, target['identity'], target['port'])
            cases['frozen-gpu-cpu-' + label] = True
        tail = len(raw) - prep.GRANT_BYTES
        for label, owner in (('firmware', 0x4f53475055464d31), ('session', 0x4f534731)):
            value = bytearray(raw)
            struct.pack_into('<Q', value, tail + 24, owner)
            path = out / ('MODEL-owner-' + label + '.bin');path.write_bytes(value)
            entry = dict(record, session_template_sha256=hashlib.sha256(value).hexdigest())
            entry_path = out / ('MODEL-owner-' + label + '.json');entry_path.write_text(json.dumps(entry))
            assert refused(lambda: prepare('refused-owner-' + label, session=path, entry=entry_path))
            cases['refuses-gpu-' + label + '-cpu-owner'] = True
        for label, change in (('cpu-feature-off', {'cpu_clock': False}),
                              ('cpu-feature-integer', {'cpu_clock': 1}),
                              ('cpu-grant-size', {'contract_profile': {'lifetime': 'supervised', 'session_bytes': 808}})):
            altered = dict(manifest, **change)
            assert refused(lambda: prep.image_manifest_check(altered, image.read_bytes()))
            cases['refuses-' + label] = True
        (out / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False,
            image_sha256=image_hash, build_id=manifest['build_id'], cases=cases,
            limits='Synthetic grant/GPU entry facts; actual preparation, native contract admission and frozen validation, without transport.'), indent=2) + '\n')
        print('PASS combined GPU/CPU preparation:', len(cases), 'focused cases')
        return
    if args.gpu_only:
        from harness import refused as reject
        prepared = tool.freeze_bundle(bundle, 1)
        tool.entry_check(prepared, target['identity'], target['port'])
        cases['frozen-launch-entry'] = True
        # Frozen admission must use the retained bytes even if every original
        # evidence path is unavailable. Preserve all fixture files on disk.
        original_read = Path.read_bytes
        try:
            def no_read(path):
                raise AssertionError('Frozen entry reopened a path: ' + str(path))
            Path.read_bytes = no_read
            tool.entry_check(prepared, target['identity'], target['port'])
        finally:
            Path.read_bytes = original_read
        cases['frozen-entry-needs-no-original-files'] = True

        def entry_case(name, change, reason):
            altered = copy.deepcopy(record)
            change(altered)
            frozen = dict(prepared['_combined_frozen'])
            frozen['board/admission.json'] = json.dumps(altered).encode()
            reject(lambda: tool.entry_check(dict(prepared, _combined_frozen=frozen),
                                             target['identity'], target['port']), reason)
            cases[name] = True

        entry_case('missing-gpu-entry', lambda value: value.pop('gpu'), 'Reviewed GPU entry')
        entry_case('changed-entry-deadline', lambda value: value['gpu']['configuration'].update(reply_us=1), 'Reviewed GPU entry')
        entry_case('missing-gpu-condition', lambda value: value['gpu']['conditions'].pop(next(iter(prep.GPU_CONDITIONS))),
                   'Incomplete GPU physical entry conditions')
        entry_case('changed-gpu-evidence', lambda value: value['gpu']['conditions'][next(iter(prep.GPU_CONDITIONS))].update(sha256='0' * 64),
                   'Evidence changed')
        entry_case('no-power-removal', lambda value: value.update(whole_board_power_removal=False), 'complete-power-removal')
        source = image.read_bytes()
        altered_manifest = copy.deepcopy(manifest)
        altered_manifest['gpu_profile']['core'] = 2
        reject(lambda: prep.image_manifest_check(altered_manifest, source), 'GPU manifest differs')
        cases['changed-manifest-placement'] = True
        missing = source.replace(b'os_gpu_configuration\0', b'xs_gpu_configuration\0')
        assert missing != source
        reject(lambda: prep.image_manifest_check(manifest, missing), 'lacks the bounded caller configuration')
        downgraded = dict(manifest, gpu_linear=False, gpu_profile=None)
        reject(lambda: prep.image_manifest_check(downgraded, missing), 'lacks the bounded caller configuration')
        cases['missing-capability-and-downgrade-refused'] = True
        elf = prep.os_link.Elf(source)

        def changed_symbol(name, offset):
            symbol = next(s for s in elf.symbols if s['name'] == name and s['shndx'] != 0)
            section = elf.sections[symbol['shndx']]
            index = section[4] + symbol['value'] - section[3] + offset
            data = bytearray(source)
            data[index] ^= 1
            return bytes(data)

        for name, offset, reason in (('gpu_firmware', 0, 'GPU firmware differs'),
                                    ('os_placement_table', 0, 'GPU manifest differs'),
                                    ('a733_irq_routes', 16 + 2, 'GPU interrupt route differs'),
                                    ('os_applications', 2 * 32 + 24, 'GPU start entry differs')):
            changed = changed_symbol(name, offset)
            reject(lambda: prep.image_manifest_check(manifest, changed), reason)
            cases['changed-' + name] = True
        profile = next(s for s in elf.symbols if s['name'] == 'os_contract_profile' and s['shndx'] != 0)
        section = elf.sections[profile['shndx']]
        at = section[4] + profile['value'] - section[3]
        resident = bytearray(source)
        struct.pack_into('<Q', resident, at, 0x415337334f524331)
        reject(lambda: prep.image_manifest_check(manifest, resident), 'actual linked supervised contract')
        cases['resident-elf-cannot-claim-supervised'] = True
        missing_contract = source.replace(b'os_contract_profile\0', b'xs_contract_profile\0')
        reject(lambda: prep.image_manifest_check(manifest, missing_contract), 'linked OS contract profile')
        cases['missing-contract-record-refused'] = True
        (out / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False,
            image_sha256=image_hash, build_id=manifest['build_id'], cases=cases,
            limits='Synthetic entry facts only. Actual preparation, native admission and frozen launcher validation; no transport.'), indent=2) + '\n')
        print('PASS supervised GPU preparation and frozen entry:', len(cases), 'focused cases')
        return
    if args.staged:
        # Reseal mutated sessions so refusal comes from native field rules.
        for name, offset, value in (('rom-takeover',532,2),('host-revision',536,1),
                                    ('typec-owner',472,13),('host-supply',528,3300000)):
            changed_session=bytearray(session);struct.pack_into('<I',changed_session,offset,value)
            session_path=out / ('native-refuse-' + name + '.bin');session_path.write_bytes(changed_session)
            changed_contract=bytearray(contract);changed_contract[192:224]=hashlib.sha256(changed_session).digest()
            contract_path=out / ('native-contract-' + name + '.bin');contract_path.write_bytes(changed_contract)
            assert not admit(contract_path,armed + HZ // 10,session_path)
            cases['native-refuses-' + name]=True

    # Preparation refuses each missing or changed reviewed fact before writing.
    def entry_with(change, name):
        value = copy.deepcopy(record)
        change(value)
        path = out / ('MODEL-admission-' + name + '.json')
        path.write_text(json.dumps(value))
        return path

    def session_with(change, name):
        """A changed template under a record naming that template, so that only
        the changed field's own rule can refuse it."""
        value = bytearray(raw)
        change(value)
        path = out / ('MODEL-session-' + name + '.bin')
        path.write_bytes(value)
        entry = entry_with(lambda v: v.update(session_template_sha256=hashlib.sha256(value).hexdigest()), name)
        return {'session': path, 'entry': entry}
    refusals = {
        'board-entry-schema': lambda: prepare('r1', entry=entry_with(lambda v: v.update(schema='a733-board-supervised-entry-v2'), 'schema')),
        'other-session-identities': lambda: prepare('r2', entry=entry_with(lambda v: v.update(lease=v['lease'] + 1), 'lease')),
        'missing-input-reservation': lambda: prepare('r3', entry=entry_with(lambda v: v.pop('mimus_ddr'), 'ddr')),
        'thermal-evidence-hash': lambda: prepare('r4', **session_with(lambda v: v.__setitem__(slice(632, 633), b'\0'), 'thermal')),
        'unexpected-host-authority' if args.staged else 'workload-two-with-storage-authority':
            lambda: prepare('r5', **session_with(lambda v: struct.pack_into('<I', v, 536 if args.staged else 400, 2), 'workload')),
        'cpu-critical-above-vendor': lambda: prepare('r12', **session_with(lambda v: struct.pack_into('<I', v, 804, 110001), 'cpu-critical')),
        'thermal-reserved': lambda: prepare('r13', **session_with(lambda v: v.__setitem__(802, 1), 'thermal-reserved')),
        'rom-takeover-reference': lambda: prepare('r11', **session_with(lambda v: struct.pack_into('<I', v, 532, 2), 'usb-reference')),
    }
    qualification = prepare('MODEL-NOT-DEPLOYABLE-70C', **session_with(
        lambda v: struct.pack_into('<I', v, 804, 70000), 'qualification-70C'))
    qualified_session = out / 'MODEL-NOT-DEPLOYABLE-70C/board/session.bin'
    assert struct.unpack_from('<I', qualified_session.read_bytes(), 804)[0] == 70000
    # Run the same C admission used by the native image, with the producer's bytes.
    subprocess.run([str(model), '--check-session', str(qualified_session)], check=True)
    cases['prepared-session-70C-native-admission'] = True
    other = dict(manifest, audio=not audio)
    (out / 'other-link-check.json').write_text(json.dumps(other))
    refusals['link-check-of-other-configuration'] = lambda: prepare('r6', check=out / 'other-link-check.json')
    (out / 'other-clock-link-check.json').write_text(json.dumps(dict(manifest, cpu_clock=not cpu_clock)))
    refusals['link-check-of-other-cpu-clock-build'] = lambda: prepare('r10', check=out / 'other-clock-link-check.json')
    resident = dict(manifest, lifetime='resident')
    (out / 'resident-link-check.json').write_text(json.dumps(resident))
    refusals['resident-link-check'] = lambda: prepare('r7', check=out / 'resident-link-check.json')
    # Preparation does not stage the radio or Bluetooth profile and firmware yet.
    for index, option in enumerate(('radio', 'headset')):
        (out / (option + '-link-check.json')).write_text(json.dumps(dict(manifest, **{option: True})))
        refusals[option + '-link-check'] = (lambda name, path: lambda: prepare(name, check=path))(
            'r%d' % (8 + index), out / (option + '-link-check.json'))
    if audio:
        refusals.update({
            'missing-audio-record': lambda: prepare('a1', entry=entry_with(lambda v: v.pop('audio'), 'audio')),
            'missing-dma-authority': lambda: prepare('a2', entry=entry_with(lambda v: v.pop('dma_nonsecure_access'), 'dma')),
            'audio-evidence-hash': lambda: prepare('a3', **session_with(lambda v: v.__setitem__(slice(832, 833), b'\0'), 'audio')),
            'capture-below-one-second': lambda: prepare('a4', **session_with(lambda v: struct.pack_into('<I', v, 972, 15999), 'samples')),
            'dma-handoff-not-lease': lambda: prepare('a5', **session_with(lambda v: struct.pack_into('<Q', v, 920, lease + 1), 'handoff')),
            'two-dma-channels': lambda: prepare('a6', **session_with(lambda v: struct.pack_into('<H', v, 966, 3), 'channels')),
        })
        partial = json.loads((facts / 'audio.json').read_bytes())
        partial['conditions'].pop('stop-after-first-failure')
        incomplete = put('audio-incomplete', partial)

        def condition_missing():
            # The session names the incomplete record, so only its conditions refuse it.
            changed = session_with(lambda v: v.__setitem__(slice(832, 864), bytes.fromhex(incomplete['sha256'])),
                                   'conditions')
            entry = json.loads(changed['entry'].read_text())
            entry['audio'] = incomplete
            changed['entry'].write_text(json.dumps(entry))
            prepare('a7', **changed)
        refusals['audio-condition-missing'] = condition_missing
    else:
        refusals['audio-authority-without-audio'] = lambda: prepare('n1', entry=entry_with(
            lambda v: v.update(audio=marker), 'unexpected-audio'))
    for name, action in refusals.items():
        assert refused(action), name
        cases['refuses-' + name] = True
    # Every refusal comes before the bundle directory is created.
    assert not any((out / n).exists() for n in ('r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'a1', 'a2', 'a3',
                                                 'a4', 'a5', 'a6', 'a7', 'n1'))

    if cpu_clock and manifest.get('perf_modes'):
        perf_clock_cases(raw, record, put, target, identities, prepare, refused, out, cases)
    elif cpu_clock:
        clock_cases(raw, record, put, target, identities, prepare, refused, out, cases)

    # The frozen bundle no longer depends on the original evidence files.
    shutil.rmtree(facts)
    prepared = tool.freeze_bundle(bundle, 1)
    assert prepared['audio'] is audio and prepared['_combined_frozen']['board/session.bin'] == session
    tool.entry_check(prepared, target['identity'], target['port'])
    assert refused(lambda: tool.entry_check(prepared, 'OTHER-USB-INSTANCE', target['port']))
    if audio:
        assert refused(lambda: tool.freeze_bundle(bundle, 3))
    (bundle / 'board/session.bin').write_bytes(session[:-1] + bytes([session[-1] ^ 1]))
    assert refused(lambda: tool.freeze_bundle(bundle, 1))
    (bundle / 'board/session.bin').write_bytes(session)
    cases['launcher-freezes-and-revalidates-without-original-evidence'] = True

    # The read-only claim check against a modeled connection; no database exists.
    claim = out / 'claim-model'
    claim.mkdir()

    def ref(path):
        return {'path': str(Path(path).resolve()), 'sha256': tool.digest(path)}
    proof = claim / 'software-model.json'
    proof.write_text(json.dumps({'schema': 'a733-os-supervised-software-v1', 'physical_access': False, 'audio': audio,
        'admission_cases': True, 'launcher_cases': True, 'host_capture': True, 'host_lifecycle': True,
        'image': ref(bundle / 'board/image.elf'), 'launcher': ref(bundle / 'board/launch.bin'),
        'sources': {name: ref(path) for name, path in tool.software_sources().items()},
        'evidence': [ref(Path(__file__))], 'fixture_only': 'Not board admission or queue submission.'}))
    prerequisites = {'supervised-ram-recovery': ref(bundle / 'board/admission.json'), 'os-supervised-software': ref(proof)}
    submission = {'prerequisites': list(prerequisites),
                  'artifacts': [ref(bundle / name) for name in prepared['_combined_files']]}
    row = {'state': 'claimed', 'owner': 'model-owner', 'submission': json.dumps(submission),
           'review': json.dumps({'prerequisites': prerequisites})}

    class Query:
        def __init__(self, value):
            self.value = value

        def fetchone(self):
            return self.value

    class Connection:
        row_factory = None

        def execute(self, sql, parameters=()):
            if 'settings' in sql:
                return Query([''])
            assert parameters == ('model-claim',)
            return Query(row)

        def close(self):
            pass

    def connect(database_uri, **kwargs):
        assert database_uri.endswith('?mode=ro') and kwargs == {'uri': True}
        return Connection()
    original = tool.sqlite3.connect
    tool.sqlite3.connect = connect
    try:
        for case in ('accepted', 'owner', 'missing-session-artifact', 'recovery-record'):
            saved = copy.deepcopy(row)
            if case == 'owner':
                row['owner'] = 'different'
            if case == 'missing-session-artifact':
                value = copy.deepcopy(submission)
                value['artifacts'] = [v for v in value['artifacts'] if not v['path'].endswith('/board/session.bin')]
                row['submission'] = json.dumps(value)
            if case == 'recovery-record':
                value = copy.deepcopy(prerequisites)
                value['supervised-ram-recovery'] = ref(proof)
                row['review'] = json.dumps({'prerequisites': value})
            try:
                tool.claimed(claim / 'never-opened.sqlite', 'model-claim', 'model-owner', bundle, prepared)
                assert case == 'accepted'
            except ValueError:
                assert case != 'accepted'
            row.clear()
            row.update(saved)
            cases['claim-' + case] = True
    finally:
        tool.sqlite3.connect = original
    assert not (claim / 'never-opened.sqlite').exists()

    # --fault-report. The supervised transport is replaced by a function that
    # hands the capture callback a modeled operation 5 device; nothing else of
    # the launcher's transport runs.
    faults = module('os_launch_fault_model', ROOT / 'checks/fault-report.py')
    report = faults.report()
    stripped = out / 'MODEL-image-without-fault-report.elf'
    subprocess.run(['/usr/lib/llvm-21/bin/llvm-objcopy', '--strip-symbol=a733_fault_previous',
                    str(bundle / 'board/image.elf'), str(stripped)], check=True)
    without = dict(prepared, _combined_frozen=dict(prepared['_combined_frozen'],
                                                   **{'board/image.elf': stripped.read_bytes()}))
    tool.require_fault_report(prepared)
    assert refused(lambda: tool.require_fault_report(without))
    calls = []

    class Unused:
        def __enter__(self):
            calls.append('launch-view')
            return out / 'never-launched', {}

        def __exit__(self, *unused):
            return False

    records = []

    def session(args, view, launch, ready, entry, capture, **unused):
        calls.append('session')
        args.evidence.mkdir()
        record = {}
        records.append(record)
        capture(device, lambda: calls.append('same'), time.monotonic() + window[0], record, lambda: calls.append('save'))
        return record
    capture_error = []
    window = [60]

    def captured(device, same, deadline, prepared_value, evidence, record, save, before=None):
        for generation in range(1, prepared_value['_recognitions'] + 1) if before else ():
            before(generation)
        calls.append(('capture', len(device.writes)))
        if capture_error:
            raise ValueError(capture_error[0])
        return record
    saved = (tool.freeze_bundle, tool.combined.launch_view, tool.supervisor.run_session,
             tool.combined.capture_device, tool.supervisor.base.fel_request, tool.thermal_profile)
    tool.combined.launch_view = lambda value: Unused()
    tool.supervisor.run_session = session
    tool.combined.capture_device = captured
    tool.thermal_profile = lambda prepared: False
    tool.supervisor.base.fel_request = lambda *unused: calls.append('fel')

    def launch(name, value, *flags):
        tool.freeze_bundle = lambda path, count: value
        calls.clear()
        return tool.main(['--bundle', str(bundle), '--fel', str(out / 'no-fel'), '--evidence', str(out / name),
                          '--database', str(claim / 'never-opened.sqlite'), '--claim', 'model-claim',
                          '--owner', 'model-owner', '--supervised-manual-reconnect', *flags])
    try:
        device = faults.Device(report, 512)
        assert refused(lambda: launch('fault-refused', without, '--fault-report'))
        assert calls == [] and not (out / 'fault-refused').exists()
        cases['fault-report-refused-for-image-without-report'] = True

        record = launch('fault-read', prepared, '--fault-report')
        assert calls[:2] == ['launch-view', 'session'] and ('capture', 1) in calls and 'fel' not in calls
        assert device.writes == [faults.tool.REQUEST] and len(device.reads) == 3
        evidence = out / 'fault-read'
        assert (evidence / 'fault-report.bin').read_bytes() == report
        decoded = json.loads((evidence / 'fault-report.json').read_text())
        assert decoded == json.loads(json.dumps(faults.decoder.decode_report(report)))
        assert record['fault_report'] == {'path': 'fault-report.bin', 'bytes': len(report),
                                          'sha256': hashlib.sha256(report).hexdigest(),
                                          'decoded': 'fault-report.json'}
        cases['fault-report-read-once-before-capture'] = True

        device = faults.Device(report, 512, stop=True)
        try:
            launch('fault-stopped', prepared, '--fault-report')
            raise AssertionError('a stopped device must end the run')
        except faults.Stopped:
            pass
        assert len(device.writes) == 1 and not any(isinstance(c, tuple) for c in calls)
        assert not (out / 'fault-stopped/fault-report.bin').exists()
        cases['fault-report-error-ends-run-before-capture'] = True

        device = faults.Device(report, 512)
        record = launch('fault-not-requested', prepared)
        assert ('capture', 0) in calls and not device.writes and 'fault_report' not in record
        cases['no-fault-report-request-without-flag'] = True

        # Operation 6 after the capture, for a link check with the NPU profile.
        # This image lacks the profile, so the record is changed in the frozen
        # copy only; the request and decoding do not depend on the image.
        assert not tool.npu_profile(prepared) and 'npu_report' not in record
        cases['no-operation-6-without-npu-profile'] = True
        frozen = prepared['_combined_frozen']
        check = json.loads(frozen['board/image-input-manifest.json'])
        with_npu = dict(prepared, _combined_frozen=dict(frozen, **{
            'board/image-input-manifest.json': json.dumps(dict(check, npu=True)).encode()}))
        assert tool.npu_profile(with_npu)
        npu_epoch, npu_generation = struct.unpack_from('<2Q', frozen['board/session.bin'], 24)
        npu_request = struct.pack('<BBHIQ', 6, 0, 0, 0, 0)

        def npu_bytes(epoch=npu_epoch, mismatches=0, kept=0):
            fields = (tool.npu_report.MAGIC, 1, tool.npu_report.REPORT_BYTES, 1, epoch, npu_generation, 0, 2, 2,
                      mismatches, 0, 0, 1, 4096, kept, tool.npu_report.PATTERN)
            return struct.pack(tool.npu_report.LAYOUT, *fields) + bytes(range(32))

        def Device(responses, stop=False):
            """checks/fault-report.py's operation 5 and 6 model, in 64-byte fragments."""
            return faults.Device(responses, 64, stop=stop)

        matched = npu_bytes()
        device = Device({6: matched})
        record = launch('npu-read', with_npu)
        assert ('capture', 0) in calls and device.writes == [npu_request] and not device.queue
        assert (out / 'npu-read/npu-report.bin').read_bytes() == matched
        expected = tool.npu_report.decode(matched, npu_epoch, npu_generation)
        assert record['npu_report'] == {'path': 'npu-report.bin', 'bytes': len(matched),
                                        'sha256': hashlib.sha256(matched).hexdigest(),
                                        'report': expected, 'classification': 'matched'}, record['npu_report']
        json.dumps(record)
        cases['npu-report-read-once-after-capture'] = True

        device = Device({6: npu_bytes(mismatches=1, kept=4096)})
        record = launch('npu-mismatch', with_npu)
        assert record['npu_report']['classification'] == 'mismatch'
        assert record['npu_report']['report']['pattern_bytes'] == 4096
        cases['npu-report-mismatch-classified'] = True

        device = Device({6: npu_bytes(epoch=npu_epoch + 1)})
        record = launch('npu-other-session', with_npu)
        assert record['npu_report']['decode_error'] == 'NPU report names another session.'
        assert 'classification' not in record['npu_report'] and 'report' not in record['npu_report']
        cases['npu-report-of-another-session-not-classified'] = True

        device = Device({5: report, 6: matched})
        record = launch('npu-after-fault', with_npu, '--fault-report')
        assert device.writes == [faults.tool.REQUEST, npu_request] and ('capture', 1) in calls
        assert record['fault_report']['bytes'] == len(report) and record['npu_report']['classification'] == 'matched'
        cases['fault-report-capture-then-npu-report'] = True

        device = Device({}, stop=True)
        try:
            launch('npu-no-reply', with_npu)
            raise AssertionError('an unanswered NPU report must end the run')
        except faults.Stopped:
            pass
        assert device.writes == [npu_request] and records[-1]['npu_report']['state'] == 'no-report'
        assert not (out / 'npu-no-reply/npu-report.bin').exists()
        cases['npu-report-without-reply-ends-run'] = True

        device = Device({6: matched})
        capture_error.append('modeled capture failure')
        try:
            launch('npu-capture-failed', with_npu)
            raise AssertionError('a failed capture must end the run')
        except ValueError as error:
            assert str(error) == 'modeled capture failure'
        capture_error.clear()
        assert not device.writes and 'npu_report' not in records[-1]
        cases['failed-capture-sends-no-operation-6'] = True

        # Performance modes: refused for an image without the service, and
        # for a count other than the generations; otherwise one switch before
        # each recognition and one readback after the capture.
        without_perf = dict(prepared, _combined_frozen=dict(frozen, **{
            'board/image-input-manifest.json': json.dumps(dict(check, perf_modes=False)).encode()}))
        assert refused(lambda: launch('perf-refused', without_perf, '--perf-sequence', 'low'))
        assert calls == [] and not (out / 'perf-refused').exists()
        with_perf = dict(prepared, _combined_frozen=dict(frozen, **{
            'board/image-input-manifest.json': json.dumps(dict(check, perf_modes=True, perf_operation=8, perf_boot_mode='high')).encode()}))
        assert refused(lambda: launch('perf-count', with_perf, '--perf-sequence', 'high,low'))
        assert refused(lambda: launch('perf-mode-name', with_perf, '--perf-sequence', 'mixed'))
        saved_run = tool.perf_modes.run
        tool.perf_modes.run = lambda exchange, command, mode=None, *unused, **kw: (
            calls.append(('perf', command, mode)) or {'mode': 2, 'result': 2})
        try:
            device = Device({})
            record = launch('perf-switch', with_perf, '--perf-sequence', 'low')
        finally:
            tool.perf_modes.run = saved_run
        perf_calls = [c for c in calls if isinstance(c, tuple)]
        assert perf_calls == [('perf', 2, 'low'), ('capture', 0), ('perf', 1, None)], perf_calls
        assert [e['command'] for e in record['perf_modes']] == ['perf-switch-1', 'perf-status']
        assert all(e['state'] == 'observed' for e in record['perf_modes'])
        cases['perf-sequence-refused-without-service-or-count'] = True
        cases['perf-sequence-switches-before-each-recognition'] = True

        for name, seconds in (('npu-window-ended', 0), ('npu-window-short', 0.05)):
            device = Device({6: matched})
            window[0] = seconds
            record = launch(name, with_npu)
            window[0] = 60
            assert not device.writes and record['npu_report'] == {
                'state': 'not-requested', 'reason': 'less than 100 ms of the capture window left'}, record['npu_report']
        cases['no-operation-6-after-the-capture-window'] = True
        cases['no-operation-6-with-under-100-ms-left'] = True
    finally:
        (tool.freeze_bundle, tool.combined.launch_view, tool.supervisor.run_session,
         tool.combined.capture_device, tool.supervisor.base.fel_request, tool.thermal_profile) = saved
    assert not any(name == 'usb' or name.startswith('usb.') for name in sys.modules)
    result = {'passed': True, 'physical_access': False, 'audio': audio, 'image_sha256': image_hash,
              'build_id': manifest['build_id'], 'tool_sha256': tool.digest(Path(tool.__file__)),
              'preparation_sha256': tool.digest(Path(prep.__file__)), 'workload': inner['workload'], 'cases': cases,
              'limits': 'All physical facts synthetic; the transport, FEL program and board are never used.'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS OS supervised preparation and launcher (%s): %d cases' % ('audio' if audio else 'no audio', len(cases)))


if __name__ == '__main__':
    main()
