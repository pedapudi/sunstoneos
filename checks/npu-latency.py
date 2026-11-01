#!/usr/bin/env python3
"""Execute both finite NPU latency producers with modeled devices and RTOS."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import struct
import subprocess
from harness import Image, environment, refused, system_register_access
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/npu-finite/queue-client.c', 'apps/npu-finite/startup.h',
                'apps/npu-finite/host.py', 'apps/npu-finite/build.py', 'checks/npu-latency-client.c',
                'include/npu_runtime_queue.h', 'include/npu_queue.h',
                'apps/npu-finite/return.c', 'apps/npu-finite/return.h', 'apps/npu-interrupt/host.py']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, default=Path.home() / '.cache/sunstoneos/evidence/npu-finite-quiescence-20261018/native')
    parser.add_argument('--cc', type=Path, default=Path('/usr/bin/clang-21'))
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('latency_interrupt_host', ROOT / 'apps/npu-interrupt/host.py')
    interrupt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(interrupt)
    host = interrupt.finite
    unit = next(u for u in json.loads((args.baseline / 'compile_commands.json').read_text())
                if u['file'].endswith('/apps/npu-convolution/main.c') and 'a733-npu-convolution-single-core.dir' in u['command'])
    old_root = unit['file'][:-len('/apps/npu-convolution/main.c')]
    flags = [v.replace(old_root, str(ROOT)) for v in shlex.split(unit['command'])]
    for option in ('-o', '-c'):
        at = flags.index(option)
        del flags[at:at + 2]
    flags[0] = str(args.cc)
    flags += ['-DSUNSTONE_NPU_RUNTIME_RETURN=1', '-DSUNSTONE_NPU_STARTUP_ONLY=1',
              '-DSUNSTONE_NPU_LATENCY=1', '-Werror']
    commands, results = [], []
    for mode in (0, 1):
        image_path = output / f'producer-{mode}.elf'
        command = flags + [f'-DSUNSTONE_NPU_LATENCY_QUEUE={mode}', str(ROOT / 'checks/npu-latency-client.c'),
                          str(ROOT / 'drivers/storage/storage_digest.c'), str(ROOT / 'runtime/memory.c'),
                          '-nostdlib', f'--ld-path={args.lld}', '-Wl,-Ttext=0x41000000',
                          '-Wl,-e,fixture_entry', '-Wl,--gc-sections', '-Wl,--undefined=npu_latency_receipt_exit',
                          '-Wl,--defsym=__npu_clean_start=0x41200000', '-Wl,--defsym=__device_pool_end=0x41f00000',
                          '-Wl,--defsym=__npu_platform_log=0x42000000', '-o', str(image_path)]
        built = subprocess.run(command, cwd=unit['directory'], env=environment(), capture_output=True, text=True)
        (output / f'build-{mode}.log').write_text(built.stdout + built.stderr)
        commands.append(command)
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        built.check_returncode()
        image = Image(image_path, layout_names=())
        for case in ((0, 1, 2, 3) if not mode else (0, 1, 2, 3, 4, 5)):
            machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            machine.mem_map(0x40000000, 0x04000000)
            machine.mem_map(0x200000, 0x10000)  # ELF header segment from the fixture link.
            for at, data in image.parts:
                machine.mem_write(at, data)
            def value(name, width=4):
                return int.from_bytes(machine.mem_read(image.address(name), width), 'little')
            machine.mem_write(image.address('fixture_case'), struct.pack('<I', case))
            state = {'stop': None}
            hooks = {image.address(n): n for n in ('platform_exit', 'platform_panic', 'vTaskSuspend', 'npu_finite_smc')}
            frequency_sites, privileged_sites = {}, set()
            for at, data in image.parts:
                for offset in range(0, len(data) - 3, 4):
                    word = int.from_bytes(data[offset:offset + 4], 'little')
                    if word & 0xffffffe0 == 0xd53be000:
                        frequency_sites[at + offset] = word & 31
                    elif system_register_access(word):
                        privileged_sites.add(at + offset)
            def code(cpu, at, size, unused):
                if at in hooks:
                    state['stop'] = hooks[at]
                    state['status'] = cpu.reg_read(UC_ARM64_REG_X0)
                    cpu.emu_stop()
                elif at in frequency_sites:
                    cpu.reg_write(UC_ARM64_REG_X0 + frequency_sites[at], 24000000)
                    cpu.reg_write(UC_ARM64_REG_PC, at + 4)
                elif at in privileged_sites:
                    cpu.reg_write(UC_ARM64_REG_PC, at + 4)
            machine.hook_add(UC_HOOK_CODE, code)
            machine.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
            machine.reg_write(UC_ARM64_REG_LR, 0x43fff000)
            machine.emu_start(image.address('fixture_entry'), 0, count=30000000)
            assert state['stop'] is not None, (mode, case, 'instruction budget', hex(machine.reg_read(UC_ARM64_REG_PC)))
            text = bytes(machine.mem_read(image.address('fixture_log'), value('fixture_log_bytes', 8))).decode()
            (output / f'producer-{mode}-case-{case}.log').write_text(text)
            if case <= 1:
                assert state == {'stop': 'platform_exit', 'status': case}, state
                assert len(text) <= 16127 and value('checked') == 1000
                geometry = {'runtime': True, 'references': ['01020304', '01020304'],
                            'latency': {'mode': 'queue' if mode else 'legacy', 'measured_jobs': 1000, 'warmup_jobs': 1, 'model': 0}}
                parsed = host.latency_report(text, case, 480000000, geometry)
                assert parsed['functional_success'] == (case == 0)
                assert parsed['timing']['warmup_excluded'] and len(parsed['timing']['job_duration_ticks']) == 1000
                if case:
                    assert parsed['first_mismatch'] == {'job': 4, 'offset': 0, 'byte': 9}
                # Run the linked native receipt writer before checking the
                # complete monitor/platform/receipt envelope and IRQ wrapper.
                machine.mem_write(0x42000000 + 24, struct.pack('<Q', len(text)))
                state['stop'] = None
                machine.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
                machine.reg_write(UC_ARM64_REG_X0, case)
                machine.emu_start(image.address('npu_latency_receipt_exit'), 0, count=10000000)
                assert state == {'stop': 'npu_finite_smc', 'status': case}, state
                receipt = bytes(machine.mem_read(image.address('npu_finite_receipt'), 88))
                (output / f'producer-{mode}-case-{case}-receipt.bin').write_bytes(receipt)
                now = value('fixture_now', 8)
                monitor = struct.pack('<16Q', 0x415337334e505231, 7, 8, 5, case,
                                      host.STARTUP_CLIENT[host.LATENCY][1], 1000000, 1, now, *([0] * 7))
                platform = bytearray(16384)
                struct.pack_into('<16Q', platform, 0, 0x534b4552, 5, case, len(text), 0, 0, 0, 0, 1000000, now, *([0] * 6))
                platform[256:256 + len(text)] = text.encode()
                prepared = {'epoch': 7, 'generation': 8, 'lease': 9, 'execution_profile': host.LATENCY,
                            'geometry': dict(geometry, startup_only=True, clean_start=0x41200000, clean_end=0x41f00000)}
                complete = interrupt.validate_reports(monitor, bytes(platform), receipt, prepared)
                assert complete['interrupt_completions'] == 1002 and complete['interrupt_stopped']
                marker = bytes(machine.mem_read(image.address('npu_latency_return_profile'), 64))
                startup_marker = struct.unpack('<4Q', machine.mem_read(image.address('npu_startup_return_profile'), 32))
                assert host.latency_extension(startup_marker, {'npu_latency_return_profile'}, lambda name: marker) == geometry['latency']
                lines = text.splitlines(keepends=True)
                fields_at = lines.index('NPU_LATENCY=\n') + 1
                for index, replacement, reason in ((3, 0, 'warmup'), (9, 1, 'quantiles'),
                                                    (18, 1 if not mode else 999, 'fence')):
                    changed = list(lines)
                    changed[fields_at + index] = f'0x{replacement:016x}\n'
                    refused(lambda: host.latency_report(''.join(changed), case, 480000000, geometry), reason)
                checksum_at = lines.index('NPU_DURATION_SHA256=\n') + 1
                changed = list(lines)
                changed[checksum_at] = ('0' if changed[checksum_at][0] != '0' else '1') + changed[checksum_at][1:]
                refused(lambda: host.latency_report(''.join(changed), case, 480000000, geometry), 'checksum')
                malformed = bytearray(marker)
                struct.pack_into('<Q', malformed, 32, 0)
                refused(lambda: host.latency_extension(startup_marker, {'npu_latency_return_profile'}, lambda name: malformed), 'extension differs')
                for changed, reason in (
                        (text.replace('NPU_STAGE=WARMUP', 'NPU_STAGE=SKIPPED', 1), 'stage'),
                        (text.replace('NPU_IRQ_COMPLETIONS=0x00000000000003ea', 'NPU_IRQ_COMPLETIONS=0x00000000000003e9'), 'stage'),
                        (text.replace('NPU_DURATION_SHA256=\n', 'NPU_DURATION_SHA256=\n0', 1), 'stage'),
                        (text.replace('NPU_INFERENCE=MATCH;', 'NPU_INFERENCE=MISMATCH;', 1) if not case else text.replace('NPU_INFERENCE=MISMATCH;', 'NPU_INFERENCE=MATCH;', 1), 'status')):
                    refused(lambda: host.latency_report(changed, case, 480000000, geometry), reason)
                results.append({'mode': mode, 'case': case, 'passed': True, 'log_bytes': len(text), 'timing': parsed['timing']})
            else:
                assert state['stop'] == 'vTaskSuspend', state
                fault = struct.unpack('<6Q', machine.mem_read(image.address('npu_latency_failure'), 48))
                expected = 40 if case == 2 else 12 if not mode else {3: 43, 4: 44, 5: 37}[case]
                assert fault[0] == expected and fault[1] == ((1 << 64) - 1 if case == 2 else 3), fault
                assert not value('fixture_closed') and value('checked') == (0 if case == 2 else 3)
                if mode and case >= 3:
                    assert value('fixture_releases') == 3
                    assert fault[2:4] == ((3, 3) if case == 3 else (4, 3) if case == 4 else (4, 4)), fault
                results.append({'mode': mode, 'case': case, 'passed': True, 'first_failure': list(fault)})
    (output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False, 'cases': results,
        'inputs': {name: sha(ROOT / name) for name in CHECK_INPUTS + ['checks/npu-latency.py']},
        'images': {p.name: sha(p) for p in output.glob('*.elf')}}, indent=2) + '\n')
    print('PASS', len(results), 'actual latency producer/parser and retained-failure cases')


if __name__ == '__main__':
    main()
