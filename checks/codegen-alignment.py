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
- a relaxed C unit calls the unaligned memory routines of runtime/memory_unaligned.c, and a strict unit the shared strict ones
  (runtime/memory.c, memmove.c): the force-included header is the whole difference between them.

Then it mutates the build record twice, once by removing -mstrict-align from a unit of the strict list and once by appending
-mno-strict-align to another, and requires the check to fail on each. It reads files only.
"""
import argparse
import copy
import json
from pathlib import Path
import re
import shlex
import sys

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('cmake/codegen-align.cmake', 'cmake/sunstone-os-core.cmake', 'cmake/sunstone-application.cmake',
                'include/sunstone_relaxed_memory.h', 'runtime/memory_unaligned.c', 'checks/codegen-alignment.py')
GATE_RUNS = [['--build', '{build:sup}', '--output', '{output}']]
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='OS build directory with compile_commands.json and alignment-classes.json')
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
    (args.output / 'result.json').write_text(json.dumps({'counts': count, 'mutations': mutants}, indent=1))
    print('PASS alignment classes: %d strict, %d strict by default, %d relaxed and %d application units follow their lists; '
          '%d mutations refused' % (count['strict'], count['default'], count['relaxed'], count['application'], len(mutants)))


main()
