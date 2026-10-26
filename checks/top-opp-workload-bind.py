#!/usr/bin/env python3
"""Run a frozen step 6 workload binding on a real session's records.

The binding (tools/top-opp-workload-bind.py, frozen in a workload bundle as
cpu-prepare.py beside binding.json) runs with no device access on the run
records of a real port 3-3 lifetime in which bundle 26 passed
(docs/designs/cpu-top-opp.md, step 6). The check requires that it binds into
a new <stem>-prepare directory with every prepared file written and writable,
from a template whose frozen files are read-only, and that it refuses:
port 3-1; a bundle 26 record that names the image.elf hash instead of the
uploaded probe.bin; a survey on another USB instance; and a continuity list
without bundle 26. Each refusal runs on copies of the real records with one
field changed. The frozen cpu-prepare.py must equal the tree script at the
commit that binding.json names. Four more refusals run the frozen script in
a writable copy of the bundle whose pins disagree with bundle 26's frozen
row: binding.json's probe.bin hash, the row's probe.bin, the row's
artifacts.json, and an ELF pin equal to the probe pin. The real session's
records and the frozen bundle are only read."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The step 6 lifetime's records from the cold DDR start to the workload's
# cache-off helper, in time order (step6-set runbooks); the survey is passed
# on its own.
CONTINUITY = ('s7-ddr', 'c6-bb2-cache-off', 'c6-bb2-survey', 'c6-bb2-run', 'c6-opp25-cache-off', 'c6-opp25-survey',
              'c6-opp25-run', 'c6-opp26-cache-off', 'c6-opp26-survey', 'c6-opp26-run', 'c6-tw-state-before',
              'c6-tw-cache-off')
PYTHON = Path.home() / '.cache/sunstoneos/venv/bin/python'


def copy_records(session, into):
    """Copies of the records' session.json and kernel.log files, contents only."""
    for name in CONTINUITY + ('c6-tw-survey',):
        (into / name).mkdir(parents=True)
        for f in ('session.json', 'kernel.log'):
            if (session / name / f).is_file():
                shutil.copyfile(session / name / f, into / name / f)


def bind(bundle, session, review, out, port='3-3', names=CONTINUITY):
    command = [str(PYTHON), '-B', str(bundle / 'cpu-prepare.py'), '--bundle', str(bundle), '--port', port,
               '--survey', str(session / 'c6-tw-survey/session.json'), '--root-review', str(review)]
    for name in names:
        command += ['--continuity', str(session / name / 'session.json')]
    command += ['--output', str(out)]
    return subprocess.run(command, capture_output=True, text=True, env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))


def copy_tree(source, target):
    """A writable copy of a directory's contents."""
    for x in sorted(source.rglob('*')):
        y = target / x.relative_to(source)
        if x.is_dir():
            y.mkdir(parents=True, exist_ok=True)
        else:
            y.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(x, y)


