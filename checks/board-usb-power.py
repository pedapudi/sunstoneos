#!/usr/bin/env python3
"""Exercise PL8 runner admission, return recovery and fault evidence without USB."""
import argparse
import importlib.util
from pathlib import Path
import sys
import struct
import types
from unittest.mock import patch

if not __debug__:
    raise RuntimeError('Checks require assertions.')

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import board
import board_usb_power as power

spec = importlib.util.spec_from_file_location('runner_fixture', ROOT/'checks/board-runner.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--from-phase', choices=('ddr-helper-return', 'additional'))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    results = []

    def passed(name):
        results.append(name)
        print('PASS', name)

    def owner_for(name, mux=1, level=1, bias=0):
        root = args.out/name
        root.mkdir()
        model = fixture.Model('normal')
        model.registers.update({power.CFG: 0xabcdef00 | mux,
            power.DAT: 0xcafe00ff | level << 8, power.PULL: 0x123000ab | bias << 16})
        owner = board.Live(fixture.arguments(root), root, {}, access=model,
            process=model.process, now=model.now, sleep=model.sleep)
        return owner, model

    if not args.from_phase:
        for mux, level, bias in ((1, 0, 0), (0, 1, 0), (15, 1, 0), (0, 0, 2)):
            owner, model = owner_for(f'safe-{mux}-{level}-{bias}', mux, level, bias)
            original = dict(model.registers)
            with owner.locked():
                owner.usb_source_off('admission')
            assert model.registers == original and not owner.quarantined
            passed(f'safe-mux-{mux}-data-{level}-pull-{bias}')

        owner, model = owner_for('high-admission')
        original = dict(model.registers)
        with owner.locked():
            try:
                owner.usb_source_off('admission')
            except power.PowerFault:
                pass
            else:
                raise AssertionError('Unsafe admission passed')
        assert model.registers[power.DAT] == original[power.DAT] & ~(1 << 8)
        assert model.registers[power.CFG] == original[power.CFG]
        assert model.registers[power.PULL] == original[power.PULL]
        assert owner.record['usb_vbus'][0]['recovered_low'] and not owner.quarantined
        assert owner.record['usb_vbus_fault']['inputs']['output_high']
        passed('admission-high-cleared-first-preserves-neighbors-and-fails')

        for mux, bias in ((2, 0), (0, 1), (15, 3)):
            owner, model = owner_for(f'unknown-{mux}-{bias}', mux, 0, bias)
            with owner.locked():
                try:
                    owner.usb_source_off('admission')
                except power.PowerFault:
                    pass
                else:
                    raise AssertionError('Unknown source state passed')
            assert owner.quarantined
            assert not any('writel' in call for call in model.calls)
            passed(f'unknown-mux-{mux}-pull-{bias}-quarantined-without-writes')

        for variant in ('normal', 'watchdog', 'return-decoder'):
            base = fixture.Model
            class HighReturn(base):
                def process(self, command, **kwargs):
                    result = super().process(command, **kwargs)
                    if 'exe' in command[3:]:
                        self.registers.update({power.CFG: 1, power.DAT: 0xa5a50100, power.PULL: 0})
                    return result
            with patch.object(fixture, 'Model', HighReturn):
                result, model, owner = fixture.lifecycle(args.out/('return-'+variant), variant)
            assert result['outcome'] == ('watchdog-reset' if variant == 'watchdog' else
                                        'host-error' if variant == 'return-decoder' else 'result-mismatch'), result
            assert result['usb_vbus_fault']['site'] == 'usb-vbus-output-high'
            assert model.registers[power.DAT] == 0xa5a50000
            assert result['usb_vbus'][-1]['recovered_low'] and not model.quarantined
            if variant == 'watchdog':
                assert result['first_failure']['site'] != 'usb-vbus-output-high'
            passed(variant+'-return-observed-recovered-and-failure-preserved')

        owner, model = owner_for('failed-transfer')
        real = model.process
        def fail_snapshot(command, **kwargs):
            if command[3:5] == ['readl', hex(power.CFG)]:
                result = real(command, **kwargs)
                result.returncode = 1
                result.stderr = 'injected GPIO read failure'
                return result
            return real(command, **kwargs)
        owner.process = fail_snapshot
        try:
            with owner.locked():
                owner.usb_source_off('admission')
        except board.TransportFailure:
            pass
        else:
            raise AssertionError('Failed transport accepted')
        assert owner.failed and owner.quarantined and len(model.calls) == 1
        passed('transport-failure-stops-without-recovery-transaction')

    for phase in ('fresh-fel', 'pending-return', 'ddr-helper-return', 'ddr-new-instance', 'transport-recovery'):
        if args.from_phase == 'additional' or args.from_phase and phase in ('fresh-fel', 'pending-return'):
            continue
        owner, model = owner_for(phase)
        with owner.locked():
            if phase == 'fresh-fel':
                with patch.object(board.os, 'access', return_value=True):
                    try: owner.reset()
                    except power.PowerFault: pass
            elif phase == 'pending-return':
                owner.arm(0x41)
                with patch.object(board.os, 'access', return_value=True):
                    try: owner.finish_pending()
                    except power.PowerFault: pass
            elif phase == 'ddr-helper-return':
                # A helper returns synchronously to FEL, without USB re-enumeration.
                owner.process = lambda command, **kw: model.process(
                    [*command[:3], *[x for x in command[3:] if x not in ('exe', '0x71000')]], **kw)
                try: owner.ddr_transaction(model.node, owner.identity, {'launch_attempted': False}, lambda: None, ('exe', '0x71000'), 30)
                except (power.PowerFault, board.DdrPowerAbort): pass
            else:
                # Real new-instance admission and the common check used by recovery.
                model.number += 1
                owner.adopt(model.node)
                try: owner.usb_source_off(phase)
                except power.PowerFault: pass
        assert owner.record['usb_vbus'][-1]['phase'] == phase
        assert owner.record['usb_vbus'][-1]['recovered_low']
        passed(phase+'-low-recovery')

    owner, model = owner_for('actual-ddr-readmission')
    with owner.locked():
        model.number += 1
        try:
            owner.ddr_transaction(model.node, model.identity(model.node), {'launch_attempted': False},
                                  lambda: None, ('version',), 30)
        except board.DdrPowerAbort:
            pass
        else:
            raise AssertionError('DDR continued after PL8 fault')
    assert owner.record['usb_vbus'][-1]['phase'] == 'ddr-new-instance'
    assert not any('version' in call for call in model.calls)
    passed('actual-ddr-readmission-stops-before-next-command')

    owner, model = owner_for('actual-transport-recovery')
    try:
        with patch.object(board.os, 'access', return_value=True), owner.locked():
            owner.arm(0x41)
            owner.failed = True
            raise board.TransportFailure('original failed transport')
    except board.TransportFailure as error:
        assert str(error) == 'original failed transport'
    else:
        raise AssertionError('Transport failure was hidden')
    assert owner.record['usb_vbus'][-1]['phase'] == 'transport-recovery'
    assert owner.record['usb_vbus'][-1]['recovered_low']
    assert owner.record['cleanup_failure'] and not owner.quarantined
    passed('actual-transport-recovery-preserves-original-failure-and-clears-high')

    owner, model = owner_for('stuck-data-write')
    original_process = model.process
    def stuck_write(command, **kw):
        if command[3:5] == ['writel', hex(power.DAT)]:
            return original_process(command[:3], **kw)
        return original_process(command, **kw)
    owner.process = stuck_write
    with owner.locked():
        try: owner.usb_source_off('admission')
        except power.PowerFault: pass
        else: raise AssertionError('Stuck high accepted')
    assert owner.quarantined and owner.record['usb_vbus'][-1]['after']['output_high']
    passed('stuck-high-recovery-quarantines-with-pre-post-evidence')

    owner, model = owner_for('live-diagnostic-clock-admission', level=0)
    model.registers.update({0x07010010: 0, 0x07090160: 0x10008002, 0x0701019c: 0})
    with owner.locked():
        power.admit_diagnostic(owner)
        for values in ((0x04000000,0x983fffb7,0x10001),(0x04000000,0x983fffb7,0x30003)):
            model.registers.update(dict(zip((0x07010010,0x07090160,0x0701019c),values)))
            power.admit_diagnostic(owner)
            assert tuple(owner.record['usb_vbus_clock_admission'].values())==values
    passed('diagnostic-records-observed-and-warm-entry-for-bounded-native-initialization')

    info = {'descriptor': {}, 'symbols': {'usb_vbus_probe_profile': 1}}
    root = args.out/'policy'; root.mkdir()
    options = fixture.arguments(root)
    for port, approved, allowed in (('3-1', None, False), ('3-3', 'coordinator', False), ('3-1', 'coordinator', True)):
        options.port, options.approved_by = port, approved
        # The fixture image is a model file, which the storage classification refuses by default; this case tests the VBUS
        # probe policy, so the classification is answered 'no-core' (an image without an SD core).
        real_load = board.images.load
        stub = types.SimpleNamespace(classify=lambda image: 'no-core')
        try:
            with patch.object(board.images, 'load', lambda name, path: stub if name == 'storage_bounds' else real_load(name, path)):
                board.images.policy(info, options)
        except ValueError as refusal:
            assert not allowed, str(refusal)
        else: assert allowed
    passed('vbus-probe-marker-requires-coordinator-and-port-3-1')
    for fault in ('write-failure', 'stuck-high', 'unknown-source'):
        base = fixture.Model
        class FaultReturn(base):
            failed_gpio_write = False
            def process(self, command, **kw):
                assert not self.failed_gpio_write, 'Request after failed GPIO transport'
                if command[3:5] == ['writel', hex(power.DAT)] and self.launched:
                    if fault == 'write-failure':
                        self.failed_gpio_write = True
                        return types.SimpleNamespace(returncode=1, stdout='', stderr='injected PL8 write transport failure')
                    if fault == 'stuck-high':
                        return super().process(command[:3], **kw)
                result = super().process(command, **kw)
                if 'exe' in command[3:]:
                    self.registers.update({power.CFG: 2 if fault == 'unknown-source' else 1,
                        power.DAT: 1 << 8, power.PULL: 0})
                return result
        with patch.object(fixture, 'Model', FaultReturn):
            result, model, owner = fixture.lifecycle(args.out/('watchdog-'+fault), 'watchdog')
        assert result['outcome'] == 'watchdog-reset', result
        assert result['first_failure']['site'] != 'usb-vbus-output-high'
        assert result['watchdog_return']['words'] and result['cleanup_failure']
        assert result['usb_vbus_fault']['inputs']['mux'] == (2 if fault == 'unknown-source' else 1)
        assert owner.quarantined
        passed('watchdog-'+fault+'-retains-primary-breadcrumb-and-cleanup-failure')

    marker_image = root/'profile.elf'
    marker_image.write_bytes(b'\x7fELFMODEL')
    real_load = board.images.load
    def load(name, path):
        if name == 'native_probe_contract':
            return types.SimpleNamespace(inspect=lambda *a, **kw: {'watchdog': {'mode': 0xb1, 'session_seconds': 20}})
        return real_load(name, path)
    names = {'usb_vbus_probe_profile': 1, 'sunstone_probe_marker': 1}
    for mode, valid in ((0, True), (1, True), (10, True), (2, False)):
        with patch.object(board.images, 'symbols', return_value=names), \
             patch.object(board.images, 'symbol_bytes', return_value=struct.pack('<3Q', 0x3153554256535355, 1, mode)), \
             patch.object(board.images, 'load', side_effect=load):
            try: info = board.images.inspect(marker_image, 'probe')
            except ValueError: assert not valid
            else: assert valid and info['native_probe'] and not info['usb_class']
    passed('exact-vbus-profile-modes-admit-without-usb-host-controller')
    board.save(args.out/'results.json', {'passed': results})


if __name__ == '__main__':
    main()
