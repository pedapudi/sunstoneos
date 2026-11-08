#!/usr/bin/env python3
"""The code generation flags of the units of an OS image: which units are compiled for strict alignment.

cmake/codegen-align.cmake lists the units that must keep -mstrict-align (code that runs with the MMU off, at EL3, or on
Device memory) and the units that run under Normal write-back memory and are built with -mno-strict-align; the build writes
both lists to alignment-classes.json beside compile_commands.json. This check reads the two files of an OS build and requires:

- every unit of the OS image's targets and of its applications has -mtune=cortex-a55;
- every unit on the strict list ends with -mstrict-align as the last of the two alignment flags (the last one wins);
- every unit on the relaxed list ends with -mno-strict-align, and a C unit of it is compiled with the force-included
  include/sunstone_relaxed_memory.h, so that it links the unaligned memory routines;
- no unit is on both lists, and a unit on neither the strict, the strict-by-default nor the relaxed list is reported;
- no function of a relaxed unit is reachable, by direct calls, from the EL3 code that runs before the MMU is on or on Device
  memory (a733_smp_boot, the return terminal): such a unit would fault on an unaligned access there and hang the boot;
- a relaxed C unit calls the unaligned memory routines of runtime/memory_unaligned.c, and a strict unit the shared strict ones
  (runtime/memory.c, memmove.c): the force-included header is the whole difference between them.

Then it mutates the build record twice, once by removing -mstrict-align from a unit of the strict list and once by appending
-mno-strict-align to another, and requires the check to fail on each. It reads files only.
"""
import argparse
import collections
import copy
import json
from pathlib import Path
import re
import shlex
import sys

ROOT = Path(__file__).resolve().parents[1]
# EL3 entry points: the monitor boot, which admits the session and enables the MMU, and the return terminal's restore.
EL3_ROOTS = ('a733_smp_boot', 'a733_qualification_restore_local')
# Where EL3 hands over to code that runs at EL1 with the MMU on; the walk from the roots stops there.
EL1_ENTRIES = {'a733_cached_el1', 'a733_cached_enter_el1', 'a733_smp_primary_el1', 'a733_smp_pre_scheduler',
               'a733_memory_local_prepare', 'local_boot', 'prepare_core', 'a733_cpu_continue_primary_boot',
               'a733_smp_diagnostic_start'}
CHECK_INPUTS = ('cmake/codegen-align.cmake', 'cmake/sunstone-os-core.cmake', 'cmake/sunstone-application.cmake',
                'include/sunstone_relaxed_memory.h', 'runtime/memory_unaligned.c', 'checks/codegen-alignment.py')
GATE_RUNS = [['--build', '{build:sup}', '--elf', '{elf:sup}', '--output', '{output}']]
ALIGN = ('-mstrict-align', '-mno-strict-align')
HEADER = 'sunstone_relaxed_memory.h'

if not __debug__:
    raise RuntimeError('checks require assertions')


def effective(arguments):
    """'strict' or 'relaxed': the last of the two alignment flags decides; none means the compiler's default (relaxed)."""
    last = [a for a in arguments if a in ALIGN]
    return None if not last else ('strict' if last[-1] == '-mstrict-align' else 'relaxed')


def units(commands, classes):
    names = set(classes['targets']) | set(classes['applications'])
    for entry in commands:
        match = re.search(r'CMakeFiles/([^/]+)\.dir', entry.get('output', '') or '')
        if match and match.group(1) in names:
            yield match.group(1), entry['file'], shlex.split(entry['command'])


def problems(commands, classes):
    strict = [re.compile(p) for p in classes['strict']]
    default = [re.compile(p) for p in classes['strict_default']]
    relaxed = [re.compile(p) for p in classes['relaxed']]
    found, count = [], {'strict': 0, 'relaxed': 0, 'default': 0, 'application': 0}
    for target, path, arguments in units(commands, classes):
        name = path
        if '-mtune=cortex-a55' not in arguments:
            found.append('%s: not built with -mtune=cortex-a55' % name)
        is_strict = any(p.search(path) for p in strict)
        is_relaxed = any(p.search(path) for p in relaxed) or target in classes['applications']
        if is_strict and is_relaxed:
            found.append('%s: on both the strict and the relaxed list' % name)
        elif is_strict:
            count['strict'] += 1
            if effective(arguments) != 'strict':
                found.append('%s: on the strict list but built without -mstrict-align' % name)
        elif is_relaxed:
            count['application' if target in classes['applications'] else 'relaxed'] += 1
            if effective(arguments) != 'relaxed':
                found.append('%s: on the relaxed list but built with -mstrict-align' % name)
            if re.search(r'\.c$', path) and not path.endswith('/runtime/memory_unaligned.c') \
                    and not any(HEADER in a for a in arguments):
                found.append('%s: relaxed C unit without the force-included %s' % (name, HEADER))
        elif any(p.search(path) for p in default):
            count['default'] += 1
            if effective(arguments) != 'strict':
                found.append('%s: strict by default but built without -mstrict-align' % name)
        elif not path.endswith('.S'):
            found.append('%s: on no alignment list (cmake/codegen-align.cmake)' % name)
    return found, count


