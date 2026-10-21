#!/usr/bin/env python3
"""Reject final ELFs whose string routines or EL3 boot code can fault on Device memory.

EL3 boot C runs with translation off, so every data access is Device-nGnRnE.
SIMD/FP instructions and DC ZVA are outside the boot rule of general registers
only, and SIMD library routines use unaligned or overlapping wide accesses that
raise alignment faults there. The check disassembles each image and fails when
memcpy, memmove or memset, or any function reachable by direct branches from
the EL3 boot roots, contains a SIMD/FP register operand or DC ZVA. The string
routines must also be leaves. Indirect calls are counted but not followed;
compiler -mstrict-align, not this check, keeps scalar accesses aligned.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

STRING_ROUTINES = ('memcpy', 'memmove', 'memset')
DEFAULT_ROOTS = r'^(local_boot|a733_cpu_.*|a733_cached_prepare)$'
FUNCTION = re.compile(r'^([0-9a-f]+) <([^>]+)>:$')
INSTRUCTION = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
SIMD_REGISTER = re.compile(r'\b(?:[vqdshb](?:[12]?[0-9]|3[01])(?:\.\w+)?|z[0-9]+|p[0-9]+)\b')
TARGET = re.compile(r'<([^>+]+)(\+0x[0-9a-f]+)?>')
THUNK = re.compile(r'^__AArch64\w*Thunk_(.+)$')


def disassemble(objdump, image):
    output = subprocess.check_output([objdump, '-d', '--no-show-raw-insn', str(image)], text=True)
    functions, current = {}, None
    for line in output.splitlines():
        match = FUNCTION.match(line)
        if match:
            current = functions.setdefault(match[2], [])
            continue
        match = INSTRUCTION.match(line)
        if match and current is not None:
            current.append((int(match[1], 16), match[2].lower(), match[3].lower()))
    return functions


def hazards(instructions):
    found = []
    for address, mnemonic, operands in instructions:
        text = TARGET.sub('', operands.split('//')[0])
        if mnemonic == 'dc' and 'zva' in text:
            found.append(f'{address:#x}: dc {text}')
        elif SIMD_REGISTER.search(text):
            found.append(f'{address:#x}: {mnemonic} {text}')
    return found


def callees(name, instructions, functions):
    direct, indirect = set(), 0
    for _, mnemonic, operands in instructions:
        if mnemonic in ('blr', 'br', 'blraa', 'braa'):
            indirect += 1
            continue
        if mnemonic not in ('bl', 'b') and not mnemonic.startswith('b.') and mnemonic not in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            continue
        match = TARGET.search(operands)
        if not match or match[2] or match[1] == name:
            continue
        target = match[1]
        thunk = THUNK.match(target)
        if thunk:
            target = thunk[1]
        if target in functions:
            direct.add(target)
    return direct, indirect


def check(image, objdump, roots):
    functions = disassemble(objdump, image)
    failures, report = [], {'image': str(image), 'string_routines': {}, 'roots': [], 'reachable': 0,
                             'indirect_calls': 0}
    for name in STRING_ROUTINES:
        if name not in functions:
            report['string_routines'][name] = 'absent'
            continue
        found = hazards(functions[name])
        calls, indirect = callees(name, functions[name], functions)
        if calls or indirect or any(m == 'bl' for _, m, _ in functions[name]):
            found.append('calls another routine')
        report['string_routines'][name] = found or 'clean'
        failures += [f'{name}: {item}' for item in found]
    pattern = re.compile(roots)
    queue = sorted(name for name in functions if pattern.match(name))
    report['roots'] = queue[:]
    reached, parents = set(queue), {name: None for name in queue}
    while queue:
        name = queue.pop()
        for item in hazards(functions[name]):
            chain, step = [name], parents[name]
            while step:
                chain.append(step)
                step = parents[step]
            failures.append(f'{" <- ".join(chain)}: {item}')
        direct, indirect = callees(name, functions[name], functions)
        report['indirect_calls'] += indirect
        for target in sorted(direct - reached):
            reached.add(target)
            parents[target] = name
            queue.append(target)
    report['reachable'] = len(reached)
    report['failures'] = failures
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('images', type=Path, nargs='+')
    parser.add_argument('--objdump', default='/usr/lib/llvm-21/bin/llvm-objdump')
    parser.add_argument('--roots', default=DEFAULT_ROOTS, help='regular expression naming EL3 boot functions')
    parser.add_argument('--output', type=Path, help='write the JSON report here')
    args = parser.parse_args()
    reports = [check(image, args.objdump, args.roots) for image in args.images]
    for report in reports:
        status = 'FAIL' if report['failures'] else 'PASS'
        print(f'{status} {report["image"]}: {len(report["roots"])} roots, {report["reachable"]} reachable '
              f'functions, {report["indirect_calls"]} indirect calls not followed')
        for failure in report['failures'][:20]:
            print(f'  {failure}')
        if len(report['failures']) > 20:
            print(f'  ... {len(report["failures"]) - 20} more')
    if args.output:
        args.output.write_text(json.dumps(reports, indent=1) + '\n')
    return 1 if any(report['failures'] for report in reports) else 0


if __name__ == '__main__':
    sys.exit(main())
