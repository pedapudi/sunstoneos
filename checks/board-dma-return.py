#!/usr/bin/env python3
"""Check returning DMA image admission and frozen inputs without USB access.

The positive authority is synthetic and uses an impossible all-zero USB
instance. Its physical flags exercise the parser and establish no authority.
"""
import argparse
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT/'tools'))
import board
import board_images as images


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT, help='OS record producer source tree.')
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    images.ROOT = args.source.resolve()
    host = images.load('dma_return_records_check', images.ROOT/'apps/os/host.py')
    info = images.inspect(args.image, 'cpu-return')
    assert info['dma'] and info['generic']
    manifest = host.image_manifest(args.image)
    original = (images.CONTROL/'board/session-template.bin').read_bytes()
    prefix = bytearray(original[:592])
    prefix[48:80] = images.load('dma_return_image_check', images.ROOT/'tools/check-a733-smp.py').image_parts(args.image)[2]
    epoch, generation, lease = struct.unpack_from('<3Q', prefix, 24)
    service = bytearray(160)
    struct.pack_into('<3Q', service, 0, images.DMA_MAGIC, 1, 160)
    service[56:80] = prefix[488:512]
    struct.pack_into('<5I', service, 80, 1, 0, 1, 0, 0)
    struct.pack_into('<3QI', service, 104, 0x733d00, lease, 0, 0)
    struct.pack_into('<2QI2H', service, 136, 0x04601000, lease, 0x3f, 0xffff, 1)
    security = images.load('dma_return_policy_check', images.ROOT/'tools/dma-nonsecure-access.py')
    authority = dict(identity=[0, 0, 0, 0, 0], port='3-1', image_sha256=images.sha(args.image),
                     epoch=epoch, generation=generation, lease=lease, controller=0x04601000,
                     service_channels=1, dma_evidence=lease, dma_handoff=lease,
                     configuration_sha256=hashlib.sha256(service[56:160]).hexdigest(),
                     physical_authority=True, modeled=False, nonsecure_channels=1, dma_autogate=4,
                     fixture='synthetic host only; no physical authority')
    authority.update({key: True for key in (*security.FACTS, *images.DMA_FACTS)})
    template = args.output/'synthetic-template'
    template.mkdir()
    launch = SimpleNamespace(image=args.image, kind='cpu-return', template=template, records=None,
                             port='3-1', input=[], approved_by=None, storage_write=False)
    rows = []

    def install(record=authority, dma=service, include=True):
        data = (json.dumps(record, sort_keys=True)+'\n').encode()
        (template/'dma-authority.json').write_bytes(data)
        value = bytearray(dma)
        value[24:56] = hashlib.sha256(data).digest()
        services = {host.THERMAL: bytearray(original[592:808]), host.CPU_GRANT: original[808:904]}
        struct.pack_into('<Q', services[host.THERMAL], 16, 1064 if include else 904)
        if include:
            services[images.DMA_EXTENT] = value
        raw = host.session_bytes(prefix, services, manifest['manifest'], manifest['sha256'], 0, host.BIND_SID)
        (template/'session.bin').write_bytes(raw)
        return raw

    def rejected(name, action):
        try:
            action()
        except ValueError as error:
            rows.append(dict(name=name, refused=str(error)))
        else:
            raise AssertionError('Unexpected admission: '+name)

    install()
    assert images.dma_admission(launch, info)
    assert images.dma_admission(launch, info, {'identity': authority['identity']})
    rows.append(dict(name='synthetic-positive-original-bindings', passed=True))
    rejected('live-instance-mismatch', lambda: images.dma_admission(launch, info, {'identity': [1, 2, 3, 4, 5]}))
    rejected('cpu-image-unexpected-dma', lambda: images.dma_admission(launch, dict(info, dma=False)))
    install(include=False)
    rejected('missing-dma-extent', lambda: images.dma_admission(launch, info))
    for name, key, value in [('modeled', 'modeled', True), ('missing-physical', 'physical_authority', False),
                             ('wrong-configuration', 'configuration_sha256', '0'*64),
                             ('wrong-nonce', 'lease', lease+1), ('wrong-image', 'image_sha256', '0'*64),
                             ('wrong-port', 'port', '3-3'), ('wrong-autogate', 'dma_autogate', 0)]:
        trial = copy.deepcopy(authority)
        trial[key] = value
        install(trial)
        rejected(name, lambda: images.dma_admission(launch, info))
    for fact in (*security.FACTS, *images.DMA_FACTS):
        trial = copy.deepcopy(authority)
        trial.pop(fact)
        install(trial)
        rejected('missing-'+fact, lambda: images.dma_admission(launch, info))
    install()
    (template/'dma-authority.json').write_bytes(b'{}\n')
    rejected('authority-digest-mismatch', lambda: images.dma_admission(launch, info))
    launch.template = args.output/'missing-authority'
    launch.template.mkdir()
    (launch.template/'session.bin').write_bytes((template/'session.bin').read_bytes())
    rejected('missing-authority-file', lambda: images.dma_admission(launch, info))
    launch.template = template
    for name, offset, fmt, value in [('cold-init', 128, '<I', 1), ('route-change', 96, '<I', 1),
                                     ('wrong-controller', 136, '<Q', 0x04602000)]:
        trial = bytearray(service)
        struct.pack_into(fmt, trial, offset, value)
        install(dma=trial)
        rejected(name, lambda: images.dma_admission(launch, info))
    before = install()
    source = json.loads((board.state.EVIDENCE/'sol-resume-20261031/port-3-3/cpu-thermal-state/session.json').read_text())
    words = {}
    for transaction in source['transactions']:
        command = transaction['command']
        addresses = [int(command[i+1], 16) for i, value in enumerate(command) if value == 'readl']
        words.update({hex(a): int(v, 16) for a, v in zip(addresses, transaction['stdout'].split())})
    plan = images.prepare(launch, info, {'words': words, 'identity': authority['identity']}, args.output/'prepared')
    after = (args.output/'prepared/session.bin').read_bytes()
    decoded = host.session_decode(after)
    new_lease = struct.unpack_from('<Q', after, 40)[0]
    assert struct.unpack_from('<Q', decoded['extents'][images.DMA_EXTENT], 112)[0] == new_lease
    assert struct.unpack_from('<Q', decoded['extents'][images.DMA_EXTENT], 144)[0] == new_lease
    assert new_lease != lease and (template/'session.bin').read_bytes() == before
    assert (args.output/'prepared/dma-authority.json').read_bytes() == (template/'dma-authority.json').read_bytes()
    assert plan['format'] == 'generic'
    rows.append(dict(name='reseeds-after-original-validation-and-freezes-authority', passed=True))
    cli = argparse.ArgumentParser()
    board.run_options(cli, submit=True)
    cli.add_argument('--worker', default='synthetic-host-fixture')
    cli.add_argument('--hypothesis', default='host input freezing only')
    queued = cli.parse_args([str(args.image), '--port', '3-1', '--kind', 'cpu-return',
                             '--template', str(template), '--reference', 'docs/board-runner.md#submit-a-frozen-image-and-hypothesis'])
    job = board.Queue(args.output/'synthetic-queue').submit(queued)
    frozen = Path(job['directory'])/'template/dma-authority.json'
    assert frozen.read_bytes() == (template/'dma-authority.json').read_bytes()
    assert job['frozen']['template/dma-authority.json'] == images.sha(frozen)
    rows.append(dict(name='queue-freezes-authority-in-input-inventory', passed=True))
    queued.template = args.output/'missing-authority'
    queued.evidence = args.output/'refused-run'
    backend = Mock(side_effect=AssertionError('USB owner must not be constructed'))
    result = board.run_image(queued, backend=backend)
    assert result['outcome'] == 'refused-before-launch'
    assert 'dma-authority.json' in result['first_failure']['detail']
    backend.assert_not_called()
    rows.append(dict(name='run-entry-refuses-before-usb-owner-construction', passed=True))
    install()
    static_admission = images.dma_admission(launch, info)
    rejected('unverified-snapshot-cannot-prepare', lambda: images.prepare(
        launch, info, {'words': words, 'identity': authority['identity']},
        args.output/'unverified', dma_binding=static_admission))

    class ReturningModel(board.Live):
        """Use the real run entry and prepare while modeling only board effects."""
        initial_identity = authority['identity']

        def __init__(self, *values):
            super().__init__(*values, access=object())
            self.identity = list(self.initial_identity)
            self.reset_calls = 0
            self.lock_held = False

        @contextlib.contextmanager
        def locked(self):
            self.lock_held = True
            try:
                yield self
            finally:
                self.lock_held = False

        def sanity(self):
            assert self.lock_held

        def reset(self):
            assert self.lock_held and self.dma_binding.identity_verified
            self.reset_calls += 1
            self.identity = [5, 4, 3, 2, 1]
            # Direct-run source files can change after admission. Preparation
            # must consume the authenticated snapshot, including its authority.
            (template/'session.bin').write_bytes(b'UNAUTHENTICATED REPLACEMENT')
            (template/'dma-authority.json').write_bytes(b'{}\n')

        def dram(self):
            assert self.reset_calls == 1

        def rails(self):
            pass

        def live_state(self, finite_npu=False):
            return dict(identity=self.identity, port=self.args.port, words=words)

        def launch(self, plan):
            assert self.lock_held and self.reset_calls == 1
            assert self.identity != list(self.dma_binding.identity)
            session = (self.evidence/'bundle/session.bin').read_bytes()
            decoded = host.session_decode(session)
            assert decoded['extents'][images.DMA_EXTENT][56:112] == service[56:112]
            assert (self.evidence/'bundle/dma-authority.json').read_bytes() == self.dma_binding.authority
            return 'returned'

        def capture(self, plan):
            return dict(valid=True, payload=b'fixture', report=dict(synthetic=True))

    owners = []

    def model_backend(*values):
        owner = ReturningModel(*values)
        owners.append(owner)
        return owner

    run_args = copy.copy(queued)
    run_args.image, run_args.template = args.image, template
    run_args.evidence = args.output/'original-instance-run'
    result = board.run_image(run_args, backend=model_backend)
    assert result['outcome'] == 'returned', result['first_failure']
    assert owners[-1].reset_calls == 1
    rows.append(dict(name='original-instance-binding-survives-reset-and-source-replacement', passed=True))
    install()
    ReturningModel.initial_identity = [1, 2, 3, 4, 5]
    run_args = copy.copy(queued)
    run_args.image, run_args.template = args.image, template
    run_args.evidence = args.output/'wrong-original-instance-run'
    result = board.run_image(run_args, backend=model_backend)
    assert result['outcome'] == 'refused-before-launch'
    assert result['first_failure']['site'] == 'dma-identity'
    assert owners[-1].reset_calls == 0
    rows.append(dict(name='wrong-original-instance-refuses-before-reset', passed=True))
    install()
    offline_args = copy.copy(queued)
    offline_args.image, offline_args.template = args.image, template
    offline_args.evidence = args.output/'offline-snapshot'
    offline_args.state = args.output/'synthetic-state.json'
    offline_args.state.write_text(json.dumps(dict(identity=authority['identity'], words=words)))
    admission_function = images.dma_admission

    def replace_after_admission(*values):
        admission = admission_function(*values)
        (template/'session.bin').write_bytes(b'UNAUTHENTICATED OFFLINE REPLACEMENT')
        (template/'dma-authority.json').write_bytes(b'{}\n')
        return admission

    with patch.object(images, 'dma_admission', side_effect=replace_after_admission) as validate:
        with contextlib.redirect_stdout(io.StringIO()):
            assert board.check_image(offline_args) == 0
        assert validate.call_count == 1
    rows.append(dict(name='offline-check-consumes-one-authenticated-snapshot', passed=True))
    install()
    report = dict(passed=True, physical_access=False, physical_authority=False, synthetic_authority=True,
                  native_execution=False, image_sha256=images.sha(args.image), cases=rows)
    (args.output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(dict(passed=True, cases=len(rows), physical_access=False)))


if __name__ == '__main__':
    main()