def call_graph(elf_path, map_path):
    """(calls, function -> object) of the linked image, by the direct branches of each function."""
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from elftools.elf.elffile import ELFFile
    objects = {}
    for line in map_path.read_text().splitlines():
        match = re.match(r'\s*[0-9a-f]+\s+[0-9a-f]+\s+[0-9a-f]+\s+\d+\s+(\S+\.obj):\(\.text\.([A-Za-z0-9_.$]+)\)', line)
        if match:
            objects[match.group(2)] = match.group(1)
    elf = ELFFile(open(elf_path, 'rb'))
    text = next(s for s in elf.iter_sections() if s.name == '.text')
    symbols = {}
    for section in elf.iter_sections():
        if section.header.sh_type == 'SHT_SYMTAB':
            for symbol in section.iter_symbols():
                if symbol['st_info']['type'] == 'STT_FUNC' and symbol['st_size']:
                    symbols[symbol['st_value']] = (symbol.name, symbol['st_size'])
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    calls = collections.defaultdict(set)
    for address, (name, size) in symbols.items():
        for instruction in disassembler.disasm(text.data()[address - text['sh_addr']:address - text['sh_addr'] + size], address):
            if instruction.mnemonic in ('bl', 'b') and instruction.op_str.startswith('#'):
                target = symbols.get(int(instruction.op_str[1:], 16))
                if target and target[0] != name:
                    calls[name].add(target[0])
    return calls, objects


def el3_problems(calls, objects, classes):
    """Functions of relaxed units reachable from the EL3 roots without passing into the EL1 world."""
    relaxed = [re.compile(p) for p in classes['relaxed']]
    assert all(root in calls for root in EL3_ROOTS), 'the EL3 roots are not in the image'
    seen, stack = {}, [(root, None) for root in EL3_ROOTS]
    while stack:
        name, parent = stack.pop()
        if name in seen or name in EL1_ENTRIES:
            continue
        seen[name] = parent
        stack.extend((callee, name) for callee in calls.get(name, ()))
    assert {'a733_smp_contract_valid', 'board_session_prepare', 'a733_cached_prepare'} <= set(seen), \
        'the walk from the EL3 roots no longer reaches the session admission'
    found = []
    for name in sorted(seen):
        obj = objects.get(name)
        if obj and any(r.search('/' + obj.split('.dir/', 1)[-1][:-4]) for r in relaxed):
            chain, at = [name], name
            while seen.get(at):
                at = seen[at]
                chain.append(at)
            found.append('%s (%s) is reachable from EL3: %s' % (name, obj.split('.dir/', 1)[-1], ' <- '.join(chain[:6])))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='OS build directory with compile_commands.json and alignment-classes.json')
    parser.add_argument('--elf', type=Path, required=True, help='the linked OS image of that build')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    commands = json.loads((args.build / 'compile_commands.json').read_text())
    classes = json.loads((args.build / 'alignment-classes.json').read_text())
    assert classes['tune'] == 'cortex-a55'
    found, count = problems(commands, classes)
    assert not found, '\n'.join(found[:30])
    assert count['strict'] and count['relaxed'] and (count['application'] or not classes['applications']), count
    # Mutation 1: a strict unit built without -mstrict-align. Mutation 2: a strict unit given -mno-strict-align after it.
    strict = [p for p in map(re.compile, classes['strict'])]
    victims = []
    for entry in commands:
        if any(p.search(entry['file']) for p in strict) and '-mstrict-align' in entry['command'] \
                and re.search(r'CMakeFiles/(%s)\.dir' % '|'.join(map(re.escape, classes['targets'])), entry.get('output', '')):
            victims.append(entry['file'])
    assert len(victims) >= 2, victims
    mutants = []
    for kind, victim in (('removed -mstrict-align', victims[0]), ('appended -mno-strict-align', victims[1])):
        changed = copy.deepcopy(commands)
        for entry in changed:
            if entry['file'] == victim:
                entry['command'] = (entry['command'].replace(' -mstrict-align', '', 1) if kind.startswith('removed')
                                    else entry['command'] + ' -mno-strict-align')
        bad, _ = problems(changed, classes)
        assert any(victim in line and 'strict' in line for line in bad), (kind, victim, bad[:3])
        mutants.append({'mutation': kind, 'unit': victim.replace(str(ROOT) + '/', ''), 'refused': True})
    relaxed_victim = next(e['file'] for e in commands if any(p.search(e['file']) for p in map(re.compile, classes['relaxed']))
                          and '-mno-strict-align' in e['command'] and e['file'].endswith('.c')
                          and not e['file'].endswith('memory_unaligned.c'))
    changed = copy.deepcopy(commands)
    for entry in changed:
        if entry['file'] == relaxed_victim:
            entry['command'] = entry['command'].replace(' -mno-strict-align', '')
    bad, _ = problems(changed, classes)
    assert any(relaxed_victim in line for line in bad), 'a relaxed unit built strict passed the check'
    mutants.append({'mutation': 'removed -mno-strict-align from a relaxed unit', 'unit': relaxed_victim.replace(str(ROOT) + '/', ''),
                    'refused': True})
    calls, objects = call_graph(args.elf, args.build / 'os-image/image.map')
    reached = el3_problems(calls, objects, classes)
    assert not reached, '\n'.join(reached[:20])
    # Mutation: the session decoder, which runs at EL3, listed as relaxed.
    changed = dict(classes, relaxed=classes['relaxed'] + [r'/apps/os/session\.c$'])
    assert any('os_session_decode_why' in line for line in el3_problems(calls, objects, changed)), \
        'a relaxed unit reachable from EL3 passed the check'
    mutants.append({'mutation': 'listed apps/os/session.c, which the EL3 boot calls, as relaxed', 'refused': True})
    (args.output / 'result.json').write_text(json.dumps({'counts': count, 'mutations': mutants}, indent=1))
    print('PASS alignment classes: %d strict, %d strict by default, %d relaxed and %d application units follow their lists, '
          'no relaxed function is reachable from EL3; %d mutations refused' % (count['strict'], count['default'], count['relaxed'], count['application'], len(mutants)))


main()
