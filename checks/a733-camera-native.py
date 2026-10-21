#!/usr/bin/env python3
"""Run the focused native camera composition and compile its changed consumers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home()/'.cache/sunstoneos'
SOURCES = [
    'drivers/camera/ov5640.c', 'drivers/camera/a733_camera_platform.c',
    'drivers/camera/a733_csi_receiver.c', 'drivers/camera/a733_camera_route.c',
    'drivers/camera/a733_camera_dma.c', 'drivers/camera/a733_camera_capture.c',
    'drivers/camera/a733_camera_native.c', 'drivers/iommu/a733_iommu.c',
    'drivers/i2c/a733_twi.c', 'drivers/i2c/a733_twi_native.c',
    'drivers/clock/a733_clock_leases.c', 'drivers/clock/a733_media_clock.c',
    'drivers/power/a733_rail_leases.c', 'drivers/power/a733_pmic.c',
    'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c',
    'drivers/pinctrl/a733_pin_tables.c', 'runtime/device_memory_pool.c']
CHANGED = ['drivers/camera/a733_camera_native.c', 'drivers/camera/a733_camera_capture.c',
           'drivers/camera/a733_camera_dma.c', 'drivers/i2c/a733_twi.c',
           'drivers/i2c/a733_twi_native.c']

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence', type=Path, required=True)
    p.add_argument('--clang', default='/usr/bin/clang-21')
    p.add_argument('--cases', default='0,1,2,3,4,5,6,7,8,9,10,11,12,13,14')
    p.add_argument('--skip-native', action='store_true')
    a = p.parse_args()
    out = a.evidence.resolve();out.mkdir(parents=True, exist_ok=False)
    compiler = Path(shutil.which(a.clang)).resolve()
    kernel = CACHE/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
    source = CACHE/'sources/u-boot-orangepi-b791be842935b27268ae3d00e943a9075495f30a'
    inputs = {};commands = []
    def save(path):
        path = path.resolve();data = path.read_bytes();h = hashlib.sha256(data).hexdigest()
        if str(path) in inputs:
            assert inputs[str(path)] == h, path
        inputs[str(path)] = h
        target = out/'inputs'/h;target.parent.mkdir(exist_ok=True)
        if not target.exists():target.write_bytes(data)
        (out/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    def run(name, argv):
        argv = list(map(str, argv))
        r = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True)
        commands.append(dict(name=name, argv=argv, returncode=r.returncode, stdout=r.stdout, stderr=r.stderr))
        (out/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
        if r.returncode:raise RuntimeError(f'{name}: {r.stdout}{r.stderr}')
        return r.stdout
    # Archive before execution. Dependencies discovered by the compiler are
    # added afterward; these private inputs must not change during the batch.
    for name in [*SOURCES, 'checks/a733-camera-native.c', 'checks/a733-camera-native.py',
                 'checks/a733-pmic.c', 'checks/a733-camera-dma.c', 'checks/a733-camera-capture.c',
                 'drivers/camera/ov5640_tables.h',
                 'platform/a733-smp-cached/FreeRTOSConfig.h', 'patches/freertos-smp-macro.patch',
                 'docs/camera-native.md', 'docs/camera-dma.md']:
        save(ROOT/name)
    for directory in [ROOT/'include', kernel/'include', kernel/'portable/GCC/ARM_AARCH64_SRE']:
        for path in directory.glob('*.h'):save(path)
    save(compiler)
    for path in [source/'include/sunxi_i2c.h', source/'drivers/i2c/sunxi_i2c.c',
                 CACHE/'sources/camera-research/A733_User_Manual_V1.00.txt',
                 CACHE/'sources/camera/bsp/drivers/vin/vin-video/vin_core.c']:
        save(path)
    text = (source/'include/sunxi_i2c.h').read_text()
    for name, value in {'I2C_START_TRANSMIT':8, 'I2C_RESTART_TRANSMIT':16,
            'I2C_ADDRWRITE_ACK':24, 'I2C_ADDRREAD_ACK':64, 'I2C_DATAWRITE_ACK':40,
            'I2C_READY':248, 'I2C_DATAREAD_NACK':88}.items():
        match = re.search(r'#define\s+'+name+r'\s+(0x[0-9a-fA-F]+)', text)
        assert match and int(match[1],16)==value
    includes = ['-Iinclude', '-I'+str(kernel/'include'), '-I'+str(kernel/'portable/GCC/ARM_AARCH64_SRE')]
    defs = ['-DINCLUDE_xSemaphoreGetMutexHolder=1', '-DINCLUDE_xTaskGetCurrentTaskHandle=1',
            '-DINCLUDE_xTaskGetSchedulerState=1']
    common = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', *defs]
    host = [*common, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-DSUNSTONE_CAMERA_MODEL=1', *includes]
    objects = []
    for name in ['checks/a733-camera-native.c', *SOURCES]:
        obj = out/(Path(name).stem+'-host.o');objects.append(obj)
        extra = ['-Wno-unused-function'] if name.startswith('checks/') else []
        run('host-'+Path(name).stem, [*host, *extra, '-MD', '-MF', obj.with_suffix('.d'), '-c', name, '-o', obj])
    for dep in out.glob('*-host.d'):
        for name in dep.read_text().replace('\\\n',' ').split(':',1)[1].split():save(ROOT/name)
    binary = out/'camera-native'
    run('host-link', [*host, *objects, '-o', binary])
    save(binary)
    results = {case:run('scenario-'+case, [binary, case]) for case in a.cases.split(',')}
    if not a.skip_native:
        port = out/'smp-port';port.mkdir()
        save(kernel/'portable/GCC/ARM_CR82/portmacro.h')
        run('port-header', ['patch', '--silent', '-o', port/'portmacro.h',
            kernel/'portable/GCC/ARM_CR82/portmacro.h', ROOT/'patches/freertos-smp-macro.patch'])
        native = [*common, '--target=aarch64-none-elf', '-O2', '-ffreestanding', '-fno-builtin',
                  '-mgeneral-regs-only', '-mstrict-align', '-mno-outline-atomics',
                  '-Iplatform/a733-smp-cached', '-Iinclude', '-Iruntime/include',
                  '-I'+str(kernel/'include'), '-I'+str(port)]
        for name in CHANGED:
            obj = out/(Path(name).stem+'-native.o')
            run('native-'+Path(name).stem, [*native, '-MD', '-MF', obj.with_suffix('.d'), '-c', name, '-o', obj])
    for name in ['checks/a733-camera-dma.c', 'checks/a733-camera-capture.c']:
        obj=out/(Path(name).stem+'-compat.o')
        run('compat-'+Path(name).stem, [*common, '-O1', '-Iinclude', '-MD', '-MF', obj.with_suffix('.d'), '-c', name, '-o', obj])
    for dep in out.glob('*.d'):
        for name in dep.read_text().replace('\\\n',' ').split(':',1)[1].split():save(ROOT/name)
    for name, digest in inputs.items():assert hashlib.sha256(Path(name).read_bytes()).hexdigest()==digest
    artifacts = {path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in out.iterdir()
                 if path.is_file() and path.suffix in ['.o','.d']}
    (out/'results.json').write_text(json.dumps(dict(status='PASS', cases=results, inputs=inputs,
        artifacts=artifacts, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        scope='Actual native camera and resource controllers; modeled MMIO, task identity, cache and boot proof. Changed production units compiled for eight-core AArch64.',
        limits='No physical camera, supply, reset, drain, prefetch, recovery or coherence qualification. No old component suites executed.'),indent=2)+'\n')
    print('PASS focused native camera composition and changed-unit compile')

if __name__ == '__main__':main()
