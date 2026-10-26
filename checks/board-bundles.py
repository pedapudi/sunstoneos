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
- a Read row is prepared with the fixed reader and its word list and the
  reader's 120 s timeout, and refused for a word list with a key outside the
  schema, a condition that is not an earlier word, and an ungated word outside
  its block;
- a returning row is prepared with its controlled outcomes and the worst case
  of the bundle's own copy of the runner, 925 s, or 955 s for a copy with one
  more FEL request; its runbook names each outcome of controlled.json, with a
  progress record where the outcome has one; the row is refused for an image
  that defines monitor_sync, a new object outside its overlay, a binary that
  is not the ELF's loadable segments, and controlled outcomes that the
  bundle's runner refuses;
- a returning row that links objects of a base row of the same run is
  prepared when each such object equals the base bundle's object of the same
  unit, and refused when one differs or when the base row was not prepared
  earlier in the run; a row whose `after` rows were refused is still
  prepared, because `after` orders runbooks only;
- with a lifetime, each row's runbook gives its record directories, the full
  --warm-return list (the lifetime's chain, then every earlier step) and the
  rows it runs after, and the run directory receives runbook.md with the
  order, each entry's gates and its stop outcomes; a lifetime that reuses a
  record stem, names a row without inputs, gives a row entry its own gates,
  or places a gate row after its dependent stops before any run directory
  exists; a stop outcome that the named row's bundle does not list as
  controlled refuses the lifetime and writes no set runbook; an external
  runbook is accepted only when its record directories lie under its
  entry's stem, apart from the DDR record, the chain and earlier runs, and
  it names its run directory;
- each prepared single-core row has a runbook naming every bundle file's
  SHA-256 and its run command, and each returning row's runbook runs the
  survey of tools/prepare-board-bundles.py's SURVEY_BUILD with the
  repository's survey runner;
- that build passes the survey runner's own input checks (manifest schema
  and physical_access, the current tools/run-launch-survey.py as
  host_sha256, the survey source, every frozen file's SHA-256, and a
  software record with its schema, return_preserved and physical_access),
  a copy that changes any one of them is refused, and the launch-survey-20261017 build,
  which names the runner from before 575565f8, is refused by itself and,
  as a run's survey build, before any run directory exists;
- the hand-written runbooks under docs/board-runbooks/ call no
  $V/common/survey.py, and those that name a survey build name that one;
- the reader, with only its FEL commands modeled, saves the 128 scratch bytes,
  records the start and end time of each command,
  reads each condition word in its own command, skips the dependents of a
  condition that does not match, restores and rereads the scratch bytes,
  issues no FEL command other than read, readl and write, and quarantines
  when the restored bytes differ;
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
import re
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
# A frozen survey build whose manifest names the runner from before 575565f8.
STALE_SURVEY = tool.EVIDENCE / 'launch-survey-20261017/build'
reader = module('board_bundles_reader_check', ROOT / tool.READER)
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


CONTROLLED_RUNNER = """
import json


def controlled_outcomes(raw):
    value = json.loads(raw)
    if value.get('schema') != 'kernel-controlled-outcomes-v1' or not value.get('outcomes'):
        raise ValueError('Fixture runner refuses these outcomes.')
    return value['outcomes']
"""
OUTCOME = {'name': 'fixture-fault', 'status': 1, 'interrupt': None, 'fail_lines': 1,
           'fault': {'esr_mask': 0xfe00007f, 'esr_value': 0x96000005, 'far': 0x400000000, 'elr_low': 0x41001000,
                     'elr_high': 0x41001040, 'task': 'fault'}, 'log_contains': ['CHECK fixture']}
PROGRESS_OUTCOME = {'name': 'fixture-stopped', 'status': 2, 'interrupt': 29, 'fail_lines': 0, 'fault': None,
                    'log_contains': ['CHECK fixture'],
                    'progress': {'offset': 128, 'magic': 0x1234, 'stage': 3, 'count_below': 1000, 'age_above': 24000}}
# A request more than the repository runner makes: the bundle's own copy
# bounds its timeout.
EXTRA_REQUEST = '\n\ndef fixture_extra_request():\n    transaction("version")\n'


