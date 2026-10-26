#!/usr/bin/env python3
"""Check that no SRAM layout overlaps the MBUS restore's window page.

  sram-window-reservation.py [OUTPUT.json]

include/a733_el3_mbus.h copies its window to A733_MBUS_SRAM and runs it there
while an image runs. This check reads that address and the window's size from
the header and fails when any SRAM layout of the repository overlaps the
4 KiB page:

- every linker script's MEMORY ORIGIN/LENGTH and absolute location (`. = X`,
  taken as the 4 KiB from X when no length is given);
- every `ldr sp, =X` in assembly, taken as the 4 KiB stack below X;
- every SRAM region table in Python tools and checks: tuples of a name, a
  base and a size, with module-level constants resolved;
- every hexadecimal literal inside the page, outside the files that own it.

Only addresses in SRAM A2 and the shared SRAM (0x40000-0xf3fff) are
considered. The DDR start (tools/run-ddr.py, boot/a733/ddr-entry.S) loads
boot0 into SRAM only before any image runs in a lifetime, and the window runs
only inside an image, so ALLOWED lists those files with that reason. No
device access."""
import ast
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SRAM = (0x40000, 0xf4000)
STACK = 0x1000
# Files that may name the page: the owner, its checks and its documents.
OWNERS = {'include/a733_el3_mbus.h', 'boot/a733/mbus-window.S', 'checks/el3-mbus.py', 'checks/a733-monitor-are.py',
          'checks/sram-window-reservation.py'}
# SRAM users that never run while an image runs.
DDR_START = ('the DDR start runs boot0 from SRAM before any image of the lifetime, and the first image '
             'starts only after it has returned to the FEL loop')
ALLOWED = {'tools/run-ddr.py': DDR_START, 'boot/a733/ddr-entry.S': DDR_START}
SKIP_PARTS = {'.git', 'build', '__pycache__'}


def window():
    text = (ROOT / 'include/a733_el3_mbus.h').read_text()
    base = int(re.search(r'#define A733_MBUS_SRAM UINT32_C\((0x[0-9a-fA-F]+)\)', text).group(1), 16)
    body = text[text.index('a733_mbus_window_code[] = {'):].split('};')[0]
    words = len(re.findall(r'0x[0-9a-f]{8}u', body))
    return base, words * 4


def files(*suffixes):
    for path in ROOT.rglob('*'):
        if path.suffix in suffixes and path.is_file() and not SKIP_PARTS & set(path.relative_to(ROOT).parts):
            yield path


def in_sram(start, end):
    return start < SRAM[1] and end > SRAM[0]


def linker_regions():
    for path in files('.ld'):
        text = path.read_text(errors='replace')
        for m in re.finditer(r'ORIGIN\s*=\s*(0x[0-9a-fA-F]+)\s*,\s*LENGTH\s*=\s*(0x[0-9a-fA-F]+|\d+[KM]?)', text):
            length = m.group(2)
            size = int(length, 16) if length.startswith('0x') else int(length.rstrip('KM')) * (
                1024 if length.endswith('K') else 1 << 20 if length.endswith('M') else 1)
            yield path, int(m.group(1), 16), size, 'MEMORY'
        for m in re.finditer(r'^\s*\.\s*=\s*(0x[0-9a-fA-F]+)\s*;', text, re.M):
            yield path, int(m.group(1), 16), STACK, 'location'


def stacks():
    for path in files('.S', '.s'):
        for m in re.finditer(r'ldr\s+sp\s*,\s*=\s*(0x[0-9a-fA-F]+)', path.read_text(errors='replace')):
            top = int(m.group(1), 16)
            yield path, top - STACK, STACK, 'stack'


def python_tables():
    for path in files('.py'):
        try:
            tree = ast.parse(path.read_text(errors='replace'))
        except SyntaxError:
            continue
        constants = {}
        for node in tree.body:
            if isinstance(node, ast.Assign):
                targets = node.targets[0]
                names = [t.id for t in (targets.elts if isinstance(targets, ast.Tuple) else [targets])
                         if isinstance(t, ast.Name)]
                values = node.value.elts if isinstance(node.value, ast.Tuple) and len(names) > 1 else [node.value]
                for name, value in zip(names, values):
                    if isinstance(value, ast.Constant) and isinstance(value.value, int):
                        constants[name] = value.value

        def number(node):
            if isinstance(node, ast.Constant) and isinstance(node.value, int):
                return node.value
            if isinstance(node, ast.Name):
                return constants.get(node.id)
            return None
        for node in ast.walk(tree):
            if isinstance(node, ast.Tuple) and len(node.elts) == 3 and isinstance(node.elts[0], ast.Constant) \
                    and isinstance(node.elts[0].value, str):
                base, size = number(node.elts[1]), number(node.elts[2])
                if base is not None and size is not None and 0 < size <= 0x100000:
                    yield path, base, size, f'table {node.elts[0].value}'


def literals(base, end):
    pattern = re.compile(r'\b0x0*([0-9a-fA-F]{5})\b')
    for path in files('.c', '.h', '.S', '.s', '.ld', '.py'):
        rel = str(path.relative_to(ROOT))
        if rel in OWNERS:
            continue
        for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
            for m in pattern.finditer(line):
                value = int(m.group(1), 16)
                if base <= value < end:
                    yield rel, number, hex(value)


def main():
    base, size = window()
    end = base + 0x1000
    problems, seen = [], 0
    if base & 0xfff or size > 0x1000 or not in_sram(base, end):
        problems.append(f'window {base:#x} with {size} bytes does not fit one SRAM page')
    for path, start, length, kind in (*linker_regions(), *stacks(), *python_tables()):
        if not in_sram(start, start + length):
            continue
        seen += 1
        rel = str(path.relative_to(ROOT))
        if rel in OWNERS:
            continue
        if start < end and start + length > base:
            reason = ALLOWED.get(rel)
            if reason is None:
                problems.append(f'{rel}: {kind} {start:#x}+{length:#x} overlaps the window page {base:#x}')
    for rel, number, value in literals(base, end):
        problems.append(f'{rel}:{number}: names {value}, inside the window page')
    result = {'physical_access': False, 'window': hex(base), 'window_bytes': size, 'sram_layouts': seen,
              'allowed': ALLOWED, 'problems': problems}
    if len(sys.argv) == 2:
        Path(sys.argv[1]).write_text(json.dumps(result, indent=1) + '\n')
    if problems:
        sys.exit('FAIL: ' + '; '.join(problems))
    print(f'PASS sram-window-reservation: {seen} SRAM layouts clear of the window page {base:#x} ({size} bytes)')


if __name__ == '__main__':
    main()
