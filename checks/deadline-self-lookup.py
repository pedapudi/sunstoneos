#!/usr/bin/env python3
"""Check that a deadline wait finds its task and slot with one read of the task register.

sunstone_deadline_wait_until (runtime/deadline.c) finds the calling task and its slot through
sunstone_task_self_local (include/sunstone_task_self.h): one MRS of TPIDRRO_EL0, whose bits [47:0] hold the running
task's TCB, and one load of the task-local pointer from that TCB. The check reads the linked OS image's code for the
function and requires exactly one MRS of TPIDRRO_EL0, ahead of every call, whose value is next masked to bits [47:0].
A task's own TCB does not change when it migrates, so that read is migration-safe; the core index in bits [55:48]
is not, since the task can move after the read.

Two mutants are relinked from the compile and link commands of the sup build, and each must be refused: the wait
with the kernel's two out-of-line calls (xTaskGetCurrentTaskHandle, then pvTaskGetThreadLocalStoragePointer), and the
wait that takes the current task of the core named by bits [55:48], as portCURRENT_TCB(ssKernelCPUId()) does.

The check proves the instruction sequence only. The register's contents are the port's (docs/smp.md), and the
deadline behaviour is checked by checks/deadline.py.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--elf', '{elf:sup}', '--mutant-build', '{build:sup}', '--output', '{output}']]
CHECK_INPUTS = ['runtime/deadline.c', 'include/sunstone_task_self.h', 'platform/qemu-smp/freertos_tasks_c_additions.h',
                'patches/freertos-smp-macro.patch', 'checks/deadline-owners.py']
OBJDUMP = '/usr/lib/llvm-21/bin/llvm-objdump'
FUNCTION = 'sunstone_deadline_wait_until'
# The self lookup in own() (runtime/deadline.c) that the mutants replace.
LOOKUP = '*self=sunstone_task_self_local(SUNSTONE_DEADLINE_TLS,&local);'
MUTANTS = {
    'two-calls': '*self=xTaskGetCurrentTaskHandle();\n'
                 '    local=*self?pvTaskGetThreadLocalStoragePointer(NULL,SUNSTONE_DEADLINE_TLS):NULL;',
    'core-indexed': '*self=xTaskGetCurrentTaskHandleForCore((BaseType_t)((ullPortGetTaskRegister()>>48)&255));\n'
                    '    local=*self?pvTaskGetThreadLocalStoragePointer(NULL,SUNSTONE_DEADLINE_TLS):NULL;',
}


def instructions(elf):
    """(mnemonic, operands) of FUNCTION in elf, in address order."""
    done = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '--disassemble-symbols=' + FUNCTION, str(elf)],
                          capture_output=True, text=True, check=True)
    out = []
    for line in done.stdout.splitlines():
        m = re.match(r'\s*[0-9a-f]+:\s+(\S+)\s*(.*)$', line)
        if m:
            out.append((m.group(1), m.group(2).split('//')[0].strip()))
    assert out, '%s has no code for %s' % (elf, FUNCTION)
    return out


def refusals(code):
    """Why the code of FUNCTION does not make one migration-safe read of the task register; empty when it does."""
    reads = [i for i, (op, args) in enumerate(code) if op == 'mrs' and args.lower().endswith('tpidrro_el0')]
    if len(reads) != 1:
        return ['%d reads of TPIDRRO_EL0, not 1' % len(reads)]
    at = reads[0]
    found = []
    calls = [i for i, (op, _) in enumerate(code) if op in ('bl', 'blr')]
    if calls and calls[0] < at:
        found.append('a call precedes the read of TPIDRRO_EL0')
    register = code[at][1].split(',')[0].strip()
    op, args = code[at + 1] if at + 1 < len(code) else ('', '')
    parts = [p.strip() for p in args.split(',')]
    if op not in ('and', 'ands') or len(parts) != 3 or parts[1] != register or parts[2] != '#0xffffffffffff':
        found.append('the read is not next masked to the TCB field: %s %s' % (op, args))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--mutant-build', type=Path, required=True, help='the build whose ELF is --elf')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    found = refusals(instructions(args.elf))
    spec = importlib.util.spec_from_file_location('deadline_owners', ROOT / 'checks/deadline-owners.py')
    owners = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(owners)
    source = ROOT / 'runtime/deadline.c'
    assert source.read_text().count(LOOKUP) == 1, 'own() in runtime/deadline.c no longer makes the lookup the mutants replace'
    caught = {}
    for name, replacement in MUTANTS.items():
        elf = owners.mutant_image(args.mutant_build, args.elf, args.output, 'mutant-' + name, lambda p: p == source,
                                  lambda text, r=replacement: text.replace(LOOKUP, r, 1))
        caught[name] = refusals(instructions(elf))
        assert caught[name], 'the mutant %s passed' % name
    report = {'image': str(args.elf), 'function': FUNCTION, 'refusals': found, 'mutants': caught,
              'passed': not found, 'physical_access': False}
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    assert not found, 'the self lookup of %s: %s' % (FUNCTION, '; '.join(found))
    print('PASS deadline self lookup: one TPIDRRO_EL0 read masked to the TCB; mutants refused: %s' %
          ', '.join('%s (%s)' % (n, r[0]) for n, r in caught.items()))


if __name__ == '__main__':
    main()
