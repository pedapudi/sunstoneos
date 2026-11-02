"""Build one of our C modules for the host with only its register layer replaced.

The module compiles unchanged from its source file. checks/host/mmio/a733_mmio.h shadows include/a733_mmio.h, so
every a733_mmio_* access reaches the strict register model of checks/host/strict_mmio.c, which refuses any offset,
width or value that the fixture does not define with a source. Nothing else is stubbed; a module that needs more
than the register layer links against the fixture's definitions of what it calls, which the compiler and linker
require explicitly.

  build(output, name, fixture, modules)   compile the fixture and the real modules; return the executable
  verify_register_layer()                 fail when the host header lacks an accessor of the real header
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / 'checks/host'
FLAGS = ['-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-missing-field-initializers', *SANITIZE]
ACCESSOR = re.compile(r'\b(a733_mmio_[a-z0-9_]+)\s*\(')


def accessor_names(header):
    return set(ACCESSOR.findall(re.sub(r'/\*.*?\*/', '', Path(header).read_text(), flags=re.S)))


def defined_names(header):
    """Names that the header defines as functions; the per-width macro lines are expanded for each width."""
    text = Path(header).read_text()
    names = {m.group(1) for m in re.finditer(r'static inline [\w ]+?\b(a733_mmio_[a-z0-9_]+)\s*\(', text)}
    for line in text.splitlines():
        if '##bits' in line:
            for bits in (8, 16, 32, 64):
                expanded = line.replace('##bits##', str(bits)).replace('##bits', str(bits))
                names |= {m.group(1) for m in re.finditer(r'\b(a733_mmio_[a-z0-9_]+)\s*\(', expanded)}
    return names


def verify_register_layer():
    real = accessor_names(ROOT / 'include/a733_mmio.h')
    host = defined_names(HOST / 'mmio/a733_mmio.h')
    missing = sorted(n for n in real if n not in host)
    if missing:
        raise AssertionError('checks/host/mmio/a733_mmio.h lacks: ' + ', '.join(missing))
    return sorted(real)


def build(output, name, fixture, modules, includes=(), defines=()):
    """Compile fixture and modules (paths under the repository root) with the strict register layer."""
    verify_register_layer()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    target = output / name
    command = [str(CLANG), *FLAGS, '-I' + str(HOST / 'mmio'), '-I' + str(HOST), *('-I' + str(i) for i in includes),
               '-I' + str(ROOT / 'include'), *('-D' + d for d in defines),
               str(HOST / 'strict_mmio.c'), str(ROOT / fixture), *(str(ROOT / m) for m in modules), '-o', str(target)]
    subprocess.run(command, check=True, env=environment())
    return target
