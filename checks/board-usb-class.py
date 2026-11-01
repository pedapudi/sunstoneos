#!/usr/bin/env python3
"""Check USB runner admission, native receipts and typed inputs without USB.

The ELF is an admission fixture with inert function bodies. It establishes
host bounds and dispatch, never device quiescence or physical authority.
"""
import argparse
import contextlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import types
from unittest.mock import patch

if not __debug__:
    raise RuntimeError('Checks require assertions.')
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import board
import board_images as images
import board_probe as probe

CHECK_INPUTS = ['tools/board.py', 'tools/board_images.py', 'tools/board_probe.py',
                'tools/board_readme.py', 'apps/os/host.py']
ROWS = []


def passed(name):
    ROWS.append(name)
    print('PASS', name)


def fixture(out, flags=1, host=True, weak=False, generic=False, pool=None, class_service=False):
    source = out/'fixture.S'
    functions = ['_start', *('sunstone_probe_'+n for n in
                 ('begin', 'complete', 'return', 'fault', 'quiesce', 'restore_interrupts', 'rom'))]
    if host:
        functions.append('a733_usb2_native_poll')
    if class_service:
        functions.append('os_usb_service_dispatch')
    text = '.section .text,"ax"\n'
    for name in functions:
        text += ('.weak ' if weak and name == 'sunstone_probe_quiesce' else '.global ') + name + '\n'
        text += f'.type {name},%function\n{name}: ret\n.size {name},.-{name}\n'
    text += '.section .rodata,"a"\n.global sunstone_probe_marker\n.type sunstone_probe_marker,%object\n'
    text += 'sunstone_probe_marker:\n.quad ' + ','.join(str(v) for v in
        (probe.MAGIC, 1, 112, 0xb1, 20, probe.SESSION, 128, probe.RECEIPT, 128,
         0x42000100, 256, 0, 0, flags)) + '\n.size sunstone_probe_marker,112\n'
    if generic:
        records = images.load('usb_fixture_records', ROOT/'apps/os/host.py')
        manifest = records.manifest_tool.assign(records.manifest_tool.load(ROOT/'apps/hello-compute/manifest.json'))
        (out/'manifest.bin').write_bytes(records.manifest_tool.pack(manifest))
        text += '.global a733_smp_build_id\n.type a733_smp_build_id,%object\na733_smp_build_id: .zero 32\n.size a733_smp_build_id,32\n'
        text += '.section sunstone_manifest,"a"\n.incbin "' + str(out/'manifest.bin') + '"\n'
        text += '.global __a733_smp_result, __a733_smp_contract, __a733_smp_contract_end\n'
        text += '.set __a733_smp_result,0x42000000\n.set __a733_smp_contract,0x42004000\n.set __a733_smp_contract_end,0x42005000\n'
        text += '.section .rodata,"a"\n.global os_cpu_return_watchdog\n.type os_cpu_return_watchdog,%object\n'
        text += 'os_cpu_return_watchdog: .quad 0x31544457534f5243,2,0xb1,0x07090000,20\n.size os_cpu_return_watchdog,40\n'
        text += '.section .bss,"aw",%nobits\n.global ss_result_record,__bss_start,__bss_end,__heap_end\n'
        text += '__bss_start:\n.type ss_result_record,%object\nss_result_record: .zero 72\n.size ss_result_record,72\n'
        text += '__bss_end:\n.set __heap_end,0x41300000\n'
        text += '.section .monitor,"aw",%nobits\n.global __monitor_bss_start,__monitor_bss_end,__a733_smp_boot_stack_top\n'
        text += '.global a733_qualification_report,a733_qualification_stacks\n__monitor_bss_start:\n'
        text += 'a733_qualification_report: .zero 8880\n.size a733_qualification_report,8880\n'
        text += 'a733_qualification_stacks: .zero 32768\n.size a733_qualification_stacks,32768\n'
        text += '__monitor_bss_end:\n__a733_smp_boot_stack_top:\n'
    if pool is not None:
        text += '.section .uncached_pool,"aw",%nobits\n.global __uncached_pool_start,__uncached_pool_end\n'
        text += '__uncached_pool_start: .zero ' + str(pool[1]) + '\n__uncached_pool_end:\n'
    source.write_text(text)
    script = out/'fixture.ld'
    script.write_text('ENTRY(_start)\nPHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); monitor PT_LOAD FLAGS(6); pool PT_LOAD FLAGS(6); }\nSECTIONS { '
                      '. = 0x41000000; .text : { *(.text) } :text '
                      '.rodata : { *(.rodata) } :text sunstone_manifest : { *(sunstone_manifest) } :text ' +
                      ('.bss 0x41200000 (NOLOAD) : { *(.bss) } :data .monitor 0x42005000 (NOLOAD) : { *(.monitor) } :monitor ' if generic else '') +
                      (f'.uncached_pool {pool[0]} (NOLOAD) : {{ *(.uncached_pool) }} :pool ' if pool else '') + '}\n')
    subprocess.run(['/usr/bin/clang-21', '--target=aarch64-none-elf', '-c', source,
                    '-o', out/'fixture.o'], check=True)
    subprocess.run([images.LLD, '-T', script, out/'fixture.o', '-o', out/'fixture.elf'], check=True)
    return out/'fixture.elf'


