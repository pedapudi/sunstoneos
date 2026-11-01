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
                'apps/npu-finite/return.c', 'apps/npu-finite/return.h', 'apps/npu-interrupt/host.py',
                'include/a733_npu_sequence.h', 'drivers/npu/a733_npu_sequence.c']
CHECK_INPUTS += ['arch/aarch64/device_memory.c', 'include/aarch64_device_memory.h']
CHECK_INPUTS += ['apps/npu-finite/latency-stats.h']
CHECK_INPUTS += ['checks/npu-latency-counters.c','apps/npu-finite/dependent-client.h']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, default=Path.home() / '.cache/sunstoneos/evidence/npu-finite-quiescence-20261018/native')
    parser.add_argument('--cc', type=Path, default=Path('/usr/bin/clang-21'))
    parser.add_argument('--mode', action='append', choices=('legacy', 'queue', 'sequence', 'chain', 'chain-repeat'), help='Run only named diagnostic modes')
    parser.add_argument('--case',action='append',type=int,help='Run only named producer cases')
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
              '-DSUNSTONE_NPU_LATENCY=1', '-include', str(ROOT / 'apps/npu-finite/latency-stats.h'), '-Werror']
    commands, results = [], []
    kernel = next(Path(u['file']) for u in json.loads((args.baseline / 'compile_commands.json').read_text())
                  if u['file'].endswith('/tasks.c') and 'a733-npu-convolution-single-core.dir' in u['command'])
    counters_image = output / 'counters.elf'
    counter_command = flags + [f'-DSUNSTONE_LATENCY_KERNEL="{kernel}"', str(ROOT / 'checks/npu-latency-counters.c'),
        '-nostdlib', f'--ld-path={args.lld}', '-Wl,-Ttext=0x41000000', '-Wl,-e,fixture_counter_entry',
        '-Wl,--gc-sections', '-o', str(counters_image)]
    built = subprocess.run(counter_command, cwd=unit['directory'], env=environment(), capture_output=True, text=True)
    (output / 'build-counters.log').write_text(built.stdout + built.stderr)
    commands.append(counter_command)
    built.check_returncode()
    counters = Image(counters_image, layout_names=())
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    machine.mem_map(0x40000000, 0x04000000)
    machine.mem_map(0x200000, 0x10000)
    for at, data in counters.parts:
        machine.mem_write(at, data)
    counter_state = []
    def counter_code(cpu, at, size, unused):
        if at in (counters.address('fixture_counter_done'), counters.address('platform_panic')):
            counter_state.append(at)
            cpu.emu_stop()
    machine.hook_add(UC_HOOK_CODE, counter_code)
    machine.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
    machine.emu_start(counters.address('fixture_counter_entry'), 0, count=100000)
    assert counter_state == [counters.address('fixture_counter_done')]
    counter_values = struct.unpack('<5Q', machine.mem_read(counters.address('fixture_counter_results'), 40))
    assert counter_values == (1300, 400, 1700, 1700, 1800), counter_values
    results.append({'case': 'actual-kernel-running-blocked-and-waiting-runtime-query', 'passed': True, 'counters': list(counter_values)})
    cache_object = output / 'actual-cache.o'
    cache_command = flags + ['-Daarch64_device_cache=fixture_actual_cache',
                             '-Daarch64_device_cache_stride=fixture_actual_stride',
                             '-c', str(ROOT / 'arch/aarch64/device_memory.c'), '-o', str(cache_object)]
    built = subprocess.run(cache_command, cwd=unit['directory'], env=environment(), capture_output=True, text=True)
    (output / 'build-actual-cache.log').write_text(built.stdout + built.stderr)
    commands.append(cache_command)
    built.check_returncode()
    for mode in (i for i, name in enumerate(('legacy', 'queue', 'sequence', 'chain', 'chain-repeat')) if not args.mode or name in args.mode):
        repeats = 32 if mode == 2 else 0
        dependent = mode in (3,4)
        chain_repeats = 1000 if mode==4 else 1
        extra_jobs = bool(repeats)+3*dependent*chain_repeats
        image_path = output / f'producer-{mode}.elf'
        command = flags + [f'-DSUNSTONE_NPU_LATENCY_QUEUE={int(bool(mode))}', f'-DSUNSTONE_NPU_LATENCY_SEQUENCE={repeats}', f'-DSUNSTONE_NPU_DEPENDENT_CHAIN={int(dependent)}',f'-DSUNSTONE_NPU_CHAIN_REPEATS={chain_repeats}', str(ROOT / 'checks/npu-latency-client.c'),
                          str(ROOT / 'drivers/storage/storage_digest.c'), str(ROOT / 'runtime/memory.c'),
                          str(ROOT / 'drivers/npu/a733_npu_sequence.c'), str(cache_object),
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
        for case in ((0, 1, 2, 3) if not mode else (0, 1, 2, 3, 4, 5, 8, 9, 10, 11) if mode == 1 else (0, 6, 7) if mode==2 else (0, 6, 7, 12) if mode==3 else (0, 13, 14, 15, 16)):
            if args.case and case not in args.case:continue
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
            machine.emu_start(image.address('fixture_entry'), 0, count=30000000)
            assert state['stop'] is not None, (mode, case, 'instruction budget', hex(machine.reg_read(UC_ARM64_REG_PC)))
            text = bytes(machine.mem_read(image.address('fixture_log'), value('fixture_log_bytes', 8))).decode()
            (output / f'producer-{mode}-case-{case}.log').write_text(text)
            if case in (0, 1, 7, 10, 15, 16):
                status = int(case in (1, 7, 15, 16))
                assert state == {'stop': 'platform_exit', 'status': status}, state
                assert len(text) <= 16127 and value('checked') == 1000
                geometry = {'runtime': True, 'references': ['01020304', '09080706' if dependent else '01020304'],
                            'latency': {'mode': 'queue' if mode else 'legacy', 'measured_jobs': 1000, 'warmup_jobs': 1, 'model': 0, 'sequence_repeats': repeats, 'doorbell_timing': True, **({'dependent_chain':True,**({'chain_repeats':chain_repeats} if chain_repeats!=1 else {})} if dependent else {})}}
                parsed = host.latency_report(text, status, 480000000, geometry)
                publish = parsed['timing']['submit_to_doorbell']
                assert publish['samples'] == 1000
                expected_publish = 1000 if not mode else 5
                assert publish['latency_ticks'] == {'p50': expected_publish+49, 'p99': expected_publish+98, 'max': expected_publish+99}
                assert publish['sum_ticks'] == 1000 * expected_publish + 49500
                assert parsed['functional_success'] == (not status)
                if case == 10:
                    phases = parsed['timing']['caller_phases']
                    assert phases['submit_elapsed_ticks'] + phases['irq_driver_elapsed_ticks'] > sum(parsed['timing']['job_duration_ticks'])
                assert parsed['timing']['warmup_excluded'] and len(parsed['timing']['job_duration_ticks']) == 1000
                counters = parsed['timing']['task_runtime']
                assert 0 < counters['client_ticks'] < parsed['timing']['duration_ticks']
                assert counters['worker_ticks'] == (200000 if not mode else 0)
                if case == 1:
                    assert parsed['first_mismatch'] == {'job': 4, 'offset': 0, 'byte': 9}
                if repeats:
                    assert parsed['repeated_sequence']['repetitions'] == 32
                    assert parsed['repeated_sequence']['functional_success'] == (case != 7)
                if dependent:
                    chain=parsed['dependent_chain']
                    assert len(chain)==2 and [p['queue_records'] for p in chain]==[2*chain_repeats,chain_repeats]
                    assert [p['submitted_before_wait'] for p in chain]==[2,1]
                    assert chain[1]['intermediate_device_completion_events']==0
                    assert all(p['interstage_cpu_copy_bytes']==0 for p in chain)
                    assert [p['functional_success'] for p in chain]==([False,False] if case==7 else [False,True] if case==15 else [True,False] if case==16 else [True,True])
                    if mode==4:
                        assert parsed['jobs']==4001 and [p['logical_stages'] for p in chain]==[2000,2000]
                        assert all(p['repetitions']==p['pending_checks']==1000 for p in chain)
                        assert all(p['chain_duration_ticks']['min']<p['chain_duration_ticks']['max'] for p in chain)
                        assert [p['chain_duration_ticks']['sum'] for p in chain]==[5118000,5064500]
                        assert all(p['task_runtime']=={'client_ticks':30000,'worker_ticks':0,'combined_ticks':30000} for p in chain)
                        if case in (15,16):
                            assert chain[case-15]['first_mismatch_repetition']==499
                            assert chain[case-15]['first_mismatch']=={'stage':1,'offset':0,'byte':0}
                # Run the linked native receipt writer before checking the
                # complete monitor/platform/receipt envelope and IRQ wrapper.
                machine.mem_write(0x42000000 + 24, struct.pack('<Q', len(text)))
                state['stop'] = None
                machine.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
                machine.reg_write(UC_ARM64_REG_X0, status)
                machine.emu_start(image.address('npu_latency_receipt_exit'), 0, count=10000000)
                assert state == {'stop': 'npu_finite_smc', 'status': status}, state
                receipt = bytes(machine.mem_read(image.address('npu_finite_receipt'), 88))
                (output / f'producer-{mode}-case-{case}-receipt.bin').write_bytes(receipt)
                now = value('fixture_now', 8)
                monitor = struct.pack('<16Q', 0x415337334e505231, 7, 8, 5, status,
                                      host.STARTUP_CLIENT[host.LATENCY][1], 1000000, 1, now, *([0] * 7))
                platform = bytearray(16384)
                struct.pack_into('<16Q', platform, 0, 0x534b4552, 5, status, len(text), 0, 0, 0, 0, 1000000, now, *([0] * 6))
                platform[256:256 + len(text)] = text.encode()
                prepared = {'epoch': 7, 'generation': 8, 'lease': 9, 'execution_profile': host.LATENCY,
                            'geometry': dict(geometry, startup_only=True, clean_start=0x41200000, clean_end=0x41f00000)}
                complete = interrupt.validate_reports(monitor, bytes(platform), receipt, prepared)
                assert complete['interrupt_completions'] == 1002 + extra_jobs and complete['interrupt_stopped']
                marker = bytes(machine.mem_read(image.address('npu_latency_return_profile'), 64))
                chain_marker=bytes(machine.mem_read(image.address('npu_dependent_return_profile'),64)) if dependent else b''
                startup_marker = struct.unpack('<4Q', machine.mem_read(image.address('npu_startup_return_profile'), 32))
                assert host.latency_extension(startup_marker, ({'npu_latency_return_profile','npu_dependent_return_profile'} if dependent else {'npu_latency_return_profile'}), lambda name: marker if name=='npu_latency_return_profile' else chain_marker) == geometry['latency']
                assert host.startup_client({'execution_profile':host.LATENCY,'geometry':geometry})[2:]==(1001+extra_jobs,1002+extra_jobs)
                if dependent:
                    assert struct.unpack('<8Q',chain_marker)==(0x4e50554445503031,1 if chain_repeats==1 else 2,0,0,1,0,2,chain_repeats)
                    for word,replacement in ((1,3),(7,0),(7,chain_repeats+1)):
                        invalid=bytearray(chain_marker);struct.pack_into('<Q',invalid,word*8,replacement)
                        refused(lambda:host.latency_extension(startup_marker,{'npu_latency_return_profile','npu_dependent_return_profile'},lambda name:marker if name=='npu_latency_return_profile' else invalid),'immutable extension')
                lines = text.splitlines(keepends=True)
                fields_at = lines.index('NPU_LATENCY=\n') + 1
                for index, replacement, reason in ((3, 0, 'warmup'), (9, 1, 'quantiles'),
                                                    (18, 1 if not mode else 999, 'fence')):
                    changed = list(lines)
                    changed[fields_at + index] = f'0x{replacement:016x}\n'
                    refused(lambda: host.latency_report(''.join(changed), status, 480000000, geometry), reason)
                publish_at = lines.index('NPU_SUBMIT_TO_DOORBELL=\n') + 1
                for index, replacement in ((0, 999), (1, 0), (2, 1), (3, 0), (4, 0)):
                    changed = list(lines)
                    changed[publish_at + index] = f'0x{replacement:016x}\n'
                    refused(lambda: host.latency_report(''.join(changed), status, 480000000, geometry), 'doorbell')
                if dependent:
                    chain_at = lines.index('NPU_DEPENDENT_CHAIN=\n' if chain_repeats==1 else 'NPU_DEPENDENT_REPEATED_CHAIN=\n') + 1
                    for index, replacement in ((2,1),(3,1),(4,0),(5,1),(10,8))+(((12,999),(13,999),(14,999),(15,0),(16,0),(17,24000001),(18,1<<63),(20,1000)) if mode==4 else ()):
                        changed=list(lines);changed[chain_at+index]=f'0x{replacement:016x}\n'
                        refused(lambda:host.latency_report(''.join(changed),status,480000000,geometry),'Dependent chain')
                checksum_at = lines.index('NPU_DURATION_SHA256=\n') + 1
                changed = list(lines)
                changed[checksum_at] = ('0' if changed[checksum_at][0] != '0' else '1') + changed[checksum_at][1:]
                refused(lambda: host.latency_report(''.join(changed), status, 480000000, geometry), 'checksum')
                malformed = bytearray(marker)
                struct.pack_into('<Q', malformed, 32, 0)
                refused(lambda: host.latency_extension(startup_marker, ({'npu_latency_return_profile','npu_dependent_return_profile'} if dependent else {'npu_latency_return_profile'}), lambda name: malformed if name=='npu_latency_return_profile' else chain_marker), 'extension differs')
                for changed, reason in (
                        (text.replace('NPU_STAGE=WARMUP', 'NPU_STAGE=SKIPPED', 1), 'stage'),
                        (text.replace(f'NPU_IRQ_COMPLETIONS=0x{1002+extra_jobs:016x}', 'NPU_IRQ_COMPLETIONS=0x00000000000003e9'), 'stage'),
                        (text.replace('NPU_DURATION_SHA256=\n', 'NPU_DURATION_SHA256=\n0', 1), 'stage'),
                        (text.replace('NPU_INFERENCE=MATCH;', 'NPU_INFERENCE=MISMATCH;', 1) if not status else text.replace('NPU_INFERENCE=MISMATCH;', 'NPU_INFERENCE=MATCH;', 1), 'status')):
                    refused(lambda: host.latency_report(changed, status, 480000000, geometry), reason)
                results.append({'mode': mode, 'case': case, 'passed': True, 'log_bytes': len(text), 'timing': parsed['timing'],'dependent_chain':parsed['dependent_chain']})
            else:
                assert state['stop'] == 'vTaskSuspend', state
                fault = struct.unpack('<6Q', machine.mem_read(image.address('npu_latency_failure'), 48))
                expected = 40 if case == 2 else 12 if not mode else {3: 43, 4: 44, 5: 37, 6: 73 if dependent else 55, 12: 62, 13: 73, 14: 73, 8: 31, 9: 31, 11: 61}[case]
                assert fault[0] == expected and fault[1] == ((1 << 64) - 1 if case in (2, 8, 9) else 1000 if case in (6,12) else 1998 if case==13 else 3499 if case==14 else 0 if case == 11 else 3), fault
                assert not value('fixture_closed') and value('checked') == (0 if case in (2, 8, 9) else 1000 if case in (6,12) else 1000 if case in (13,14) else 0 if case == 11 else 3)
                if case == 6:
                    assert value('fixture_releases') == 1000 and fault[2:4] == ((2,0) if dependent else (1,0))
                elif case in (13,14):
                    expected_releases=1998 if case==13 else 3499
                    assert value('fixture_releases')==expected_releases
                    assert fault[2:4]==((1000,998) if case==13 else (500,499)),fault
                    assert value('fixture_returned')==(2 if case==13 else 4)
                elif mode and 3 <= case <= 5:
                    assert value('fixture_releases') == 3
                    assert fault[2:4] == ((3, 3) if case == 3 else (4, 3) if case == 4 else (4, 4)), fault
                results.append({'mode': mode, 'case': case, 'passed': True, 'first_failure': list(fault)})
    (output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False, 'cases': results,
        'inputs': {name: sha(ROOT / name) for name in CHECK_INPUTS + ['checks/npu-latency.py']},
        'images': {p.name: sha(p) for p in output.glob('*.elf')}}, indent=2) + '\n')
    print('PASS', len(results), 'actual latency producer/parser and retained-failure cases')


if __name__ == '__main__':
    main()