def edit(path, **fields):
    record = json.loads(path.read_text())
    record.update(fields)
    path.write_text(json.dumps(record))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True, help='frozen workload bundle')
    parser.add_argument('--session', type=Path, required=True, help='real session directory, <...>/port-3-3')
    parser.add_argument('--output', type=Path, required=True, help='new directory')
    a = parser.parse_args()
    bundle, session, out = a.bundle.resolve(), a.session.resolve(), a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binding = json.loads((bundle / 'binding.json').read_text())
    # The frozen script is the tree script at the commit binding.json names.
    source = binding['binding_source']
    frozen = (bundle / 'cpu-prepare.py').read_bytes()
    at_commit = subprocess.run(['git', '-C', str(ROOT), 'show', f"{source['commit']}:{source['path']}"],
                               capture_output=True, check=True).stdout
    assert frozen == at_commit, 'the frozen cpu-prepare.py differs from the tree script at its recorded commit'
    tree_now = (ROOT / source['path']).read_bytes() == frozen
    template = bundle / 'template/prepared/admission.json'
    assert not os.access(template, os.W_OK), 'the frozen template must be read-only for this check'
    review = out / 'MODEL-root-review.json'
    review.write_text(json.dumps({'status': 'Coordinator reviewed this offline binding check only; not an admission '
                                            'record.'}))
    cases = {'frozen-binding-equals-the-tree-script-at-its-commit': True}

    # The real records, bound into a scratch <stem>-prepare directory.
    prepare = out / 'c6-tw-prepare'
    result = bind(bundle, session, review, prepare)
    assert result.returncode == 0, result.stdout + result.stderr
    for name in ('rationale.json', 'recovery.json', 'ddr.json', 'platform.json', 'admission.json', 'command.json',
                 'offline-preparation.json', 'prepared/prepared.json', 'prepared/request.bin', 'prepared/contract.bin'):
        assert (prepare / name).is_file(), name
    assert all(os.access(x, os.W_OK) for x in prepare.rglob('*')), 'a prepared file or directory is read-only'
    assert (prepare / 'prepared/admission.json').read_bytes() == (prepare / 'admission.json').read_bytes()
    command = json.loads((prepare / 'command.json').read_text())
    assert str(out / 'c6-tw-run') in command, 'the run record is not <stem>-run beside the preparation'
    record = json.loads((session / 'c6-opp26-run/session.json').read_text())
    assert record['image_sha256'] == binding['bundle26']['probe_sha256'] != binding['bundle26']['elf_sha256']
    cases['real-records-bind-from-a-read-only-template'] = True

    # Refusals, each on copies of the real records with one change.
    def refused(name, expected, change, port='3-3', names=CONTINUITY):
        copy = out / ('refusal-' + name) / 'port-3-3'
        copy_records(session, copy)
        change(copy)
        result = bind(bundle, copy, review, out / ('refusal-' + name) / 'c6-tw-prepare', port, names)
        assert result.returncode != 0 and expected in result.stdout + result.stderr, (name, result.stderr[-400:])
        cases['refuses-' + name] = True

    refused('port-3-1', "port 3-1's speed bin", lambda c: None, port='3-1')
    refused('bundle-26-named-by-its-elf-hash', 'The bundle 26 record names another image',
            lambda c: edit(c / 'c6-opp26-run/session.json', image_sha256=binding['bundle26']['elf_sha256']))
    survey = json.loads((session / 'c6-tw-survey/session.json').read_text())
    refused('survey-on-another-instance', "The survey is not on the instance that bundle 26's run returned to",
            lambda c: edit(c / 'c6-tw-survey/session.json', identity=survey['identity'][:1] + [survey['identity'][1] + 4]
                           + survey['identity'][2:]))
    refused('no-bundle-26', "is not bundle 26's pass", lambda c: None, names=tuple(n for n in CONTINUITY if not n.startswith('c6-opp26')))
    # Pins that disagree with bundle 26's frozen row, in a writable copy of the
    # bundle; the run records are the real ones.
    row = Path(binding['bundle26']['row'])

    def pin_refused(name, expected, pins=None, row_change=None):
        base = out / ('pins-' + name)
        copy_tree(bundle, base / 'bundle')
        changed = json.loads(json.dumps(binding))
        if row_change:
            copy_tree(row, base / 'row')
            listed = json.loads((base / 'row/artifacts.json').read_text())
            for item in listed:
                item['path'] = str(base / 'row' / Path(item['path']).relative_to(row))
            (base / 'row/artifacts.json').write_text(json.dumps(listed))
            row_change(base / 'row')
            changed['bundle26']['row'] = str(base / 'row')
        changed['bundle26'].update(pins or {})
        (base / 'bundle/binding.json').write_text(json.dumps(changed))
        result = bind(base / 'bundle', session, review, base / 'c6-tw-prepare')
        assert result.returncode != 0 and expected in result.stdout + result.stderr, (name, result.stderr[-400:])
        assert not (base / 'c6-tw-prepare').exists(), name
        cases['refuses-' + name] = True

    def flip(path):
        data = bytearray(path.read_bytes())
        data[-1] ^= 1
        path.write_bytes(bytes(data))

    def relist(r):
        listed = json.loads((r / 'artifacts.json').read_text())
        for item in listed:
            if item['path'].endswith('/bundle/probe.bin'):
                item['sha256'] = '0' * 64
        (r / 'artifacts.json').write_text(json.dumps(listed))

    pin_refused('binding-probe-pin-differs', "Bundle 26's frozen probe.bin differs",
                pins={'probe_sha256': hashlib.sha256(b'other').hexdigest()})
    pin_refused('row-probe-bin-differs', "Bundle 26's frozen probe.bin differs",
                row_change=lambda r: flip(r / 'bundle/probe.bin'))
    pin_refused('row-artifacts-list-differs', "Bundle 26's frozen probe.bin differs", row_change=relist)
    pin_refused('elf-pin-equals-probe-pin', "Bundle 26's frozen image.elf differs",
                pins={'elf_sha256': binding['bundle26']['probe_sha256']})
    # The same copies with no change bind: the refusals come from the changes.
    copy = out / 'copies-unchanged' / 'port-3-3'
    copy_records(session, copy)
    result = bind(bundle, copy, review, out / 'copies-unchanged' / 'c6-tw-prepare')
    assert result.returncode == 0, result.stdout + result.stderr
    cases['unchanged-copies-bind'] = True

    (out / 'result.json').write_text(json.dumps({
        'pass': True, 'physical_access': False, 'cases': cases, 'bundle': str(bundle), 'session': str(session),
        'binding_sha256': hashlib.sha256(frozen).hexdigest(), 'binding_source': source,
        'tree_script_unchanged_since': tree_now}, indent=1) + '\n')
    print(f'PASS step 6 workload binding: frozen script equals {source["path"]} at {source["commit"]}; real records '
          f'of {session.parent.name} bind from a read-only template; '
          f'{sum(k.startswith("refuses-") for k in cases)} refusals'
          + ('' if tree_now else '; the tree script has changed since that commit'))


if __name__ == '__main__':
    sys.exit(main())