def args_for(image, evidence, approved=False):
    parser = argparse.ArgumentParser()
    board.run_options(parser)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--state', type=Path)
    return parser.parse_args([str(image), '--kind', 'usb-class', '--port', '3-1',
        '--evidence', str(evidence), *(['--approved-by', 'coordinator'] if approved else [])])


def admission(out):
    variants = [('returnable', 1, True, False, True), ('power-removal', 2, True, False, False),
                ('weak-quiesce', 1, True, True, False), ('no-host', 1, False, False, False)]
    for name, flags, host, weak, accepted in variants:
        folder = out/name
        folder.mkdir()
        image = fixture(folder, flags, host, weak)
        try:
            info = images.inspect(image, 'usb-class')
        except ValueError:
            assert not accepted, name
        else:
            assert accepted and info['native_probe'] and info['usb_class']
            args = args_for(image, out/'offline', True)
            with contextlib.redirect_stdout(io.StringIO()):
                assert board.check_image(args) == 0
            report = json.loads((out/'offline/admission.json').read_text())
            assert report['admitted'] and not report['physical_access']
            plan = images.prepare(args, info, {}, out/'capture-plan')
            payload = b'class output'
            raw = struct.pack('<16Q', probe.RESULT_MAGIC, 1, 128, plan['epoch'], plan['generation'],
                plan['lease'], 0, len(payload), probe.fnv(payload), 7, 0xfe, 24, 48, 0, 0, probe.COMPLETE)
            class Owner:
                evidence = out
                def read(self, address, size, path):
                    data = raw if address == probe.RECEIPT else payload
                    assert len(data) == size
                    path.write_bytes(data)
                    return data
            captured = images.capture(Owner(), plan, args)
            assert captured['valid'] and captured['payload'] == payload
            for index, value in ((3, plan['epoch'] ^ 1), (9, 3), (10, 0), (14, 0xb1)):
                altered = bytearray(raw)
                struct.pack_into('<Q', altered, index*8, value)
                try:
                    probe.receipt(altered, plan)
                except ValueError:
                    pass
                else:
                    raise AssertionError('Invalid USB cleanup receipt accepted')
        passed('native-contract-'+name)
    image = out/'returnable/fixture.elf'
    for name, target, approved in [('approval', image, False),
                                   ('retained', out/'retained.bin', True),
                                   ('power-removal', out/'power-removal/fixture.elf', True)]:
        if name == 'retained':
            target.write_bytes(b'Retained OS fixture; no native probe contract')
        args = args_for(target, out/('refuse-'+name), approved)
        def no_access(*values):
            raise AssertionError('Refused image constructed a device backend')
        with contextlib.redirect_stdout(io.StringIO()):
            result = board.run_image(args, no_access)
        assert result['outcome'] == 'refused-before-launch' and not result['launch_attempted']
        assert result['first_failure']['site'] == 'preflight'
        passed('before-device-'+name)
    args = args_for(image, out/'unused', True)
    args.worker, args.hypothesis = 'host-model', 'Native probe route freezes no CPU template'
    args.expect = 'class output'
    job = board.Queue(out/'queue').submit(args)
    assert job['options']['kind'] == 'usb-class' and 'template' not in job['options']
    passed('queue-native-contract-without-CPU-template')


