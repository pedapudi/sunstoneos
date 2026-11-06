#!/usr/bin/env python3
"""The pinned sources and host tools that the board and OS images need, in one place.

An image configures against the sources that sources.lock.json pins (FreeRTOS, TinyUSB, FreeRTOS+TCP, wpa_supplicant, the freestanding
toolchain and its C headers, the GPU firmware), the host linker, the SMP C and C++ runtime that tools/build-newlib-smp.py builds and the Zephyr
BR/EDR security sources. This module derives every path from the lock file's revisions and directories, never from a second copy of a hash, and
names the command that fetches or builds whatever is missing. tools/configure.py and the checks that build an image from a clean tree take the
CMake arguments from here.

    python3 checks/image_dependencies.py            print the -D arguments, one per line
    from image_dependencies import cmake_arguments  the same, as a list
    from image_dependencies import missing          what is absent, each with the command that provides it
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
LOCK = ROOT / 'sources.lock.json'
LLVM_BIN = 'host/usr/lib/llvm-21/bin'


class MissingDependency(FileNotFoundError):
    """One or more image dependencies are absent; the message names each with the command that provides it."""


def _lock():
    return json.loads(LOCK.read_text())


def _newest(directories):
    return next(iter(sorted((d for d in directories if d.is_dir()), key=lambda d: d.stat().st_mtime, reverse=True)), None)


def locations(cache=CACHE):
    """name -> (directory or file, kind, the command that provides it); the path is only a location, it may not exist."""
    cache = Path(cache)
    lock = _lock()
    sources = cache / 'sources'
    stacks = sources / 'radio/stacks'
    toolchain = cache / 'host' / lock['aarch64-newlib-toolchain']['directory']
    fetch = 'python3 tools/fetch-sources.py '
    return {
        'freertos': (sources / lock['freertos']['directory'], 'directory', fetch + 'freertos'),
        'tinyusb': (sources / lock['tinyusb']['directory'], 'directory', fetch + 'tinyusb'),
        # The radio stacks are fetched as archives (the lock names no directory for them) and extracted next to them.
        'tcp': (stacks / ('FreeRTOS-Plus-TCP-' + lock['freertos-tcp']['revision']), 'directory',
                fetch + 'freertos-tcp  # then extract %s in %s' % (lock['freertos-tcp']['archive'], stacks)),
        'rsn': (stacks / ('wpa_supplicant-' + lock['wpa-supplicant']['revision']), 'directory',
                fetch + 'wpa-supplicant  # then extract %s in %s' % (lock['wpa-supplicant']['archive'], stacks)),
        'newlib': (toolchain, 'directory', fetch + 'aarch64-newlib-toolchain'),
        'headers': (toolchain / 'aarch64-none-elf/include', 'directory', fetch + 'aarch64-newlib-toolchain'),
        # The OS kit's SMP C and C++ runtime: the most recently built distribution (its name is a hash of the builder and sources).
        'newlib_smp': (_newest((cache / 'host').glob('newlib-smp-*')) or cache / 'host/newlib-smp-<key>', 'directory',
                       'python3 tools/build-newlib-smp.py'),
        'lld': (cache / LLVM_BIN / 'ld.lld', 'file',
                'the host LLVM 21 tools live under %s (docs/build.md)' % (cache / 'host/usr')),
        'gpu_firmware': (sources / lock['gpu-firmware']['archive'], 'file', fetch + 'gpu-firmware'),
        # No lock entry pins these Zephyr files; the directory's own sources.json names each file's URL and SHA-256 (docs/bluetooth-native.md).
        'bt_security': (sources / 'bluetooth-security-zephyr-v4.2.1', 'directory',
                        'populate %s from the files in its sources.json (docs/bluetooth-native.md)' % (sources / 'bluetooth-security-zephyr-v4.2.1')),
        # The pinned libsbc of the headset's mSBC codec; apps/bluetooth/msbc-source.json names each file's SHA-256.
        'bt_sbc': (sources / 'bluetooth-msbc', 'directory',
                   'populate %s from the files in apps/bluetooth/msbc-source.json (docs/bluetooth-native.md)' % (sources / 'bluetooth-msbc')),
    }


def missing(cache=CACHE):
    """[(name, path, command)] for every dependency that is not present."""
    return [(name, path, command) for name, (path, kind, command) in locations(cache).items()
            if not (path.is_dir() if kind == 'directory' else path.is_file())]


def paths(cache=CACHE):
    """The dependency paths, each checked to exist; raises MissingDependency (a FileNotFoundError) naming every missing one."""
    absent = missing(cache)
    if absent:
        raise MissingDependency('image dependencies missing under %s:\n%s' % (
            cache, '\n'.join('  %s: %s\n    provide it with: %s' % (name, path, command) for name, path, command in absent)))
    return {name: path for name, (path, _, _) in locations(cache).items()}


# CMake cache variable of each dependency.
OPTIONS = {'freertos': 'FREERTOS_DIR', 'tinyusb': 'TINYUSB_DIR', 'tcp': 'SUNSTONE_TCP_DIR', 'rsn': 'SUNSTONE_RSN_DIR',
           'headers': 'SUNSTONE_NETWORK_C_HEADERS', 'newlib': 'NEWLIB_ROOT', 'newlib_smp': 'NEWLIB_SMP_ROOT',
           'lld': 'SUNSTONE_LLD', 'gpu_firmware': 'SUNSTONE_GPU_FIRMWARE', 'bt_security': 'SUNSTONE_BT_SECURITY_DIR',
           'bt_sbc': 'SUNSTONE_BT_SBC_DIR'}


def cmake_arguments(cache=CACHE, supervised=True, skip=()):
    """-D arguments for any image: the dependency directories, the linker, the GPU firmware, the C and C++ runtimes and the supervised board
    switch the board images need. skip names options (such as SUNSTONE_BT_SECURITY_DIR) that the caller sets itself; a skipped dependency
    need not exist."""
    wanted = {name: option for name, option in OPTIONS.items() if option not in skip}
    found = locations(cache)
    absent = [(n, found[n][0], found[n][2]) for n in wanted if not (found[n][0].is_dir() if found[n][1] == 'directory' else found[n][0].is_file())]
    if absent:
        raise MissingDependency('image dependencies missing under %s:\n%s' % (
            cache, '\n'.join('  %s: %s\n    provide it with: %s' % item for item in absent)))
    arguments = ['-D%s=%s' % (option, found[name][0]) for name, option in wanted.items()]
    if supervised and 'SUNSTONE_BOARD_SUPERVISED' not in skip:
        arguments.append('-DSUNSTONE_BOARD_SUPERVISED=ON')
    return arguments


def host_path(cache=CACHE):
    """The directory of the host LLVM tools that the configure and the build put first on PATH."""
    return str(Path(cache) / LLVM_BIN)


if __name__ == '__main__':
    print('\n'.join(cmake_arguments()))
