#!/usr/bin/env python3
"""Check that the host pre-flight runs an OS image's own EL3 admission and names the failing term.

An image whose manifest did not fit its session lifetime once reached the board and stopped at EL3 admission with
the unnamed a733_smp_stop(1, 1). tools/host_admission.py builds the image's admission code for the host (apps/os/
admission.c or its cpu-return copy, apps/os/session.c, apps/board/admission.c, the image's build ID and manifest
section, the compile definitions of the image's own configured build) and runs it on the session and contract the
runner prepares. This check prepares the session and contract of each built image with the runner's own code
(tools/board_images.py, with captured board words in place of a live read) and requires:
  accepted    the image's own manifest, session and contract are admitted;
  run_us      a run deadline one microsecond past the lifetime minus the return reserve is refused as
              session_decode (run_deadline), and one exactly at that bound is admitted;
  reserve     a return reserve of the whole lifetime is refused as session_decode (return_reserve);
  build ID    a contract naming another build is refused as build_id;
  magic       a contract with another magic is refused as magic_version (magic);
  lifetimes   a contract with the supervised or the resident OS magic is refused as magic_version (magic);
  identity    a contract whose epoch differs from the session's is refused as contract_field (identity);
  digest      a contract with another session digest is refused as session_bind (digest);
  launcher    a launcher that arms another lifetime than the image's is refused as timing (diagnostic);
  startup     an admission check after the startup deadline is refused as timing (startup);
  radio       for an image whose manifest requires the radio device (the scanning radio image, board workload 3): the
              radio device with the full station's workload 1, or with the storage-free workload 2, is refused as
              session_decode (devices), and a session without the radio device as session_decode (manifest);
and that the stop the board records for the unnamed (1, 1) admission stop equals the refusal in every case.
Usage: admission-preflight.py --build BUILD_DIR [--build BUILD_DIR ...] --output DIR, where each BUILD_DIR is a configured
build of one returning OS image (SUNSTONE_A733_OS_CPU_RETURN) built with its sunstone-a733-os-*.elf target.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import sys
GATE_RUNS = [['--build', '{build:ret-latency}', '--build', '{build:ret-radio-scan}', '--output', '{output}']]

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
CHECK_INPUTS = ['tools/host_admission.py', 'tools/host_admission.c', 'tools/host_admission_io.c', 'tools/session_lifetime.py',
                'include/a733_admission_refusal.h', 'apps/os/admission.c', 'apps/os/session.c', 'apps/board/admission.c',
                'apps/os/lifetime.c', 'apps/os/host-admission-units.txt']
MANIFEST_RUN, MANIFEST_RESERVE = 56, 64       # offsets of run_deadline_us and return_reserve_us in struct ss_manifest
SESSION_MANIFEST_HASH = 592 + 32              # prefix bytes plus the offset of manifests_sha256 in the session header
# struct a733_smp_contract: reserved[] starts at 176; reserved[2..5] hold the session digest.
CONTRACT_MAGIC, CONTRACT_EPOCH, CONTRACT_BUILD_ID, CONTRACT_DIGEST = 0, 24, 48, 192
# A733_OS_CONTRACT_MAGIC and A733_OS_RESIDENT_CONTRACT_MAGIC (include/a733_board_application.h)
SUPERVISED_CONTRACT_MAGIC, RESIDENT_CONTRACT_MAGIC = 0x415337334f534331, 0x415337334f524331
SESSION_DEVICES, PREFIX_WORKLOAD = 592 + 24, 400  # devices in struct ss_session_header; workload in the board prefix


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare(board, image, out):
    """The runner's session and contract for the image, as tools/board_images.py builds them."""
    import argparse as ap
    images = board.images
    parser = ap.ArgumentParser()
    board.run_options(parser, False)
    parser.add_argument('--evidence', type=Path)
    host = images.load('admission_preflight_records', ROOT / 'apps/os/host.py')
    manifest = host.image_manifest(image)['manifest']
    inputs = []
    for i, region in enumerate(host.regions(manifest, (1,))):
        path = out / f'input-{i}.bin'
        path.write_bytes(b'x')
        inputs.append(str(path))
    values = [str(image), '--port', '3-1', '--evidence', str(out / 'run'), '--kind', 'cpu-return', '--preserve-dram']
    for path in inputs:
        values += ['--input', path]
    args = parser.parse_args(values)
    words = images.offline_words()
    assert words, 'the captured board words are not on this machine'
    info = images.inspect(image, 'cpu-return')
    keep = images.policy
    images.policy = lambda *a, **k: None       # a write-capable image needs the coordinator's record; admission does not
    try:
        images.prepare(args, info, {'words': words}, out / 'bundle')
    finally:
        images.policy = keep
    return (out / 'bundle/session.bin').read_bytes(), (out / 'bundle/contract.bin').read_bytes()