def typed_inputs(out):
    folder = out/'typed'
    folder.mkdir()
    image = fixture(folder, generic=True)
    records = images.load('typed_records', ROOT/'apps/os/host.py')
    manifest = records.image_manifest(image)
    prefix = bytearray(592)
    struct.pack_into('<I', prefix, 400, 2)
    thermal = struct.pack('<3Q', records.SERVICE_MAGIC[records.THERMAL], 1, 808) + bytes(192)
    jpeg = bytes(prefix) + struct.pack('<3Q', 0x415337334a504132, 4, 856) + bytes(240)
    memory = struct.pack('<4Q', 1, 0x42200000, 0x800000, 0x7a000000) + b'MODEL ONLY'.ljust(32, b'.')
    assert len(memory) == 64 and len(jpeg) == 856
    services = {records.THERMAL: thermal, records.JPEG: jpeg, records.USB_MEMORY: memory}
    geometry_only = dict(services)
    geometry_only[records.USB_MEMORY] = memory[:32] + bytes(32)
    records.session_bytes(prefix, geometry_only, manifest['manifest'], manifest['sha256'],
                          0, records.BIND_SID, [b'old input'])
    passed('USB-memory-geometry-does-not-create-authority')
    original = records.session_bytes(prefix, services, manifest['manifest'], manifest['sha256'],
                                     0, records.BIND_SID, [b'old input'])
    (folder/'session.bin').write_bytes(original)
    args = args_for(image, out/'unused', True)
    args.kind, args.template = 'cpu-return', folder
    real_load = images.load
    def modeled_state(name, path):
        if name == 'runner_dev':
            return types.SimpleNamespace(state_check=lambda words: ([], {'apb1': 0, 'apb1_hz': 24000000,
                                                                          'configure_adc_clock': 1}))
        return real_load(name, path)
    info = {'generic': True, 'descriptor': {}, 'watchdog': {'mode': 0xb1, 'session_seconds': 20},
            'report_geometry': {}}
    with patch.object(images, 'load', side_effect=modeled_state):
        plan = images.prepare(args, info, {'words': {}}, folder/'prepared')
    raw = (folder/'prepared/session.bin').read_bytes()
    updated = records.session_decode(raw, manifest['manifest'], manifest['sha256'])
    assert updated['extents'][records.USB_MEMORY] == memory
    assert updated['extents'][records.JPEG][:592] == raw[:592] != original[:592]
    assert updated['extents'][records.JPEG][592:] == jpeg[592:]
    assert updated['extents'][records.INPUTS] != records.session_decode(original)['extents'][records.INPUTS]
    assert plan['format'] == 'generic'
    passed('typed-USB-JPEG-preserved-with-fresh-inputs-and-prefix')
    for name, pool, accepted in [('exact', (0x42200000, 0x800000), True),
                                 ('oversized', (0x42200000, 0xa00000), False)]:
        place = out/('pool-'+name)
        place.mkdir()
        target = fixture(place, generic=True, pool=pool)
        try:
            spans = records.backup_regions(target)
        except ValueError:
            assert not accepted
        else:
            assert accepted and {'address': 0x42200000, 'bytes': 0x800000} in spans
        passed('linked-backup-pool-'+name)


def wrappers(out):
    image_dir = out/'fixture'
    image_dir.mkdir()
    image = fixture(image_dir)
    args = args_for(image, out/'unused')
    info = images.inspect(image, 'probe')
    try:
        images.policy(info, args)
    except ValueError as error:
        assert 'coordinator' in str(error)
    else:
        raise AssertionError('Probe kind bypassed USB source review')
    passed('probe-kind-preserves-USB-approval')
    with contextlib.redirect_stdout(io.StringIO()):
        assert board.development(['usb-class', '--check-only', '--image', str(image),
            '--approved-by', 'coordinator', '--evidence', str(out/'development')]) == 0
    report = json.loads((out/'development/admission.json').read_text())
    assert report['admitted'] and not report['physical_access']
    passed('development-wrapper-offline-native-probe')
    for name, selected in [('CPU-control', False), ('generic-class', True)]:
        folder = out/name
        folder.mkdir()
        target = fixture(folder, generic=True, class_service=selected)
        info = images.inspect(target, 'cpu-return')
        assert info['usb_class'] == selected
        try:
            images.policy(info, args)
        except ValueError as error:
            assert selected and 'coordinator' in str(error)
        else:
            assert not selected
        passed('generic-kind-'+name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', '--out', dest='out', type=Path, required=True)
    parser.add_argument('--only', choices=('all', 'admission', 'typed', 'wrappers'), default='all')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    if args.only in ('all', 'admission'):
        admission(args.out)
    if args.only in ('all', 'typed'):
        typed_inputs(args.out)
    if args.only in ('all', 'wrappers'):
        wrappers(args.out)
    (args.out/'result.json').write_text(json.dumps({'physical_access': False, 'passed': ROWS}, indent=2)+'\n')


if __name__ == '__main__':
    main()
