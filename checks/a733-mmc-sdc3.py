#!/usr/bin/env python3
"""Check selected eMMC routes and ownership without replaying prior protocol suites."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sources', type=pathlib.Path, default=pathlib.Path.home()/'.cache/sunstoneos/sources')
parser.add_argument('--evidence', type=pathlib.Path, required=True)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
args.evidence.mkdir(parents=True, exist_ok=False)
references = {
    'mmc-sdc3/sunxi-mmc-v4p5x.c': 'f7b14c8dce66eefc222ac7d06b8aad041ea132c463a1f035a201dd940fb95f7f',
    'mmc-sdc3/sunxi-mmc-v4p5x.h': '8c3105a3fd28fda6655fc240130dc0f7f866d166c9e5d092af56b50b01b82275',
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

# Match named functions in the actual silicon table, not the FPGA branch or
# the generic pin manager's permissive function bitmap.
pinctrl = (args.sources/'board-kernel/pinctrl-sun60iw2.c').read_text().split('#else', 1)[1]
manual = (args.sources/'camera-research/A733_User_Manual_V1.00.txt').read_text()
route_table = manual.rsplit('Table 18-30 Port C Multiplex Function', 1)[1].split('Table 18-31', 1)[0]
assert 'Function2' in route_table and 'Function3' in route_table and 'Function4' in route_table
signals = {5: 'CLK', 6: 'CMD', 10: 'D0'}
for pin, signal in signals.items():
    entry = re.search(r'SUNXI_PIN\(SUNXI_PINCTRL_PIN\(C, '+str(pin)+r'\),(.*?)(?=SUNXI_PIN\(|static)', pinctrl, re.S).group(1)
    assert 'SUNXI_FUNCTION(0x2, "nand")' in entry
    assert 'SUNXI_FUNCTION(0x3, "sdc2")' in entry
    assert 'SUNXI_FUNCTION(0x4, "sdc3")' in entry
    assert re.search(r'^PC'+str(pin)+r'\s+I/O\s+NAND-\S+\s+SDC2-'+signal+r'\s+SDC3-'+signal, route_table, re.M)
dtsi = (args.sources/'board-kernel/sun60iw2p1.dtsi').read_text()
assert 'sdc3: sdmmc@4023000' in dtsi
node = dtsi.split('sdc3: sdmmc@4023000', 1)[1].split('};', 1)[0]
assert '0x04023000' in node and 'GIC_SPI 164 ' in node and 'sunxi-mmc-v5p6x' in node
ccu = (args.sources/'board-kernel/ccu-sun60iw2.c').read_text()
assert re.search(r'smhc3_clk, "smhc3",\s*smhc2_parents, 0x0D30', ccu)
assert re.search(r'smhc3_bus_clk, "smhc3-bus",\s*"dcxo",\s*0x0D3C, BIT\(0\)', ccu)
assert re.search(r'RST_BUS_SMHC3\].*0x0d3c, BIT\(16\)', ccu)
reference = (args.sources/'mmc-sdc3/sunxi-mmc-v4p5x.c').read_text()
assert '#define SUNXI_DES_SIZE_SDMMC_V4P5X\t(12)' in reference
assert '#define SUNXI_DES_SIZE_SDMMC_V5P6X\t(11)' in reference
for label in ['v4p6x', 'v5p6x']:
    body = reference.split('void sunxi_mmc_init_priv_'+label, 1)[1].split('EXPORT_SYMBOL_GPL', 1)[0]
    assert 'sunxi_mmc_clk_set_rate_for_sdmmc_v4p5x' in body
    assert 'mmc_readl(host, REG_SMCV)' in body
assert 'SMHC_NDMA_MODE_CTL' in manual and 'DMA_RX_EN' in manual and 'DMA_TX_EN' in manual
(args.evidence/'source-routes.json').write_text(json.dumps({
    'result': 'pass', 'named_sdc2_mux': 3, 'named_sdc3_mux': 4,
    'signals': signals, 'base': '0x04023000', 'irq': 196,
    'module': '0x02002d30', 'bus_reset': '0x02002d3c',
    'version_policy': 'Observe actual version; compatibility name never substitutes for admission'
}, indent=2)+'\n')

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

host_sources = ['drivers/storage/a733_mmc_platform.c', 'drivers/storage/sunxi_mmc.c',
                'drivers/clock/a733_clock_leases.c', 'drivers/power/a733_rail_leases.c',
                'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c',
                'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
                'checks/a733-mmc-sdc3.c']
flags = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-Iinclude', '-Ichecks', '-I'+str(args.evidence)]
kernel = args.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
native_sources = ['drivers/storage/a733_mmc_platform.c', 'drivers/storage/a733_mmc_platform_native.c',
                  'drivers/clock/a733_clock_leases.c']
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
paths.update(root/name for name in host_sources+native_sources+['checks/a733-mmc-sdc3.py','checks/sunxi-mmc.c','checks/mmc-clock-leases.c'])
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

binary = args.evidence/'selected-emmc'
run(flags+host_sources+['-o', binary], 'host-build.log')
run([binary], 'host.log')
scope = args.evidence/'sdc3-scopes'
run(flags+['drivers/clock/a733_clock_leases.c','checks/mmc-clock-leases.c','-o',scope], 'scope-build.log')
run([scope], 'scope.log')
for i, source in enumerate(native_sources):
    run(native+['-c', source, '-o', args.evidence/(pathlib.Path(source).stem+'.o')], f'native-build-{i}.log')
assert all(hashlib.sha256(pathlib.Path(r['path']).read_bytes()).hexdigest()==r['sha256'] for r in records)
(args.evidence/'result.json').write_text(json.dumps({
    'result': 'pass', 'physical': False, 'old_protocol_suites_replayed': False,
    'native_configuration': 'AArch64 SRE single-core headers',
    'checks': ['named pin/descriptor source comparison', 'selected eMMC provider composition under ASan/UBSan',
               'SDC3 and joint eMMC scope admission', 'affected native core/adapter/registry compilation'],
    'artifacts': [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                  for p in args.evidence.iterdir() if p.is_file()]}, indent=2)+'\n')
