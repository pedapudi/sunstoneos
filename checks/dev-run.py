#!/usr/bin/env python3
"""Check development-command routing, offline preparation and retained rail validation."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import sys
import types
from unittest.mock import patch

if not __debug__:
    raise RuntimeError('Checks require assertions.')
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import board
import board_state as dev
rows = []

def passed(name):
    rows.append({'name': name, 'passed': True})


HELPER_RECORD = {'complete': True, 'transport_failed': False, 'mode': 'kernel',
                 'result': {'magic': 0x534b4552, 'stage': 5, 'status': 0}}

def helper_log(dcdc5, dcdc3, volt=None, end=None):
    clocks = []
    for phase in ('before', 'after'):
        for name, pll, mux, mhz in dev.BOOT_ROWS:
            clocks.append(f'{name} {phase} pll={pll:#x} mux={mux:#x} mhz={mhz} pat0=0x0 pat1=0x30000000 lfm=0x40000001')
    if volt is None:
        volt = [f'VOLT {r}_selector=0x9e {r}_uv=800000' for r, mv in (('dcdc3', dcdc3), ('dcdc5', dcdc5)) if mv != 800]
    return '\n'.join(['BEGIN board bundle 33: ' + dev.RAIL_COMPLETE[len('COMPLETE board bundle 33: '):],
                      'TEMP set=0 level=0', *clocks[:3],
                      f'SUPPLY dcdc5_uv={dcdc5 * 1000} dcdc3_uv={dcdc3 * 1000} sid=0x496 bin=4 dcxo_hz=26000000',
                      *volt, *clocks[3:], end or dev.RAIL_COMPLETE]) + '\n'

def helper_cases():
    for dcdc5, dcdc3, written in ((1000, 1050, ['dcdc3', 'dcdc5']), (800, 1050, ['dcdc3']),
                                  (1000, 800, ['dcdc5']), (800, 800, [])):
        r = dev.helper_report(HELPER_RECORD, helper_log(dcdc5, dcdc3))
        assert r['complete'] and r['written'] == written and len(r['volt']) == len(written), r
        assert r['supply'] == [f'SUPPLY dcdc5_uv={dcdc5 * 1000} dcdc3_uv={dcdc3 * 1000} sid=0x496 bin=4 dcxo_hz=26000000']
    refused = {'complete': False, 'transport_failed': False, 'mode': 'kernel',
               'result': {'magic': 0x534b4552, 'stage': 5, 'status': 1}}
    for name, record, log, reason in (
            ('refused', refused, helper_log(1000, 1050, volt=[], end='REFUSED board bundle 33: boot clock words'),
             'did not return normally'),
            ('missing-volt', HELPER_RECORD, helper_log(1000, 1050, volt=['VOLT dcdc3_selector=0x9e dcdc3_uv=800000']),
             'VOLT lines'),
            ('dcdc5-first', HELPER_RECORD, helper_log(1000, 1050, volt=['VOLT dcdc5_selector=0x9e dcdc5_uv=800000',
                                                                       'VOLT dcdc3_selector=0x9e dcdc3_uv=800000']),
             'VOLT lines'),
            ('wrong-readback', HELPER_RECORD, helper_log(1000, 800, volt=['VOLT dcdc5_selector=0x9f dcdc5_uv=810000']),
             'VOLT lines'),
            ('clock-after', HELPER_RECORD, helper_log(1000, 1050).replace('CPU-B after pll=0xf8802700',
                                                                          'CPU-B after pll=0xf8805300'), 'CPU-B after'),
            # A log truncated after the VOLT word.
            ('truncated-volt', HELPER_RECORD, helper_log(1000, 1050, volt=[
                'VOLT ', 'VOLT dcdc5_selector=0x9e dcdc5_uv=800000']), 'VOLT lines')):
        r = dev.helper_report(record, log)
        assert not r['complete'] and reason in r['reason'], (name, r)
        assert r['supply'] and r['outcome'], (name, r)
    passed('rail-helper-report')

def routing(out):
    for kind in ('cpu-return', 'gpu-return', 'npu', 'rail-helper'):
        with patch.object(board, 'main', return_value=7) as launch:
            result = board.development([kind, '--port', '3-3', '--evidence', str(out/kind)])
        assert result == 7
        command = launch.call_args.args[0]
        assert command[0] == 'run' and command[command.index('--kind')+1] == kind
        assert command[command.index('--port')+1] == '3-3'
    passed('existing-subcommands-use-board-runner')
    with patch.object(board, 'main', return_value=0) as launch, contextlib.redirect_stderr(io.StringIO()):
        board.development(['cpu-return', '--state', str(out/'captured.json'), '--force', '--no-normalize-rails',
                           '--input', 'first.bin', '--input', 'second.bin', '--decoder', 'decoder.py'])
    command = launch.call_args.args[0]
    assert command[0] == 'check' and '--force' not in command and '--no-normalize-rails' not in command
    assert [command[i+1] for i,v in enumerate(command) if v == '--input'] == ['first.bin','second.bin']
    passed('captured-state-stays-offline-and-cannot-bypass-admission')
    wrapper = board.images.load('checked_development_wrapper', ROOT/'tools/dev-run.py')
    with patch.object(board, 'development', return_value=9) as dispatch:
        assert wrapper.main(['npu','--image','image.elf']) == 9
        assert dispatch.call_args.args[0] == ['npu','--image','image.elf']
    passed('thin-wrapper-preserves-arguments-and-exit-status')
    for operation in ('run-launch-survey.py', 'run-a64-entry-probe.py', 'run-native-startup-probe.py'):
        script = board.images.load('legacy_'+operation.replace('-', '_'), ROOT/'tools'/operation)
        import types
        args=types.SimpleNamespace(build=out, bundle=out, fel=out/'fel', evidence=out/'legacy')
        with patch.object(board, 'legacy_run', return_value=4) as dispatch:
            assert script.run(args) == 4 and dispatch.call_count == 1
    passed('legacy-probe-launches-route-through-admission')


def offline(out):
    source = json.loads((dev.EVIDENCE/'sol-resume-20261031/port-3-3/cpu-thermal-state/session.json').read_text())
    words = {}
    for t in source['transactions']:
        c=t['command'];addresses=[int(c[i+1],16) for i,x in enumerate(c) if x=='readl']
        words.update({hex(a):int(v,16) for a,v in zip(addresses,t['stdout'].split())})
    captured=out/'captured.json';captured.write_text(json.dumps({'words':words}))
    with patch.object(board, 'Live', side_effect=AssertionError('Offline preparation touched USB')), contextlib.redirect_stdout(io.StringIO()):
        assert board.development(['cpu-return','--image',str(dev.CONTROL_IMAGE),'--state',str(captured),
                                  '--no-preflight','the control image is a cached build whose admission sources are not in this tree',
                                  '--evidence',str(out/'offline')]) == 0
    report=json.loads((out/'offline/admission.json').read_text())
    assert report['admitted'] and report['physical_access'] is False
    # The application default is the original 1 MiB diagnostic input, in manifest order.
    assert (out/'offline/input-0.bin').stat().st_size == 1048576
    bad=dict(words);bad[hex(0x8871000)]=0
    captured.write_text(json.dumps({'words':bad}))
    try:
        with patch.object(board, 'Live', side_effect=AssertionError('Offline refusal touched USB')), contextlib.redirect_stdout(io.StringIO()):
            board.development(['cpu-return','--image',str(dev.CONTROL_IMAGE),'--state',str(captured),
                               '--no-preflight','the control image is a cached build whose admission sources are not in this tree',
                               '--force','--evidence',str(out/'unsafe')])
    except ValueError as error:
        assert 'CPU-L PLL/mux' in str(error)
    else:
        raise AssertionError('Unsafe captured state admitted')
    passed('real-typed-session-preparation-and-unsafe-clock-refusal-stay-offline')


def npu_cases(out):
    """Exercise the current inspector with geometry from a frozen parser."""
    images = board.images
    image = out/'npu-image.elf'
    image.write_bytes(b'modeled immutable NPU image')
    marker = {'mode': 0xb1, 'session_seconds': 20}
    reader = types.SimpleNamespace(watchdog_marker=lambda image: marker)
    layout = (456, 224, 232, 72, 96, 128)
    finite = types.SimpleNamespace(
        image_profile=lambda data, manifest: ([], 0x41000000, bytes(32), layout, {'startup_only': True}),
        geometry_profile=lambda geometry: finite.STARTUP,
        STARTUP='npu-startup-return-v1', INFERENCE='npu-single-inference-return-v1',
        RUNTIME='npu-runtime-return-v1', RESTART='npu-finite-restart-return-v1',
        SWITCH='npu-model-switch-return-v1', SDK='npu-sdk-return-v1', POWER='npu-power-cycle-return-v1')
    manifest = {'execution_profile': finite.STARTUP}
    facts = images.finite_npu_info(image, reader, finite, manifest)
    assert facts['watchdog'] == marker and facts['geometry']['startup_only']
    assert facts['session_layout'] == list(layout)
    for altered in (None, {'mode': 0xa1, 'session_seconds': 20},
                    {'mode': 0xb1, 'session_seconds': 19}):
        with patch.object(reader, 'watchdog_marker', return_value=altered):
            try:
                images.finite_npu_info(image, reader, finite, manifest)
            except ValueError as error:
                assert 'exact host-armed watchdog' in str(error)
            else:
                raise AssertionError('NPU accepted another watchdog contract')
    with patch.object(finite, 'image_profile', return_value=([], 0x41000000, bytes(32), (904, 224, 232), {})):
        try:
            images.finite_npu_info(image, reader, finite, manifest)
        except ValueError as error:
            assert 'session layout differs' in str(error)
        else:
            raise AssertionError('NPU accepted another session layout')
    passed('npu-image-geometry-and-exact-watchdog')
    frozen = out/'frozen-host-tree'
    def load(name, path):
        if name == 'board_records':
            return reader
        assert name == 'inspect_npu_irq' and path == frozen/'apps/npu-interrupt/host.py'
        return types.SimpleNamespace(finite=finite)
    with patch.object(images, 'load', side_effect=load):
        for profile in (finite.STARTUP, finite.INFERENCE, finite.RUNTIME, finite.RESTART, finite.SWITCH, finite.SDK):
            with patch.object(finite, 'geometry_profile', return_value=profile):
                info = images.inspect(image, 'npu', host_tree=frozen, manifest=manifest)
                assert info['execution_profile'] == profile and info['finite_npu']
        passed('npu-current-inspector-admits-local-restart-switch-sdk-with-frozen-parser')
        with patch.object(finite, 'geometry_profile', return_value=finite.POWER):
            # The frozen parser still recognizes POWER; current admission owns the refusal.
            assert images.finite_npu_info(image, reader, finite, manifest)['execution_profile'] == finite.POWER
            try:
                images.inspect(image, 'npu', host_tree=frozen, manifest=manifest)
            except ValueError as error:
                assert 'physical NPU OFF' in str(error)
            else:
                raise AssertionError('Unsupported physical NPU OFF accepted')
    passed('npu-current-inspector-refuses-physical-off-with-frozen-parser')


def queued_decoder(out):
    source = out/'application'
    source.mkdir()
    (source/'input.bin').write_bytes(b'original input')
    decoder = source/'host.py'
    decoder.write_text('from pathlib import Path\ndef inputs():\n'
                       '    return [(Path(__file__).parent/"input.bin").read_bytes()]\n'
                       'def decode(payload):\n    return payload\n')
    import argparse
    parser=argparse.ArgumentParser()
    board.run_options(parser, submit=True)
    args=parser.parse_args([str(dev.CONTROL_IMAGE),'--kind','cpu-return','--port','3-1',
                           '--decode-application','--decoder',str(decoder),
                           '--reference','docs/board-runner.md#submit-a-frozen-image-and-hypothesis'])
    args.worker='compatibility';args.hypothesis='Frozen decoder inputs retain submission bytes'
    job=board.Queue(out/'queue').submit(args)
    options=job['options']
    assert Path(options['input'][0]).read_bytes() == b'original input'
    assert Path(options['decoder']).read_bytes() == decoder.read_bytes()
    assert options['decoder_origin'] == str(decoder)
    # Input construction uses source-relative files at submission; queued bytes are independent.
    (source/'input.bin').write_bytes(b'changed after submission')
    decoder.write_text('raise RuntimeError("changed after submission")\n')
    assert Path(options['input'][0]).read_bytes() == b'original input'
    import types
    frozen=board.load_decoder(types.SimpleNamespace(decoder=Path(options['decoder']), decoder_origin=decoder))
    assert frozen.decode(b'payload') == b'payload' and frozen.__file__ == str(decoder)
    passed('queue-freezes-decoder-and-source-relative-default-inputs')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True)
    routing(args.out)
    helper_cases()
    npu_cases(args.out)
    offline(args.out)
    queued_decoder(args.out)
    (args.out/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':rows},indent=2)+'\n')
    print(f'{len(rows)} checks passed; no device access')


if __name__ == '__main__':
    main()
