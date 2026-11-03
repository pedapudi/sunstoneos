#!/usr/bin/env python3
"""Configure a SunstoneOS source tree for a cross build, with every pinned dependency path.

Usage: configure.py [--dry-run] SRC BUILD [cmake options...]

Runs `cmake -S SRC -B BUILD` with the aarch64 clang toolchain file of SRC and the -D arguments of checks/image_dependencies.py: FreeRTOS,
TinyUSB, FreeRTOS+TCP, wpa_supplicant, the freestanding toolchain headers and root, the SMP runtime, the host linker, the GPU firmware and the
Zephyr BR/EDR sources, with the host LLVM tools first on PATH and TMPDIR on the main disk. When SRC carries apps/mimus/source.json, MIMUS_DIR is
that file's local_worktree. Options after BUILD go to cmake last, so a -D of yours wins over the pinned one (and that dependency need not exist).
A dependency that is missing is refused with the command that provides it. --dry-run prints the cmake command and runs nothing.

Build the target afterwards with the same PATH, for example:
    PATH=~/.cache/sunstoneos/host/usr/lib/llvm-21/bin:$PATH cmake --build BUILD --target sunstone-a733-os -j8
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'checks'))
import host_space
import image_dependencies as dependencies


def mimus_dir(source):
    """The mimus checkout that SRC names in apps/mimus/source.json, or None for a tree without the mimus application."""
    descriptor = source / 'apps/mimus/source.json'
    if not descriptor.is_file():
        return None
    record = json.loads(descriptor.read_text())
    path = Path(record['local_worktree'])
    if not path.is_dir():
        raise dependencies.MissingDependency(
            'image dependencies missing:\n  mimus: %s\n    provide it with: git clone %s %s && git -C %s checkout %s' % (
                path, record['repository'], path, path, record['revision']))
    return path


def command(source, build, extra, cache=dependencies.CACHE):
    """The cmake argument list for SRC and BUILD, with the pinned dependency options before the caller's own."""
    given = {m.group(1) for item in extra for m in [re.match(r'-D([A-Za-z0-9_]+)[:=]', item)] if m}
    arguments = dependencies.cmake_arguments(cache, supervised=True, skip=given)
    if 'MIMUS_DIR' not in given:
        mimus = mimus_dir(source)
        if mimus is not None:
            arguments.append('-DMIMUS_DIR=%s' % mimus)
    toolchain = [] if 'CMAKE_TOOLCHAIN_FILE' in given else ['-DCMAKE_TOOLCHAIN_FILE=%s' % (source / 'cmake/aarch64-clang.cmake')]
    return ['cmake', '-S', str(source), '-B', str(build), *toolchain, *arguments, *extra]


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    dry = False
    if argv and argv[0] in ('-h', '--help'):
        print(__doc__)
        return 0
    if argv and argv[0] == '--dry-run':
        dry = True
        argv = argv[1:]
    if len(argv) < 2:
        print('usage: configure.py [--dry-run] SRC BUILD [cmake options...]', file=sys.stderr)
        return 2
    source, build, extra = Path(argv[0]).resolve(), Path(argv[1]).resolve(), argv[2:]
    if not (source / 'CMakeLists.txt').is_file():
        print('%s is not a source tree (no CMakeLists.txt)' % source, file=sys.stderr)
        return 2
    try:
        cmake = command(source, build, extra)
    except dependencies.MissingDependency as error:
        print('refused: %s' % error, file=sys.stderr)
        return 1
    if dry:
        print(' '.join(shlex.quote(item) for item in cmake))
        return 0
    try:
        host_space.use_disk_tmp(dependencies.CACHE / 'build')
    except (OSError, ValueError, RuntimeError) as error:
        print('refused: %s' % error, file=sys.stderr)
        return 1
    env = dict(os.environ, PATH=dependencies.host_path() + os.pathsep + os.environ.get('PATH', ''))
    return subprocess.run(cmake, env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
