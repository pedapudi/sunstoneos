#!/usr/bin/env python3
"""Check the finite NPU failure return: the production return writer, the host validator and the runner inputs.

The return writer (apps/npu-finite/return.c) runs natively in Unicorn with the failure return linked. A client
requested failure and a refused success return both end at status 2 with a receipt and a failure record when the NPU
is quiesced, and at the refusal stop (status 4, no receipt) when it is not. The report validator then accepts the
quiesced return and refuses a stale receipt, a missing receipt, a mismatched status, a missing record and an image
without the failure-return marker. The runner's capture reads the same bytes, restores DRAM and reports a failed
result, and its admission flags only an image that links the failure return. Modeled hardware; no board is used.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import struct
import subprocess
import sys
from harness import Image, environment, refused, skip_crumbs, system_register_access
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/npu-finite/return.c', 'apps/npu-finite/return.h', 'apps/npu-finite/monitor.c', 'apps/npu-finite/host.py',
                'apps/npu-interrupt/host.py', 'apps/npu-finite/startup.h', 'checks/npu-failure-return.c',
                'tools/board.py', 'tools/board_images.py']


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--failure-image', type=Path, required=True, help='finite image built with the failure return')
    parser.add_argument('--plain-image', type=Path, required=True, help='finite image built without it')
    parser.add_argument('--baseline', type=Path, default=Path.home() / '.cache/sunstoneos/evidence/npu-finite-quiescence-20261018/native')
    parser.add_argument('--cc', type=Path, default=Path('/usr/bin/clang-21'))
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    interrupt = load('failure_return_host', 'apps/npu-interrupt/host.py')
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
              '-DSUNSTONE_NPU_FAILURE_RETURN=1', '-DSUNSTONE_CRUMBS=1', '-Werror']
    image_path = output / 'return-writer.elf'
    command = flags + [str(ROOT / 'checks/npu-failure-return.c'), '-nostdlib', f'--ld-path={args.lld}', '-Wl,-Ttext=0x41000000',
                       '-Wl,-e,fixture_run', '-Wl,--gc-sections',
                       '-Wl,--defsym=__npu_clean_start=0x41200000', '-Wl,--defsym=__device_pool_end=0x41f00000',
                       '-Wl,--defsym=__npu_platform_log=0x42000000', '-o', str(image_path)]
    built = subprocess.run(command, cwd=unit['directory'], env=environment(), capture_output=True, text=True)
    (output / 'build.log').write_text(built.stdout + built.stderr)
    built.check_returncode()
    image = Image(image_path, layout_names=())
    cases = []

    def run(status, quiet, ready, stride=64, now=1000):
        machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        machine.mem_map(0x40000000, 0x04000000)
        machine.mem_map(0x200000, 0x10000)
        for at, data in image.parts:
            machine.mem_write(at, data)
        crumbs, failures = [], []
        skip_crumbs(machine, image, crumbs, failures)
        put = lambda symbol, value, width=4: machine.mem_write(image.address(symbol), int(value).to_bytes(width, 'little'))
        put('fixture_quiet', quiet);put('fixture_ready', ready);put('fixture_stride', stride);put('fixture_now', now, 8)
        stop = {}
        smc = image.address('npu_finite_smc')
        frequency_sites, privileged_sites = {}, {}
        for at, data in image.parts:
            for offset in range(0, len(data) - 3, 4):
                word = int.from_bytes(data[offset:offset + 4], 'little')
                if word & 0xffffffe0 == 0xd53be000:
                    frequency_sites[at + offset] = word & 31
                elif system_register_access(word):
                    privileged_sites[at + offset] = word

        def code(cpu, at, size, unused):
            if at == smc:
                stop['status'] = cpu.reg_read(UC_ARM64_REG_X0)
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
        machine.reg_write(UC_ARM64_REG_X0, status)
        machine.emu_start(image.address('fixture_run'), 0, count=50000000)
        assert 'status' in stop, ('return writer did not reach the monitor call', status, quiet, ready)
        length = int.from_bytes(machine.mem_read(0x42000018, 8), 'little')
        log = bytes(machine.mem_read(0x42000100, length)).decode()
        receipt = bytes(machine.mem_read(image.address('npu_finite_receipt'), 88))
        return stop['status'], log, receipt, crumbs, failures, machine

    # Positive: a client-requested failure and a refused success return of a quiesced NPU.
    for label, status, ready, reason in (('client-requested failure', 2, 1, 9), ('refused success return', 0, 0, 2)):
        smc_status, log, receipt, crumbs, failures, machine = run(status, 1, ready)
        assert smc_status == 2, (label, smc_status)
        assert log.endswith(f'NPU_FAILURE=0x{reason:016x}\n'), (label, log)
        r = struct.unpack('<11Q', receipt)
        assert r[4:6] == (2, 3) and r[10] == len(log), r
        assert failures and failures[-1][1] == (0xc00 | reason), failures
        cases.append(f'{label}: status 2, receipt and record')
        if label == 'client-requested failure':
            ok_log, ok_receipt = log, receipt

    # Negative: the NPU is not quiesced, so the return keeps the refusal stop and publishes no receipt or record.
    for label, status, ready in (('failure return of a running NPU', 2, 1), ('refused success of a running NPU', 0, 0)):
        smc_status, log, receipt, crumbs, failures, machine = run(status, 0, ready)
        assert smc_status == 4 and 'NPU_FAILURE' not in log and receipt == bytes(88), (label, smc_status, log)
        assert failures and failures[-1][1] >> 8 == 0xd, failures
        cases.append(label + ': refusal stop 4, no receipt, no record')
    for label, kwargs in (('stride', {'stride': 32}), ('late', {'now': 480001000})):
        smc_status, log, receipt, *rest = run(2, 1, 1, **kwargs)
        assert smc_status == 4 and receipt == bytes(88), (label, smc_status)
        cases.append(f'failure return refused with a {label} violation')
    smc_status, log, receipt, *rest = run(0, 1, 1)
    assert smc_status == 0 and 'NPU_FAILURE' not in log, 'a ready success return changed'
    cases.append('a ready success return is unchanged')

    # The report validator.
    epoch, generation, lease = 7, 8, 9
    now = 2000
    geometry = {'runtime': True, 'references': ['01020304', '01020304'], 'failure_return': True, 'clean_start': 0x41200000,
                'clean_end': 0x41f00000, 'startup_only': True,
                'latency': {'mode': 'queue', 'measured_jobs': 1000, 'warmup_jobs': 1, 'model': 0, 'sequence_repeats': 0, 'doorbell_timing': True}}
    prepared = {'epoch': epoch, 'generation': generation, 'lease': lease, 'execution_profile': host.LATENCY, 'geometry': geometry}
    marker = host.startup_client(prepared)

    def reports(receipt=ok_receipt, log=ok_log, status=2, monitor_epoch=epoch):
        r = list(struct.unpack('<11Q', receipt))
        monitor = struct.pack('<16Q', 0x415337334e505231, monitor_epoch, generation, 5, status, marker[1], 1000, 1, now, *([0] * 7))
        platform = bytearray(16384)
        struct.pack_into('<16Q', platform, 0, 0x534b4552, 5, status, len(log), 0, 0, 0, 0, 1000, now, *([0] * 6))
        platform[256:256 + len(log)] = log.encode()
        return monitor, bytes(platform), receipt

    good = bytearray(ok_receipt)
    # The model receipt holds the model identity: bind it to the validator's identity words.
    struct.pack_into('<4Q', good, 0, host.STARTUP_CLIENT[host.LATENCY][0], epoch, generation, lease)
    struct.pack_into('<2Q', good, 32, 2, 3)
    struct.pack_into('<Q', good, 72, 480000000 + 1000)
    good = bytes(good)
    # The deadline word binds the armed time: use the validator's own relation.
    armed = 1000 - 1
    struct.pack_into('<Q', good := bytearray(good), 72, armed + 480000000)
    good = bytes(good)
    result = interrupt.validate_reports(*reports(good), prepared)
    assert result['failure_return'] and result['status'] == 2 and result['failure']['reason'] == 9, result
    cases.append('validator accepts the quiesced failure return')
    stale = bytearray(good);struct.pack_into('<Q', stale, 8, epoch + 1)
    refused(lambda: interrupt.validate_reports(*reports(bytes(stale)), prepared), 'Receipt identity', ValueError)
    cases.append('validator refuses a stale receipt')
    refused(lambda: interrupt.validate_reports(*reports(bytes(88)), prepared), 'Receipt identity', ValueError)
    cases.append('validator refuses a missing receipt')
    wrong = bytearray(good);struct.pack_into('<2Q', wrong, 32, 1, 2)
    refused(lambda: interrupt.validate_reports(*reports(bytes(wrong)), prepared), 'Receipt identity', ValueError)
    cases.append('validator refuses a receipt whose status differs from the monitor')
    refused(lambda: interrupt.validate_reports(*reports(good, log=ok_log.replace('NPU_FAILURE=', 'NPU_FAILURX=')), prepared),
            'failure return record', ValueError)
    cases.append('validator refuses a log without the failure record')
    plain = dict(prepared, geometry={k: v for k, v in geometry.items() if k != 'failure_return'})
    refused(lambda: interrupt.validate_reports(*reports(good), plain), 'No matched normal monitor completion', ValueError)
    cases.append('validator refuses status 2 from an image without the failure-return marker')

    # The runner: admission flags only the failure-return image; capture restores DRAM and reports a failed result.
    sys.path.insert(0, str(ROOT / 'tools'))
    images = load('failure_return_images', 'tools/board_images.py')
    for image_file, expected in ((args.failure_image, True), (args.plain_image, False)):
        manifest = json.loads((image_file.parent / 'manifest.json').read_text())
        info = images.inspect(image_file, 'npu', None, ROOT, manifest)
        assert info['failure_return'] is expected, (image_file, info['failure_return'])
    cases.append('runner admission flags only an image that links the failure return')
    uploads = []
    backup = output / 'dram-backup.bin';backup.write_bytes(b'saved dram')
    monitor, platform, receipt = reports(good)
    store = {0x42000000: monitor, 0x41200000: platform, 0x41200400: receipt}

    class Owner:
        evidence = output
        record = {}

        def read(self, address, size, path):
            data = {128: monitor, 16384: platform, 88: receipt}[size]
            Path(path).write_bytes(data)
            return data

        def upload(self, address, path):
            uploads.append(Path(path).read_bytes())

        def journal(self):
            pass
    plan = {'format': 'npu', 'finite': host, 'geometry': dict(geometry, log=0x42000000, receipt=0x41200400), 'dram_backup': backup,
            'dram_backup_sha256': hashlib.sha256(backup.read_bytes()).hexdigest(), 'epoch': epoch, 'generation': generation,
            'lease': lease, 'execution_profile': host.LATENCY, 'finite_prepared': prepared}
    plan.update({k: prepared[k] for k in ('epoch', 'generation', 'lease', 'execution_profile')})
    plan['geometry'] = dict(geometry, log=0x42000000, receipt=0x41200400)
    owner = Owner()
    interrupt_plan = dict(plan)
    captured = images.capture(owner, interrupt_plan, None)
    assert captured['valid'] is False and captured['report']['failure_return'] and uploads == [b'saved dram'], captured
    assert owner.record['dram_restored'] is True
    cases.append('runner capture restores DRAM and returns a failed, failure-return result')

    (output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False, 'cases': cases,
        'inputs': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in CHECK_INPUTS},
        'limits': 'Modeled hardware; the monitor and the runner outcome mapping are not executed.'}, indent=2) + '\n')
    print('PASS', len(cases), 'finite NPU failure return cases')


if __name__ == '__main__':
    main()
