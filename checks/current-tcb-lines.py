#!/usr/bin/env python3
"""Fail when two cores' pxCurrentTCBs entries can share a 64-byte cache line.

Every context switch writes the switching core's entry of the SMP kernel's
pxCurrentTCBs, and the port reads it on every exception entry and exit. With
eight 8-byte entries on one line, a real-time core's switch took the line from
whichever core switched last, and from DRAM when that core was streaming
(docs/designs/scheduler-realtime.md). include/sunstone_current_tcb.h gives each
core its own line: C indexes the array with portCURRENT_TCB(core) and assembly
shifts the core ID by portCURRENT_TCB_SHIFT.

The check has three parts:

- Source scan of every tracked file and patch, and of the generated kernel and
  port sources in each --build directory: a subscript of pxCurrentTCBs other
  than index 0, the macro itself or a declaration sized with
  portCURRENT_TCB_STRIDE fails; so does assembly that scales a core ID by 8
  (LSL #3) within eight lines after it takes the array's address, and a
  Python offset from the symbol that does not use the header's line size.
- Layout of each --elf image and of each ELF at the top of each --build
  directory that defines pxCurrentTCBs: the symbol must start on a line and
  hold one line per core (the core count from xIdleTaskHandles).
- Before scanning, the check confirms that its rules flag a mutated C, assembly
  and Python line and a packed layout.

    current-tcb-lines.py [--build DIR ...] [--elf ELF ...]
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'include/sunstone_current_tcb.h'
CHECK_INPUTS = ['apps', 'boot', 'checks', 'include', 'kernel', 'patches', 'platform', 'runtime', 'tools']
GATE_RUNS = [['--build', '{build:latency-cause-model}', '--build', '{build:scheduler-control-models}',
              '--elf', '{elf:sup}', '--elf', '{elf:ret-latency}']]
SUBSCRIPT = re.compile(r'pxCurrentTCBs\[([^\]]*)\]')
ADDRESS = re.compile(r'(adrp\s+x\d+,\s*pxCurrentTCBs\b|ldr\s+x\d+,\s*=\s*pxCurrentTCBs\b)', re.I)
SCALE_BY_EIGHT = re.compile(r'lsl\s*#\s*3\b|lsl\s+x\d+,\s*x\d+,\s*#\s*3\b', re.I)
PYTHON_OFFSET = re.compile(r"\['pxCurrentTCBs'\]\s*\+")


def line_bytes():
    return int(re.search(r'#define portCURRENT_TCB_BYTES (\d+)', HEADER.read_text()).group(1))


def c_hits(text):
    found = []
    for number, line in enumerate(text.split('\n'), 1):
        for match in SUBSCRIPT.finditer(line):
            index = match.group(1).strip()
            if index in ('', '0', '( 0 )', '(0)') or 'portCURRENT_TCB_STRIDE' in index:
                continue
            found.append((number, line.strip()))
    return found


def asm_hits(text):
    found, window = [], 0
    for number, line in enumerate(text.split('\n'), 1):
        code = line.split('//')[0].split('/*')[0]
        if ADDRESS.search(code):
            window = 8
            continue
        if window:
            window -= 1
            if SCALE_BY_EIGHT.search(code):
                found.append((number, line.strip()))
                window = 0
            elif 'portCURRENT_TCB_SHIFT' in code:
                window = 0
    return found


def python_hits(text):
    return [(number, line.strip()) for number, line in enumerate(text.split('\n'), 1)
            if PYTHON_OFFSET.search(line) and 'current_tcb_bytes()' not in line]


def hits(name, text):
    if 'pxCurrentTCBs' not in text:
        return []
    if name.endswith('.py'):
        return python_hits(text)
    if name.endswith(('.S', '.s')):
        return asm_hits(text)
    if name.endswith('.patch'):
        # A removed line describes the source before the patch; only context and added lines reach the build.
        kept = '\n'.join(l[1:] if l[:1] in (' ', '+') and not l.startswith('+++') else ''
                         for l in text.split('\n'))
        return c_hits(kept) + asm_hits(kept)
    return c_hits(text)


def layout_hits(elf):
    with open(elf, 'rb') as f:
        table = ELFFile(f).get_section_by_name('.symtab')
        current = table.get_symbol_by_name('pxCurrentTCBs') if table else None
        idle = table.get_symbol_by_name('xIdleTaskHandles') if table else None
        if not current:
            return None
        return layout_problem(current[0]['st_value'], current[0]['st_size'],
                              idle[0]['st_size'] // 8 if idle else None)


def layout_problem(address, size, cores):
    step = line_bytes()
    if address % step:
        return f'pxCurrentTCBs at {address:#x} does not start a {step}-byte line'
    if cores is None or size != cores * step:
        return f'pxCurrentTCBs holds {size} bytes, not one {step}-byte line for each of {cores} cores'
    return ''


def self_test():
    assert c_hits('x = pxCurrentTCBs[ xCoreID ];') and c_hits('return &pxCurrentTCBs[c];')
    assert not c_hits('x = portCURRENT_TCB( xCoreID );') and not c_hits('if (pxCurrentTCBs[0] != t)')
    assert not c_hits('TCB_t * volatile pxCurrentTCBs[ configNUMBER_OF_CORES * portCURRENT_TCB_STRIDE ];')
    assert asm_hits('adrp X0, pxCurrentTCBs\nadd X0, X0, :lo12:pxCurrentTCBs\nMRS X1, tpidr_el1\n'
                    'LDR X0, [ X0, X1, LSL # 3 ]')
    assert asm_hits('ldr x6, =pxCurrentTCBs\nldr x6, [x6, x20, lsl #3]')
    assert asm_hits('adrp X0, pxCurrentTCBs\nadd X0, X0, :lo12:pxCurrentTCBs\nMRS X2, tpidr_el1\n'
                    'AND X2, X2, # 0xff\nLSL X2, X2, # 3\nADD X0, X0, X2')
    assert not asm_hits('adrp X0, pxCurrentTCBs\nadd X0, X0, :lo12:pxCurrentTCBs\nMRS X2, tpidr_el1\n'
                        'LSL X2, X2, # portCURRENT_TCB_SHIFT\nADD X0, X0, X2\nadrp X1, other\n'
                        'LDR X3, [X1, X2, LSL #3]')
    assert python_hits("uc.mem_write(image.symbols['pxCurrentTCBs'] + core * 8, data)")
    assert not python_hits("uc.mem_write(image.symbols['pxCurrentTCBs'] + core * current_tcb_bytes(), data)")
    assert hits('x.patch', '--- a\n+++ a\n-    t = pxCurrentTCBs[ c ];\n+    t = portCURRENT_TCB( c );') == []
    assert hits('x.patch', '--- a\n+++ a\n+    t = pxCurrentTCBs[ c ];')
    assert layout_problem(0x41000000, 64, 8) and layout_problem(0x41000020, 512, 8)
    assert not layout_problem(0x41000040, 512, 8)


def tracked():
    names = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z'], capture_output=True, check=True).stdout
    for name in names.decode().split('\0'):
        path = ROOT / name
        if name and not name.startswith('docs/') and not name.endswith(('.md', '.jsonl')) and path.is_file():
            yield name, path


def generated(build):
    for pattern in ('freertos-*/*.c', 'freertos-*/*.S', 'latency/*.c', 'latency-ports/*/*.c', 'latency-ports/*/*.S'):
        for path in sorted(build.glob(pattern)):
            yield str(path), path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, action='append', default=[],
                        help='a configured build: scan its generated kernel and port sources and check its ELFs')
    parser.add_argument('--elf', type=Path, action='append', default=[], help='an image to check (repeatable)')
    args = parser.parse_args()
    self_test()
    failures, sources, images = [], 0, 0
    files = list(tracked())
    for build in args.build:
        files += list(generated(build))
    for name, path in files:
        try:
            text = path.read_text()
        except UnicodeDecodeError:
            continue
        found = hits(name, text)
        sources += 'pxCurrentTCBs' in text
        failures += [f'{name}:{number}: {line}' for number, line in found]
    elves = list(args.elf) + [p for b in args.build for p in sorted(b.glob('*.elf'))]
    for elf in elves:
        problem = layout_hits(elf)
        if problem is None:
            continue
        images += 1
        if problem:
            failures.append(f'{elf}: {problem}')
    for line in failures:
        print('FAIL ' + line)
    if failures:
        sys.exit(f'{len(failures)} places where two cores can share a pxCurrentTCBs line')
    if args.elf and not images:
        sys.exit('no --elf image defines pxCurrentTCBs')
    print(f'PASS current task lines: {sources} sources name pxCurrentTCBs and index it one line per core; '
          f'{images} images hold one {line_bytes()}-byte line per core')


if __name__ == '__main__':
    main()