def fixture_build(work, name, elf, objects=None, controlled=True, bad_bin=False, outcomes=(OUTCOME,), runner_extra=''):
    """A build.json as checks/board-bundles-apps.py writes it, for one fixture image. Its runner is the
    repository's kernel runner with a fixture controlled_outcomes() and runner_extra appended."""
    directory = work / 'builds' / name
    directory.mkdir(parents=True)
    binary = directory / 'probe.bin'
    subprocess.run([tool.OBJCOPY, '-O', 'binary', str(elf), str(binary)], check=True)
    if bad_bin:
        binary.write_bytes(binary.read_bytes() + b'x')
    runner = directory / 'runner.py'
    runner.write_text((ROOT / 'tools/check-a733-kernel.py').read_text() + CONTROLLED_RUNNER + runner_extra)
    record = {'elf': str(elf), 'bin': str(binary), 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
              'objects': objects or {'checks/kernel.c.obj': {'sha256': '1' * 64, 'origin': 'new'},
                                     'platform/a733/platform.c.obj': {'sha256': '2' * 64, 'origin': 'rank-9-usb-v4'},
                                     'platform/a733/monitor.c.obj': {'sha256': '3' * 64,
                                                                     'origin': 'rank-4-kernel-check'}},
              'overlay': ['checks/kernel.c.obj'],
              'controlled': {'schema': 'kernel-controlled-outcomes-v1' if controlled != 'bad' else 'other',
                             'outcomes': list(outcomes)} if controlled else None,
              'expect': 'CHECK fixture',
              'host_tools': {'check-a733-kernel.py': str(runner), 'check-a733-smp.py': str(ROOT / tool.HOST_TOOLS[0]),
                             'usb-access.py': str(ROOT / tool.HOST_TOOLS[1])}}
    (directory / 'build.json').write_text(json.dumps(record))
    return str(directory / 'build.json')


class FakeFel:
    """sunxi-fel's read, readl and write commands over a word map and the 128 scratch bytes."""

    def __init__(self, words, restore=True):
        self.words, self.restore = dict(words), restore
        self.scratch = bytes(range(128))
        self.commands, self.ops = [], []

    def run(self, command, **kwargs):
        args = command[3:]
        self.commands.append(args)
        out = []
        while args:
            op = args[0]
            self.ops.append(op)
            if op == 'readl':
                address = int(args[1], 16)
                assert address in self.words, hex(address)
                # The stub and the word's slot: the first 52 bytes.
                self.scratch = bytes(52) + self.scratch[52:]
                out.append(f'0x{self.words[address]:08x}')
                args = args[2:]
            elif op == 'read':
                assert int(args[1], 16) == reader.SCRATCH and int(args[2]) == 128
                Path(args[3]).write_bytes(self.scratch)
                args = args[4:]
            elif op == 'write':
                assert int(args[1], 16) == reader.SCRATCH
                if self.restore:
                    self.scratch = Path(args[2]).read_bytes()
                args = args[3:]
            else:
                raise AssertionError(op)
        return subprocess.CompletedProcess(command, 0, '\n'.join(out) + '\n', '')


def run_reader(work, name, words, values, restore=True):
    """reader.main() with its device modules and FEL process modeled; returns the session, commands and quarantine."""
    from contextlib import contextmanager, redirect_stdout
    import io
    from types import SimpleNamespace
    from unittest.mock import patch
    fel = FakeFel(values, restore)
    quarantined, held = [], []

    @contextmanager
    def board_lock():
        yield Path('/dev/bus/usb/003/011')

    @contextmanager
    def sealed_source(data):
        path = work / f'{name}-sealed.bin'
        path.write_bytes(data)
        yield str(path), 0

    def stop_after_failure(record, save, quarantine, error):
        quarantine()

    def require(condition, message):
        if not condition:
            raise RuntimeError(message)

    base = SimpleNamespace(require=require, digest=lambda path: 'pinned', FEL_HASH='pinned',
                           save_json=lambda path, value: Path(path).write_text(json.dumps(value)),
                           sync_file=lambda path: None, sealed_source=sealed_source,
                           stop_after_failure=stop_after_failure, hold_ownership=lambda: held.append(True))
    access = SimpleNamespace(board_lock=board_lock, require_session=lambda node: None,
                             require_rom=lambda node: None, identity=lambda node: [7, 1, 2, 0, 0],
                             target=lambda node: Path('/sys/bus/usb/devices/3-3'),
                             quarantine_instance=lambda *a: quarantined.append(a))
    words_path = work / f'{name}-WORDS.json'
    words_path.write_text(json.dumps({'schema': 'board-read-words-v1', 'purpose': 'fixture', 'words': words}))
    evidence = work / f'{name}-evidence'
    modules = {'board_read_base': base, 'board_read_access': access}
    error = None
    with patch.object(reader, 'module', lambda n, path: modules[n]), patch.object(reader.subprocess, 'run', fel.run), \
            patch.object(sys, 'argv', ['board-read.py', str(words_path), str(evidence)]), \
            redirect_stdout(io.StringIO()):
        try:
            reader.main()
        except RuntimeError as e:
            error = str(e)
    assert set(fel.ops) <= {'read', 'readl', 'write'}, fel.ops
    return json.loads((evidence / 'session.json').read_text()), fel.commands, quarantined, error


