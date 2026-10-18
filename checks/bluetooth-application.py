#!/usr/bin/env python3
"""Execute only the new Bluetooth application composition after native image review/link."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['output', 'freertos', 'tcp', 'hci-reference', 'image', 'build-dir']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cc', default='/usr/bin/clang-21')
    parser.add_argument('--lld', default='/home/sunil/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    parser.add_argument('--resume', action='store_true')
    a = parser.parse_args();out = a.output.resolve();out.mkdir(parents=True, exist_ok=a.resume)
    state_path = out / 'steps.json'
    steps = json.loads(state_path.read_text()) if state_path.exists() else {}
    inputs = json.loads((out / 'inputs.json').read_text()) if (out / 'inputs.json').exists() else {}
    tool = Path(a.cc).resolve()
    tools = {str(tool): sha(tool), str(Path(sys.executable).resolve()): sha(Path(sys.executable).resolve())}
    for configured in [a.lld, '/usr/lib/llvm-21/bin/llvm-objcopy']:
        resolved = Path(configured).resolve();tools[str(resolved)] = sha(resolved)
    native_captured = False
    if (out / 'tools.json').exists():
        assert tools == json.loads((out / 'tools.json').read_text())
    (out / 'tools.json').write_text(json.dumps(tools, indent=2) + '\n')

    def capture(path):
        path = Path(path).resolve();identity = sha(path);target = out / 'inputs' / identity / path.name
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            shutil.copyfile(path, target)
        inputs[str(path)] = {'sha256': identity, 'snapshot': str(target.relative_to(out))}

    def preserve():
        nonlocal native_captured
        for path in [Path(__file__), ROOT / 'CMakeLists.txt', ROOT / 'cmake/a733-bluetooth.cmake',
                     *[p for p in (ROOT / 'apps/bluetooth').iterdir() if p.is_file()]]:
            capture(path)
        for path in (out / 'launcher').glob('*'):
            if path.is_file():
                capture(path)
        for dep in [*out.glob('*.d'), *(out / 'launcher').glob('*.d'), *([] if native_captured else a.build_dir.rglob('*.o.d'))]:
            capture(dep)
            for name in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]):
                path = Path(name)
                if not path.is_absolute():
                    path = (a.build_dir if dep.is_relative_to(a.build_dir) else ROOT) / path
                if path.is_file():
                    capture(path)
        for path in ([] if native_captured else a.build_dir.rglob('*')):
            if path.is_file() and (path.name in ['CMakeCache.txt', 'compile_commands.json', 'link.txt', 'flags.make', 'build.make'] or
                    path.suffix in ['.ld'] or path.parent.name == 'bluetooth-application'):
                capture(path)
        native_captured = True
        (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        (out / 'artifacts.json').write_text(json.dumps({p.name: sha(p) for p in out.iterdir() if p.is_file() and p.suffix in ['.o', '.elf', '.bin']}, indent=2) + '\n')

    def run(name, command, artifact=None, dep=None):
        command = list(map(str, command));old = steps.get(name)
        if old and old.get('returncode') == 0 and old['command'] == command and all(sha(Path(p)) == h for p, h in old.get('dependencies', {}).items()):
            if artifact is None or sha(artifact) == old['artifact_sha256']:
                return
        # Keep prior artifact/log bytes before an affected step can replace them.
        for path in [artifact, out / (name + '.log')]:
            if path is not None and path.is_file():
                target = out / 'prior' / sha(path) / path.name;target.parent.mkdir(parents=True, exist_ok=True);shutil.copyfile(path, target)
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1'})
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        record = {'command': command, 'returncode': result.returncode, 'dependencies': {str(tool): sha(tool)}}
        for argument in command[1:]:
            p = Path(argument)
            if p.is_file() and p != artifact:
                record['dependencies'][str(p.resolve())] = sha(p)
        if artifact is not None and artifact.exists():
            record['artifact_sha256'] = sha(artifact)
        if dep is not None and dep.exists():
            for item in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]):
                p = Path(item).resolve() if Path(item).is_absolute() else (ROOT / item).resolve()
                record['dependencies'][str(p)] = sha(p)
        elif name.endswith('-run'):
            record['dependencies'][command[0]] = sha(Path(command[0]))
        steps[name] = record;state_path.write_text(json.dumps(steps, indent=2) + '\n')
        preserve();result.check_returncode()

    try:
        assert sha(a.hci_reference / 'source-hci.h') == '9da36dad169c8c02ba3a676a3c884527e4efb7d152b975b131f33aaa59347bb7'
        reference = a.hci_reference / 'bt-ref.h'
        original = json.loads((a.hci_reference / 'results.json').read_text())
        assert sha(reference) == original['artifacts']['bt-ref.h']
        for path in [reference, a.hci_reference / 'source-hci.h', a.hci_reference / 'results.json', a.image]:
            capture(path)
        shutil.copyfile(reference, out / 'bt-ref.h')
        native_image = out / 'bluetooth-native.elf';shutil.copyfile(a.image, native_image)
        manifest_path = a.build_dir / 'bluetooth-application/image-manifest.json'
        manifest = json.loads(manifest_path.read_text());capture(manifest_path)
        assert manifest['target'] == 'sunstone-a733-bluetooth' and manifest['execution_profile'] == 'bluetooth-supervised-v1'
        assert manifest['final_elf'] == sha(native_image)
        assert manifest['build_id'] == (a.build_dir / 'bluetooth-application/build-id.txt').read_text().strip()
        common = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DSUNSTONE_AIC8800_RSN=1',
                  '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-ffunction-sections', '-fdata-sections',
                  '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Inetwork/include', '-Iinclude',
                  '-I' + str(a.freertos / 'include'), '-I' + str(a.freertos / 'portable/GCC/ARM_AARCH64_SRE'),
                  '-I' + str(a.tcp / 'source/include'), '-I' + str(a.tcp / 'source/portable/Compiler/GCC'), '-I' + str(out)]
        sources = ['drivers/uart/a733_uart.c', 'drivers/bluetooth/bt_h4.c', 'drivers/bluetooth/bt_hci.c',
                   'drivers/storage/storage_digest.c', 'checks/bluetooth-application.c', 'checks/bluetooth-session.c',
                   'checks/bluetooth-wake.c', 'drivers/clock/a733_clock_leases.c', 'drivers/power/a733_rail_leases.c',
                   'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c',
                   'drivers/pinctrl/a733_pin_tables.c', 'drivers/storage/sdio_card.c', 'drivers/storage/sunxi_sdio.c',
                   'apps/bluetooth/result.c','checks/bluetooth-uart.c','drivers/uart/a733_uart_platform.c']
        objects = [];setup_errors = []
        for index, source in enumerate(sources):
            obj = out / f'host-{index}.o';dep = out / f'host-{index}.d'
            try:
                run('compile-' + str(index), [a.cc, *common, '-MMD', '-MF', dep, '-c', source, '-o', obj], obj, dep)
            except subprocess.CalledProcessError as error:
                setup_errors.append(str(error))
            objects.append(obj)
        launcher = out / 'launcher';launcher.mkdir(exist_ok=True)
        prep = load('bluetooth_launcher_preparation', ROOT / 'apps/bluetooth/prepare.py')
        try:
            def launcher_run(name, command, artifact):
                if '-c' in command:
                    dep = artifact.with_suffix('.d');command = [*command, '-MMD', '-MF', dep]
                    run(name, command, artifact, dep)
                else:
                    run(name, command, artifact)
            prep.build_launcher(launcher, a.cc, a.lld, launcher_run)
        except (SystemExit, subprocess.CalledProcessError) as error:
            setup_errors.append(str(error))
        if setup_errors:
            raise SystemExit('Independent setup failures (no runtime cases executed):\n' + '\n'.join(setup_errors))
        groups = {'application': [0, 1, 2, 3, 4, 15], 'session': [5],
                  'wake': [6, 7, 8, 9, 10, 11, 12, 13, 14], 'uart': [0,7,11,12,16,17]}
        for name, indices in groups.items():
            executable = out / ('bluetooth-' + name + '.elf')
            try:
                run(name + '-link', [a.cc, '-fsanitize=address,undefined', '-Wl,--gc-sections',
                    *[objects[i] for i in indices], '-o', executable], executable)
            except subprocess.CalledProcessError as error:
                setup_errors.append(str(error))
        if setup_errors:
            raise SystemExit('Independent link failures (no runtime cases executed):\n' + '\n'.join(setup_errors))
        preserve()
        for name in groups:
            args = [out / 'captured-frame.bin', out / 'captured-profile.bin'] if name == 'application' else []
            run(name + '-run', [out / ('bluetooth-' + name + '.elf'), *args])
        host = load('bluetooth_host_checks', ROOT / 'checks/bluetooth-host.py')
        host.check(out / 'host')
        (out / 'result.json').write_text(json.dumps({'passed': True, 'native_elf_sha256': sha(native_image),
            'scope': 'New application with actual UART/H4/HCI over modeled native providers and firmware checkpoints; actual bootstrap-only session admission; actual PM3/PM4 shared pin acquisition; offline profile/admission/result boundaries.',
            'limits': 'No physical SID/radio/UART/HCI qualification, pairing/security, HFP, SCO or headset claim.'}, indent=2) + '\n')
    finally:
        preserve()


if __name__ == '__main__':
    main()