def mutated_image(image, out, run_us=None, reserve_us=None):
    """A copy of the image with a changed manifest, and the session hash that goes with it."""
    from elftools.elf.elffile import ELFFile
    data = bytearray(image.read_bytes())
    with image.open('rb') as stream:
        section = ELFFile(stream).get_section_by_name('sunstone_manifest')
        offset, size = section['sh_offset'], section['sh_size']
    if run_us is not None:
        struct.pack_into('<Q', data, offset + MANIFEST_RUN, run_us)
    if reserve_us is not None:
        struct.pack_into('<Q', data, offset + MANIFEST_RESERVE, reserve_us)
    out.mkdir(parents=True, exist_ok=True)
    target = out / image.name
    target.write_bytes(data)
    return target, hashlib.sha256(bytes(data[offset:offset + size])).digest()


def one_build(build, out):
    admission = load('admission_preflight_host_admission', ROOT / 'tools/host_admission.py')
    crumbs = load('admission_preflight_crumbs', ROOT / 'tools/a733-crumbs.py')
    board = load('admission_preflight_board', ROOT / 'tools/board.py')
    manifests = load('admission_preflight_manifest', ROOT / 'tools/sunstone-manifest.py')
    host = load('admission_preflight_session_records', ROOT / 'apps/os/host.py')
    found = list(build.glob('sunstone-a733-os-*.elf'))
    assert len(found) == 1, f'{build}: expected one sunstone-a733-os-*.elf, found {len(found)}'
    image = found[0]
    work = out / build.name
    work.mkdir(parents=True)
    cache = out / 'cache-root' / 'preflight'          # where tools/host_admission.py looks for a harness (SUNSTONE_CACHE)
    located = admission.find_build(image)
    session, contract = prepare(board, image, work)
    facts = admission.image_facts(image)
    seconds = facts['seconds']
    parsed = manifests.unpack(facts['manifest'])[0]
    run_us, reserve_us = parsed['run_deadline_us'], parsed['return_reserve_us']
    report = admission.preflight(image, session, contract, build=located, cache=cache)
    assert report['admitted'] and report['lifetime_seconds'] == seconds, report
    cases = ['accepted']

    def run(label, expected, run_us=None, reserve_us=None, launcher_seconds=None, edit=None, delay=0, edit_session=None):
        """Run the admission of a variant; expected is None (admitted) or (term, sub) of the board's stop."""
        target, digest = mutated_image(image, work / label.replace(' ', '-'), run_us, reserve_us)
        variant = bytearray(session)
        if edit_session:
            edit_session(variant)
        if run_us is not None or reserve_us is not None:
            variant[SESSION_MANIFEST_HASH:SESSION_MANIFEST_HASH + 32] = digest
        binary = admission.build_harness(target, located, cache)
        completed, now = admission.completed_contract(contract, launcher_seconds or seconds, facts['contract_magic'])
        completed = bytearray(completed)
        completed[CONTRACT_DIGEST:CONTRACT_DIGEST + 32] = hashlib.sha256(bytes(variant)).digest()   # the runner digests the session it sends
        if edit:
            edit(completed)
        result = admission.evaluate(binary, variant, bytes(completed), now + delay)
        assert result['detail'] == result['stop'], (label, 'the recorded stop differs from the refusal', result)
        named = crumbs.refusal_detail(result['detail'])
        got = (named['term'], named.get('sub')) if named else None
        assert got == expected, (label, got, expected)
        cases.append(label)
        return result

    bound = seconds * 1_000_000 - reserve_us
    run('run_us at the bound', None, run_us=bound)
    run('run_us one microsecond over', ('session_decode', 'run_deadline'), run_us=bound + 1)
    run('reserve of the whole lifetime', ('session_decode', 'return_reserve'), reserve_us=seconds * 1_000_000)
    run('another build ID', ('build_id', None), edit=lambda c: c.__setitem__(CONTRACT_BUILD_ID, c[CONTRACT_BUILD_ID] ^ 1))
    run('another contract magic', ('magic_version', 'magic'), edit=lambda c: c.__setitem__(CONTRACT_MAGIC, c[CONTRACT_MAGIC] ^ 1))
    # A returning image admits the returning OS magic alone: the magics of the other lifetimes are refused.
    run('the supervised contract magic', ('magic_version', 'magic'),
        edit=lambda c: struct.pack_into('<Q', c, CONTRACT_MAGIC, SUPERVISED_CONTRACT_MAGIC))
    run('the resident contract magic', ('magic_version', 'magic'),
        edit=lambda c: struct.pack_into('<Q', c, CONTRACT_MAGIC, RESIDENT_CONTRACT_MAGIC))
    # A CPU clock grant that names clusters names this boot's epoch: another value is refused, and the zero of an
    # unassigned card grant takes the board's epoch (apps/os/session.c).
    # The session of this build carries such a grant; a session without one would skip the case, so its absence fails.
    grant = host.session_decode(session)['offsets'].get(host.CPU_GRANT)
    assert grant is not None and struct.unpack_from('<I', session, grant + 48)[0], 'the session has no CPU clock grant that names clusters'

    def grant_epoch(value):
        return lambda s: struct.pack_into('<Q', s, grant + 32, value(struct.unpack_from('<Q', s, grant + 32)[0]))
    run('grant names another epoch', ('session_decode', 'service'), edit_session=grant_epoch(lambda e: e ^ 1))
    run('grant with an unassigned epoch', None, edit_session=grant_epoch(lambda e: 0))
    run('another epoch', ('contract_field', 'identity'), edit=lambda c: c.__setitem__(CONTRACT_EPOCH, c[CONTRACT_EPOCH] ^ 1))
    run('another session digest', ('session_bind', 'digest'), edit=lambda c: c.__setitem__(CONTRACT_DIGEST, c[CONTRACT_DIGEST] ^ 1))
    run('launcher arms another lifetime', ('timing', 'diagnostic'), launcher_seconds=seconds + 30)
    run('check after the startup deadline', ('timing', 'startup'), delay=admission.COUNTER_HZ * 2)
    radio = manifests.DEVICES['radio']
    if parsed['devices_required'] & radio:
        # The radio device grants the station only with this image's radio workload (apps/os/session.c).
        assert struct.unpack_from('<I', session, PREFIX_WORKLOAD)[0] == 3 and struct.unpack_from('<I', session, SESSION_DEVICES)[0] & radio
        for workload in (1, 2):
            run('radio device with workload %d' % workload, ('session_decode', 'devices'),
                edit_session=lambda s, w=workload: struct.pack_into('<I', s, PREFIX_WORKLOAD, w))
        run('radio workload without the radio device', ('session_decode', 'manifest'),
            edit_session=lambda s: struct.pack_into('<I', s, SESSION_DEVICES, struct.unpack_from('<I', s, SESSION_DEVICES)[0] & ~radio))
    # The message the runner prints names the numbers of the lifetime terms.
    target, digest = mutated_image(image, work / 'message', run_us=bound + 1)
    admission.build_harness(target, located, cache)
    variant = bytearray(session)
    variant[SESSION_MANIFEST_HASH:SESSION_MANIFEST_HASH + 32] = digest
    variant_contract = bytearray(contract)
    variant_contract[CONTRACT_DIGEST:CONTRACT_DIGEST + 32] = hashlib.sha256(bytes(variant)).digest()
    try:
        admission.preflight(target, bytes(variant), bytes(variant_contract), build=located, cache=cache)
        raise AssertionError('a run deadline over the lifetime was admitted')
    except admission.AdmissionRefused as error:
        assert (error.term, error.sub) == ('session_decode', 'run_deadline'), (error.term, error.sub)
        message = str(error)
    assert f'a733_smp_stop(1, {0x168:#x})' in message and 'at least' in message and 'derives' in message and f'{seconds} s' in message, message
    cases.append('refusal message')
    # The submission path: `submit` runs the same admission on the frozen inputs, and a refusal queues nothing.
    submit(board, admission, image, target, located, cache, work, cases)
    return cases, seconds, (run_us, reserve_us), message


