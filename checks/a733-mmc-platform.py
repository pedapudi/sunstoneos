#!/usr/bin/env python3
"""Check new storage resource composition without replaying card protocol suites."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sources', type=pathlib.Path, default=pathlib.Path.home()/'.cache/sunstoneos/sources')
parser.add_argument('--evidence', type=pathlib.Path, required=True)
parser.add_argument('--alias-only', action='store_true', help='Only changed private-storage admission and affected core compilation')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
args.evidence.mkdir(parents=True, exist_ok=False)
references = {
    'board-kernel/sun60i-a733-orangepi-zero3w.dts': '31145367f604fd756f2c6a67447e5134e4be0ad5fae87672cfaa9b7c6d055a39',
    'board-kernel/sun60iw2p1.dtsi': '1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa',
    'board-kernel/pinctrl-sun60iw2.c': 'ab05ddccecd152edf3fafd238ade22299aebe3448646f02f0551edef0c6e1ec7',
    'board-kernel/ccu-sun60iw2.c': '1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77',
    'camera-research/A733_User_Manual_V1.00.pdf': 'cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d',
    'camera-research/A733_User_Manual_V1.00.txt': '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2',
    'board-documents/schematic-v1.2.pdf': '4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0',
    'u-boot-orangepi-b791be842935b27268ae3d00e943a9075495f30a/include/sunxi_power/pmu_axp8191.h': '7f8e6b5742b2906da8f448caedbd0de99f0300fa22814515cfed09612f19738f',
    'u-boot-orangepi-b791be842935b27268ae3d00e943a9075495f30a/drivers/sunxi_power/pmu_axp8191.c': 'e6faa518a123c374b8f9148327f5e4aeeb2d89eb2bc3e91dae9a35e06d964c59',
}
for name, digest in references.items():
    assert hashlib.sha256((args.sources/name).read_bytes()).hexdigest() == digest, name

# Reuse only the existing card model, without its tests or reference harness.
original = (root/'checks/sunxi-mmc.c').read_text()
assert original.count('struct model {') == original.count('static int barrier(void *ctx)') == 1
model = original[original.index('struct model {'):original.index('static int barrier(void *ctx)')]
(args.evidence/'mmc-card-model.h').write_text(model)
(args.evidence/'model-extraction.json').write_text(json.dumps({
    'original_sha256': hashlib.sha256(original.encode()).hexdigest(),
    'extracted_sha256': hashlib.sha256(model.encode()).hexdigest(),
    'original_tests_executed': False}, indent=2)+'\n')

commands = []
compiler = shutil.which('clang-21') or shutil.which('clang')
assert compiler

def run(command, name):
    command = [str(v) for v in command]
    commands.append(command)
    result = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (args.evidence/name).write_text(result.stdout)
    (args.evidence/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    print(result.stdout, end='')
    if result.returncode:
        raise SystemExit(result.returncode)

host_sources = ['drivers/storage/a733_storage_update.c', 'drivers/storage/storage_digest.c', 'drivers/storage/a733_mmc_platform.c', 'drivers/storage/sunxi_mmc.c',
                'drivers/clock/a733_clock_leases.c', 'drivers/power/a733_rail_leases.c',
                'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c',
                'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
                'checks/a733-mmc-platform.c']
flags = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-Iinclude', '-Ichecks', '-I'+str(args.evidence)]
kernel = args.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
native_sources = ['drivers/storage/a733_mmc_platform.c']
if not args.alias_only:
    native_sources.append('drivers/storage/a733_mmc_platform_native.c')
native = [compiler, '--target=aarch64-none-elf', '-std=c11', '-O2', '-ffreestanding',
          '-fno-builtin', '-Wall', '-Wextra', '-Werror', '-fstack-usage', '-Iinclude',
          '-Iruntime/include', '-I'+str(kernel/'include'),
          '-I'+str(kernel/'portable/GCC/ARM_AARCH64_SRE'),
          '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-DINCLUDE_xTaskGetSchedulerState=1',
          '-DINCLUDE_xTaskGetCurrentTaskHandle=1']
for i, source in enumerate(host_sources):
    run(flags+['-M', '-MF', args.evidence/f'host-{i}.d', source], f'host-dependencies-{i}.log')
for i, source in enumerate(native_sources):
    run(native+['-M', '-MF', args.evidence/f'native-{i}.d', source], f'native-dependencies-{i}.log')
paths = {args.sources/name for name in references}
paths.update(root/name for name in host_sources+native_sources+['checks/a733-mmc-platform.py','checks/sunxi-mmc.c'])
paths.add(pathlib.Path(compiler).resolve())
for dep in args.evidence.glob('*.d'):
    names = dep.read_text().replace('\\\n', ' ').split(':', 1)[1].split()
    paths.update((root/name).resolve() for name in names)
records = []
for path in sorted(paths):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    target = args.evidence/'inputs'/digest
    target.parent.mkdir(exist_ok=True)
    if not target.exists():
        target.write_bytes(data)
    records.append({'path': str(path), 'sha256': digest, 'snapshot': str(target.relative_to(args.evidence))})
(args.evidence/'inputs.json').write_text(json.dumps(records, indent=2)+'\n')

binary = args.evidence/'composed-mmc'
run(flags+host_sources+['-o', binary], 'host-build.log')
run([binary]+(['--alias'] if args.alias_only else []), 'host.log')
for i, source in enumerate(native_sources):
    run(native+['-c', source, '-o', args.evidence/(pathlib.Path(source).stem+'.o')], f'native-build-{i}.log')
assert all(hashlib.sha256(pathlib.Path(r['path']).read_bytes()).hexdigest()==r['sha256'] for r in records)
(args.evidence/'result.json').write_text(json.dumps({
    'result': 'pass', 'physical': False, 'old_protocol_suites_replayed': False,
    'native_configuration': 'AArch64 SRE single-core headers',
    'checks': (['actual private-storage admission under ASan/UBSan', 'changed native core compilation']
               if args.alias_only else ['actual-provider composition under ASan/UBSan', 'native core/adapter compilation']),
    'artifacts': [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                  for p in args.evidence.iterdir() if p.is_file()]}, indent=2)+'\n')
