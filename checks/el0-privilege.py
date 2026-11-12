#!/usr/bin/env python3
"""Fail if code reachable from the application interface (ss_*) executes cache, TLB or translation maintenance.

Applications and the tasks they create run at EL0, where dc (other than zva), ic, tlbi and at trap. The first scanning-radio
probe took such a trap: ss_finish called os_wifi_credential_wipe, whose dc civac faulted at EL0 (ESR class 0x18). The check
disassembles the image, takes every global ss_* function as a root, follows direct calls (bl and b to another function, not
into the FreeRTOS kernel API, which the port enters at raised privilege) and reports any such instruction with the path from
its root. Indirect calls are not followed.

The board_fail and board_fail_at paths skip the Wi-Fi credential wipe at EL0. The check executes both linked entry bodies
with the actual privilege predicate before excluding their guarded calls from the static graph. It also verifies that the
existing board_fail_from_el0 flag defers the wipe, including when a later caller runs at EL1. No credential bytes are read.

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
GUARDS = {name: ('board_fail_from_el0', ('ss_fail', 'ss_fail_value'))
          for name in ('board_fail', 'board_fail_at')}


def disassemble(image):
    text = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', image], capture_output=True, text=True, check=True).stdout
    return parse_disassembly(text)


def parse_disassembly(text):
    # Local symbols can share a name across translation units. Resolve calls
    # by address so a later unrelated `fail` cannot replace an earlier one.
    entries, current = [], None
    for line in text.splitlines():
        header = re.match(r'^([0-9a-f]+) <(.+)>:$', line)
        if header:
            current = []
            entries.append((int(header.group(1), 16), header.group(2), current))
            continue
        instruction = re.match(r'^\s*[0-9a-f]+:\s+(.*)$', line)
        if instruction and current is not None:
            current.append(instruction.group(1).split(';')[0].strip())
    counts = {}
    for _, name, _ in entries:
        counts[name] = counts.get(name, 0) + 1
    names = {address: name if counts[name] == 1 else '%s@%x' % (name, address)
             for address, name, _ in entries}
    functions = {}
    for address, _, instructions in entries:
        resolved = []
        for instruction in instructions:
            branch = re.match(r'^(?:bl|b)\s+(?:0x)?([0-9a-f]+) <([^>+]+)(?:\+0x([0-9a-f]+))?>$', instruction)
            if branch:
                base = int(branch.group(1), 16) - int(branch.group(3) or '0', 16)
                if base in names:
                    instruction = instruction.replace('<' + branch.group(2), '<' + names[base], 1)
            resolved.append(instruction)
        functions[names[address]] = resolved
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


def references_variable(instructions, variable, table):
    return any('<%s>' % variable in i for i in instructions) or bool(
        table and variable in table and references(instructions, table[variable]))


def guard_problems(functions, table=None):
    problems = []
    for pruned, (variable, setters) in GUARDS.items():
        if pruned not in functions:
            continue
        for setter in setters:
            if setter not in functions:
                continue
            if not references_variable(functions[setter], variable, table):
                problems.append('%s does not set %s, so the guarded path through %s is not allowed' % (setter, variable, pruned))
        if not references_variable(functions[pruned], variable, table):
            problems.append('%s does not reference its guard %s' % (pruned, variable))
    return problems


def check(functions, roots, allow=(), table=None, verified_guards=()):
    allow = set(allow)
    guarded = {} if guard_problems(functions, table) else GUARDS
    bad = []
    seen = {}
    contexts = set()
    pending = [(root, [root]) for root in sorted(roots) if root in functions]
    while pending:
        name, path = pending.pop()
        protected = tuple(target for target, (_, setters) in guarded.items()
                          if any(setter in path[:-1] for setter in setters))
        context = name, protected
        if context in contexts or name in allow:
            continue
        if name in protected and name not in verified_guards:
            continue
        contexts.add(context)
        seen[name] = path
        for instruction in functions.get(name, []):
            for reason in violations(instruction):
                bad.append((reason, instruction, path))
            target = callee(instruction)
            if name in verified_guards and target == 'os_wifi_credential_wipe':
                continue
            if target and target in functions and not KERNEL_API.match(target):
                pending.append((target, path + [target]))
    return bad, seen


def self_test():
    duplicate = parse_disassembly('''00000010 <ss_clean>:
  10: bl 0x20 <fail>
00000020 <fail>:
  20: ret
00000030 <ss_dirty>:
  30: bl 0x40 <fail>
00000040 <fail>:
  40: dc civac, x0
''')
    assert check(duplicate, ['ss_clean'])[0] == []
    assert check(duplicate, ['ss_dirty'])[0][0][2] == ['ss_dirty', 'fail@40']
    functions = {
        'ss_ok': ['mrs x0, CNTPCT_EL0', 'bl 0x10 <helper>', 'ret'],
        'helper': ['ret'],
        'ss_bad': ['bl 0x20 <wipe>', 'ret'],
        'wipe': ['dc civac, x0', 'dsb sy', 'ret'],
        'ss_zva': ['dc zva, x0'], 'ss_tlb': ['tlbi vmalle1is'],
        'ss_fail': ['adrp x8, 0x10 <board_fail_from_el0>', 'bl 0x30 <board_fail>'],
        'ss_fail_value': ['adrp x8, 0x10 <board_fail_from_el0>', 'bl 0x40 <board_fail_at>'],
        'board_fail': ['adrp x8, 0x10 <board_fail_from_el0>', 'bl 0x20 <wipe>'],
        'board_fail_at': ['adrp x8, 0x10 <board_fail_from_el0>', 'bl 0x20 <wipe>'],
    }
    assert check(functions, ['ss_fail'])[0] == []                        # guarded by the flag
    assert check(functions, ['ss_fail_value'])[0] == []
    unguarded = dict(functions, ss_fail=['bl 0x30 <board_fail>'])
    assert [r for r, _, _ in check(unguarded, ['ss_fail'])[0]] == ['cache maintenance']
    for target in GUARDS:
        missing = dict(functions, **{target: ['bl 0x20 <wipe>']})
        assert check(missing, ['ss_fail', 'ss_fail_value'])[0]
        bypass = dict(functions, ss_direct_fail=['bl 0x30 <%s>' % target])
        assert check(bypass, ['ss_direct_fail'])[0]
        shared = dict(functions, ss_fail=['adrp x8, 0x10 <board_fail_from_el0>', 'bl 0x50 <shared>'],
                      ss_direct_fail=['bl 0x50 <shared>'], shared=['bl 0x30 <%s>' % target])
        assert check(shared, ['ss_fail', 'ss_direct_fail'])[0]
    bad, _ = check(functions, [n for n in functions if n.startswith('ss_')])
    reasons = sorted((path[-1], reason) for reason, _, path in bad)
    assert reasons == [('ss_tlb', 'TLB maintenance'), ('wipe', 'cache maintenance')], reasons
    assert check(functions, ['ss_ok', 'ss_zva'])[0] == []
    native = {'ss_direct_fail': ['bl 0x30 <board_fail>'],
              'board_fail': ['dc civac, x1', 'bl 0x40 <board_fail_note>', 'bl 0x50 <os_wifi_credential_wipe>'],
              'board_fail_note': ['tlbi vmalle1is'], 'os_wifi_credential_wipe': ['dc civac, x0']}
    assert sorted(reason for reason, _, _ in check(native, ['ss_direct_fail'], verified_guards=GUARDS)[0]) == [
        'TLB maintenance', 'cache maintenance']
    nested = dict(native, board_fail=['bl 0x40 <board_fail_note>'],
                  board_fail_note=['bl 0x50 <os_wifi_credential_wipe>'])
    assert check(nested, ['ss_direct_fail'], verified_guards=GUARDS)[0][0][2] == [
        'ss_direct_fail', 'board_fail', 'board_fail_note', 'os_wifi_credential_wipe']
    print('PASS el0-privilege self-test: maintenance is found; both board failure entries require the flag and a setter path')


def native_guard_cases(path, table):
    """Execute the actual guard; only its external effects are intercepted."""
    required = {'os_task_at_el1', 'board_fail', 'board_fail_at', 'board_fail_note',
                'board_fail_from_el0', 'os_wifi_credential_wipe'}
    if not required <= table.keys():
        return set()
    from harness import Image, machine
    from unicorn import UC_HOOK_CODE
    from unicorn.arm64_const import (UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC,
                                     UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                     UC_ARM64_REG_TPIDRRO_EL0)
    image = Image(path, layout_names={'board_application', 'a733_board_session'})
    cpu = machine()
    cpu.mem_map(0x41000000, 0x2000000)
    cpu.mem_map(0x50000000, 0x10000)
    for address, data in image.parts:
        if data:
            cpu.mem_write(address, data)
    board = image.address('board', 'os-image/main.c')
    workload = board + image.layouts['board_application']['session'] + image.layouts['a733_board_session']['workload']
    fatal = board + image.layouts['board_application']['fatal']
    flag = image.address('board_fail_from_el0')
    stop = 0x5000fff0
    wipes = []

    def hook(uc, address, size, unused):
        if address == stop:
            uc.emu_stop()
        elif address == table['os_wifi_credential_wipe']:
            wipes.append(address)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
        elif address == table['board_fail_note']:
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
        elif address == table.get('board_session_lifetime'):
            # A CPU-return failure is not an application failure, so the
            # actual return claimant declines it and reaches fail_session.
            uc.reg_write(UC_ARM64_REG_X0, 3)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    cpu.hook_add(UC_HOOK_CODE, hook)

    def call(name, tag, args=()):
        wipes.clear()
        cpu.reg_write(UC_ARM64_REG_TPIDRRO_EL0, tag)
        cpu.reg_write(UC_ARM64_REG_SP, 0x5000f000)
        cpu.reg_write(UC_ARM64_REG_LR, stop)
        for register, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2), args):
            cpu.reg_write(register, value)
        cpu.emu_start(table[name], stop, count=10000)
        assert cpu.reg_read(UC_ARM64_REG_PC) == stop, (name, hex(tag), 'did not return')
        return cpu.reg_read(UC_ARM64_REG_X0)

    tags = [(core << 48, True) for core in range(8)]
    tags += [(tcb | core << 48 | privileged << 56, bool(privileged))
             for tcb in (1, 0x41001000, (1 << 48) - 1)
             for core in range(8) for privileged in (0, 1)]
    count = 0
    for entry in GUARDS:
        for tag, privileged in tags:
            for radio in (0, 1):
                for deferred in (0, 1):
                    cpu.mem_write(workload, radio.to_bytes(4, 'little'))
                    cpu.mem_write(flag, deferred.to_bytes(4, 'little'))
                    cpu.mem_write(fatal, bytes(4))
                    call(entry, tag, (0x80003738, 0, 0x41001234))
                    expected = bool(radio and privileged and not deferred)
                    assert bool(wipes) == expected, (entry, hex(tag), radio, deferred, 'credential wipe', wipes)
                    assert int.from_bytes(cpu.mem_read(fatal, 4), 'little') == 1, (entry, 'admission not closed')
                    count += 1
    for tag, privileged in tags:
        assert call('os_task_at_el1', tag) == int(privileged), ('task privilege', hex(tag), privileged)
    print('PASS el0-privilege native guards: %d task tags and %d board failure cases; credential bytes untouched' % (len(tags), count))
    return set(GUARDS)


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
    verified = native_guard_cases(image, table)
    for problem in guard_problems(functions, table):
        print('EL0 guard: ' + problem)
    bad, seen = check(functions, roots, set(allow), table, verified)
    for reason, instruction, path in bad:
        print('EL0-reachable %s: "%s" via %s' % (reason, instruction, ' -> '.join(path)))
    if bad:
        return 1
    print('PASS el0-privilege: %s: %d functions reachable from %d ss_* entry points, none executes cache or TLB maintenance' % (image, len(seen), len(roots)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
