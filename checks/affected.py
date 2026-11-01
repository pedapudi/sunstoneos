#!/usr/bin/env python3
"""List checks affected by a Git range or repository paths, plus one OS baseline.

Dependency coverage comes from check-index.py, including literal CHECK_INPUTS declared
by checks. Git ranges use both endpoint inventories, retaining dependencies
removed by a deletion or rename. Textual inference cannot find every dynamically
assembled path; the result is a selection aid, not proof of complete coverage.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location('check_index', Path(__file__).with_name('check-index.py'))
indexer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(indexer)
ROOT = indexer.ROOT
BASELINE = 'sup'


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT).decode().strip()


def endpoints(value):
    separator = '...' if '...' in value else '..'
    if separator not in value or len(value.split(separator)) != 2:
        raise ValueError('range must be BASE..HEAD or BASE...HEAD')
    base, head = (git('rev-parse', '--verify', part + '^{commit}') for part in value.split(separator))
    if separator == '...':
        base = git('merge-base', base, head)
    return base, head


def inventory(revision):
    tree = indexer.Revision(revision)
    result = {}
    for path in tree.files:
        if path.parent != ROOT / 'checks' or path.suffix != '.py' or path.name in ('harness.py', 'check-index.py', 'affected.py'):
            continue
        inputs = indexer.covered(path, tree)
        result[path.name] = {'inputs': sorted(inputs), 'input_directories': sorted(
            p for p in inputs if ROOT / p in tree.directories)}
    return result


def needs(entry):
    if entry is None:
        return ['check absent from the current checkout']
    if entry['pure_host']:
        return []
    return (['helper module'] * entry['library'] + ['USB access'] * entry['usb'] +
            entry['required_arguments'] + ['cache/' + a for a in entry['cache_areas']
                                          if a not in indexer.NEUTRAL_AREAS] +
            ([entry['input_reason']] if entry.get('input_reason') else [])) or ['additional inputs']


def plan(value=None, changed=None, index=None):
    index = index if index is not None else indexer.generate()
    selected = set()
    revisions = None
    if value:
        revisions = endpoints(value)
        paths = subprocess.check_output(['git', 'diff', '--name-only', '-z', '--no-renames', *revisions], cwd=ROOT)
        changed = [p.decode() for p in paths.split(b'\0') if p]
        for revision in dict.fromkeys(revisions):
            selected.update(indexer.select(inventory(revision), changed))
    changed = changed or []
    selected.update(indexer.select(index, changed))
    checks = []
    for name in sorted(selected):
        entry = index.get(name)
        required = needs(entry)
        checks.append({'check': name, 'status': 'pending' if required else 'runnable', 'needs': required,
                       'output': bool(entry and '--output' in entry['required_arguments'])})
    return {'range': value, 'revisions': revisions, 'changed': changed, 'baseline': BASELINE,
            'checks': checks, 'coverage': 'declared and inferred repository inputs'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('range', nargs='?')
    selection.add_argument('--changed', nargs='+', metavar='PATH')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args(argv)
    try:
        result = plan(args.range, args.changed)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    if args.json:
        print(json.dumps(result, indent=2))
        return
    print('BUILD sunstone-a733-os --variant ' + result['baseline'])
    for check in result['checks']:
        detail = ': ' + ', '.join(check['needs']) if check['needs'] else ''
        print(('PENDING ' if check['needs'] else 'RUN ') + 'checks/' + check['check'] + detail)
    print(f"{len(result['checks'])} selected checks; coverage uses declared and inferred inputs")


if __name__ == '__main__':
    main()
