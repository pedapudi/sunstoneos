#!/usr/bin/env python3
"""Check only radio entropy buffering and ownership over the actual CE service."""
import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--freertos', type=Path, required=True)
    parser.add_argument('--tcp', type=Path, required=True)
    parser.add_argument('--cc', default='/usr/bin/clang-21')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    freertos, tcp = args.freertos.resolve(), args.tcp.resolve()
    commands = []

    def run(name, command):
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1'})
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'{name}: exit {result.returncode}; see saved log')
        return result.stdout

    try:
        # This accessor is declared in the already pinned FreeRTOS+TCP source.
        ip_header = tcp / 'source/include/FreeRTOS_IP.h'
        assert 'TaskHandle_t FreeRTOS_GetIPTaskHandle( void );' in ip_header.read_text()
        common = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                  '-DINCLUDE_xTaskGetCurrentTaskHandle=1', '-DINCLUDE_xTaskGetSchedulerState=1',
                  '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-Iinclude',
                  '-I' + str(freertos / 'include')]
        sources = ['checks/a733-radio-entropy.c', 'drivers/net/a733_radio_entropy.c',
                   'drivers/crypto/a733_ce_entropy.c', 'drivers/crypto/a733_ce_session.c',
                   'drivers/crypto/a733_ce.c', 'drivers/clock/a733_clock_leases.c',
                   'runtime/device_memory_pool.c']
        objects = []
        for index, source in enumerate(sources):
            obj = out / f'host-{index}.o'
            run(f'host-{index}', [args.cc, *common, '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-I' + str(freertos / 'portable/GCC/ARM_AARCH64_SRE'),
                '-MMD', '-MF', str(out / f'host-{index}.d'), '-c', source, '-o', str(obj)])
            objects.append(obj)
        model = out / 'radio-entropy-model'
        run('host-link', [args.cc, '-fsanitize=address,undefined', *map(str, objects), '-o', str(model)])
        # The changed adapter is compiled against both supported native task ABIs.
        # No unchanged CE driver scenario or native model is executed again.
        smp = out / 'smp-port'
        smp.mkdir()
        macro = (freertos / 'portable/GCC/ARM_CR82/portmacro.h').read_text()
        original = '#define portMEMORY_BARRIER()    __asm volatile ( "" ::: "memory" )'
        assert macro.count(original) == 1
        (smp / 'portmacro.h').write_text(macro.replace(original,
            '#define portMEMORY_BARRIER()    __asm volatile ( "dmb ish" ::: "memory" )'))
        for name, port, config in [('single', freertos / 'portable/GCC/ARM_AARCH64_SRE', ROOT / 'include'),
                                   ('cached', smp, ROOT / 'platform/a733-smp-cached')]:
            run('native-' + name, [args.cc, '--target=aarch64-none-elf', '-mcpu=cortex-a55',
                '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-I' + str(config),
                *common, '-I' + str(port), '-MMD', '-MF', str(out / (name + '.d')),
                '-c', 'drivers/net/a733_radio_entropy.c', '-o', str(out / (name + '.o'))])
        # Atomic session-fault publication changes one shared header. Compile
        # each direct native consumer once; do not replay their protocol cases.
        native_consumers = ['a733_ce_session', 'a733_ce_entropy', 'a733_ce_hash',
                            'a733_ce_cipher', 'a733_ce_native']
        for name in native_consumers:
            run('native-' + name, [args.cc, '--target=aarch64-none-elf', '-mcpu=cortex-a55',
                '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', *common,
                '-MMD', '-MF', str(out / (name + '.d')),
                '-c', 'drivers/crypto/' + name + '.c', '-o', str(out / (name + '.o'))])
        dependencies = {ROOT / n for n in sources}
        dependencies.update([Path(__file__).resolve(), ip_header, tcp / 'source/FreeRTOS_IP.c',
                             ROOT / 'patches/freertos-smp-macro.patch',
                             freertos / 'portable/GCC/ARM_CR82/portmacro.h'])
        for path in out.glob('*.d'):
            text = path.read_text().replace('\\\n', ' ')
            for name in shlex.split(text.split(':', 1)[1]):
                p = Path(name)
                dependencies.add(p.resolve() if p.is_absolute() else (ROOT / p).resolve())
        manifest = {}
        for index, source in enumerate(sorted(dependencies)):
            target = out / 'inputs' / str(index) / source.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            manifest[str(source)] = {'sha256': sha(target), 'snapshot': str(target.relative_to(out))}
        (out / 'inputs.json').write_text(json.dumps(manifest, indent=2) + '\n')
        artifacts = {str(p.relative_to(out)): sha(p) for p in [model, *objects, out / 'single.o', out / 'cached.o',
                     *[out / (name + '.o') for name in native_consumers]]}
        (out / 'artifacts.json').write_text(json.dumps(artifacts, indent=2) + '\n')
        (out / 'compiler.json').write_text(json.dumps({'path': args.cc,
            'sha256': sha(Path(args.cc).resolve())}, indent=2) + '\n')
        output = run('model', [str(model)])
        assert 'PASS radio entropy:' in output
        (out / 'result.json').write_text(json.dumps({'passed': True, 'elf_sha256': sha(model),
            'scope': 'Actual adapter, CE entropy/session/controller, clock registry and pool; modeled counter, task identity, mutex dispatch, CE MMIO and DMA bytes. Native adapter compiled against single-core and cached eight-core headers; five affected CE native consumers compiled once.',
            'limits': 'No physical entropy qualification, RF, network application execution or performance measurement.'}, indent=2) + '\n')
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')


if __name__ == '__main__':
    main()
