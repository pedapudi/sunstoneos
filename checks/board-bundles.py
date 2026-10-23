#!/usr/bin/env python3
"""Host check of tools/prepare-board-bundles.py on fixture rows in a fixture repository.

The check builds small AArch64 ELF files and a Git repository of fixture
preparation modules and a fixture runner, then runs the tool once on fixture
rows. It checks that:

- preparation and its child processes see only the fourteen sandbox /dev
  entries, empty /sys, /tmp and /run, the loopback interface, the four set
  environment variables, and a read-only file system outside the run
  directory;
- a nested bundle with board/ and assets/ directories is frozen with no
  writable entry, every file is hashed once (bytes read equal the bundle
  size), and the snapshot holds the sources and artifacts.json only;
- each submission passes submission() from tools/board-test-queue.py, with
  the runner's PREREQUISITES and the Monitor prerequisite and 1,200 s, or a
  Terminal timeout at least the attempt bound computed here with the
  supervised transport's own transfer_timeout() and REGIONS from the
  bundle's file sizes;
- a Terminal row is refused when its runner does not declare
  COMPLETION_COUNTS, and when its attempt bound exceeds 3,600 s;
- a bundle that passes its module's validate_bundle() but whose admitted
  value is not the row's build is refused, for a Monitor and a Terminal row,
  and a row without admission values is refused;
- the nm scan accepts a Monitor image linked with --discard-locals and refuses
  a stripped image, an image without monitor_entry, each USB and
  secondary-core prefix, and a memory image without monitor_renewal_cap;
- a refusing prepare(), an uncommitted source and a missing module refuse
  only their own rows, and unknown or excluded input rows stop before any run
  directory exists;
- the fixed row table names existing runners and transports, every
  Terminal runner's attempt bound without uploads lies within 3,600 s, and
  the table records which runners declare COMPLETION_COUNTS;
- the sandbox starts within 100 ms.

Every fixture lives in a new directory under
~/.cache/sunstoneos/build/board-bundles/, outside the /tmp that the sandbox
hides; it is removed after a pass. No fixture names a USB device or board lock,
so a broken sandbox fails the check instead of reaching a device.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
WORK = Path.home() / '.cache/sunstoneos/build/board-bundles'
DEV = {'core', 'fd', 'full', 'null', 'ptmx', 'pts', 'random', 'shm', 'stderr', 'stdin', 'stdout', 'tty',
       'urandom', 'zero'}


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


tool = module('board_bundles_tool', ROOT / 'tools/prepare-board-bundles.py')
queue = module('board_bundles_queue', ROOT / 'tools/board-test-queue.py')
transport = module('board_bundles_transport', ROOT / tool.SUPERVISED_TRANSPORT)
SCHEMAS = {'monitor.py': 'fixture-monitor-v1', 'nested.py': 'fixture-nested-v1', 'many.py': 'fixture-many-v1'}


def attempt_bound(sizes, capture_seconds):
    """run_session's worst case with the transport's own bounds: guard wait, version, backups, per-upload
    readl and write+readback, final readl, exe, capture window, refusal report and SRAM restore."""
    request, timeout = transport.FEL_REQUEST_SECONDS, transport.transfer_timeout
    lengths = [length for _, _, length in transport.REGIONS]
    return (615 + request + sum(timeout(n) for n in lengths) + sum(request + timeout(2 * n) for n in sizes)
            + 2 * request + capture_seconds + timeout(128) + request + sum(timeout(2 * n) for n in lengths))

VIEW = r'''
import json, os


def view():
    net = open('/proc/self/net/dev').read().splitlines()[2:]
    return {'dev': sorted(os.listdir('/dev')), 'sys': os.listdir('/sys'), 'tmp': os.listdir('/tmp'),
            'run': os.listdir('/run'), 'env': sorted(os.environ), 'net': sorted(l.split(':')[0].strip() for l in net)}
'''

NESTED = '''
import json, os, subprocess, sys
from pathlib import Path
VIEW = %r


def prepare(source, output):
    names = {}
    exec(VIEW, names)
    own = names['view']()
    child = subprocess.run([sys.executable, '-c', VIEW + 'print(json.dumps(view()))'], capture_output=True,
                           text=True, check=True).stdout
    try:
        (Path(source).parent / 'outside.txt').write_text('x')
        outside = False
    except OSError:
        outside = True
    (output / 'board').mkdir(parents=True)
    (output / 'assets').mkdir()
    (output / 'board' / 'image.elf').write_bytes(Path(source).read_bytes())
    (output / 'board' / 'session.bin').write_bytes(bytes(range(256)) * 4000)
    (output / 'assets' / 'input.wav').write_bytes(b'RIFF' + bytes(3 << 20))
    (output / 'board' / 'view.json').write_text(json.dumps({'child': json.loads(child), 'own': own,
                                                            'outside_write_refused': outside}))
    (output / 'prepared.json').write_text(json.dumps({'schema': 'fixture-nested-v1'}))
    return validate_bundle(output)


def validate_bundle(output):
    value = json.loads((output / 'prepared.json').read_text())
    assert (output / 'board/image.elf').is_file() and (output / 'assets/input.wav').is_file()
    return value
''' % VIEW

MONITOR = '''
import shutil


def prepare(image, output):
    output.mkdir()
    shutil.copyfile(image, output / 'image.elf')
    return validate_bundle(output)


def validate_bundle(output):
    return {'schema': 'fixture-monitor-v1', '_frozen': {'image.elf': (output / 'image.elf').read_bytes()}}
'''

MANY = '''
import json


def prepare(source, output):
    output.mkdir()
    for index in range(100):
        (output / f'part-{index:03}.bin').write_bytes(bytes(64))
    (output / 'prepared.json').write_text(json.dumps({'schema': 'fixture-many-v1'}))
    return json.loads((output / 'prepared.json').read_text())
'''

FAILING = '''
def prepare(source, output):
    raise ValueError('Fixture admission refused this bundle.')
'''

RUNNER = '''
PREREQUISITES = {'supervised-ram-recovery', 'fixture-supervised-software'}
COMPLETION_COUNTS = ('interrupt_completions', 'fallback_completions', 'client_wakeups')


def capture(launch):
    return launch(capture_seconds=19)
'''


def build_elf(work, name, body, *flags):
    source = work / (name + '.c')
    source.write_text(body)
    obj = work / (name + '.o')
    elf = work / (name + '.elf')
    subprocess.run([str(harness.CLANG), '--target=aarch64-none-elf', '-O0', '-ffreestanding', '-c', str(source),
                    '-o', str(obj)], check=True)
    subprocess.run([str(harness.LLD), str(obj), '-o', str(elf), '-e', 'monitor_entry' if 'monitor_entry' in body
                    else 'other_entry', *flags], check=True)
    return elf


def git(repo, *arguments):
    return subprocess.run(['git', '-C', str(repo), '-c', 'user.name=Fixture', '-c', 'user.email=fixture@invalid',
                           *arguments], check=True, capture_output=True, text=True).stdout


def fixture_repository(work):
    repo = work / 'repo'
    (repo / 'fixture').mkdir(parents=True)
    uncounted = RUNNER.replace('COMPLETION_COUNTS', 'OTHER_COUNTS')
    for name, text in [('nested.py', NESTED), ('monitor.py', MONITOR), ('failing.py', FAILING), ('many.py', MANY),
                       ('runner.py', RUNNER), ('uncounted.py', uncounted), ('dirty.c', 'int dirty;\n'),
                       ('service.c', 'int service;\n'),
                       ('transport.py', (ROOT / tool.SUPERVISED_TRANSPORT).read_text())]:
        (repo / 'fixture' / name).write_text(text)
    git(repo, 'init', '-q')
    git(repo, 'add', '.')
    git(repo, 'commit', '-q', '-m', 'Fixture sources')
    (repo / 'fixture/dirty.c').write_text('int dirty = 1;\n')
    return repo


def row(name, cls, module_name, sources=(), **extra):
    value = {'name': name, 'class': cls, 'component': 'Fixture ' + name, 'module': 'fixture/' + module_name,
             'runner': 'fixture/runner.py', 'transport': 'fixture/transport.py',
             'admission': {'schema': SCHEMAS.get(module_name, 'fixture-other-v1')},
             'sources': ['fixture/' + module_name, 'fixture/runner.py', 'fixture/transport.py', *sources],
             'expected_result': 'Fixture result.'}
    value.update(extra)
    return value


def unwritable(path):
    return all(not (os.lstat(p).st_mode & 0o222) for p in [path, *path.rglob('*')])


def writable_tree(path):
    for directory, dirs, files in os.walk(path):
        for name in dirs + files:
            p = Path(directory) / name
            if not p.is_symlink():
                os.chmod(p, os.lstat(p).st_mode | 0o700)


def check_table():
    """The fixed rows: runners and transports exist, and Terminal attempt bounds without uploads fit 3,600 s."""
    names = [r['name'] for r in tool.ROWS]
    assert len(names) == len(set(names))
    classes = [r['class'] for r in tool.ROWS]
    assert classes == sorted(classes, key=['monitor', 'terminal', 'excluded'].index), 'rows follow the plan order'
    results = {}
    for r in tool.ROWS:
        if r['class'] == 'excluded':
            assert r['reason']
            continue
        assert (ROOT / r['runner']).is_file(), r['runner']
        admission = r['admission']
        assert admission is None or (isinstance(admission, dict) and admission), r['name']
        entry = {'admission_named': admission is not None, 'module_present': (ROOT / r['module']).is_file()}
        if r['class'] == 'terminal':
            prerequisites, counts, seconds = tool.runner_terms(ROOT / r['runner'])
            assert queue.SUPERVISED_RECOVERY in prerequisites
            missing = [s for s in r['sources'] if not (ROOT / s).is_file()]
            assert not missing, missing
            assert r['transport'] in r['sources']
            fixed, _ = tool.terminal_timeout(seconds, [], tool.transport_bounds(ROOT / r['transport']))
            assert fixed >= attempt_bound([], seconds) and fixed <= 3600, fixed
            entry.update(prerequisites=prerequisites, counts_declared=counts == sorted(tool.COUNT_FIELDS),
                         timeout_without_uploads=fixed)
        else:
            prerequisites, _, _ = tool.terms(r, ROOT)
            # 615 s of churn-guard wait plus 435 s of transaction and return bounds.
            assert prerequisites == [queue.RECOVERY] and tool.MONITOR_TIMEOUT >= 615 + 435
            entry.update(prerequisites=prerequisites, timeout_seconds=tool.MONITOR_TIMEOUT)
        results[r['name']] = entry
    return results


def sandbox_start(work):
    times = []
    for _ in range(15):
        started = time.perf_counter()
        subprocess.run(tool.sandbox(work) + ['/bin/true'], check=True)
        times.append((time.perf_counter() - started) * 1000)
    return {'median_ms': round(statistics.median(times), 2), 'max_ms': round(max(times), 2)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    WORK.mkdir(parents=True, exist_ok=True)
    work = WORK / ('check-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%f'))
    work.mkdir()
    result = {'table': check_table()}

    elves = work / 'elf'
    elves.mkdir()
    good = 'int monitor_entry(void){return 0;}\n'
    cap = 'int monitor_renewal_cap(void){return 1;}\n'
    inputs = {'memory-good': {'image': str(build_elf(elves, 'memory-good', good + cap, '--discard-locals'))},
              'monitor-no-cap': {'image': str(build_elf(elves, 'monitor-no-cap', good))},
              'memory-no-cap': {'image': str(build_elf(elves, 'memory-no-cap', good))},
              'memory-stripped': {'image': str(build_elf(elves, 'memory-stripped', good + cap, '--strip-all'))},
              'memory-no-entry': {'image': str(build_elf(elves, 'memory-no-entry',
                                                         'int other_entry(void){return 0;}\n' + cap))}}
    rows = [row('memory-good', 'monitor', 'monitor.py', renewal_cap=True, procedure=['Run {runner} on {bundle}.'],
                stop_condition='Stop at the first failure.'),
            row('monitor-no-cap', 'monitor', 'monitor.py', procedure=['Run {runner}.'], stop_condition='Stop.'),
            row('memory-no-cap', 'monitor', 'monitor.py', renewal_cap=True),
            row('memory-stripped', 'monitor', 'monitor.py', renewal_cap=True),
            row('memory-no-entry', 'monitor', 'monitor.py', renewal_cap=True),
            row('memory-wrong-build', 'monitor', 'monitor.py', renewal_cap=True,
                admission={'schema': 'fixture-memory-v1'}),
            row('memory-unnamed', 'monitor', 'monitor.py', renewal_cap=True, admission=None)]
    for name in ('memory-wrong-build', 'memory-unnamed'):
        inputs[name] = inputs['memory-good']
    for prefix in tool.FORBIDDEN_PREFIXES:
        name = 'prefix-' + prefix.rstrip('_').replace('_', '-')
        body = good + cap + f'static int {prefix}probe(void){{return 2;}}\nint keep(void){{return {prefix}probe();}}\n'
        inputs[name] = {'image': str(build_elf(elves, name, body))}
        rows.append(row(name, 'monitor', 'monitor.py', renewal_cap=True))
    source = str(inputs['memory-good']['image'])
    rows += [row('nested', 'terminal', 'nested.py', ['fixture/service.c']),
             row('failing', 'terminal', 'failing.py'),
             row('dirty', 'terminal', 'nested.py', ['fixture/dirty.c']),
             row('absent', 'terminal', 'absent.py'),
             row('service-wrong-build', 'terminal', 'nested.py', admission={'schema': 'fixture-npu-service-v1'}),
             row('service-unnamed', 'terminal', 'nested.py', admission=None),
             row('uncounted', 'terminal', 'nested.py', runner='fixture/uncounted.py'),
             row('too-long', 'terminal', 'many.py'),
             row('no-inputs', 'terminal', 'nested.py'),
             {'name': 'excluded', 'class': 'excluded', 'reason': 'Fixture needs physical recovery.'}]
    inputs.update({name: {'source': source} for name in ('nested', 'failing', 'dirty', 'absent', 'service-wrong-build',
                                                         'service-unnamed', 'uncounted', 'too-long')})
    repo = fixture_repository(work)
    evidence = work / 'evidence'

    harness.refused(lambda: tool.run({'unknown': {}}, rows, evidence, repo), 'unknown rows')
    harness.refused(lambda: tool.run({'excluded': {}}, rows, evidence, repo), 'excluded rows')
    assert not evidence.exists(), 'refused inputs created no run directory'

    status, run_dir = tool.run(inputs, rows, evidence, repo)
    assert status == 1 and run_dir == evidence / f'board-bundles-{datetime.now(timezone.utc):%Y%m%d}-1'
    record = json.loads((run_dir / 'run.json').read_text())
    states = {name: r['status'] for name, r in record['rows'].items()}
    expected = {'memory-good': 'prepared', 'monitor-no-cap': 'prepared', 'nested': 'prepared',
                'no-inputs': 'no-inputs', 'excluded': 'excluded'}
    assert states == {r['name']: expected.get(r['name'], 'refused') for r in rows}, states
    reasons = {'memory-no-cap': 'monitor_renewal_cap', 'memory-stripped': 'no symbol table',
               'memory-no-entry': 'does not define monitor_entry', 'failing': 'Fixture admission refused',
               'dirty': 'Uncommitted row sources: fixture/dirty.c', 'absent': 'does not exist',
               'memory-wrong-build': "schema is 'fixture-monitor-v1', not 'fixture-memory-v1'",
               'service-wrong-build': "schema is 'fixture-nested-v1', not 'fixture-npu-service-v1'",
               'memory-unnamed': 'refused until it does', 'service-unnamed': 'refused until it does',
               'uncounted': 'does not declare COMPLETION_COUNTS', 'too-long': "more than the queue's 3600 s"}
    stages = {'memory-wrong-build': 'admission', 'service-wrong-build': 'admission', 'memory-unnamed': 'admission',
              'service-unnamed': 'admission', 'uncounted': 'inputs', 'too-long': 'timeout'}
    reasons.update({'prefix-' + p.rstrip('_').replace('_', '-'): p + 'probe' for p in tool.FORBIDDEN_PREFIXES})
    for name, reason in reasons.items():
        assert reason in record['rows'][name]['message'], (name, record['rows'][name])
    for name, stage in stages.items():
        assert record['rows'][name]['stage'] == stage, (name, record['rows'][name])
    assert not (run_dir / 'absent').exists() and not (run_dir / 'dirty/submission.json').exists()

    view = json.loads((run_dir / 'nested/bundle/board/view.json').read_text())
    assert view['outside_write_refused'] is True
    for side in ('own', 'child'):
        seen = view[side]
        assert set(seen['dev']) == DEV, seen['dev']
        assert seen['sys'] == seen['tmp'] == seen['run'] == [] and seen['net'] == ['lo'], seen
        assert seen['env'] == sorted(tool.ENVIRONMENT), seen['env']

    prepared = {}
    for name in ('memory-good', 'monitor-no-cap', 'nested'):
        r = record['rows'][name]
        bundle = run_dir / name / 'bundle'
        files = sorted(p for p in bundle.rglob('*') if p.is_file())
        artifacts = json.loads((run_dir / name / 'artifacts.json').read_text())
        assert [a['path'] for a in artifacts] == [str(p) for p in files]
        assert all(hashlib.sha256(Path(a['path']).read_bytes()).hexdigest() == a['sha256'] for a in artifacts)
        size = sum(p.stat().st_size for p in files)
        metrics = r['metrics']
        assert metrics == {'bundle_bytes': size, 'read_bytes': size, 'writable_entries': 0}, metrics
        assert unwritable(bundle)
        manifest = json.loads((run_dir / name / 'snapshot/manifest.json').read_text())
        row_sources = next(x for x in rows if x['name'] == name)['sources']
        assert sorted(manifest['sources']) == sorted(row_sources)
        assert [Path(a['original']) for a in manifest['artifacts']] == [run_dir / name / 'artifacts.json']
        copies = [p for p in (run_dir / name / 'snapshot').rglob('*') if p.is_file() and p.name != 'manifest.json']
        assert len(copies) == len(row_sources) + 1, 'the snapshot copies no bundle file'
        submission = json.loads((run_dir / name / 'submission.json').read_text())
        queue.submission(submission)
        assert submission['artifacts'] == artifacts and submission['kind'] == 'peripheral'
        assert submission['sources'] == [{'repository': str(repo), 'revision': git(repo, 'rev-parse', 'HEAD').strip()}]
        if name == 'nested':
            assert submission['prerequisites'] == ['fixture-supervised-software', 'supervised-ram-recovery']
            bound = attempt_bound([p.stat().st_size for p in files], 19)
            assert bound <= submission['timeout_seconds'] == sum(r['timeout_terms'].values()) <= 3600, r
            assert r['timeout_terms']['margin'] == tool.TERMINAL_MARGIN
            assert str(bundle) in submission['procedure'][1]
            prepared['nested_timeout'] = {'bound': bound, **r['timeout_terms']}
        else:
            assert submission['prerequisites'] == [tool.MONITOR_PREREQUISITE]
            assert submission['timeout_seconds'] == tool.MONITOR_TIMEOUT
        prepared[name] = {'files': len(files), **metrics}
    assert {p.parent.name for p in (run_dir / 'nested/bundle').rglob('*') if p.is_file()} == {'bundle', 'board',
                                                                                             'assets'}
    status, second = tool.run({}, rows, evidence, repo)
    assert status == 0 and second.name.endswith('-2'), 'a second run gets its own directory'

    result.update(rows=states, prepared=prepared, sandbox_start=sandbox_start(work))
    assert result['sandbox_start']['median_ms'] <= 100, result['sandbox_start']
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    writable_tree(work)
    shutil.rmtree(work)
    print(json.dumps({'prepared': prepared, 'sandbox_start': result['sandbox_start']}))


if __name__ == '__main__':
    main()
