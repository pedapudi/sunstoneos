#!/usr/bin/env python3
"""Check composed UART ownership and native compilation; preserve exact inputs."""
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
    'dma-platform/ccu-sun60iw2.c': '1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77',
    'board-kernel/ccu-sun60iw2-r.c': '9285e1b35bfae975e2c1257d477d843ad277916b9c555c12c5f88e1c7ea02554',
    'board-kernel/sun60iw2p1.dtsi': '1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa',
    'board-kernel/pinctrl-sun60iw2.c': 'ab05ddccecd152edf3fafd238ade22299aebe3448646f02f0551edef0c6e1ec7',
    'board-kernel/pinctrl-sun60iw2-r.c': '431fac7b767f5be39015cdf67fdd3dec230cbc1ebeb2fa0c0de08ca0f6e223a1',
    'camera-research/A733_User_Manual_V1.00.pdf': 'cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d',
    'board-documents/schematic-v1.2.pdf': '4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0',
}
for name, digest in references.items():
    assert hashlib.sha256((args.sources/name).read_bytes()).hexdigest() == digest, name
# Source declarations corroborate the manual's leaf gate offsets and bits.
main = (args.sources/'dma-platform/ccu-sun60iw2.c').read_text()
always = (args.sources/'board-kernel/ccu-sun60iw2-r.c').read_text()
for port in range(7):
    body = re.search(r'SUNXI_CCU_GATE\(uart'+str(port)+r'_clk,.*?\);', main, re.S)[0]
    assert re.search(r'0x0?[eE]'+f'{port*4:02x}'+r',\s*BIT\(0\)', body, re.I), body
for port in range(2):
    body = re.search(r'SUNXI_CCU_GATE\(r_uart'+str(port)+r'_clk,.*?\);', always, re.S)[0]
    assert re.search(r'0x018[cC],\s*BIT\('+str(port)+r'\)', body), body

commands = []
compiler = shutil.which('clang-21') or shutil.which('clang')
assert compiler

def run(command):
    command = [str(v) for v in command]
    commands.append(command)
    result = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    with (args.evidence/'run.log').open('a') as f:
        f.write(result.stdout)
    (args.evidence/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    print(result.stdout, end='')
    if result.returncode:
        raise SystemExit(result.returncode)

host_sources = ['drivers/uart/a733_uart_platform.c', 'drivers/uart/a733_uart.c',
                'drivers/clock/a733_clock_leases.c', 'drivers/pinctrl/a733_pins.c',
                'drivers/pinctrl/a733_pin_tables.c', 'checks/a733-uart-platform.c']
flags = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-Iinclude']
objects = []
for source in host_sources:
    obj = args.evidence/(pathlib.Path(source).stem+'-host.o')
    run(flags+['-MD', '-MF', obj.with_suffix('.d'), '-c', source, '-o', obj])
    objects.append(obj)
binary = args.evidence/'composed-uart'
run(flags+objects+['-o', binary])
run([binary])

kernel = args.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
smp_port = args.evidence/'smp-port'
smp_port.mkdir()
run(['patch', '--silent', '-o', smp_port/'portmacro.h',
     kernel/'portable/GCC/ARM_CR82/portmacro.h', root/'patches/freertos-smp-macro.patch'])
native = [compiler, '--target=aarch64-none-elf', '-std=c11', '-O2', '-ffreestanding',
          '-fno-builtin', '-Wall', '-Wextra', '-Werror']
for source in ['drivers/uart/a733_uart_platform.c', 'drivers/uart/a733_uart_platform_native.c']:
    variants = ['single', 'smp'] if source.endswith('_native.c') else ['single']
    for variant in variants:
        obj = args.evidence/(pathlib.Path(source).stem+'-'+variant+'.o')
        config = ['-Iplatform/qemu-smp'] if variant == 'smp' else []
        run(native+config+['-Iinclude', '-Iruntime/include', '-I'+str(kernel/'include'),
            '-I'+str(smp_port if variant == 'smp' else kernel/'portable/GCC/ARM_AARCH64_SRE'),
            '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-DINCLUDE_xTaskGetSchedulerState=1',
            '-DINCLUDE_xTaskGetCurrentTaskHandle=1', '-MD', '-MF', obj.with_suffix('.d'),
            '-c', source, '-o', obj])

paths = {args.sources/name for name in references}
paths.update(root/name for name in host_sources+['checks/a733-uart-platform.py'])
paths.add(pathlib.Path(compiler).resolve())
paths.add(kernel/'portable/GCC/ARM_CR82/portmacro.h')
paths.add(root/'patches/freertos-smp-macro.patch')
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
artifacts = [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in args.evidence.iterdir() if p.is_file() and p.name != 'artifacts.json']
(args.evidence/'artifacts.json').write_text(json.dumps(artifacts, indent=2)+'\n')
print('Composed ASan/UBSan UART ownership check and native single-core/SMP compilation passed.')
