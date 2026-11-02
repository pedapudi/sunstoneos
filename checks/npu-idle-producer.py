#!/usr/bin/env python3
"""Execute the actual NPU idle benchmark producer for each policy with modeled devices.

The idle benchmark producer (apps/npu-finite/queue-client.c with idle-client.h)
runs natively in Unicorn: the baseline, control and gating policies each print
their complete report, which the production decoder parses. The scheduler, the
shared deadline facility, the queue and the power owner are modeled by
checks/npu-latency-client.c; the receipt writer then runs and the monitor,
platform and receipt envelope is validated. Modeled time establishes no board
timing, and the gating counters are modeled, not produced by the owner.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import struct
import subprocess
from harness import Image, environment, system_register_access
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/npu-finite/queue-client.c', 'apps/npu-finite/idle-client.h', 'apps/npu-finite/startup.h',
                'apps/npu-finite/host.py', 'apps/npu-finite/build.py', 'checks/npu-latency-client.c',
                'apps/npu-finite/return.c', 'apps/npu-finite/latency-stats.h', 'apps/npu-interrupt/host.py',
                'include/npu_runtime_queue.h', 'include/npu_idle.h', 'include/a733_npu_idle.h']
POLICIES = {2: 'baseline', 0: 'control', 1: 'gate'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, default=Path.home() / '.cache/sunstoneos/evidence/npu-finite-quiescence-20261018/native')
    parser.add_argument('--cc', type=Path, default=Path('/usr/bin/clang-21'))
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    parser.add_argument('--policy', action='append', type=int, choices=sorted(POLICIES))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('idle_producer_host', ROOT / 'apps/npu-interrupt/host.py')
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
    flags += ['-DSUNSTONE_NPU_RUNTIME_RETURN=1', '-DSUNSTONE_NPU_STARTUP_ONLY=1', '-DSUNSTONE_NPU_LATENCY=1',
              '-DSUNSTONE_NPU_QUEUE_TIMING=1', '-DSUNSTONE_NPU_IDLE_BENCH=1', '-DSUNSTONE_DEADLINE_SINGLE_CORE=1',
              '-DINCLUDE_xTaskGetIdleTaskHandle=1', '-include', str(ROOT / 'apps/npu-finite/latency-stats.h'), '-Werror']
    cache_object = output / 'actual-cache.o'
    cache_command = flags + ['-Daarch64_device_cache=fixture_actual_cache', '-Daarch64_device_cache_stride=fixture_actual_stride',
                             '-c', str(ROOT / 'arch/aarch64/device_memory.c'), '-o', str(cache_object)]
    built = subprocess.run(cache_command, cwd=unit['directory'], env=environment(), capture_output=True, text=True)
    (output / 'build-actual-cache.log').write_text(built.stdout + built.stderr)
    built.check_returncode()
    references = ['01020304', '01020304']
    results = []
    for policy in args.policy or sorted(POLICIES):
        name = POLICIES[policy]
        image_path = output / f'producer-{name}.elf'
        command = flags + ['-DSUNSTONE_NPU_LATENCY_QUEUE=1', '-DSUNSTONE_NPU_LATENCY_SEQUENCE=0', '-DSUNSTONE_NPU_DEPENDENT_CHAIN=0',
                           '-DSUNSTONE_NPU_CHAIN_REPEATS=1', f'-DSUNSTONE_NPU_IDLE_GATE={policy}',
                           str(ROOT / 'checks/npu-latency-client.c'), str(ROOT / 'drivers/storage/storage_digest.c'),
                           str(ROOT / 'runtime/memory.c'), str(ROOT / 'drivers/npu/a733_npu_sequence.c'), str(cache_object),
                           '-nostdlib', f'--ld-path={args.lld}', '-Wl,-Ttext=0x41000000', '-Wl,-e,fixture_entry',
                           '-Wl,--gc-sections', '-Wl,--undefined=npu_latency_receipt_exit',
                           '-Wl,--defsym=__npu_clean_start=0x41200000', '-Wl,--defsym=__device_pool_end=0x41f00000',
                           '-Wl,--defsym=__npu_platform_log=0x42000000', '-o', str(image_path)]
        built = subprocess.run(command, cwd=unit['directory'], env=environment(), capture_output=True, text=True)
        (output / f'build-{name}.log').write_text(built.stdout + built.stderr)
        built.check_returncode()
        image = Image(image_path, layout_names=())
        machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        machine.mem_map(0x40000000, 0x04000000)
        machine.mem_map(0x200000, 0x10000)
        for at, data in image.parts:
            machine.mem_write(at, data)
        value = lambda symbol, width=4: int.from_bytes(machine.mem_read(image.address(symbol), width), 'little')
        state = {'stop': None}
        hooks = {image.address(n): n for n in ('platform_exit', 'platform_panic', 'vTaskSuspend', 'npu_finite_smc')}
        frequency_sites, privileged_sites = {}, {}
        for at, data in image.parts:
            for offset in range(0, len(data) - 3, 4):
                word = int.from_bytes(data[offset:offset + 4], 'little')
                if word & 0xffffffe0 == 0xd53be000:
                    frequency_sites[at + offset] = word & 31
                elif system_register_access(word):
                    privileged_sites[at + offset] = word

        def code(cpu, at, size, unused):
            if at in hooks:
                state['stop'] = hooks[at]
                state['status'] = cpu.reg_read(UC_ARM64_REG_X0)
                cpu.emu_stop()
            elif at in frequency_sites:
                cpu.reg_write(UC_ARM64_REG_X0 + frequency_sites[at], 24000000)
                cpu.reg_write(UC_ARM64_REG_PC, at + 4)
            elif at in privileged_sites:
                word = privileged_sites[at]
                values = {0xd5384240: 4, 0xd5381000: 5, 0xd53b0020: 0x04040000}
                if word & 0xffffffe0 in values:
                    cpu.reg_write(UC_ARM64_REG_X0 + (word & 31), values[word & 0xffffffe0])
                cpu.reg_write(UC_ARM64_REG_PC, at + 4)
        machine.hook_add(UC_HOOK_CODE, code)
        machine.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
        machine.reg_write(UC_ARM64_REG_LR, 0x43fff000)
        machine.emu_start(image.address('fixture_entry'), 0, count=20000000000)
        assert state['stop'] == 'platform_exit' and state['status'] == 0, (name, state, struct.unpack('<6Q', machine.mem_read(image.address('npu_latency_failure'), 48)))
        text = bytes(machine.mem_read(image.address('fixture_log'), value('fixture_log_bytes', 8))).decode()
        (output / f'producer-{name}.log').write_text(text)
        assert len(text) <= 16127, f'report of {len(text)} bytes exceeds the platform log'
        geometry = {'runtime': True, 'references': references,
                    'latency': {'mode': 'queue', 'measured_jobs': 1000, 'warmup_jobs': 1, 'model': 0, 'sequence_repeats': 0,
                                'doorbell_timing': True,
                                'idle': {'policy': name if name != 'gate' else 'gate', 'cycles': [100, 100, 30], 'gaps_us': [9000, 11000, 100000],
                                         'threshold_us': 10000, 'resume_budget_us': 100, 'windows': 5, 'window_jobs': 12000}}}
        geometry['latency']['idle']['policy'] = {'baseline': 'baseline', 'control': 'control', 'gate': 'gate'}[name]
        parsed = host.latency_report(text, 0, 480000000, geometry)
        idle = parsed['idle']
        assert idle['functional_success'] and parsed['functional_success'] and idle['policy'] == geometry['latency']['idle']['policy']
        assert [w['jobs'] for w in idle['windows']] == [12000] * 5 and len(idle['groups']) == 3
        assert [g['gates'] for g in idle['groups']] == ([0, 100, 30] if policy == 1 else [0, 0, 0]), idle['groups']
        assert parsed['jobs'] == 61231
        # The receipt writer and the monitor, platform and receipt envelope.
        machine.mem_write(0x42000000 + 24, struct.pack('<Q', len(text)))
        state['stop'] = None
        machine.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
        machine.reg_write(UC_ARM64_REG_X0, 0)
        machine.emu_start(image.address('npu_latency_receipt_exit'), 0, count=10000000)
        assert state == {'stop': 'npu_finite_smc', 'status': 0}, state
        receipt = bytes(machine.mem_read(image.address('npu_finite_receipt'), 88))
        now = value('fixture_now', 8)
        monitor = struct.pack('<16Q', 0x415337334e505231, 7, 8, 5, 0, host.STARTUP_CLIENT[host.LATENCY][1], 1000000, 1, now, *([0] * 7))
        platform = bytearray(16384)
        struct.pack_into('<16Q', platform, 0, 0x534b4552, 5, 0, len(text), 0, 0, 0, 0, 1000000, now, *([0] * 6))
        platform[256:256 + len(text)] = text.encode()
        prepared = {'epoch': 7, 'generation': 8, 'lease': 9, 'execution_profile': host.LATENCY,
                    'geometry': dict(geometry, startup_only=True, clean_start=0x41200000, clean_end=0x41f00000)}
        complete = interrupt.validate_reports(monitor, bytes(platform), receipt, prepared)
        assert complete['interrupt_completions'] == 61232 and complete['interrupt_stopped']
        marker = bytes(machine.mem_read(image.address('npu_idle_return_profile'), 104))
        latency_marker = bytes(machine.mem_read(image.address('npu_latency_return_profile'), 64))
        startup_marker = struct.unpack('<4Q', machine.mem_read(image.address('npu_startup_return_profile'), 32))
        names = {'npu_latency_return_profile', 'npu_idle_return_profile', 'npu_runtime_queue_power_start', 'npu_idle_gate',
                 'npu_idle_resume', 'sunstone_deadline_wait_until', 'sunstone_deadline_irq', 'npu_finite_deadline_setup'}
        extension = host.latency_extension(startup_marker, names, lambda n: marker if n == 'npu_idle_return_profile' else latency_marker)
        assert extension['idle'] == geometry['latency']['idle'], extension
        results.append({'policy': name, 'passed': True, 'log_bytes': len(text), 'jobs': parsed['jobs'],
                        'windows': [w['duration_ticks'] for w in idle['windows']], 'groups': [g['gates'] for g in idle['groups']]})
        print('PASS', name, len(text), 'bytes', flush=True)
    (output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False, 'cases': results,
        'inputs': {n: sha(ROOT / n) for n in CHECK_INPUTS + ['checks/npu-idle-producer.py']},
        'limits': 'Modeled time, scheduler, queue and power owner. No board timing or gating evidence.'}, indent=2) + '\n')
    print('PASS', len(results), 'actual idle benchmark producer policies')


if __name__ == '__main__':
    main()
