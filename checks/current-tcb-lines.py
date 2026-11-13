#!/usr/bin/env python3
"""Fail when two cores' pxCurrentTCBs entries can share a 64-byte cache line in a SunstoneOS kernel.

Every context switch writes the switching core's entry of the SMP kernel's
pxCurrentTCBs, and the port reads it on every exception entry and exit. With
eight 8-byte entries on one line, a real-time core's switch took the line from
whichever core switched last, and from DRAM when that core was streaming
(docs/designs/scheduler-realtime.md). include/sunstone_current_tcb.h gives each
core its own line in the kernels that patch FreeRTOS tasks.c: C indexes the
array with portCURRENT_TCB(core) and assembly shifts the core ID by
portCURRENT_TCB_SHIFT. The unmodified tasks.c, which the emulator models of the
device services use, keeps 8-byte entries, and each image's port must use its
kernel's layout.

The check has four parts:

- Source scan of every tracked file and patch, and of the generated kernel and
  port sources in each --build directory: a subscript of pxCurrentTCBs other
  than index 0, the macro itself or a declaration sized with
  portCURRENT_TCB_STRIDE fails; so does assembly that scales a core ID by 8
  (LSL #3) within eight lines after it takes the array's address, and a
  Python offset from the symbol that does not use the header's line size.
- Layout of each --elf image and of each ELF at the top of each --build
  directory that defines pxCurrentTCBs. An image whose kernel defines
  ss_current_tcb_layout_6 must hold one aligned line per core (the core count
  from xIdleTaskHandles) and its port's ss_current_tcb_port_6; an image with
  the unmodified kernel must hold 8-byte entries and the port's
  ss_current_tcb_port_3.
- Link pairing: small units that use the header's kernel and port guards are
  compiled with each shift and linked with --gc-sections, as the images are.
  A line-per-core kernel links only with a port built with shift 6, the
  unmodified kernel only with shift 3, and a line-per-core kernel does not
  compile with shift 3.
- Before scanning, the check confirms that its rules flag a mutated C, assembly
  and Python line and a packed layout.

    current-tcb-lines.py --clang CLANG --linker LD_LLD [--build DIR ...] [--elf ELF ...]
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'include/sunstone_current_tcb.h'
CHECK_INPUTS = ['apps', 'boot', 'checks', 'include', 'kernel', 'patches', 'platform', 'runtime', 'tools']
GATE_RUNS = [['--clang', '/usr/lib/llvm-21/bin/clang', '--linker', '{cache}/host/usr/lib/llvm-21/bin/ld.lld',
              '--build', '{build:latency-cause-model}', '--build', '{build:scheduler-control-models}',
              '--elf', '{elf:sup}', '--elf', '{elf:ret-latency}']]
SUBSCRIPT = re.compile(r'pxCurrentTCBs\[([^\]]*)\]')
ADDRESS = re.compile(r'(adrp\s+x\d+,\s*pxCurrentTCBs\b|ldr\s+x\d+,\s*=\s*pxCurrentTCBs\b)', re.I)
SCALE_BY_EIGHT = re.compile(r'lsl\s*#\s*3\b|lsl\s+x\d+,\s*x\d+,\s*#\s*3\b', re.I)
PYTHON_OFFSET = re.compile(r"\['pxCurrentTCBs'\]\s*\+")


def line_bytes():
    return int(re.search(r'#define portCURRENT_TCB_LINE_BYTES (\d+)', HEADER.read_text()).group(1))


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
            if PYTHON_OFFSET.search(line) and 'current_tcb_bytes(' not in line]


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
        guards = {name for name in GUARDS if table.get_symbol_by_name(name)}
        return layout_problem(current[0]['st_value'], current[0]['st_size'],
                              idle[0]['st_size'] // 8 if idle else None, guards)


GUARDS = ('ss_current_tcb_layout_6', 'ss_current_tcb_port_6', 'ss_current_tcb_port_3')


def layout_problem(address, size, cores, guards):
    """The kernel's layout symbol decides the stride; the port's symbol must name the same one."""
    if 'ss_current_tcb_layout_6' in guards:
        step = line_bytes()
        if 'ss_current_tcb_port_6' not in guards:
            return 'a line-per-core kernel without a port built with shift 6'
        if address % step:
            return f'pxCurrentTCBs at {address:#x} does not start a {step}-byte line'
    elif 'ss_current_tcb_port_3' in guards:
        step = 8
    else:
        return 'pxCurrentTCBs without a current-task layout guard'
    if cores is None or size != cores * step:
        return f'pxCurrentTCBs holds {size} bytes, not {step} bytes for each of {cores} cores'
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
    lines = {'ss_current_tcb_layout_6', 'ss_current_tcb_port_6'}
    assert layout_problem(0x41000000, 64, 8, lines) and layout_problem(0x41000020, 512, 8, lines)
    assert layout_problem(0x41000040, 512, 8, {'ss_current_tcb_layout_6', 'ss_current_tcb_port_3'})
    assert layout_problem(0x41000040, 512, 8, set()) and layout_problem(0x41000040, 512, 8, {'ss_current_tcb_port_3'})
    assert not layout_problem(0x41000040, 512, 8, lines)
    assert not layout_problem(0x41000008, 64, 8, {'ss_current_tcb_port_3'})


KERNEL_UNIT = '''#include "sunstone_current_tcb.h"
void *pxCurrentTCBs[ 8 * portCURRENT_TCB_STRIDE ] __attribute__( ( aligned( portCURRENT_TCB_BYTES ) ) );
portCURRENT_TCB_KERNEL_GUARD();
void *current( unsigned core ) { return portCURRENT_TCB( core ); }
'''
UPSTREAM_UNIT = '''void *pxCurrentTCBs[ 8 ];
void *current( unsigned core ) { return pxCurrentTCBs[ core ]; }
'''
PORT_UNIT = '''#include "sunstone_current_tcb.h"
   portCURRENT_TCB_PORT_GUARD
   .text
   .globl _start
_start:
   adrp x0, pxCurrentTCBs
   add x0, x0, :lo12:pxCurrentTCBs
   mrs x1, tpidr_el1
   add x0, x0, x1, lsl # portCURRENT_TCB_SHIFT
   ldr x0, [ x0 ]
   b current
'''


def link_pairs(clang, linker, work):
    """(case, expected to build, built) for every pairing of kernel kind and port shift."""
    def compile_unit(name, text, suffix, shift):
        source = work / f'{name}{suffix}'
        source.write_text(text)
        out = work / f'{name}-{shift}.o'
        r = subprocess.run([clang, '--target=aarch64-none-elf', '-march=armv8-a', '-ffreestanding', '-O2',
                            '-ffunction-sections', '-fdata-sections', f'-I{ROOT / "include"}',
                            f'-DportCURRENT_TCB_SHIFT={shift}', '-c', str(source), '-o', str(out)],
                           capture_output=True, text=True)
        return out if r.returncode == 0 else None

    def links(kernel, port):
        if kernel is None or port is None:
            return False
        r = subprocess.run([linker, '--gc-sections', '-e', '_start', '-Ttext=0x40000000', str(kernel), str(port),
                            '-o', str(work / 'pair.elf')], capture_output=True, text=True)
        return r.returncode == 0

    port = {shift: compile_unit('port', PORT_UNIT, '.S', shift) for shift in (3, 6)}
    assert port[3] and port[6], 'the port guard does not assemble'
    lines, upstream = compile_unit('kernel', KERNEL_UNIT, '.c', 6), compile_unit('upstream', UPSTREAM_UNIT, '.c', 3)
    assert lines and upstream, 'the kernel units do not compile'
    return [('line-per-core kernel with a shift-6 port', True, links(lines, port[6])),
            ('line-per-core kernel with a shift-3 port', False, links(lines, port[3])),
            ('unmodified kernel with a shift-3 port', True, links(upstream, port[3])),
            ('unmodified kernel with a shift-6 port', False, links(upstream, port[6])),
            ('line-per-core kernel compiled with shift 3', False,
             compile_unit('kernel', KERNEL_UNIT, '.c', 3) is not None)]


def tracked():
    names = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z'], capture_output=True, check=True).stdout
    own = Path(__file__).resolve().relative_to(ROOT).as_posix()
    for name in names.decode().split('\0'):
        path = ROOT / name
        # The check's own self-test holds the patterns it must flag.
        if name and name != own and not name.startswith('docs/') and not name.endswith(('.md', '.jsonl')) and path.is_file():
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
    parser.add_argument('--clang', required=True, help='clang that targets aarch64-none-elf')
    parser.add_argument('--linker', required=True, help='ld.lld')
    args = parser.parse_args()
    self_test()
    failures, sources, images = [], 0, 0
    with tempfile.TemporaryDirectory() as work:
        pairs = link_pairs(args.clang, args.linker, Path(work))
    failures += [f'link pairing: {case} {"fails" if expected else "builds"}' for case, expected, built in pairs
                 if built != expected]
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
    print(f'PASS current task lines: {sources} sources name pxCurrentTCBs and index it by the kernel\'s stride; '
          f'{images} images hold their kernel\'s layout; {len(pairs)} kernel and port pairings link or fail as required')


if __name__ == '__main__':
    main()
