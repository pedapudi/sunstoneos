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
Functions are told apart by address, because static functions of different
units share names (prepare_step, walk); a branch is followed to the address it
names, whatever the case of its symbol. The walk stops at the EL1 entries that
EL3 code reaches only after an exception return, because EL1 runs with its own
translation and is outside the boot rule.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
GATE_RUNS = [['{elf:sup}']]

STRING_ROUTINES = ('memcpy', 'memmove', 'memset')
# C entries and fault handlers that EL3 assembly reaches with translation off.
DEFAULT_ROOTS = (r'^(local_boot|monitor_init|a733_cpu_.*|a733_smp_boot|a733_cached_prepare'
                 r'|a733_cached_el3_fault|a733_cached_cold_fault|a733_stage2_main)$')
# Entered at EL1 after the eret in a733_cached_enter_el1 (platform/a733-smp-cached/continue.S), with EL1 translation on.
EL1_ENTRIES = ('a733_cached_el1',)
FUNCTION = re.compile(r'^([0-9a-f]+) <([^>]+)>:$')
INSTRUCTION = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
# Literal data inside a code section, such as SHA-256 round constants.
DATA = re.compile(r'^\s*[0-9a-f]+:(?:\s+[0-9a-f]{2})+\s+\.(?:byte|short|word|long|quad)\b')
SIMD_REGISTER = re.compile(r'\b(?:[vqdshb](?:[12]?[0-9]|3[01])(?:\.\w+)?|z[0-9]+|p[0-9]+)\b')
TARGET = re.compile(r'<([^>+]+)(\+0x[0-9a-f]+)?>')
BRANCH = re.compile(r'(?:0x)?([0-9a-f]+) <([^>+]+)(\+0x[0-9a-f]+)?>')
THUNK = re.compile(r'^__AArch64\w*Thunk_(.+)$')


def disassemble(objdump, image):
    """{start address: (name, instructions)}."""
    output = subprocess.check_output([objdump, '-d', '--no-show-raw-insn', str(image)], text=True)
    functions, current = {}, None
    for line in output.splitlines():
        match = FUNCTION.match(line)
        if match:
            current = functions.setdefault(int(match[1], 16), (match[2], []))[1]
            continue
        match = INSTRUCTION.match(line)
        if match and current is not None and not DATA.match(line):
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


def callees(start, instructions, functions, named):
    direct, indirect = set(), 0
    for _, mnemonic, operands in instructions:
        if mnemonic in ('blr', 'br', 'blraa', 'braa'):
            indirect += 1
            continue
        if mnemonic not in ('bl', 'b') and not mnemonic.startswith('b.') and mnemonic not in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            continue
        match = BRANCH.search(operands)
        if not match or match[3]:
            continue
        target = int(match[1], 16)
        thunk = THUNK.match(match[2])
        if thunk:
            # A range-extension thunk names its target; follow every function of that name.
            direct |= named.get(thunk[1], set()) - {start}
        elif target in functions and target != start:
            direct.add(target)
    return direct, indirect


def check(image, objdump, roots):
    functions = disassemble(objdump, image)
    named = {}
    for start, (name, _) in functions.items():
        named.setdefault(name, set()).add(start)
    failures, report = [], {'image': str(image), 'string_routines': {}, 'roots': [], 'reachable': 0,
                             'indirect_calls': 0}
    for name in STRING_ROUTINES:
        if name not in named:
            report['string_routines'][name] = 'absent'
            continue
        found = []
        for start in sorted(named[name]):
            instructions = functions[start][1]
            found += hazards(instructions)
            calls, indirect = callees(start, instructions, functions, named)
            if calls or indirect or any(m == 'bl' for _, m, _ in instructions):
                found.append('calls another routine')
        report['string_routines'][name] = found or 'clean'
        failures += [f'{name}: {item}' for item in found]
    pattern = re.compile(roots)
    queue = sorted(start for start, (name, _) in functions.items() if pattern.match(name))
    report['roots'] = sorted(functions[start][0] for start in queue)
    reached, parents = set(queue), {start: None for start in queue}
    while queue:
        start = queue.pop()
        for item in hazards(functions[start][1]):
            chain, step = [functions[start][0]], parents[start]
            while step is not None:
                chain.append(functions[step][0])
                step = parents[step]
            failures.append(f'{" <- ".join(chain)}: {item}')
        direct, indirect = callees(start, functions[start][1], functions, named)
        direct = {target for target in direct if functions[target][0] not in EL1_ENTRIES}
        report['indirect_calls'] += indirect
        for target in sorted(direct - reached):
            reached.add(target)
            parents[target] = start
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
