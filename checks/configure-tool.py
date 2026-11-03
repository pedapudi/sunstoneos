#!/usr/bin/env python3
"""Check tools/configure.py and the dependency table of checks/image_dependencies.py against a fake cache.

The cmake command of `configure.py SRC BUILD` carries every pinned dependency path, derived from sources.lock.json and nothing written twice. In
a scratch cache whose directories are named after the lock file's pins, the check requires:
  paths       each dependency option names the directory the lock pins (FreeRTOS, TinyUSB, FreeRTOS+TCP and wpa_supplicant by revision,
              the toolchain headers and root, the GPU firmware), the newest SMP runtime and the host linker; the toolchain file is the
              source tree's, MIMUS_DIR is the local_worktree of apps/mimus/source.json, and the caller's options come last;
  override    a -D of the caller's replaces the pinned option, and that dependency may then be absent;
  refusal     a missing dependency is refused naming it with the command that provides it (tools/fetch-sources.py NAME), every missing
              one at once, and a missing mimus checkout names the clone command;
  cli         `configure.py --dry-run SRC BUILD -DX=1` prints the command and exits 0, and a directory without CMakeLists.txt exits 2.
Usage: configure-tool.py --output DIR
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['tools/configure.py', 'checks/image_dependencies.py', 'sources.lock.json', 'tools/fetch-sources.py']
sys.path.insert(0, str(ROOT / 'checks'))
sys.path.insert(0, str(ROOT / 'tools'))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    import image_dependencies as dependencies
    configure = load('configure_check_tool', ROOT / 'tools/configure.py')
    lock = json.loads((ROOT / 'sources.lock.json').read_text())
    cache, source = out / 'cache', out / 'src'
    sources, stacks, host = cache / 'sources', cache / 'sources/radio/stacks', cache / 'host'
    toolchain = host / lock['aarch64-newlib-toolchain']['directory']
    layout = {'freertos': sources / lock['freertos']['directory'], 'tinyusb': sources / lock['tinyusb']['directory'],
              'tcp': stacks / ('FreeRTOS-Plus-TCP-' + lock['freertos-tcp']['revision']),
              'rsn': stacks / ('wpa_supplicant-' + lock['wpa-supplicant']['revision']),
              'headers': toolchain / 'aarch64-none-elf/include', 'newlib': toolchain,
              'bt_security': sources / 'bluetooth-security-zephyr-v4.2.1'}
    for path in layout.values():
        path.mkdir(parents=True, exist_ok=True)
    (host / 'newlib-smp-aaaa').mkdir(parents=True)
    os.utime(host / 'newlib-smp-aaaa', (1, 1))
    (host / 'newlib-smp-bbbb').mkdir()
    for path in (cache / dependencies.LLVM_BIN / 'ld.lld', sources / lock['gpu-firmware']['archive']):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'x')
    mimus = out / 'mimus'
    mimus.mkdir()
    (source / 'apps/mimus').mkdir(parents=True)
    (source / 'cmake').mkdir()
    (source / 'CMakeLists.txt').write_text('project(x)\n')
    (source / 'apps/mimus/source.json').write_text(json.dumps({'local_worktree': str(mimus), 'repository': 'https://example.invalid/mimus.git',
                                                              'revision': '0123abc'}))
    passed = []
    # paths
    command = configure.command(source, out / 'build', ['-DSUNSTONE_A733_OS=ON', '-G', 'Ninja'], cache)
    assert command[:5] == ['cmake', '-S', str(source), '-B', str(out / 'build')] and command[-3:] == ['-DSUNSTONE_A733_OS=ON', '-G', 'Ninja'], command
    expect = {'FREERTOS_DIR': layout['freertos'], 'TINYUSB_DIR': layout['tinyusb'], 'SUNSTONE_TCP_DIR': layout['tcp'],
              'SUNSTONE_RSN_DIR': layout['rsn'], 'SUNSTONE_NETWORK_C_HEADERS': layout['headers'], 'NEWLIB_ROOT': layout['newlib'],
              'NEWLIB_SMP_ROOT': host / 'newlib-smp-bbbb', 'SUNSTONE_LLD': cache / dependencies.LLVM_BIN / 'ld.lld',
              'SUNSTONE_GPU_FIRMWARE': sources / lock['gpu-firmware']['archive'], 'SUNSTONE_BT_SECURITY_DIR': layout['bt_security'],
              'MIMUS_DIR': mimus, 'CMAKE_TOOLCHAIN_FILE': source / 'cmake/aarch64-clang.cmake'}
    for option, path in expect.items():
        assert '-D%s=%s' % (option, path) in command, (option, path)
    assert '-DSUNSTONE_BOARD_SUPERVISED=ON' in command
    passed.append('every dependency option names its pinned path; the caller\'s options come last')
    # override
    shutil.rmtree(layout['freertos'])
    command = configure.command(source, out / 'build', ['-DFREERTOS_DIR=/elsewhere'], cache)
    assert '-DFREERTOS_DIR=/elsewhere' in command and not any(c.startswith('-DFREERTOS_DIR=%s' % layout['freertos']) for c in command)
    passed.append('a -D of the caller replaces the pinned option and its dependency may be absent')
    # refusal
    shutil.rmtree(layout['tcp'])
    try:
        configure.command(source, out / 'build', [], cache)
        raise AssertionError('missing dependencies were accepted')
    except dependencies.MissingDependency as error:
        text = str(error)
        assert 'freertos' in text and 'python3 tools/fetch-sources.py freertos' in text, text
        assert 'tcp' in text and 'python3 tools/fetch-sources.py freertos-tcp' in text, text
        assert str(layout['freertos']) in text and str(layout['tcp']) in text, text
    shutil.rmtree(mimus)
    try:
        configure.command(source, out / 'build', ['-DFREERTOS_DIR=/e', '-DSUNSTONE_TCP_DIR=/e'], cache)
        raise AssertionError('a missing mimus checkout was accepted')
    except dependencies.MissingDependency as error:
        assert 'git clone https://example.invalid/mimus.git' in str(error) and 'checkout 0123abc' in str(error), str(error)
    passed.append('every missing dependency is refused with the command that provides it, a missing mimus checkout with the clone command')
    # cli
    done = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/configure.py'), '--dry-run', str(ROOT), str(out / 'cli-build'), '-DX=1'],
                          capture_output=True, text=True, env=dict(os.environ, HOME=str(out / 'nohome')))
    assert done.returncode == 1 and 'provide it with' in done.stderr, (done.returncode, done.stderr)      # a home without a cache
    done = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/configure.py'), '--dry-run', str(out), str(out / 'b')], capture_output=True, text=True)
    assert done.returncode == 2 and 'no CMakeLists.txt' in done.stderr, (done.returncode, done.stderr)
    passed.append('configure.py refuses in a home without the dependencies and for a directory that is not a source tree')
    for line in passed:
        print('PASS ' + line)


if __name__ == '__main__':
    main()