def check_reader(work):
    """The fixed reader's command sequence, with the device modeled."""
    w = tool.word
    gate = (0x0701020c, 0x10001, 0x10001)
    words = [w(0x02002580, 'ccu'), w(0x02002588, 'ccu'), w(gate[0], 'r_ccu'), w(0x07090160, condition=gate),
             w(0x07090164, condition=gate), w(0x08871000, 'cpu_pll')]
    values = {0x02002580: 0xc3000000, 0x02002588: 0x81000000, gate[0]: 0x10001, 0x07090160: 0x983fb0f7,
              0x07090164: 0x1, 0x08871000: 0xf8802600}
    session, commands, quarantined, error = run_reader(work, 'open', words, values)
    assert error is None and session['complete'] and session['scratch_restored'] and not quarantined, session
    assert session['words'] == {hex(a): v for a, v in values.items()} and session['skipped'] == []
    # Each FEL command records its wall-clock start and end.
    assert all(isinstance(t.get('start'), float) and t['start'] <= t.get('end', -1.0)
               for t in session['transactions']), session['transactions']
    verbs = [c[0] for c in commands]
    assert verbs[0] == 'read' and commands[-1][0] == 'write' and commands[-1][3] == 'read', commands
    readls = [c for c in commands if c[0] == 'readl']
    assert ['readl', hex(gate[0])] in readls, 'the condition word is read in its own command'
    assert ['readl', '0x2002580', 'readl', '0x2002588'] in readls
    assert ['readl', '0x7090160', 'readl', '0x7090164'] in readls
    closed = {**values, gate[0]: 0x1}
    session, commands, quarantined, error = run_reader(work, 'closed', words, closed)
    assert error is None and session['complete'] and not quarantined
    assert [x['address'] for x in session['skipped']] == ['0x7090160', '0x7090164'], session['skipped']
    assert not any('0x7090160' in c for c in commands), 'no dependent of a closed gate is read'
    session, commands, quarantined, error = run_reader(work, 'unrestored', words, values, restore=False)
    assert error == 'Scratch restoration failed' and len(quarantined) == 1 and not session['complete'], error
    refusals = {}
    for name, bad, reason in [
            ('extra-key', [dict(words[0], width=8)], 'keys outside'),
            ('later-condition', [words[3], words[2]], 'not an earlier word'),
            ('outside-block', [w(0x07010000, 'ccu')], 'not inside the ungated block'),
            ('both-gates', [dict(words[3], block='ccu')], 'exactly one of'),
            ('unaligned', [w(0x02002582, 'ccu')], 'aligned'),
            ('value-outside-mask', [words[2], w(0x07090160, condition=(gate[0], 1, 3))], 'outside it')]:
        try:
            reader.plan({'schema': 'board-read-words-v1', 'purpose': 'x', 'words': bad})
        except reader.Refused as e:
            assert reason in str(e), (name, str(e))
            refusals[name] = True
        else:
            raise AssertionError(name)
    return {'commands_open': len(commands), 'refusals': refusals}


def fixture_repository(work):
    repo = work / 'repo'
    (repo / 'fixture').mkdir(parents=True)
    (repo / 'tools').mkdir()
    # The single-core rows load the tool itself, its reader and host modules,
    # and the kernel runner whose FEL requests bound the returning timeout.
    # The survey runner and source let the returning runbooks name the survey build.
    (repo / 'boot/a733').mkdir(parents=True)
    for rel in ('tools/prepare-board-bundles.py', tool.READER, *tool.HOST_TOOLS, 'tools/check-a733-kernel.py',
                tool.SURVEY_RUNNER, tool.SURVEY_SOURCE):
        shutil.copyfile(ROOT / rel, repo / rel)
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


def check_survey(work):
    """The runbooks' survey build names the current survey runner and source; the launch-survey-20261017
    build, which names the runner from before 575565f8, is refused."""
    tool.check_survey(tool.SURVEY_BUILD, ROOT)
    harness.refused(lambda: tool.check_survey(STALE_SURVEY, ROOT), 'the runner would refuse it')
    # Copies of the build and the software record with one input changed each.
    def variant(name, change_manifest=None, change_binary=False, change_software=None):
        build = work / ('survey-' + name)
        shutil.copytree(tool.SURVEY_BUILD, build)
        manifest = json.loads((build / 'manifest.json').read_text())
        if change_manifest:
            change_manifest(manifest)
        (build / 'manifest.json').write_text(json.dumps(manifest))
        if change_binary:
            data = bytearray((build / 'launch-survey.bin').read_bytes())
            data[0] ^= 1
            (build / 'launch-survey.bin').write_bytes(bytes(data))
        software = json.loads(tool.SURVEY_SOFTWARE.read_text())
        if change_software:
            change_software(software)
        (work / ('software-' + name + '.json')).write_text(json.dumps(software))
        return build, work / ('software-' + name + '.json')
    for name, kwargs, message in [
            ('schema', {'change_manifest': lambda m: m.update(schema='a733-launch-survey-v0')}, 'launch-survey-v1'),
            ('physical', {'change_manifest': lambda m: m.update(physical_access=True)}, 'launch-survey-v1'),
            ('binary', {'change_binary': True}, 'differs from its manifest'),
            ('software-schema', {'change_software': lambda r: r.update(schema='other')}, 'return evidence'),
            ('return', {'change_software': lambda r: r.update(return_preserved=False)}, 'return evidence'),
            ('software-physical', {'change_software': lambda r: r.update(physical_access=True)}, 'return evidence')]:
        build, software = variant(name, **kwargs)
        harness.refused(lambda: tool.check_survey(build, ROOT, software), message)
    build, software = variant('unchanged')
    tool.check_survey(build, ROOT, software)
    # The hand-written runbooks name the same survey build and never the bundle's survey.py.
    books = sorted((ROOT / 'docs/board-runbooks').glob('*.md'))
    # $R names a rank's bundle in rank-9-usb.md, so the survey runner's repository is $SR.
    stale = [b.name for b in books if '$V/common/survey.py' in b.read_text()
             or '$R/tools/run-launch-survey.py' in b.read_text()]
    assert not stale, f'runbooks still call $V/common/survey.py or name the runner through $R: {stale}'
    named = {b.name for b in books if 'SB=~/.cache/sunstoneos/evidence/' in b.read_text()}
    for b in named:
        build = re.search(r'^SB=~/\.cache/sunstoneos/evidence/(\S+)$', (ROOT / 'docs/board-runbooks' / b).read_text(), re.M)
        assert build and tool.EVIDENCE / build[1] == tool.SURVEY_BUILD, f'{b} names another survey build'
    assert 'README.md' in named, 'the runbook README does not name the survey build'
    manifest = json.loads((tool.SURVEY_BUILD / 'manifest.json').read_text())
    return {'build': str(tool.SURVEY_BUILD), 'runner_sha256': manifest['host_sha256']}


