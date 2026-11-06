#!/usr/bin/env python3
"""Fail if code reachable from the application interface (ss_*) executes cache, TLB or translation maintenance.

Applications and the tasks they create run at EL0, where dc (other than zva), ic, tlbi and at trap. The first scanning-radio
probe took such a trap: ss_finish called os_wifi_credential_wipe, whose dc civac faulted at EL0 (ESR class 0x18). The check
disassembles the image, takes every global ss_* function as a root, follows direct calls (bl and b to another function, not
into the FreeRTOS kernel API, which the port enters at raised privilege) and reports any such instruction with the path from
its root. Indirect calls are not followed.

One edge is allowed on a runtime guard: the failure path board_fail wipes the Wi-Fi credential, which is right for the EL1
callers and wrong for ss_fail. ss_fail and ss_fail_value set board_fail_from_el0 first and board_fail then skips the wipe.
The check requires both functions to reference that variable before it stops at board_fail.

usage: el0-privilege.py IMAGE.elf|BUILD_DIR [--allow FUNCTION ...]  (the self-test always runs first)
"""
GATE_RUNS = [['{build:sup}'], ['{build:ret-radio-scan}']]
import argparse
import re
import subprocess
import sys

OBJDUMP = '/usr/bin/llvm-objdump-21'
# System registers that EL0 may read or write (Arm ARM D1.10, trapped accesses): counters, thread IDs, flags.
# (kept for reference: registers EL0 may use)
EL0_REGISTERS = {'cntpct_el0', 'cntvct_el0', 'cntfrq_el0', 'nzcv', 'fpcr', 'fpsr', 'tpidr_el0', 'tpidrro_el0', 'dczid_el0',
                 'ctr_el0', 'daif'}
FORBIDDEN = [('cache maintenance', re.compile(r'^\s*(dc\s+(?!zva\b)\w+|ic\s+\w+)')),
             ('TLB maintenance', re.compile(r'^\s*tlbi\b')),
             ('address translation', re.compile(r'^\s*at\s'))]
GUARDS = {'board_fail': ('board_fail_from_el0', ('ss_fail', 'ss_fail_value'))}


def disassemble(image):
    text = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', image], capture_output=True, text=True, check=True).stdout
    functions, current = {}, None
    for line in text.splitlines():
        header = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if header:
            current = header.group(1)
            functions[current] = []
            continue
        instruction = re.match(r'^\s*[0-9a-f]+:\s+(.*)$', line)
        if instruction and current:
            functions[current].append(instruction.group(1).split(';')[0].strip())
    return functions


def callee(instruction):
    match = re.match(r'^(?:bl|b)\s+(?:0x)?[0-9a-f]+ <([^>+]+)(?:\+0x[0-9a-f]+)?>$', instruction)
    return match.group(1) if match else None


# The FreeRTOS kernel API (xTaskGenericNotify, vTaskSuspend, ...): application code enters it through the privilege-safe
# port, which raises privilege for the call, so what it executes is not EL0 code.
KERNEL_API = re.compile(r'^(x|v|ul|ux|pv|px|prv|port)[A-Z]')


def violations(instruction):
    found = [name for name, pattern in FORBIDDEN if pattern.match(instruction)]
    return found


