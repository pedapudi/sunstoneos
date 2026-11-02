#!/usr/bin/env python3
"""Build and run C fixtures against the host FreeRTOS shim (checks/host/).

A fixture is one or more C files that link real SunstoneOS compilation units. This
module compiles them with the unmodified kernel sources (tasks.c, queue.c, list.c),
the shim's port and clock (checks/host/host_rtos.c), and the same sanitizer flags the
other host checks use. Include roots are checks/host first, so the shim's
FreeRTOSConfig.h and portmacro.h win, then the kernel headers, then include/.
"""
import hashlib
import os
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, CACHE, environment

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / 'checks/host'
KERNEL = CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
OBJECTS = CACHE / 'build/host-shim-objects'
KERNEL_SOURCES = ('tasks.c', 'queue.c', 'list.c')
FLAGS = ['-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-Wno-unused-parameter',
         '-Wno-sign-compare', *SANITIZE]


def build(output, sources, name, defines=(), includes=(), flags=(), shim=True, link=(), deadline=None):
    """Compile sources and the shim into output/name; return the executable path.

    Each source is a path under the repository root or an absolute path. Include
    paths are searched after checks/host and the kernel, before include/.
    """
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    target = output / name
    command = [str(CLANG), *FLAGS, *flags, '-I' + str(HOST), '-I' + str(KERNEL / 'include'),
               *('-I' + str(i) for i in includes), '-I' + str(ROOT / 'include'),
               *('-D' + d for d in defines)]
    units = [str(KERNEL / k) for k in KERNEL_SOURCES] + [str(HOST / 'host_rtos.c')] if shim else []
    if deadline is None:
        deadline = any(str(x).endswith('runtime/deadline.c') for x in sources)
    if shim and deadline:
        units.append(str(HOST / 'host_deadline.c'))
    # The kernel is third-party code: its own warnings are not the fixture's.
    objects = []
    # The kernel and the shim are the same for every check that uses the same flags, so
    # their objects are shared across runs and checks. The key covers the full command,
    # the unit and every shim header; a write is atomic, so concurrent builds are safe.
    shim_text = b''.join(p.read_bytes() for p in sorted(HOST.iterdir()) if p.suffix in ('.h', '.c'))
    for unit in units:
        quiet = ['-Wno-error', '-w'] if unit.startswith(str(KERNEL)) else []
        key = hashlib.sha256(repr((command, quiet, unit)).encode() + Path(unit).read_bytes() + shim_text).hexdigest()[:24]
        obj = OBJECTS / (Path(unit).name + '-' + key + '.o')
        if not obj.exists():
            OBJECTS.mkdir(parents=True, exist_ok=True)
            partial = obj.with_name(obj.name + '.%d' % os.getpid())
            subprocess.run([*command, *quiet, '-c', unit, '-o', str(partial)], check=True, env=environment())
            os.replace(partial, obj)
        objects.append(str(obj))
    for source in sources:
        path = Path(source)
        path = path if path.is_absolute() else ROOT / path
        obj = output / (hashlib.sha1(str(path).encode()).hexdigest()[:8] + '-' + path.name + '.o')
        subprocess.run([*command, '-c', str(path), '-o', str(obj)], check=True, env=environment())
        objects.append(str(obj))
    subprocess.run([str(CLANG), *SANITIZE, *link, *objects, '-o', str(target)], check=True, env=environment())
    return target


def run(target, timeout=120):
    """Execute a fixture; return the completed process with text output."""
    return subprocess.run([str(target)], capture_output=True, text=True, env=environment(), timeout=timeout)


def mutated(output, source, old, new, name=None):
    """Write a copy of a source file with one exact text replaced; return its path.

    This builds a negative control: the same check, run against code that has the
    bug it guards reintroduced, must fail. The replaced text must occur exactly once.
    """
    path = Path(source)
    path = path if path.is_absolute() else ROOT / path
    text = path.read_text()
    assert text.count(old) == 1, ('mutation site must occur once', str(path), old)
    out = Path(output) / 'mutants' / (name or path.name)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text.replace(old, new))
    return out