def check_table():
    """The fixed rows: runners and transports exist, and Terminal attempt bounds without uploads fit 3,600 s."""
    names = [r['name'] for r in tool.ROWS]
    assert len(names) == len(set(names))
    positioned = [r for r in tool.ROWS if 'position' in r]
    # Positions rise strictly, and together with the numbers that unmerged
    # branches hold (tool.RESERVED_POSITIONS) they count 1, 2, 3, ... with no
    # gap, so a dropped row is still caught.
    numbers = [r['position'] for r in positioned]
    reserved = set(tool.RESERVED_POSITIONS)
    assert all(a < b for a, b in zip(numbers, numbers[1:])) and not reserved & set(numbers), 'single-core plan order'
    assert sorted(set(numbers) | reserved) == list(range(1, max(numbers) + 1)), 'a bundle number is missing'
    assert tool.ROWS[:len(positioned)] == positioned, 'the single-core rows come first'
    classes = [r['class'] for r in tool.ROWS[len(positioned):]]
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
        elif r['class'] == 'read':
            prerequisites, _, _ = tool.terms(r, ROOT)
            assert prerequisites == [queue.SUPERVISED_RECOVERY] and r['runner'] == tool.READER
            planned = reader.plan({'schema': 'board-read-words-v1', 'purpose': r['purpose'], 'words': r['words']})
            assert r['admission'] == {'schema': 'board-read-bundle-v1', 'row': r['name']}
            entry.update(prerequisites=prerequisites, timeout_seconds=tool.READ_TIMEOUT, words=len(planned),
                         conditions=sum(1 for x in planned if x[2] is not None))
        elif r.get('returning'):
            prerequisites, _, _ = tool.terms(r, ROOT)
            timeout, parts = tool.returning_timeout(ROOT / r['runner'])
            # The guard's 615 s, eight 30 s FEL requests (version, the ROM entry
            # words, the result-page read, the upload and readback, the result
            # blank, reset64, version, the result read), the 10 s return
            # window and 60 s of margin.
            assert prerequisites == [queue.SUPERVISED_RECOVERY] and timeout == 615 + 8 * 30 + 10 + 60 == 925, parts
            schema = 'board-gpu-returning-bundle-v1' if r.get('variant') else 'board-returning-bundle-v1'
            assert r['admission'] is None or r['admission'] == {'schema': schema, 'row': r['name']}
            assert r['admission'] is not None or r['needs'], r['name']
            missing = [s for s in r['sources'] if not (ROOT / s).is_file()]
            assert not missing, (r['name'], missing)
            entry.update(prerequisites=prerequisites, timeout_seconds=timeout)
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
    result = {'table': check_table(), 'survey': check_survey(work), 'reader': check_reader(work)}

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
    # Read and returning rows, as the single-core table declares them.
    read_words = [tool.word(0x02002580, 'ccu'), tool.word(0x0701020c, 'r_ccu'),
                  tool.word(0x07090160, condition=(0x0701020c, 0x10001, 0x10001))]
    single = ['tools/prepare-board-bundles.py', tool.READER, *tool.HOST_TOOLS]

    def read_fixture(name, words, position):
        value = tool.read_row(name, position, 'Fixture ' + name, 'fixture words', words, 'Fixture result.')
        value['sources'] = single
        return value

    def returning_fixture(name, position, controlled=True, base=None, after=()):
        value = tool.returning_row(name, position, 'Fixture ' + name, ['checks/kernel.c.obj'], 'CHECK fixture',
                                   controlled, 'Fixture result.', 'Fixture image.', base=base, after=after)
        value['sources'] = single + ['tools/check-a733-kernel.py']
        return value

    # A base whose platform.c is new, and images that link it.
    base_objects = {'checks/kernel.c.obj': {'sha256': '1' * 64, 'origin': 'rank-4-kernel-check'},
                    'platform/a733/platform.c.obj': {'sha256': '4' * 64, 'origin': 'new'},
                    'platform/a733/monitor.c.obj': {'sha256': '3' * 64, 'origin': 'rank-4-kernel-check'}}

    def on_base(platform_sha):
        return {'checks/kernel.c.obj': {'sha256': '5' * 64, 'origin': 'new'},
                'platform/a733/platform.c.obj': {'sha256': platform_sha, 'origin': tool.SET_BASE},
                'platform/a733/monitor.c.obj': {'sha256': '3' * 64, 'origin': 'rank-4-kernel-check'}}

    sync = build_elf(elves, 'returning-sync', good + 'int monitor_sync(void){return 3;}\n')
    plain = Path(inputs['monitor-no-cap']['image'])
    rows += [read_fixture('read-good', read_words, 1),
             read_fixture('read-extra-key', [dict(read_words[0], width=8)], 2),
             read_fixture('read-later-condition', [read_words[2], read_words[1]], 3),
             read_fixture('read-outside-block', [tool.word(0x07010000, 'ccu')], 4),
             returning_fixture('returning-good', 5), returning_fixture('returning-plain', 6, controlled=False),
             returning_fixture('returning-sync', 7), returning_fixture('returning-new-object', 8),
             returning_fixture('returning-bad-bin', 9), returning_fixture('returning-bad-controlled', 10),
             returning_fixture('returning-progress', 11), returning_fixture('returning-extra-request', 12),
             returning_fixture('returning-base', 13, controlled=False),
             returning_fixture('returning-on-base', 14, base='returning-base'),
             returning_fixture('returning-after', 15, base='returning-base', after=['returning-on-base']),
             returning_fixture('returning-base-differs', 16, base='returning-base'),
             returning_fixture('returning-base-refused', 17, base='returning-sync'),
             returning_fixture('returning-after-refused', 18, after=['returning-sync'])]
    rows[-6]['overlay'] = ['checks/kernel.c.obj', 'platform/a733/platform.c.obj']
    inputs.update({name: {} for name in ('read-good', 'read-extra-key', 'read-later-condition', 'read-outside-block')})
    inputs.update({
        'returning-good': {'build': fixture_build(work, 'good', plain)},
        'returning-plain': {'build': fixture_build(work, 'plain', plain, controlled=False)},
        'returning-sync': {'build': fixture_build(work, 'sync', sync)},
        'returning-new-object': {'build': fixture_build(work, 'new-object', plain, objects={
            'checks/kernel.c.obj': {'sha256': '1' * 64, 'origin': 'new'},
            'platform/a733/platform.c.obj': {'sha256': '2' * 64, 'origin': 'new'}})},
        'returning-bad-bin': {'build': fixture_build(work, 'bad-bin', plain, bad_bin=True)},
        'returning-bad-controlled': {'build': fixture_build(work, 'bad-controlled', plain, controlled='bad')},
        'returning-progress': {'build': fixture_build(work, 'progress', plain, outcomes=(PROGRESS_OUTCOME,))},
        'returning-extra-request': {'build': fixture_build(work, 'extra-request', plain, runner_extra=EXTRA_REQUEST)},
        'returning-base': {'build': fixture_build(work, 'base', plain, controlled=False, objects=base_objects)},
        'returning-on-base': {'build': fixture_build(work, 'on-base', plain, objects=on_base('4' * 64))},
        'returning-after': {'build': fixture_build(work, 'after', plain, objects=on_base('4' * 64))},
        'returning-base-differs': {'build': fixture_build(work, 'base-differs', plain, objects=on_base('6' * 64))},
        'returning-base-refused': {'build': fixture_build(work, 'base-refused', plain, objects=on_base('4' * 64))},
        'returning-after-refused': {'build': fixture_build(work, 'after-refused', plain)}})
    repo = fixture_repository(work)
    evidence = work / 'evidence'
    lifetime = {'port': '3-3', 'ddr': 'ddr-f', 'chain': ['link-1', 'link-2'],
                'order': [{'row': 'returning-base', 'stem': 'f-base', 'run': 'f-base-run'},
                          {'row': 'returning-on-base', 'stem': 'f-on', 'run': 'f-on-run'},
                          {'external': 'fixture-usb', 'stem': 'f-usb', 'run': 'f-usb-run', 'runbook': 'usb.md',
                           'after': ['returning-on-base'], 'stop_if': {'returning-on-base': ['fixture-fault']}},
                          {'row': 'returning-after', 'stem': 'f-after', 'run': 'f-after-run'}],
                'state': ['Fixture state.']}

    harness.refused(lambda: tool.run({'unknown': {}}, rows, evidence, repo), 'unknown rows')
    harness.refused(lambda: tool.run({'excluded': {}}, rows, evidence, repo), 'excluded rows')
    reused = dict(lifetime, order=lifetime['order'] + [{'external': 'again', 'stem': 'f-base', 'run': 'x',
                                                        'runbook': 'x.md'}])
    harness.refused(lambda: tool.run(inputs, rows, evidence, repo, lifetime=reused), 'reuses a record stem')
    absent = dict(lifetime, order=[{'row': 'returning-good', 'stem': 'g', 'run': 'g-run'}])
    harness.refused(lambda: tool.run({}, rows, evidence, repo, lifetime=absent), 'names no returning row')
    swapped = dict(lifetime, order=[lifetime['order'][1], lifetime['order'][0]] + lifetime['order'][2:])
    harness.refused(lambda: tool.run(inputs, rows, evidence, repo, lifetime=swapped), 'not placed before it')
    late_stop = dict(lifetime, order=[dict(lifetime['order'][0], stop_if={'returning-after': ['fixture-fault']})]
                     + lifetime['order'][1:])
    harness.refused(lambda: tool.run(inputs, rows, evidence, repo, lifetime=late_stop), 'not placed before it')
    own_gate = dict(lifetime, order=[dict(lifetime['order'][0], after=['returning-good'])] + lifetime['order'][1:])
    harness.refused(lambda: tool.run(inputs, rows, evidence, repo, lifetime=own_gate), 'keys outside')
    harness.refused(lambda: tool.run(inputs, rows, evidence, repo, lifetime=lifetime, survey_build=STALE_SURVEY),
                    'the runner would refuse it')
    assert not evidence.exists(), 'refused inputs created no run directory'

    status, run_dir = tool.run(inputs, rows, evidence, repo, lifetime=lifetime)
    assert status == 1 and run_dir == evidence / f'board-bundles-{datetime.now(timezone.utc):%Y%m%d}-1'
    record = json.loads((run_dir / 'run.json').read_text())
    states = {name: r['status'] for name, r in record['rows'].items()}
    expected = {'memory-good': 'prepared', 'monitor-no-cap': 'prepared', 'nested': 'prepared',
                'no-inputs': 'no-inputs', 'excluded': 'excluded', 'read-good': 'prepared',
                'returning-good': 'prepared', 'returning-plain': 'prepared', 'returning-progress': 'prepared',
                'returning-extra-request': 'prepared', 'returning-base': 'prepared', 'returning-on-base': 'prepared',
                'returning-after': 'prepared', 'returning-after-refused': 'prepared'}
    assert states == {r['name']: expected.get(r['name'], 'refused') for r in rows}, states
    reasons = {'memory-no-cap': 'monitor_renewal_cap', 'memory-stripped': 'no symbol table',
               'memory-no-entry': 'does not define monitor_entry', 'failing': 'Fixture admission refused',
               'dirty': 'Uncommitted row sources: fixture/dirty.c', 'absent': 'does not exist',
               'memory-wrong-build': "schema is 'fixture-monitor-v1', not 'fixture-memory-v1'",
               'service-wrong-build': "schema is 'fixture-nested-v1', not 'fixture-npu-service-v1'",
               'memory-unnamed': 'refused until it does', 'service-unnamed': 'refused until it does',
               'uncounted': 'does not declare COMPLETION_COUNTS', 'too-long': "more than the queue's 3600 s",
               'read-extra-key': 'keys outside', 'read-later-condition': 'not an earlier word',
               'read-outside-block': 'not inside the ungated block', 'returning-sync': 'uncapped renewal',
               'returning-new-object': 'outside the overlay: checks/kernel.c.obj, platform/a733/platform.c.obj',
               'returning-bad-bin': 'not the loadable segments', 'returning-bad-controlled': 'Fixture runner refuses',
               'returning-base-differs': "differ from the base bundle's object of the same unit: "
                                         'platform/a733/platform.c.obj',
               'returning-base-refused': 'base row returning-sync is not prepared earlier in this run'}
    stages = {'memory-wrong-build': 'admission', 'service-wrong-build': 'admission', 'memory-unnamed': 'admission',
              'service-unnamed': 'admission', 'uncounted': 'inputs', 'too-long': 'timeout',
              'returning-base-refused': 'inputs'}
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
    for name, timeout, files in (('read-good', tool.READ_TIMEOUT, {'board-read.py', 'WORDS.json', 'check-a733-smp.py',
                                                                   'usb-access.py'}),
                                 ('returning-good', 925, {'image.elf', 'probe.bin', 'equivalence.json',
                                                          'controlled.json', 'check-a733-kernel.py',
                                                          'check-a733-smp.py', 'usb-access.py'}),
                                 ('returning-plain', 925, {'image.elf', 'probe.bin', 'equivalence.json',
                                                           'check-a733-kernel.py', 'check-a733-smp.py',
                                                           'usb-access.py'}),
                                 ('returning-progress', 925, {'image.elf', 'probe.bin', 'equivalence.json',
                                                              'controlled.json', 'check-a733-kernel.py',
                                                              'check-a733-smp.py', 'usb-access.py'}),
                                 ('returning-extra-request', 955, {'image.elf', 'probe.bin', 'equivalence.json',
                                                                   'controlled.json', 'check-a733-kernel.py',
                                                                   'check-a733-smp.py', 'usb-access.py'})):
        submission = json.loads((run_dir / name / 'submission.json').read_text())
        queue.submission(submission)
        assert submission['prerequisites'] == [queue.SUPERVISED_RECOVERY] and submission['timeout_seconds'] == timeout
        bundle = run_dir / name / 'bundle'
        assert {p.name for p in bundle.rglob('*') if p.is_file()} == files, name
        assert unwritable(bundle)
        text = (run_dir / name / 'runbook.md').read_text()
        artifacts = json.loads((run_dir / name / 'artifacts.json').read_text())
        assert all(a['sha256'] in text for a in artifacts) and '## Commands' in text and '## Stop conditions' in text
        assert ('board-read.py $B/WORDS.json' if name == 'read-good' else 'check-a733-kernel.py --fel') in text
        controlled = name in ('returning-good', 'returning-progress', 'returning-extra-request')
        assert ('--controlled $B/controlled.json' in text) == controlled, name
        if controlled:
            # Outcome names come from the bundle's controlled.json.
            outcome = PROGRESS_OUTCOME if name == 'returning-progress' else OUTCOME
            assert f'CONTROLLED: {outcome["name"]}, status {outcome["status"]}' in text, name
            assert ('progress record at result offset 128' in text) == (name == 'returning-progress'), name
        assert f'Queue timeout: {timeout} s' in text, name
        if name != 'read-good':
            assert (f'-- $PY -B {repo / tool.SURVEY_RUNNER} run --fel $FEL --build {tool.SURVEY_BUILD} \\\n'
                    in text and '--prerequisite launch-survey-software=' in text and '$V/common/survey.py' not in text
                    and '--claim {claim} --owner {owner}' in text), name
        prepared[name] = {'files': len(files), 'timeout_seconds': timeout}
    assert (run_dir / 'read-good/bundle/board-read.py').read_bytes() == (ROOT / tool.READER).read_bytes()
    # The base and the rows on it, with the lifetime's records.
    admitted = {name: json.loads((run_dir / name / 'submission.json').read_text()) for name in
                ('returning-base', 'returning-on-base', 'returning-after')}
    assert all(queue.submission(v) is None or True for v in admitted.values())
    text = {name: (run_dir / name / 'runbook.md').read_text() for name in admitted}
    links = {'returning-base': ['link-1', 'link-2'], 'returning-on-base': ['link-1', 'link-2', 'f-base-run'],
             'returning-after': ['link-1', 'link-2', 'f-base-run', 'f-on-run', 'f-usb-run']}
    for name, chain in links.items():
        found = [line.split('$S/')[1].split('/')[0] for line in text[name].splitlines() if '--warm-return $S/' in line]
        assert found == chain, (name, found)
        assert '--ddr-evidence $S/ddr-f/session.json' in text[name], name
    assert '$S/f-on-run' in text['returning-on-base']
    assert '1 equal objects of the image of `returning-base`' in text['returning-on-base']
    assert '## Run only after' not in text['returning-base']
    assert '- `returning-base` of this set passed' in text['returning-on-base']
    assert '- `returning-on-base` of this set passed' in text['returning-after']
    order = (run_dir / 'runbook.md').read_text()
    assert ('| 3 | `fixture-usb`, `usb.md` | `$S/f-usb-*`; link `f-usb-run` | `returning-on-base` passed; not if '
            '`returning-on-base` ended in `fixture-fault` |') in order, order
    assert 'runner does not check them' in order and 'is not a pass' in order
    assert '`link-1`, `link-2`' in order and 'Fixture state.' in order
    prepared['lifetime'] = {name: len(chain) for name, chain in links.items()}
    # External runbooks: record directories under the entry's stem only.
    (run_dir / 'usb.md').write_text('$S/f-usb-capture $S/f-usb-run/session.json $S/link-1/session.json '
                                    '$S/f-on-run/session.json\n')
    assert tool.check_externals(run_dir) == {'fixture-usb': ['f-usb-capture', 'f-usb-run']}
    (run_dir / 'usb.md').write_text('$S/f-usb-capture $S/f-usb-run/session.json $S/f-old-usb/session.json\n')
    harness.refused(lambda: tool.check_externals(run_dir), 'outside the stem f-usb: f-old-usb')
    (run_dir / 'usb.md').write_text('$S/f-usb-capture\n')
    harness.refused(lambda: tool.check_externals(run_dir), 'never names its run directory f-usb-run')
    # A stop outcome that the named row does not list refuses the lifetime.
    unknown = dict(lifetime, order=lifetime['order'][:2] + [dict(lifetime['order'][2],
                                                                stop_if={'returning-on-base': ['no-such-outcome']})]
                   + lifetime['order'][3:])
    status, third = tool.run({k: inputs[k] for k in ('returning-base', 'returning-on-base', 'returning-after')}, rows,
                             evidence, repo, lifetime=unknown)
    record = json.loads((third / 'run.json').read_text())
    assert status == 1 and 'no-such-outcome' in record['lifetime_refused'] and not (third / 'runbook.md').exists()
    # --run-dir: a named new directory under the evidence directory given, here
    # with a lifetime that starts with no warm-return link and a start section.
    fresh = {'port': '3-1', 'ddr': 'n-ddr', 'chain': [], 'start': ['Fixture start.'],
             'order': [{'row': 'returning-base', 'stem': 'n-base', 'run': 'n-base-run'}]}
    named = evidence / 'named-set' / 'port-3-1'
    harness.refused(lambda: tool.run({'returning-base': inputs['returning-base']}, rows, evidence, repo,
                                     lifetime=fresh, run_dir=work / 'elsewhere' / 'port-3-1'),
                    'is not under the evidence directory')
    assert not (work / 'elsewhere').exists(), 'a refused run directory is not created'
    status, chosen = tool.run({'returning-base': inputs['returning-base']}, rows, evidence, repo, lifetime=fresh,
                              run_dir=named)
    assert status == 0 and chosen == named.resolve(), (status, chosen)
    base_book = (chosen / 'returning-base/runbook.md').read_text()
    assert 'no `--warm-return`' in base_book and '--warm-return $S' not in base_book, base_book
    assert f'--platform-survey $S/n-base-survey/session.json\n   ```' in base_book, base_book
    assert 'Fixture start.' in (chosen / 'runbook.md').read_text() and 'no returning run' in (chosen / 'runbook.md').read_text()
    harness.refused(lambda: tool.run({}, rows, evidence, repo, run_dir=named), 'exists', kind=FileExistsError)
    prepared['run_dir'] = str(chosen.relative_to(evidence))
    # A gate that an earlier set's row passed in this lifetime: named in passed with its chain record.
    seeded = {'port': '3-1', 'ddr': 'p-ddr', 'chain': ['p-sync-run'], 'passed': {'returning-sync': 'p-sync-run'},
              'order': [{'row': 'returning-after-refused', 'stem': 'p-after', 'run': 'p-after-run'}]}
    harness.refused(lambda: tool.run({'returning-after-refused': inputs['returning-after-refused']}, rows, evidence,
                                     repo, lifetime=dict(seeded, chain=[])), 'passed maps a row')
    harness.refused(lambda: tool.run({'returning-after-refused': inputs['returning-after-refused']}, rows, evidence,
                                     repo, lifetime=dict(seeded, passed={})), 'not placed before it')
    status, gated = tool.run({'returning-after-refused': inputs['returning-after-refused']}, rows, evidence, repo,
                             lifetime=seeded, run_dir=evidence / 'named-set' / 'port-seeded')
    book = (gated / 'returning-after-refused/runbook.md').read_text()
    assert status == 0 and '`returning-sync` passed earlier in this lifetime, from an earlier set, as `$S/p-sync-run`' in book, book
    assert '--warm-return $S/p-sync-run/session.json' in book, book
    assert '`returning-sync` passed (`p-sync-run`, already in the chain)' in (gated / 'runbook.md').read_text()
    # A base row prepared again in a later set after its image passed earlier in this lifetime: the
    # dependent row's runbook names that pass instead of saying the base has not run.
    rebased = {'port': '3-1', 'ddr': 'q-ddr', 'chain': ['q-base-run'], 'passed': {'returning-base': 'q-base-run'},
               'order': [{'row': 'returning-on-base', 'stem': 'q-on', 'run': 'q-on-run'}]}
    status, again = tool.run({'returning-base': inputs['returning-base'], 'returning-on-base': inputs['returning-on-base']},
                             rows, evidence, repo, lifetime=rebased, run_dir=evidence / 'named-set' / 'port-rebased')
    on_book = (again / 'returning-on-base/runbook.md').read_text()
    assert status == 0 and 'which passed earlier in this lifetime as `$S/q-base-run`' in on_book, on_book
    assert 'which has not run when the set is prepared' not in on_book, on_book
    status, second = tool.run({}, rows, evidence, repo)
    assert status == 0 and second.name.endswith('-3'), 'a later run gets its own directory'

    result.update(rows=states, prepared=prepared, sandbox_start=sandbox_start(work))
    assert result['sandbox_start']['median_ms'] <= 100, result['sandbox_start']
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    writable_tree(work)
    shutil.rmtree(work)
    print(json.dumps({'prepared': prepared, 'sandbox_start': result['sandbox_start']}))


if __name__ == '__main__':
    main()