def symbols(image):
    text = subprocess.run([OBJDUMP, '-t', image], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 5 and re.fullmatch(r'[0-9a-f]{16}', fields[0]):
            table[fields[-1]] = int(fields[0], 16)
    return table


def references(instructions, address):
    """True if the instructions form an address of `address` (adrp plus add, the way the compiler addresses a global)
    or name the symbol directly."""
    pages = {}
    for instruction in instructions:
        page = re.match(r'^adrp\s+(\w+),\s*0x([0-9a-f]+)', instruction)
        if page:
            pages[page.group(1)] = int(page.group(2), 16)
            continue
        add = re.match(r'^add\s+(\w+),\s*(\w+),\s*#0x([0-9a-f]+)', instruction)
        if add and add.group(2) in pages and pages[add.group(2)] + int(add.group(3), 16) == address:
            return True
        add = re.match(r'^add\s+(\w+),\s*(\w+),\s*#(\d+)', instruction)
        if add and add.group(2) in pages and pages[add.group(2)] + int(add.group(3)) == address:
            return True
    return False


def guard_problems(functions, table=None):
    problems = []
    for pruned, (variable, setters) in GUARDS.items():
        if pruned not in functions:
            continue
        for setter in setters:
            if setter not in functions:
                continue
            named = any('<%s>' % variable in i for i in functions[setter])
            if not named and not (table and variable in table and references(functions[setter], table[variable])):
                problems.append('%s does not set %s, so the guarded path through %s is not allowed' % (setter, variable, pruned))
    return problems


def check(functions, roots, allow=(), table=None):
    allow = set(allow) | set(p for p in GUARDS if not guard_problems(functions, table))
    bad = []
    seen = {}
    pending = [(root, [root]) for root in sorted(roots) if root in functions]
    while pending:
        name, path = pending.pop()
        if name in seen or name in allow:
            continue
        seen[name] = path
        for instruction in functions.get(name, []):
            for reason in violations(instruction):
                bad.append((reason, instruction, path))
            target = callee(instruction)
            if target and target in functions and target not in seen and not KERNEL_API.match(target):
                pending.append((target, path + [target]))
    return bad, seen


def self_test():
    functions = {
        'ss_ok': ['mrs x0, CNTPCT_EL0', 'bl 0x10 <helper>', 'ret'],
        'helper': ['ret'],
        'ss_bad': ['bl 0x20 <wipe>', 'ret'],
        'wipe': ['dc civac, x0', 'dsb sy', 'ret'],
        'ss_zva': ['dc zva, x0'], 'ss_tlb': ['tlbi vmalle1is'],
        'ss_fail': ['adrp x8, 0x10 <board_fail_from_el0>', 'bl 0x30 <board_fail>'], 'board_fail': ['bl 0x20 <wipe>'],
    }
    assert check(functions, ['ss_fail'])[0] == []                        # guarded by the flag
    unguarded = dict(functions, ss_fail=['bl 0x30 <board_fail>'])
    assert [r for r, _, _ in check(unguarded, ['ss_fail'])[0]] == ['cache maintenance']
    bad, _ = check(functions, [n for n in functions if n.startswith('ss_')])
    reasons = sorted((path[-1], reason) for reason, _, path in bad)
    assert reasons == [('ss_tlb', 'TLB maintenance'), ('wipe', 'cache maintenance')], reasons
    assert check(functions, ['ss_ok', 'ss_zva'])[0] == []
    print('PASS el0-privilege self-test: dc civac behind ss_bad and tlbi are found; dc zva and counters are not; board_fail is allowed only behind the guard flag')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', help='an image ELF, or a build directory whose *.elf images are scanned')
    parser.add_argument('--allow', nargs='*', default=[])
    args = parser.parse_args()
    self_test()
    import pathlib
    target = pathlib.Path(args.image)
    images = sorted(target.glob('*.elf')) if target.is_dir() else [target]
    if not images:
        sys.exit('no image in ' + str(target))
    status = 0
    for image in images:
        status |= scan(str(image), args.allow)
    return status


def scan(image, allow):
    functions, table = disassemble(image), symbols(image)
    roots = [n for n in functions if n.startswith('ss_')]
    for problem in guard_problems(functions, table):
        print('EL0 guard: ' + problem)
    bad, seen = check(functions, roots, set(allow), table)
    for reason, instruction, path in bad:
        print('EL0-reachable %s: "%s" via %s' % (reason, instruction, ' -> '.join(path)))
    if bad:
        return 1
    print('PASS el0-privilege: %s: %d functions reachable from %d ss_* entry points, none executes cache or TLB maintenance' % (image, len(seen), len(roots)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
