#!/usr/bin/env python3
"""Check upper Bluetooth retirement and actual synchronous-audio wire closure.

The wire fixture is built from the pinned checks/bt-sco.py references, or an
existing output can be supplied. Core and HFP references construct the
controller replies independently of these changes.
The upper fixture models protocol boundaries and checks the production owner's
ordering, deadlines and retained buffers. Neither fixture qualifies hardware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

from harness import CACHE, CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build', '{build:headset}', '--output', '{output}']]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--wire-fixture', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--remaining', action='store_true', help='Run mutations and native compiles; reuse an already passing upper/SCO batch')
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    if a.wire_fixture is None:
        a.wire_fixture = out / 'wire-reference'
        command = [sys.executable, str(ROOT / 'checks/bt-sco.py'), '--references-only',
                   '--output', str(a.wire_fixture)]
        for option, source in [('source', 'bluetooth-hci'), ('l2cap-source', 'bluetooth-l2cap'),
                               ('sdp-source', 'bluetooth-sdp'), ('rfcomm-source', 'bluetooth-rfcomm'),
                               ('hfp-source', 'bluetooth-hfp'), ('audio-source', 'bluetooth-audio'),
                               ('core-source', 'bluetooth-incoming')]:
            command += ['--' + option, str(CACHE / 'sources' / source)]
        result = subprocess.run(command, cwd=ROOT, env=environment(), capture_output=True, text=True)
        (out / 'references-command.json').write_text(json.dumps(command, indent=2) + '\n')
        (out / 'references.log').write_text(result.stdout + result.stderr)
        assert result.returncode == 0, 'see ' + str(out / 'references.log')
    reference = json.loads((a.wire_fixture / 'results.json').read_text())
    assert reference['primary_sources']['Core54_HTML'] == '66d656a886aa0f0aa3e62baddffe5c517ce2fcda9886002c58208fe3f000b0e6'
    assert reference['primary_sources']['HFP19'] == '56f5fcce59cc4342c38d13ed44cc87b28ce73d1f2214dc909d121c378575fa41'
    for name, digest in reference['artifacts'].items():
        assert hashlib.sha256((a.wire_fixture / name).read_bytes()).hexdigest() == digest
    (out / 'headset-sco-fixture.h').write_text((ROOT / 'checks/bt-sco.c').read_text().replace(
        'int main(int argc,char **argv)', 'int previous_sco_main(int argc,char **argv)'))
    original = (ROOT / 'apps/bluetooth/headset.c').read_text()
    def function(name):
        start = original.index('static int ' + name + '(')
        body = original.index('{', start); depth = 1; end = body + 1
        while depth:
            depth += (original[end] == '{') - (original[end] == '}'); end += 1
        return original[start:end]
    scan_source = '\n'.join(function(n) for n in ('stop_audio_records', 'stop_control_step',
                                                 'stop_audio_service', 'stop_consumers_step', 'stop_acl_step'))
    bindings = {'voice.audio': 'scan.q', 'voice.acl': 'scan.g.rf.d.lower.a',
                'voice.l2cap': 'scan.g.rf.d.lower.q', 'voice.ag': 'scan.g.ag',
                'voice.rfcomm': 'scan.g.rf.client', 'voice.discovery': 'scan.g.rf.d.client',
                'voice.server': 'scan.g.sdp', 'voice.partial_serial': 'scan_partial_serial',
                'voice.partial_offset': 'scan_partial_offset'}
    for name, value in bindings.items():
        scan_source = scan_source.replace(name, value)
    (out / 'headset-scan-step.h').write_text(scan_source + '\n')
    kernel = CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
    tcp = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT / 'include')]
    upper = ['-DSUNSTONE_AIC8800_RSN=1', '-DINCLUDE_xSemaphoreGetMutexHolder=1',
             '-DSUNSTONE_BLUETOOTH_HEADSET=1', '-I' + str(ROOT / 'network/include'),
             '-I' + str(ROOT / 'apps/bluetooth'), '-I' + str(kernel / 'include'),
             '-I' + str(kernel / 'portable/GCC/ARM_AARCH64_SRE'), '-I' + str(tcp / 'include'),
             '-I' + str(tcp / 'portable/Compiler/GCC'), '-I' + str(a.build / 'os-image/bluetooth')]
    commands = []

    def run(label, command, succeeds=True):
        if a.remaining and label in ('upper-build', 'upper', 'sco-build', 'sco'):
            return ''
        command = list(map(str, command)); commands.append(command)
        result = subprocess.run(command, cwd=ROOT, env=environment(), capture_output=True, text=True)
        (out / (label + '.log')).write_text(result.stdout + result.stderr)
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        assert (result.returncode == 0) == succeeds, label + ': see ' + str(out / (label + '.log'))
        return result.stdout

    run('upper-build', [CLANG, *upper, *flags, ROOT / 'checks/bluetooth-headset-return.c',
                       '-Wl,--gc-sections', '-o', out / 'upper'])
    run('upper', [out / 'upper'])
    workers = ['bt_h4', 'bt_hci', 'bt_acl', 'bt_l2cap', 'bt_sdp', 'bt_rfcomm', 'bt_sdp_server', 'bt_hfp_ag', 'bt_sco']
    sources = [ROOT / ('drivers/bluetooth/' + name + '.c') for name in workers]
    run('sco-build', [CLANG, *flags, '-I' + str(out), '-I' + str(a.wire_fixture), '-I' + str(ROOT / 'checks'),
                     ROOT / 'drivers/uart/a733_uart.c', *sources, ROOT / 'checks/bluetooth-headset-return-sco.c', '-o', out / 'sco'])
    run('sco', [out / 'sco'])
    run('timing-build', [CLANG, *flags, '-I' + str(out), '-I' + str(a.wire_fixture), '-I' + str(ROOT / 'checks'),
                        ROOT / 'drivers/uart/a733_uart.c', *sources,
                        ROOT / 'checks/bluetooth-headset-return-timing.c', '-o', out / 'timing'])
    run('timing', [out / 'timing'])
    consumer_source = function('stop_consumers_step')
    for name, value in bindings.items():
        consumer_source = consumer_source.replace(name, value)
    assert scan_source.count(consumer_source) == 1
    service = '||!stop_audio_service()'
    assert scan_source.count(service) == 4
    pump_mutations = {
        'scan-without-consumers': (scan_source.replace(consumer_source, 'static int stop_consumers_step(uint64_t bound){return bt_acl_step(&scan.g.rf.d.lower.a,64,bound);}\n'), []),
        'voice-without-consumers': (scan_source.replace(consumer_source, 'static int stop_consumers_step(uint64_t bound){return bt_hfp_ag_step(&scan.g.ag,64,128,bound);}\n'), ['--voice-only']),
        'discovery-without-consumers': (scan_source.replace(consumer_source, 'static int stop_consumers_step(uint64_t bound){return bt_sdp_step(&scan.g.rf.d.client,64,bound);}\n'), ['--discovery-only']),
        'defer-sco-service': (scan_source.replace(service, ''), ['--cadence-only']),
    }
    for name, (body, arguments) in pump_mutations.items():
        variant = out / name; variant.mkdir()
        (variant / 'headset-scan-step.h').write_text(body)
        run(name + '-build', [CLANG, *flags, '-Wno-unused-function', '-I' + str(variant),
            '-I' + str(out), '-I' + str(a.wire_fixture), '-I' + str(ROOT / 'checks'),
            ROOT / 'drivers/uart/a733_uart.c', *sources, ROOT / 'checks/bluetooth-headset-return-sco.c', '-o', variant / 'run'])
        run(name, [variant / 'run', *arguments], succeeds=False)
    acl_source = function('stop_acl_step')
    for name, value in bindings.items():
        acl_source = acl_source.replace(name, value)
    assert scan_source.count(acl_source) == 1
    timing_mutations = {
        'rotate-consumers': scan_source.replace(consumer_source,
            'static int stop_consumers_step(uint64_t bound){return previous_consumer_rotation(bound);}'),
        'defer-acl-refusal': scan_source.replace(acl_source,
            'static int stop_acl_step(uint64_t bound){return previous_acl_progress(bound);}'),
    }
    for name, body in timing_mutations.items():
        variant = out / name; variant.mkdir()
        (variant / 'headset-scan-step.h').write_text(body)
        run(name + '-build', [CLANG, *flags, '-Wno-unused-function', '-I' + str(variant),
            '-I' + str(out), '-I' + str(a.wire_fixture), '-I' + str(ROOT / 'checks'),
            ROOT / 'drivers/uart/a733_uart.c', *sources,
            ROOT / 'checks/bluetooth-headset-return-timing.c', '-o', variant / 'run'])
        run(name, [variant / 'run'], succeeds=False)
    mutations = {
        'skip-audio-drain': ('bt_sco_quiescent(&voice.audio)', '1'),
        'release-pending-pdu': ('if(voice.l2cap.tx[i].state!=BT_L2CAP_FREE||voice.l2cap.rx[i].state!=BT_L2CAP_FREE)return 0;',
                                'if(voice.l2cap.tx[i].state!=BT_L2CAP_FREE||voice.l2cap.rx[i].state!=BT_L2CAP_FREE)break;'),
    }
    for name, (before, after) in mutations.items():
        assert original.count(before) == (3 if name == 'skip-audio-drain' else 1)
        provider = out / (name + '.c'); provider.write_text(original.replace(before, after))
        fixture = out / (name + '-fixture.c')
        fixture.write_text((ROOT / 'checks/bluetooth-headset-return.c').read_text().replace(
            '#include "../apps/bluetooth/headset.c"', '#include "' + str(provider) + '"'))
        run(name + '-build', [CLANG, *upper, *flags, fixture, '-Wl,--gc-sections', '-o', out / name])
        run(name, [out / name], succeeds=False)
    units = json.loads((a.build / 'compile_commands.json').read_text())
    unit = next(u for u in units if u['file'].endswith('/apps/bluetooth/headset.c'))
    source_root = str(Path(unit['file']).parents[2])
    native = [f.replace(source_root, str(ROOT)) for f in shlex.split(unit['command'])]
    i = native.index('-o'); native[i + 1] = str(out / 'headset.o')
    run('native-headset', native)
    run('native-sco', [CLANG, '--target=aarch64-none-elf', '-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-ffreestanding', '-fno-builtin', '-O2', '-I' + str(ROOT / 'include'),
                       '-c', ROOT / 'drivers/bluetooth/bt_sco.c', '-o', out / 'bt_sco.o'])
    inputs = [ROOT / 'apps/bluetooth/headset.c', ROOT / 'apps/bluetooth/headset.h', *sources,
              ROOT / 'include/bt_sco.h', ROOT / 'checks/bluetooth-headset-return.c',
              ROOT / 'checks/bluetooth-headset-return-sco.c', ROOT / 'checks/bluetooth-headset-return-timing.c',
              ROOT / 'checks/bt-sco.py', Path(__file__), a.wire_fixture / 'results.json']
    (out / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False,
        upper_cases=0 if a.remaining else 9, refused_modes=[] if a.remaining else [4, 5, 6],
        sco_wire_cases=0 if a.remaining else 9, owned_scan_composition=not a.remaining,
        owned_voice_composition=not a.remaining, concurrent_discovery_composition=not a.remaining,
        next_pass_refusal_cadence=not a.remaining, late_acl_data_proof=not a.remaining,
        timing_comparisons=10, caught_mutations=[*pump_mutations, *timing_mutations, *mutations],
        references=reference['primary_sources'], inputs={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}), indent=2) + '\n')
    print('PASS ten timing comparisons, eight mutations and native objects' if a.remaining else
          'PASS nine upper cases, nine SCO wire cases, five compositions, ten timing comparisons, three mode refusals, eight mutations and native objects')


if __name__ == '__main__':
    main()