def submit(board, admission, image, refused_image, located, cache, work, cases):
    import argparse as ap
    parser = ap.ArgumentParser()
    board.run_options(parser, True)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--worker', default='worker')
    parser.add_argument('--hypothesis', default='h')
    host = board.images.load('admission_preflight_submit_records', ROOT / 'apps/os/host.py')
    names = []
    for i, _ in enumerate(host.regions(host.image_manifest(image)['manifest'], (1,))):
        names += ['--input', str(work / f'input-{i}.bin')]

    def arguments(path, hypothesis):
        return parser.parse_args([str(path), '--port', 'any', '--kind', 'cpu-return', '--preserve-dram',
                                  '--reference', 'docs/board-runner.md#submit-a-frozen-image-and-hypothesis',
                                  '--worker', 'worker', '--hypothesis', hypothesis, *names])

    queue = board.Queue(work / 'queue')
    keep = board.images.policy
    board.images.policy = lambda *a, **k: None
    try:
        # The refused image's harness was built in the check's cache; the submission finds it by the image digest.
        job = queue.submit(arguments(image, 'the unchanged image is admitted'))
        assert job['preflight'].get('admitted') is True, job['preflight']
        cases.append('submit admits the image')
        with queue.locked() as state:
            before = len(state['jobs']) + len(state['history'])
        try:
            queue.submit(arguments(refused_image, 'the changed manifest is refused'))
        except board.images.AdmissionRefusal as error:
            assert 'session_decode (run_deadline)' in str(error) and '0x168' in str(error), str(error)
        else:
            raise AssertionError('submit queued an image the board would refuse')
        with queue.locked() as state:
            after = len(state['jobs']) + len(state['history'])
        assert before == after, 'a refused submission was queued'
        leftover = [d for d in (queue.root / 'entries').iterdir() if not (d / 'entry.json').exists()]
        assert not leftover, f'a refused submission left a half-built entry directory: {leftover}'
        cases.append('submit refuses the changed manifest')
        # With no recorded board words the pre-flight cannot run: the submission refuses, and only a recorded reason queues it.
        with queue.locked() as state:
            before = len(state['jobs']) + len(state['history'])

        def refused_unavailable():
            try:
                queue.submit(arguments(image, 'no recorded words'))
            except board.PreflightUnavailable as error:
                assert 'Pre-flight cannot run' in str(error), str(error)
                return True
            return False

        from unittest.mock import patch
        with patch.object(board.images, 'offline_words', return_value=None):
            assert refused_unavailable(), 'submit queued an image whose pre-flight could not run'
            with queue.locked() as state:
                assert before == len(state['jobs']) + len(state['history']), 'a refused submission was queued'
            with patch.object(board, 'apply_override', lambda args, record: record):
                assert not refused_unavailable(), 'the check does not detect warn-and-continue'
            cases.append('submit refuses an unavailable pre-flight; warn-and-continue fails this check')
            overridden = arguments(image, 'override')
            overridden.no_preflight = 'words rebuilt by the next run'
            job = queue.submit(overridden)
            assert job['preflight']['override'] == 'words rebuilt by the next run' and job['preflight']['unavailable'], job['preflight']
            cases.append('submit --no-preflight records its reason')
    finally:
        board.images.policy = keep


def main():
    import os
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', type=Path, action='append', required=True)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    os.environ['SUNSTONE_CACHE'] = str(a.output / 'cache-root')     # the harness cache of this run, read by tools/host_admission.py
    for build in a.build:
        cases, seconds, (run_us, reserve_us), message = one_build(build, a.output)
        print(f'PASS admission pre-flight {build.name}: lifetime {seconds} s, run_us {run_us}, return_reserve_us {reserve_us}; ' + '; '.join(cases))
        print('  example refusal: ' + message)


if __name__ == '__main__':
    main()
