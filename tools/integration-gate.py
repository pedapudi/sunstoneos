#!/usr/bin/env python3
"""Run explicitly selected integration builds and checks, once each.

Host configuration stays in CACHE/bin/cfg.sh. This tool never launches a board.
Build directories belong to the resolved source checkout, so gates on different
checkouts never share a CMake cache; a lock refuses a second concurrent gate on
the same checkout. Every gate also waits for the host-wide lock CACHE/gate.lock (CACHE/gate-full.lock for --full), so
gates never run side by side. The recorded source identity includes uncommitted changes:
the revision, the tracked diff and every untracked file that Git does not
ignore. The gate fails if that state cannot be read completely, or if it
changes before the gate holds the lock or by the end of the last step.
Use checks/check-index.py --changed PATH... to help choose focused checks; its
textual coverage is advisory. Component checks outside this gate run directly.
The full gate conditions hook fixtures on --affected or --changed scope. Its
default combines HEAD^..HEAD (all tracked files at a root commit) with staged,
unstaged and nonignored untracked changes in the checkout.

The full gate keeps going: it runs every selected step, records every failure
in summary.txt and exits nonzero if any step failed. A step whose build failed
is recorded as SKIP and counts as a failure. --fail-fast stops at the first
failure and runs the steps one at a time in their fixed order; the targeted
selections (--target, --variant, --check, --affected, --changed) keep that
behavior unless --keep-going is given. Builds run one at a time and finish
before the checks that need them. Checks run in parallel (--jobs workers, no
new check while MemAvailable is below --min-available-gb); checks that run
QEMU or time a result run one at a time after the parallel group. Every check
runs with the environment recorded in environment.txt. Documentation-link and
status-record checks report WARN lines and do not fail the gate unless
--strict-docs is given.

--resume DIRECTORY continues an affected gate after host-check repairs. It
recomputes the complete selection with the same base or an ancestral advance, reuses verified
PASS commands and inputs, and runs failed, unexecuted and newly affected checks.
Native or configuration changes rebuild all preparation; only unaffected
indexed host checks may then reuse their passes. Checkpoints retain hashes
and reuse provenance; older evidence requires a recorded clean commit and
unchanged preparation tree keys. Sessions and bundles keep their original
paths, and continuation never cleans the original evidence.
"""
import argparse
import atexit
import fcntl
import hashlib
import importlib.util
import json
import io
from pathlib import Path
from host_evidence import unique_directory
import host_priority
import gate_images
import host_space
import os
import re
import shlex
import shutil
import subprocess
import sys
import threading
import tarfile
import time
from concurrent.futures import ThreadPoolExecutor

VARIANTS = {
    # The one OS image: it admits the supervised and the resident lifetime and selects one from the contract magic.
    'sup': [],
    # An application that declares the headset device links the headset library (the Bluetooth stack and mSBC).
    'headset': ['-DSUNSTONE_APPLICATION=headset-fixture', '-DSUNSTONE_BT_SBC_DIR={cache}/sources/bluetooth-msbc'],
    # The NPU service and its EL0 ring, the CPU clock owner and the performance service are in every image, and a session
    # grants them (docs/designs/one-os-image.md, docs/designs/one-os-image-cpu.md).
}
# OS-core checks and the central target each needs. The core central build is
# configured without external applications.
DEPENDENCIES = {'kernel': 'kernel-check', 'soak8': 'smp8-soak-model'}
CORE_SCRIPTS = ('shared-helpers', 'progress', 'check-index', 'pmic-tree', 'power-domains-tree')
CORE_CHECKS = (*DEPENDENCIES, *CORE_SCRIPTS)
def external_applications(source, paths):
    """Load each selected application's own build targets and check commands."""
    applications = {}
    for path in dict.fromkeys(Path(p).resolve() for p in paths if p):
        port = path / 'sunstoneos'
        record = json.loads((port / 'gate.json').read_text())
        name = record['name']
        if not re.fullmatch(r'[a-z][a-z0-9_-]*', name) or name in applications:
            raise ValueError('application names must be unique lowercase identifiers')
        checks = record.get('checks', {})
        if not isinstance(checks, dict):
            raise ValueError('application checks must be an object')
        for check, spec in checks.items():
            if (not re.fullmatch(r'[a-z][a-z0-9_-]*', check) or check in CORE_CHECKS or
                not isinstance(spec, list) or len(spec) != 2 or
                not isinstance(spec[0], str) or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.+-]*', spec[0]) or
                not isinstance(spec[1], list) or not spec[1] or not all(isinstance(x, str) for x in spec[1])):
                raise ValueError('invalid application check: ' + str(check))
            if any(check in app['checks'] for app in applications.values()):
                raise ValueError('duplicate application check: ' + check)
        applications[name] = dict(record, root=path)
    return applications


def output_option(entry):
    """The option that names a check's new output directory, or None."""
    declared = entry.get('output_option')
    if declared:
        return declared
    return '--output' if '--output' in entry.get('required_arguments', ()) or entry.get('output') is True else None


def available_gib():
    """MemAvailable in GiB, or None when the system does not report it."""
    try:
        for line in Path('/proc/meminfo').read_text().splitlines():
            if line.startswith('MemAvailable:'):
                return int(line.split()[1]) / 1048576
    except (OSError, ValueError, IndexError):
        pass
    return None


# Needs that only a board or the host's USB ports can meet. A check that has only these needs is reported, never refused.
BOARD_NEEDS = {'USB access', '--fel'}


def load_gate_inputs(index):
    """What the checks declare for the gate (GATE_RUNS, GATE_BOARD, GATE_WAIVED; checks/check-index.py) and the images it builds."""
    gate = {name: entry.get('gate', {}) for name, entry in index.items()}
    for name, declaration in gate.items():
        if 'runs' in declaration and not declaration['runs']:
            raise ValueError('%s declares no gate runs' % name)
    return {'images': gate_images.IMAGES, 'bundles': gate_images.BUNDLES, 'fixtures': gate_images.FIXTURES,
            'checks': {n: {'runs': [{'args': a} for a in g['runs']]} for n, g in gate.items() if 'runs' in g},
            'board': [n for n, g in gate.items() if 'board' in g],
            'waived': {n: g['waived'] for n, g in gate.items() if 'waived' in g}}


def gate_entries(checks, index):
    """Selected checks with declared runs or inputs beyond their default host mode."""
    return [check for check in checks if check['status'] == 'pending' or
            'runs' in index.get(check['check'], {}).get('gate', {})]


def classify_affected(entries, index, inputs):
    """Sort affected checks with declared runs or additional inputs into the reported groups.

    entries: [{'check': file name, 'needs': [...]}]. Returns a dict:
      supplied    in the registry, so the gate builds the image and runs them;
      board       need a board or USB access: reported, they do not refuse;
      waived      listed in the registry's waivers with a reason (inputs the gate does not supply yet);
      unsupplied  everything else: the gate refuses, naming each."""
    result = {'supplied': [], 'board': [], 'waived': [], 'unsupplied': []}
    for entry in entries:
        name, needs = entry['check'], entry.get('needs', [])
        record = index.get(name)
        if record is None or record.get('library') or needs == ['helper module']:
            continue                                       # a deleted check, or a helper module that no one runs
        if name in inputs['checks']:
            result['supplied'].append(name)
        elif record.get('usb') or name in inputs['board'] or (needs and set(needs) <= BOARD_NEEDS):
            result['board'].append(name)
        elif name in inputs['waived']:
            result['waived'].append(name)
        else:
            result['unsupplied'].append((name, needs))
    return result


def placeholders(command):
    """The (kind, name) pairs a registered command asks for: {elf:IMAGE}, {build:IMAGE}, {session:IMAGE}, {bundle:NAME} and {fixture:NAME}."""
    return sorted({(m.group(1), m.group(2)) for part in command
                   for m in re.finditer(r'\{(elf|build|session|bundle|fixture):([A-Za-z0-9_-]+)\}', part)})


def compatibility_variables(source):
    """Optional application source records define legacy NAME_DIR selectors."""
    return [path.parent.name.upper().replace('-', '_') + '_DIR'
            for path in sorted((source / 'apps').glob('*/source.json'))
            if 'application_directory' in json.loads(path.read_text())]


def tree_state(source):
    """The checkout's revision, status text, and the SHA-256 of the tracked
    diff and of the untracked files.

    Each untracked, non-ignored path contributes its name and the SHA-256 of
    its contents (a symbolic link's target; a nested repository's name only).
    None stands for a value Git or the file system could not produce,
    including an untracked file that cannot be read."""
    def git(*command, text=False):
        return subprocess.run(['git', *command], cwd=source, capture_output=True, text=text)
    head = git('rev-parse', 'HEAD', text=True)
    status = git('status', '--porcelain', text=True)
    diff = git('diff', '--binary', 'HEAD')
    listed = git('ls-files', '--others', '--exclude-standard', '-z')
    untracked = None
    if listed.returncode == 0:
        names = sorted(name for name in listed.stdout.split(b'\0') if name)
        digest = hashlib.sha256()
        try:
            for name in names:
                path = source / os.fsdecode(name)
                if path.is_symlink():
                    content = os.fsencode(os.readlink(path))
                elif path.is_dir():
                    content = b''
                else:
                    content = path.read_bytes()
                digest.update(name + b'\0' + hashlib.sha256(content).digest())
        except OSError:
            pass
        else:
            untracked = (digest.hexdigest(), len(names))
    return (head.stdout.strip() if head.returncode == 0 else None,
            status.stdout if status.returncode == 0 else None,
            hashlib.sha256(diff.stdout).hexdigest() if diff.returncode == 0 else None, untracked)


def source_files(source, revision=None):
    """Content identity of repository inputs, including nonignored local files."""
    if revision:
        archive = subprocess.check_output(['git', 'archive', '--format=tar', revision], cwd=source)
        result = {}
        with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
            for entry in stream:
                if entry.isfile() or entry.issym():
                    data = entry.linkname.encode() if entry.issym() else stream.extractfile(entry).read()
                    kind = b'link\0' if entry.issym() else b'executable\0' if entry.mode & 0o111 else b'file\0'
                    result[entry.name] = hashlib.sha256(kind + data).hexdigest()
        return result
    names = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=source)
    result = {}
    for name in set(os.fsdecode(n) for n in names.split(b'\0') if n):
        path = source / name
        if path.is_symlink():
            data = os.fsencode(os.readlink(path))
        elif path.is_file():
            data = path.read_bytes()
        else:
            continue
        kind = b'link\0' if path.is_symlink() else b'executable\0' if path.stat().st_mode & 0o111 else b'file\0'
        result[name] = hashlib.sha256(kind + data).hexdigest()
    return result


def selection_identity(args):
    """Invocation policy; labels, reasons and scheduling limits are metadata."""
    return {name: getattr(args, name) for name in
            ('target', 'variant', 'check', 'affected', 'changed', 'full', 'trials', 'strict_docs')}


def input_identity(files, names):
    """Include directory inventories, absent inputs and the check itself."""
    selected = {}
    for name in names:
        selected[name] = files.get(name)
        selected.update((p, value) for p, value in files.items() if p.startswith(name.rstrip('/') + '/'))
    return selected


def command_files(command, source):
    """Resolved file arguments (ELFs, sessions and bundles) are check inputs."""
    result = {}
    for word in command[1:]:
        path = Path(word)
        if not path.is_absolute():
            path = source / path
        if path.is_file():
            result[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def identity_hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


class Resume:
    """Reuse unchanged host checks, with preparation reuse for host-only repairs.

    Old gates without checkpoints are admitted only from their recorded clean
    commit. Their artifact tree keys and pre-stop file timestamps supply the
    legacy preparation witness; checkpoints additionally hash every artifact.
    """
    def __init__(self, directory, source, args, parser):
        self.source = source
        self.directory = directory.resolve()
        if not self.directory.is_dir():
            raise ValueError('resume directory does not exist')
        checkpoint = self.directory / 'checkpoint.json'
        self.legacy = not checkpoint.exists()
        if not self.legacy:
            self.record = json.loads(checkpoint.read_text())
            if self.record.get('version') != 1 or self.record.get('source') != str(source):
                raise ValueError('resume checkpoint version or source differs')
        else:
            summary = (self.directory / 'summary.txt').read_text()
            def field(name):
                found = re.findall('^' + re.escape(name) + r': (.*)$', summary, re.M)
                if len(found) != 1:
                    raise ValueError('resume has no unique ' + name)
                return found[0]
            empty = hashlib.sha256(b'').hexdigest()
            revision = field('revision')
            if re.search(r'^FAIL (?:the source|external .*changed|external .*unreadable)', summary, re.M):
                raise ValueError('legacy resume source identity failed during the original gate')
            if (field('source') != str(source) or field('status') != 'clean' or
                field('diff-sha256') != empty or field('untracked-sha256') != empty + ' (0 files)'):
                raise ValueError('legacy resume requires a complete clean source identity')
            previous = parser.parse_args(shlex.split(field('selection')))
            state = (revision, '', empty, (empty, 0))
            self.record = dict(version=1, source=str(source), files=source_files(source, revision),
                selection=selection_identity(previous), state=state,
                artifact_key=hashlib.sha256(repr(state).encode()).hexdigest()[:16], steps={})
            verdicts = {name: verdict for verdict, name in re.findall(
                r'^(PASS|FAIL|WARN|SKIP) ([A-Za-z0-9_.-]+)(?: |$)', summary, re.M)}
            for line in (self.directory / 'commands.txt').read_text().splitlines():
                name, command = line.split(': ', 1)
                if name in self.record['steps'] or not re.fullmatch(r'[A-Za-z0-9_.-]+', name):
                    raise ValueError('duplicate or invalid resume command')
                log = self.directory / (name + '.log')
                self.record['steps'][name] = dict(command=shlex.split(command), verdict=verdicts.get(name),
                    log_sha256=hashlib.sha256(log.read_bytes()).hexdigest() if log.is_file() else None)
        previous_selection = dict(self.record.get('selection', {}))
        current_selection = selection_identity(args)
        # A symbolic base can move between runs. A pinned spelling is admitted
        # only when the resolved base below is equal or advances along ancestry.
        previous_selection['affected'] = bool(previous_selection.get('affected'))
        current_selection['affected'] = bool(current_selection.get('affected'))
        if previous_selection != current_selection:
            raise ValueError('resume selection or policy differs')
        if (not isinstance(self.record.get('files'), dict) or not isinstance(self.record.get('steps'), dict) or
            subprocess.run(['git', 'merge-base', '--is-ancestor', self.record['state'][0], 'HEAD'],
                           cwd=source, capture_output=True).returncode):
            raise ValueError('resume source identity is incomplete or is not an ancestor')
        self.affected = json.loads((self.directory / 'affected.json').read_text())
        if args.affected and (self.affected.get('revisions') or [None, None])[1] != self.record['state'][0]:
            raise ValueError('resume original selection does not end at its recorded source revision')
        self.current = source_files(source)
        self.changed = sorted(p for p in self.current.keys() | self.record['files'].keys()
                              if self.current.get(p) != self.record['files'].get(p))
        self.environment = (self.directory / 'environment.txt').read_text()
        self.legacy_finished_ns = (self.directory / 'summary.txt').stat().st_mtime_ns
        if self.legacy and any((self.directory / name).stat().st_mtime_ns > self.legacy_finished_ns
                               for name in ('commands.txt', 'affected.json', 'environment.txt')):
            raise ValueError('legacy resume metadata changed after the recorded stop')
        self.refresh_native = False

    def admit(self, index, directories):
        """Native changes invalidate all preparation and image/core checks."""
        for path in self.changed:
            p = Path(path)
            host = (p.parent == Path('checks') and
                    ((p.suffix == '.py' and index.get(p.name, {}).get('pure_host')) or
                     (p.suffix in ('.c', '.h') and any(e.get('pure_host') and path in e.get('inputs', [])
                                                      for e in index.values()))))
            if path not in ('tools/integration-gate.py', 'checks/integration-gate.py') and not host:
                self.refresh_native = True
        # A host fixture may also be a model image's source. Check actual CMake
        # and compiler dependency records before treating its change as host-only.
        needles = [Path(p).name for p in self.changed if p != 'tools/integration-gate.py']
        for directory in directories if not self.refresh_native else ():
            if not needles or not directory.is_dir():
                continue
            for path in directory.rglob('*'):
                if path.is_file() and (path.suffix in ('.d', '.make', '.cmake') or path.name == 'compile_commands.json'):
                    text = path.read_text(errors='replace')
                    if any(n in text for n in needles):
                        self.refresh_native = True
                        return

    def reusable(self, name, command, script, names, artifacts):
        step = self.record['steps'].get(name)
        if not step or step.get('verdict') != 'PASS':
            return None
        if not self.legacy and not step.get('stable'):
            return None
        normalize = lambda words, output: [str(w).replace(str(output), '{gate-output}') for w in words]
        if normalize(step['command'], self.directory) != normalize(command, self.output):
            return None
        log = self.directory / (name + '.log')
        if not log.is_file() or hashlib.sha256(log.read_bytes()).hexdigest() != step.get('log_sha256'):
            return None
        if self.legacy and log.stat().st_mtime_ns > self.legacy_finished_ns:
            return None
        before = input_identity(self.record['files'], names)
        if input_identity(self.current, names) != before:
            return None
        if not self.legacy and step.get('inputs_sha256') != identity_hash(before):
            return None
        if not self.legacy and step.get('command_files') != command_files(command, self.source):
            return None
        if self.legacy and any(Path(p).stat().st_mtime_ns > self.legacy_finished_ns
                               for p in command_files(command, self.source)):
            return None
        for path in artifacts:
            if not path.is_file():
                return None
            if path.name == '.gate-tree-key':
                if path.read_text() != self.record['artifact_key']:
                    return None
                continue
            if self.legacy:
                if path.stat().st_mtime_ns > self.legacy_finished_ns:
                    return None
            elif step.get('artifacts', {}).get(str(path)) != hashlib.sha256(path.read_bytes()).hexdigest():
                return None
        return step

def gate_environment(source, cache, build_root):
    """The environment of every build, configure step and check: a temporary directory on the main disk (never /tmp), the host
    compilers on PATH and no bytecode files in the source."""
    host_space.use_disk_tmp(build_root)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    env['SUNSTONEOS_DIR'] = str(source)
    env['PATH'] = str(cache / 'host/usr/lib/llvm-21/bin') + os.pathsep + env.get('PATH', '')
    return env


def take_gate_lock(cache, name):
    """Block until this process holds CACHE/NAME; the returned file keeps the lock while it is referenced."""
    cache.mkdir(parents=True, exist_ok=True)
    handle = open(cache / name, 'a+')
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        handle.seek(0)
        print('waiting for %s, held by %s' % (cache / name, handle.read().strip() or 'another gate'), flush=True)
        fcntl.flock(handle, fcntl.LOCK_EX)
    handle.seek(0)
    handle.truncate()
    handle.write('pid %d since %s\n' % (os.getpid(), time.strftime('%Y-%m-%dT%H:%M:%S')))
    handle.flush()
    return handle


def main(argv=None):
    host_priority.lower()             # nice 10 and the idle I/O class for the gate and everything it starts (tools/host_priority.py)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('label', help='evidence label; existing directories receive a unique child')
    parser.add_argument('--reason', default='', help='optional changed behavior and risk this selection checks')
    parser.add_argument('--target', action='append', default=[], help='OS-core central CMake target; repeat to add targets')
    parser.add_argument('--variant', choices=VARIANTS, action='append', default=[])
    parser.add_argument('--check', action='append', default=[], help='core check or check declared by an external application')
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--affected', metavar='RANGE', help='select indexed checks for BASE..HEAD and one supervised OS baseline')
    selection.add_argument('--changed', nargs='+', metavar='PATH', help='select indexed checks for repository paths and one OS baseline')
    parser.add_argument('--full', action='store_true', help='explicit broad integration milestone: all core and application central targets, every OS variant and all listed checks')
    parser.add_argument('--cache', type=Path, default=Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos')))
    parser.add_argument('--build-root', type=Path, help='build and gate evidence directory; defaults to CACHE/build')
    parser.add_argument('--application', type=Path, action='append', default=[],
                        help='external checkout with sunstoneos/gate.json; repeat to add applications')
    parser.add_argument('--trials', type=int, default=1, help='predeclared soak8 trials, not failure retries (default: 1)')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--fail-fast', action='store_true',
                      help='stop at the first failure and run steps one at a time (the default for targeted selections)')
    mode.add_argument('--keep-going', action='store_true',
                      help='run every selected step and report every failure (the default for --full)')
    parser.add_argument('--jobs', type=int, default=8, help='parallel checks when keeping going (default: 8)')
    parser.add_argument('--min-available-gb', type=float, default=32,
                        help='start no new check while MemAvailable is below this many GiB (default: 32)')
    parser.add_argument('--memory-wait', type=int, default=3600,
                        help='seconds a check waits for MemAvailable to recover before it fails unstarted (default: 3600)')
    parser.add_argument('--check-timeout', type=int, default=3600, help='seconds allowed to each check (default: 3600)')
    parser.add_argument('--strict-docs', action='store_true',
                        help='fail, instead of warn, when a documentation-link or status-record check fails')
    parser.add_argument('--allow-dirty', action='store_true', help='compatibility option; local changes are always recorded')
    parser.add_argument('--resume', type=Path, help='continue a retained affected gate; native/configuration changes refresh preparation and retain only unchanged host passes')
    args = parser.parse_args(argv)
    if not args.full and not (args.target or args.variant or args.check or args.affected or args.changed):
        parser.error('select --target, --variant or --check; --full is only for justified broad validation')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', args.label):
        parser.error('label must be a single filename beginning with a letter or digit')
    if args.jobs < 1 or args.min_available_gb < 0 or args.check_timeout < 1:
        parser.error('--jobs and --check-timeout must be positive and --min-available-gb cannot be negative')
    if args.trials < 1 or (args.trials != 1 and not args.full and 'soak8' not in args.check):
        parser.error('--trials must be positive and applies only to soak8')
    if any(not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.+-]*', t) or t in
           {'help', 'clean', 'install', 'edit_cache', 'rebuild_cache', 'depend'} for t in args.target):
        parser.error('--target must name a build target')
    source, cache = args.source.resolve(), args.cache.resolve()
    if not (source / 'CMakeLists.txt').is_file():
        parser.error('source must be a SunstoneOS checkout')
    resume = None
    if args.resume:
        if args.full or not (args.affected or args.changed) or args.application:
            parser.error('--resume requires an affected selection without external applications or --full')
        try:
            resume = Resume(args.resume, source, args, parser)
        except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
            parser.error('cannot resume: ' + str(error))
    # One gate at a time on the host, whoever starts it: a direct call waits behind a landing's gate instead of running beside
    # it. The full suite takes its own lock so that it blocks no landing. The lock is held until the process exits.
    gate_lock = take_gate_lock(cache, 'gate-full.lock' if args.full else 'gate.lock')
    targets = list(dict.fromkeys(args.target))
    variants = list(VARIANTS) if args.full else list(dict.fromkeys(args.variant))
    indexed, pending, affected_plan, hook_scope = [], [], None, None
    index = {}
    inputs, classified = None, {'supplied': [], 'board': [], 'waived': [], 'unsupplied': []}
    if args.full or args.affected or args.changed:
        try:
            spec = importlib.util.spec_from_file_location('affected', source / 'checks/affected.py')
            selector = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(selector)
            if args.full:
                index = selector.indexer.generate()
                indexed = [{'check': name, 'output': bool(output_option(entry)), 'output_option': output_option(entry)}
                           for name, entry in sorted(index.items()) if entry['pure_host']]
                pending = [{'check': name, 'needs': selector.needs(entry)}
                           for name, entry in sorted(index.items()) if not entry['pure_host'] and not entry['library']]
                scope_range, scope_paths = args.affected, args.changed
                basis = 'explicit range or paths'
                dirty_paths = []
                if not (scope_range or scope_paths):
                    commit = subprocess.check_output(['git', 'cat-file', '-p', 'HEAD'], cwd=source)
                    if any(line.startswith(b'parent ') for line in commit.split(b'\n\n', 1)[0].splitlines()):
                        scope_range, basis = 'HEAD^..HEAD', 'latest first-parent change'
                    else:
                        tracked = subprocess.check_output(['git', 'ls-tree', '-r', '--name-only', '-z', 'HEAD'], cwd=source)
                        scope_paths = [os.fsdecode(path) for path in tracked.split(b'\0') if path]
                        basis = 'all tracked files at the root commit'
                    tracked_changes = subprocess.check_output(['git', 'diff', '--name-only', '--no-renames', '-z', 'HEAD'], cwd=source)
                    untracked = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=source)
                    dirty_paths = sorted({os.fsdecode(path) for path in (tracked_changes + untracked).split(b'\0') if path})
                scope = selector.plan(scope_range, scope_paths, index=index)
                selected = any(check['check'] == 'pre-push-hook.py' for check in scope['checks'])
                if dirty_paths:
                    local = selector.plan(changed=dirty_paths, index=index)
                    selected |= any(check['check'] == 'pre-push-hook.py' for check in local['checks'])
                indexed = [check for check in indexed if check['check'] != 'pre-push-hook.py' or selected]
                hook_scope = {'basis': basis, 'selected': selected, 'plan': scope, 'dirty_paths': dirty_paths}
                # Only affected checks select declared modes in a full gate.
                # Unaffected pure-host checks retain their default invocation.
                affected_pending = {c['check']: c for c in gate_entries(scope['checks'], index)}
                if dirty_paths:
                    affected_pending.update({c['check']: c for c in gate_entries(local['checks'], index)})
                affected_entries = list(affected_pending.values())
            else:
                index = selector.indexer.generate()
                affected_plan = selector.plan(args.affected, args.changed, index=index)
                if resume:
                    current_revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
                    if args.affected and (affected_plan.get('revisions') or [None, None])[1] != current_revision:
                        raise ValueError('resume affected selection must end at the current source HEAD')
                    previous_base = (resume.affected.get('revisions') or [None])[0]
                    current_base = (affected_plan.get('revisions') or [None])[0]
                    if current_base != previous_base:
                        if (not previous_base or not current_base or
                            subprocess.run(['git', 'merge-base', '--is-ancestor', previous_base, current_base],
                                           cwd=source, capture_output=True).returncode or
                            subprocess.run(['git', 'merge-base', '--is-ancestor', current_base, 'HEAD'],
                                           cwd=source, capture_output=True).returncode):
                            raise ValueError('resume affected base differs without an ancestor chain')
                        resume.refresh_native = True
                    added = selector.plan(changed=resume.changed, index=index)
                    selected = {c['check']: c for c in affected_plan['checks']}
                    selected.update((c['check'], c) for c in added['checks'])
                    affected_plan['checks'] = [selected[n] for n in sorted(selected)]
                    affected_plan['changed'] = sorted(set(affected_plan.get('changed') or []) | set(resume.changed))
                indexed = [check for check in affected_plan['checks'] if check['status'] == 'runnable']
                pending = [check for check in affected_plan['checks'] if check['status'] == 'pending']
                affected_entries = gate_entries(affected_plan['checks'], index)
                variants = list(dict.fromkeys([*variants, affected_plan['baseline']]))
            inputs = load_gate_inputs(index)
            classified = classify_affected(affected_entries, index, inputs)
            # A declaration specifies the complete gate invocation, including
            # image modes of otherwise pure-host checks. Do not run its default
            # a second time; declarations can list both modes when needed.
            indexed = [check for check in indexed if check['check'] not in classified['supplied']]
        except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
            parser.error('cannot select indexed checks: ' + str(error))
        for name in classified['supplied']:
            for run_ in inputs['checks'][name]['runs']:
                for kind, image in placeholders(run_['args']):
                    if kind == 'fixture':
                        if image not in inputs['fixtures']:
                            parser.error('%s names the unknown fixture %s' % (name, image))
                    elif kind == 'bundle':
                        if image not in inputs['bundles']:
                            parser.error('%s names the unknown bundle %s' % (name, image))
                    elif image in VARIANTS and image not in variants:
                        variants.append(image)
                    elif image not in VARIANTS and image not in inputs['images']:
                        parser.error('%s names the unknown image %s' % (name, image))
    # Prose-only and timing-sensitive checks come from the check index.
    prose, serial = {}, set()
    try:
        if args.full or args.affected or args.changed:
            module = selector.indexer
        else:
            spec = importlib.util.spec_from_file_location('affected', source / 'checks/affected.py')
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            module = module.indexer
        prose, serial = dict(getattr(module, 'PROSE_CHECKS', {})), set(getattr(module, 'SERIAL_CHECKS', ()))
    except Exception:
        pass
    selectors = compatibility_variables(source)
    paths = list(args.application)
    paths += [p for p in os.environ.get('SUNSTONE_EXTERNAL_APPLICATIONS', '').split(';') if p]
    paths += [os.environ[key] for key in selectors if os.environ.get(key)]
    try:
        configured = external_applications(source, paths)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error('invalid external application: ' + str(error))
    if resume and configured:
        parser.error('--resume does not support external application state')
    application_checks = {check: name for name, app in configured.items() for check in app['checks']}
    known_checks = (*CORE_CHECKS, *application_checks)
    checks = list(known_checks) if args.full else list(dict.fromkeys(args.check))
    # Core commands already run these scripts with their established arguments.
    indexed = [check for check in indexed if Path(check['check']).stem not in set(checks) & set(CORE_SCRIPTS)]
    if any(check not in known_checks for check in checks):
        parser.error('unknown check; external checks require --application or SUNSTONE_EXTERNAL_APPLICATIONS')
    for check in checks:
        if check in DEPENDENCIES and DEPENDENCIES[check] not in targets:
            targets.append(DEPENDENCIES[check])
    # Each selected application and the central targets of its selected checks.
    applications = {}
    for check in checks:
        if check in application_checks:
            name = application_checks[check]
            target = configured[name]['checks'][check][0]
            applications.setdefault(name, [])
            if target not in applications[name]:
                applications[name].append(target)
    status = subprocess.run(['git', 'status', '--porcelain'], cwd=source, capture_output=True, text=True)
    if status.returncode != 0:
        parser.error('source must be a Git checkout: ' + status.stderr.strip())
    state = tree_state(source)
    build_root = args.build_root.resolve() if args.build_root else cache / 'build'
    try:
        host_space.preflight(build_root)
    except host_space.SpaceShortage as error:
        print('REFUSED: ' + str(error), file=sys.stderr)
        return 2
    output = build_root / ('gate-' + args.label)
    output = unique_directory(output)
    env = gate_environment(source, cache, build_root)
    # One build tree per resolved checkout: the name keeps it recognizable and
    # the path hash separates checkouts that share a name.
    tree = re.sub(r'[^A-Za-z0-9_.-]', '_', source.name) + '-' + hashlib.sha256(str(source).encode()).hexdigest()[:10]
    cfg, build_dir = cache / 'bin/cfg.sh', build_root / ('gate-central-' + tree)
    application_builds = {name: build_root / ('gate-' + name + '-' + tree) for name in configured}
    python = cache / 'venv/bin/python'
    qemu = cache / 'host/usr/bin/qemu-system-aarch64'
    evidence = output / 'evidence'

    def record_peak():
        """Record what this gate's directories occupy: its output and the build directories of this checkout.
        preflight sizes the next gate's free space from the last ten (tools/host_space.py)."""
        try:
            if not (output / host_space.PEAK_FILE).exists():
                host_space.write_peak(output, [output, *build_root.glob('gate-*-' + tree)])
        except OSError:
            pass
    atexit.register(record_peak)         # a failed gate keeps everything and still records its peak

    fail_fast = args.fail_fast or not (args.full or args.keep_going)
    jobs = 1 if fail_fast else args.jobs
    # Every check, build and configure step runs with this environment; it is
    # recorded in environment.txt. Only the configure steps use cfg.sh.
    results, timings, executed = {}, [], set()
    guard = threading.Lock()
    files_error = None
    try:
        files = source_files(source)
    except OSError as error:
        files, files_error = {}, str(error)
    artifact_key = resume.record['artifact_key'] if resume else hashlib.sha256(repr(state).encode()).hexdigest()[:16]
    checkpoint = dict(version=1, source=str(source), state=state, files=files,
                      selection=selection_identity(args), artifact_key=artifact_key, steps={})
    if resume:
        resume.output = output
        checkpoint['resume_from'] = str(resume.directory)

    def save_step(name, command, script, verdict, artifacts, reuse=None):
        names = step_inputs(name, script)
        log = output / (name + '.log')
        record = dict(command=command, script=script, verdict=verdict, inputs_sha256=identity_hash(input_identity(files, names)),
            command_files=command_files(command, source),
            log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(),
            stable=tree_state(source) == state and files_match(),
            artifacts={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts
                       if p.is_file() and p.name != '.gate-tree-key'})
        if reuse:
            record['reused_from'] = dict(gate=str(resume.directory), step=name,
                                        log_sha256=reuse['log_sha256'], command=reuse['command'])
        with guard:
            checkpoint['steps'][name] = record
            temporary = output / 'checkpoint.json.new'
            temporary.write_text(json.dumps(checkpoint, indent=1) + '\n')
            temporary.replace(output / 'checkpoint.json')

    def files_match():
        try:
            return source_files(source) == files
        except OSError:
            return False

    def step_inputs(name, script):
        if script:
            return sorted({'checks/' + script, *index.get(script, {}).get('inputs', [])})
        if name.startswith(('check-', 'image-', 'indexed-')):
            return sorted(files)
        return sorted(p for p in files if not p.startswith('checks/') and p != 'tools/integration-gate.py')

    class Stop(Exception):
        """Fail-fast mode ends the gate at the first failed step."""

    def note(message):
        with guard:
            print(message, flush=True)
            with (output / 'summary.txt').open('a') as stream:
                stream.write(message + '\n')

    def prose_only(script, log):
        """Whether a failed check failed only on documentation prose."""
        if args.strict_docs or script not in prose:
            return False
        allowed = prose[script]
        if allowed is True:
            return True
        failed = set(re.findall(r'^(?:FAIL|ERROR): (\w+)', log, re.M))
        return bool(failed) and failed <= set(allowed)

    def run(name, command, script=None, timeout=None, artifacts=(), allow_reuse=True):
        """Run one step; the result is recorded as PASS, WARN or FAIL."""
        command = [str(value) for value in command]
        artifacts = list(artifacts)
        with guard:
            with (output / 'commands.txt').open('a') as stream:
                stream.write(name + ': ' + shlex.join(command) + '\n')
        eligible = not resume or not resume.refresh_native or (
            name.startswith('indexed-') and script and index.get(script, {}).get('pure_host'))
        reused = resume.reusable(name, command, script, step_inputs(name, script), artifacts) if resume and allow_reuse and eligible else None
        if reused:
            shutil.copy2(resume.directory / (name + '.log'), output / (name + '.log'))
            results[name] = 'PASS'
            save_step(name, command, script, 'PASS', artifacts, reused)
            note('REUSE PASS ' + name + ' from ' + str(resume.directory))
            return True
        if any(str(path) in retained_outputs for path in artifacts):
            note('FAIL ' + name + ': retained preparation proof changed; original output is preserved')
            raise Stop
        executed.add(name)
        started, detail = time.monotonic(), ''
        with (output / (name + '.log')).open('w') as log:
            try:
                result = subprocess.run(command, cwd=source, env=env, stdin=subprocess.DEVNULL, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=timeout)
                ok = result.returncode == 0
            except subprocess.TimeoutExpired:
                log.write('\nGate deadline of %d seconds expired.\n' % timeout)
                ok, detail = False, ' (deadline of %d seconds expired)' % timeout
            except OSError as error:
                log.write(str(error) + '\n')
                ok = False
        elapsed = time.monotonic() - started
        verdict = 'PASS' if ok else 'FAIL'
        if not ok and prose_only(script, (output / (name + '.log')).read_text(errors='replace')):
            verdict = 'WARN'
            detail = ' (documentation or status-record prose; see ' + name + '.log)'
        results[name] = verdict
        save_step(name, command, script, verdict, artifacts)
        with guard:
            timings.append((name, elapsed))
        note(verdict + ' ' + name + detail)
        if verdict == 'FAIL' and fail_fast:
            note('Stopped at first failure; diagnose it before running only the affected steps with a new label.')
            raise Stop
        return verdict != 'FAIL'

    def skip(name, reason):
        results[name] = 'SKIP'
        note('SKIP ' + name + ': ' + reason)

    note('reason: ' + args.reason)
    note('source: ' + str(source))
    note('selection: ' + shlex.join(sys.argv[1:] if argv is None else argv))
    if affected_plan is not None:
        (output / 'affected.json').write_text(json.dumps(affected_plan, indent=2) + '\n')
    if hook_scope is not None:
        (output / 'hook-scope.json').write_text(json.dumps(hook_scope, indent=2) + '\n')
        note('hook fixture: ' + ('selected' if hook_scope['selected'] else 'skipped') + '; ' + hook_scope['basis'])
    note(f'indexed checks: {len(indexed)} runnable, {len(pending)} pending')
    for check in pending:
        note('PENDING checks/' + check['check'] + ': ' + ', '.join(check['needs']))
    if inputs is not None:
        note('affected checks that need more than the host: %d supplied by the gate, %d need a board or USB (reported, not refused), '
             '%d waived by their own GATE_WAIVED, %d unsupplied' % (len(classified['supplied']), len(classified['board']),
                                                                        len(classified['waived']), len(classified['unsupplied'])))
        for name in classified['board']:
            note('BOARD checks/' + name + ': needs a board or USB access; not run by the gate')
        if classified['waived']:
            (output / 'waived.txt').write_text(''.join('%s: %s\n' % (n, inputs['waived'][n]) for n in classified['waived']))
            note('WAIVED %d affected checks (reasons in waived.txt)' % len(classified['waived']))
        if classified['unsupplied']:
            for name, needs in classified['unsupplied']:
                note('FAIL unsupplied affected check checks/%s: the gate cannot supply %s; declare GATE_RUNS, GATE_BOARD or GATE_WAIVED in it'
                     % (name, ', '.join(needs) or 'its inputs'))
            note('INCOMPLETE affected coverage: %d affected checks have inputs the gate does not supply' % len(classified['unsupplied']))
            return 2
    revision, changes, diff, untracked = state
    note('revision: ' + (revision or 'unavailable'))
    note('diff-sha256: ' + (diff or 'unavailable'))
    note('untracked-sha256: ' + ('%s (%d files)' % untracked if untracked else 'unavailable'))
    note('status: ' + ('unavailable' if changes is None else 'clean' if not changes else
                       str(len(changes.splitlines())) + ' changed paths'))
    if changes:
        (output / 'status.txt').write_text(changes)
    application_states = {}
    for name, app in configured.items():
        app_state = tree_state(app['root'])
        application_states[name] = app_state
        if None in app_state:
            note('FAIL external ' + name + ' state is unreadable')
            return 1
        note(name + '-source: ' + str(app['root']))
        note(name + '-revision: ' + app_state[0])
        note(name + '-diff-sha256: ' + app_state[2])
        note(name + '-untracked-sha256: %s (%d files)' % app_state[3])
        if app_state[1]:
            (output / (name + '-status.txt')).write_text(app_state[1])

    def unchanged(when):
        """The recorded state must be complete and still describe the tree."""
        if files_error or None in state or tree_state(source) != state or not files_match():
            note('FAIL the source revision, status, diff or untracked files changed or could not be read '
                 + when + '; rerun with a new label')
            return False
        for name, app in configured.items():
            if tree_state(app['root']) != application_states[name]:
                note('FAIL external ' + name + ' changed or could not be read ' + when)
                return False
        return True
    lock_path = build_root / ('gate-' + tree + '.lock')
    lock = lock_path.open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        note('FAIL another gate holds ' + str(lock_path) + ' for this checkout')
        return 1
    # The state recorded above must describe the tree that this gate builds
    # while it holds the lock, both before the first step and after the last.
    if not unchanged('between the status check and the lock'):
        return 1
    # An image is built once per tree state: the key names the revision, the tracked diff and the untracked files.
    tree_key = artifact_key
    recipes, bundles, sessions, fixtures = {}, {}, [], {}
    retained_outputs = set()
    for check in classified['supplied']:
        for run_ in inputs['checks'][check]['runs']:
            for kind, image in placeholders(run_['args']):
                if kind == 'fixture':
                    fixtures[image] = inputs['fixtures'][image]
                elif kind == 'bundle':
                    bundles[image] = inputs['bundles'][image]
                elif image in inputs['images']:
                    recipes[image] = inputs['images'][image]
                if kind == 'session' and image not in sessions:
                    sessions.append(image)

    def mark_built(directory):
        directory.mkdir(parents=True, exist_ok=True)
        marker = directory / '.gate-tree-key'
        if not marker.is_file() or marker.read_text() != tree_key:
            marker.write_text(tree_key)

    def built_for_tree(directory, artifact):
        marker = Path(directory) / '.gate-tree-key'
        return marker.is_file() and marker.read_text() == tree_key and Path(artifact).is_file()

    def image_directory(image):
        return build_root / (('gate-os-' if image in VARIANTS else 'gate-img-') + image + '-' + tree)

    def image_file(image):
        directory = image_directory(image)
        return directory / ('sunstone-a733-os.elf' if image in VARIANTS else inputs['images'][image]['elf'])

    def bundle_file(name):
        if resume:
            step = resume.record['steps'].get('bundle-' + name + '-build', {})
            command = step.get('command', [])
            if not resume.refresh_native and '--evidence' in command:
                old = Path(command[command.index('--evidence') + 1]) / 'build/probe.elf'
                bundle_args = bundles[name]
                builder, builder_args = ((bundle_args['builder'], bundle_args.get('args', [])) if isinstance(bundle_args, dict)
                                         else ('apps/board-bundles-power/build-opp.py', bundle_args))
                wanted = [str(python), builder, *builder_args, '--platform-build', str(build_dir), '--evidence', str(old.parents[1])]
                step_name = 'bundle-' + name + '-build'
                if 'central-configure' not in executed and resume.reusable(step_name, wanted, None, step_inputs(step_name, None), [old]):
                    retained_outputs.add(str(old))
                    return old
            suffix = hashlib.sha256(str(output).encode()).hexdigest()[:10]
            return build_root / ('gate-bundle-' + name + '-' + tree) / ('resume-' + suffix) / 'build/probe.elf'
        return build_root / ('gate-bundle-' + name + '-' + tree) / ('out-' + tree_key) / 'build/probe.elf'

    def session_file(image):
        if resume and not resume.refresh_native:
            step = resume.record['steps'].get('session-' + image, {})
            command = step.get('command', [])
            old = Path(command[-1]) if command else resume.directory / 'evidence/sessions' / (image + '.bin')
            wanted = [str(python), 'tools/gate_session.py', str(image_file(image)), str(old)]
            step_name = 'session-' + image
            if 'img-' + image + '-build' not in executed and resume.reusable(step_name, wanted, None, step_inputs(step_name, None), [old]):
                retained_outputs.add(str(old))
                return old
        return evidence / 'sessions' / (image + '.bin')

    def build_artifacts(directory, artifact):
        return [Path(artifact), Path(directory) / '.gate-tree-key', Path(directory) / 'CMakeCache.txt',
                Path(directory) / 'compile_commands.json']

    def fixture_directory(name):
        return cache / inputs['fixtures'][name]['directory']

    def resolve(argument):
        def one(match):
            kind, name = match.group(1), match.group(2)
            return str({'elf': image_file, 'build': image_directory, 'bundle': bundle_file, 'session': session_file,
                        'fixture': fixture_directory}[kind](name))
        return re.sub(r'\{(elf|build|session|bundle|fixture):([A-Za-z0-9_-]+)\}', one, argument).replace('{cache}', str(cache)).replace('{source}', str(source))

    def discover(name, directory):
        """The build targets that CMake lists, or None after a failure."""
        if not run(name + '-targets', ['cmake', '--build', directory, '--target', 'help']):
            return None
        excluded = {'all', 'clean', 'depend', 'edit_cache', 'rebuild_cache', 'install', 'list_install_components'}
        discovered = re.findall(r'^\.\.\. ([A-Za-z0-9_][A-Za-z0-9_.+-]*)\s*$',
                                (output / (name + '-targets.log')).read_text(), re.M)
        if not discovered:
            results[name + '-targets'] = 'FAIL'
            note('FAIL ' + name + '-targets: unsupported target listing; select targets explicitly')
            if fail_fast:
                raise Stop
            return None
        return [t for t in discovered if t not in excluded and not t.endswith(('.o', '.i', '.s', '.obj'))]

    def run_steps():
        started = time.monotonic()
        core_targets, targets_to_build = [], list(targets)
        # Image generators use the same dependency environment as the checks.
        # CMake's PATH search can otherwise select a Python without elftools.
        core_options = ['-DPython3_EXECUTABLE=' + str(python), '-DSUNSTONE_EXTERNAL_APPLICATIONS=',
                        *['-D' + key + '=' for key in selectors]]
        if targets_to_build or args.full or bundles:
            # Override any external selections supplied by the host configuration. The bundle builders read the compile
            # flags of this build (tools/returning_build.py).
            if run('central-configure', [cfg, source, build_dir, '-DSUNSTONE_APPLICATION=', *core_options,
                                         *(['-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'] if bundles else [])],
                   artifacts=[build_dir / 'CMakeCache.txt']):
                if args.full:
                    discovered = discover('central', build_dir)
                    if discovered is None:
                        skip('central-build', 'central target discovery failed')
                        targets_to_build = None
                    else:
                        core_targets = discovered
                        targets_to_build = list(dict.fromkeys(targets_to_build + core_targets))
                if targets_to_build:
                    # One invocation lets CMake reuse shared dependencies across selected targets.
                    run('central-build', ['cmake', '--build', build_dir, '--target', *targets_to_build, '-j', '12'])
            else:
                skip('central-build', 'central-configure did not pass')
        for variant in variants:
            directory = build_root / ('gate-os-' + variant + '-' + tree)
            options = [value.replace('{cache}', str(cache)) for value in VARIANTS[variant]]
            if not resume and built_for_tree(directory, directory / 'sunstone-a733-os.elf'):
                results['os-' + variant + '-configure'] = results['os-' + variant + '-build'] = 'PASS'
                note('CACHED os-' + variant + ': built for this tree state (' + tree_key + ')')
                continue
            if run('os-' + variant + '-configure', [cfg, source, directory,
                   *core_options, '-DSUNSTONE_APPLICATION=', '-DSUNSTONE_A733_OS=ON',
                   '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', *options], artifacts=[directory / 'CMakeCache.txt']):
                if run('os-' + variant + '-build', ['cmake', '--build', directory, '--target', 'sunstone-a733-os', '-j', '12'],
                       artifacts=build_artifacts(directory, directory / 'sunstone-a733-os.elf')):
                    mark_built(directory)
            else:
                skip('os-' + variant + '-build', 'os-' + variant + '-configure did not pass')
        for image, recipe in recipes.items():
            directory = image_directory(image)
            if not resume and built_for_tree(directory, image_file(image)):
                results['img-' + image + '-configure'] = results['img-' + image + '-build'] = 'PASS'
                note('CACHED img-' + image + ': built for this tree state (' + tree_key + ')')
                continue
            flags = [value.replace('{source}', str(source)).replace('{cache}', str(cache)) for value in recipe['configure']]
            if run('img-' + image + '-configure', [cfg, source, directory, *core_options, '-DSUNSTONE_A733_OS=ON',
                                                   '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', *flags], artifacts=[directory / 'CMakeCache.txt']):
                if run('img-' + image + '-build', ['cmake', '--build', directory, '--target',
                                                  *([recipe['target']] if isinstance(recipe['target'], str) else recipe['target']), '-j', '12'],
                       artifacts=build_artifacts(directory, image_file(image))):
                    mark_built(directory)
            else:
                skip('img-' + image + '-build', 'img-' + image + '-configure did not pass')
        for name, bundle_args in bundles.items():
            output_dir = bundle_file(name).parents[1]
            if not resume and bundle_file(name).is_file():
                results['bundle-' + name + '-build'] = 'PASS'
                note('CACHED bundle-' + name + ': built for this tree state (' + tree_key + ')')
            elif results.get('central-configure') != 'PASS':
                skip('bundle-' + name + '-build', 'central-configure did not pass')
            else:
                # A bundle is a list of build-opp.py arguments, or {"builder": script, "args": [...]} for another builder
                # that takes --platform-build and --evidence and writes build/probe.elf (apps/storage-read-returning/build.py).
                builder, builder_args = ((bundle_args['builder'], bundle_args.get('args', [])) if isinstance(bundle_args, dict)
                                         else ('apps/board-bundles-power/build-opp.py', bundle_args))
                run('bundle-' + name + '-build', [python, builder, *builder_args,
                                                   '--platform-build', build_dir, '--evidence', output_dir], artifacts=[bundle_file(name)])
        for name, recipe in fixtures.items():
            # Idempotent: a fixture that is current is left alone, one that a cache cleanup deleted is rebuilt.
            directory = fixture_directory(name)
            artifacts = sorted(p for p in directory.rglob('*') if p.is_file()) if directory.is_dir() else []
            if not artifacts:
                artifacts = [directory]  # Missing fixture data cannot reuse a PASS.
            run('fixture-' + name, [python, *[value.replace('{directory}', str(directory)) for value in recipe['build']]], artifacts=artifacts)
        for image in sessions:
            if results.get('img-' + image + '-build') == 'PASS':
                run('session-' + image, [python, 'tools/gate_session.py', image_file(image), session_file(image)],
                    artifacts=[session_file(image)])
            else:
                skip('session-' + image, 'img-' + image + '-build did not pass')
        for name in (configured if args.full else applications):
            directory, selected = application_builds[name], applications.get(name, [])
            selected = list(dict.fromkeys(['sunstone-a733-os', *selected]))
            if not run(name + '-configure', [cfg, source, directory, *core_options,
                       '-DSUNSTONE_EXTERNAL_APPLICATIONS=' + str(configured[name]['root']),
                       '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=' + name]):
                skip(name + '-build', name + '-configure did not pass')
                continue
            if args.full:
                discovered = discover(name, directory)
                if discovered is None:
                    skip(name + '-build', name + ' target discovery failed')
                    continue
                # The application build repeats the core targets; build only its own.
                selected = list(dict.fromkeys(selected + [t for t in discovered if t not in core_targets]))
            if selected:
                run(name + '-build', ['cmake', '--build', directory, '--target', *selected, '-j', '12'])
        commands = {
            'kernel': ['tools/check-kernel.py', build_dir / 'kernel-check.elf', '--qemu', qemu, '--evidence', evidence / 'kernel'],
            'soak8': ['tools/check-smp.py', build_dir / 'smp8-soak-model.elf', '--case', 'soak8', '--trials', str(args.trials), '--qemu', qemu, '--evidence', evidence / 'soak8'],
            'shared-helpers': ['checks/shared-helpers.py', '--output', evidence / 'shared-helpers'],
            'progress': ['checks/progress.py', '--output', evidence / 'progress'],
            'check-index': ['checks/check-index.py', '--verify'],
            'pmic-tree': ['checks/pmic-tree.py'],
            'power-domains-tree': ['checks/power-domains-tree.py'],
        }
        for check, name in application_checks.items():
            places = {'build': application_builds[name], 'cache': cache, 'qemu': qemu, 'evidence': evidence,
                      'application': configured[name]['root']}
            commands[check] = [value.format(**places) for value in configured[name]['checks'][check][1]]
        # Each task names the build steps it needs. QEMU checks run one at a time.
        tasks = []
        for check in checks:
            command = commands[check]
            needs = ['central-build'] if check in DEPENDENCIES else (
                [application_checks[check] + '-build'] if check in application_checks else [])
            script = Path(command[0]).name if Path(command[0]).parent == Path('checks') else None
            tasks.append({'name': 'check-' + check, 'command': [python, *command], 'script': script, 'needs': needs,
                          'serial': check in DEPENDENCIES or check in application_checks or script in serial})
        for check in indexed:
            command = [python, 'checks/' + check['check']]
            option = output_option(check)
            if option:
                command += ([option] if option.startswith('-') else []) + [evidence / 'indexed' / Path(check['check']).stem]
            tasks.append({'name': 'indexed-' + Path(check['check']).stem, 'command': command, 'script': check['check'],
                          'needs': [], 'serial': check['check'] in serial})

        for check in classified['supplied']:
            for number, run_ in enumerate(inputs['checks'][check]['runs']):
                stem = Path(check).stem + ('' if len(inputs['checks'][check]['runs']) == 1 else '-%d' % number)
                directory = evidence / 'images' / stem
                command = [python, 'checks/' + check, *[resolve(a).replace('{output}', str(directory)) for a in run_['args']]]
                needs = [('fixture-' + image) if kind == 'fixture' else ('bundle-%s-build' % image) if kind == 'bundle' else
                         ('session-' + image) if kind == 'session' else
                         ('os-' + image if image in VARIANTS else 'img-' + image) + '-build'
                         for kind, image in placeholders(run_['args'])]
                tasks.append({'name': 'image-' + stem, 'command': command, 'script': check,
                              'needs': list(dict.fromkeys(needs)), 'serial': check in serial})

        def start(task):
            missing = [need for need in task['needs'] if results.get(need) != 'PASS']
            if missing:
                skip(task['name'], ', '.join(missing) + ' did not pass')
            else:
                run(task['name'], task['command'], task['script'], args.check_timeout,
                    allow_reuse=not any(need in executed for need in task['needs']))

        def wait_for_memory(name):
            deadline, announced = time.monotonic() + args.memory_wait, False
            while True:
                free = available_gib()
                if free is None or free >= args.min_available_gb:
                    return True
                if not announced:
                    note('WAIT %s: MemAvailable %.1f GiB is below %g GiB' % (name, free, args.min_available_gb))
                    announced = True
                if time.monotonic() >= deadline:
                    return False
                time.sleep(min(5, max(0.05, deadline - time.monotonic())))

        def run_group(batch, workers):
            slots = threading.BoundedSemaphore(workers)

            def work(task):
                try:
                    start(task)
                except Exception as error:
                    results[task['name']] = 'FAIL'
                    note('FAIL ' + task['name'] + ': ' + type(error).__name__ + ': ' + str(error))
                finally:
                    slots.release()
            with ThreadPoolExecutor(workers) as pool:
                for task in batch:
                    slots.acquire()
                    if wait_for_memory(task['name']):
                        pool.submit(work, task)
                    else:
                        slots.release()
                        results[task['name']] = 'FAIL'
                        note('FAIL %s: not started; MemAvailable stayed below %g GiB for %d seconds'
                             % (task['name'], args.min_available_gb, args.memory_wait))
        if fail_fast:
            for task in tasks:
                start(task)
        else:
            run_group([t for t in tasks if not t['serial']], jobs)
            run_group([t for t in tasks if t['serial']], 1)
        return time.monotonic() - started

    environment_text = (
        ''.join(key + '=' + env[key] + '\n' for key in ('PATH', 'SUNSTONEOS_DIR', 'PYTHONDONTWRITEBYTECODE', 'TMPDIR') if key in env) +
        'cwd=' + str(source) + '\nstdin=/dev/null\nconfigure steps run ' + str(cfg) + '; checks do not source it\n')
    (output / 'environment.txt').write_text(environment_text)
    if resume:
        try:
            if environment_text != resume.environment or files != resume.current:
                raise ValueError('resume environment or source changed during admission')
            resume.admit(index, [build_dir, *application_builds.values(),
                                *[image_directory(n) for n in [*variants, *recipes]]])
            if resume.refresh_native:
                artifact_key = tree_key = hashlib.sha256(repr(state).encode()).hexdigest()[:16]
                checkpoint['artifact_key'] = artifact_key
                checkpoint['native_refreshed'] = True
        except (OSError, ValueError, KeyError, TypeError) as error:
            note('REFUSED resume: ' + str(error))
            return 2
        note('resume: ' + str(resume.directory) + '; only verified PASS steps may be reused')
        if resume.refresh_native:
            note('resume: native/configuration changes refresh all preparation and image/core checks')
    note('mode: ' + ('fail-fast' if fail_fast else 'keep-going, %d parallel checks, memory guard %g GiB'
                     % (jobs, args.min_available_gb)) + (', strict documentation checks' if args.strict_docs else ''))
    try:
        elapsed = run_steps()
    except Stop:
        return 1
    failed = sorted(name for name, verdict in results.items() if verdict == 'FAIL')
    skipped = sorted(name for name, verdict in results.items() if verdict == 'SKIP')
    warned = sorted(name for name, verdict in results.items() if verdict == 'WARN')
    note('steps: %d passed, %d warned, %d failed, %d skipped; %.0f s' % (
        sum(v == 'PASS' for v in results.values()), len(warned), len(failed), len(skipped), elapsed))
    (output / 'timing.txt').write_text(''.join('%8.1f %s\n' % (seconds, name) for name, seconds in
                                              sorted(timings, key=lambda item: -item[1])))
    if not unchanged('during the selected steps'):
        return 1
    if failed or skipped:
        note('FAIL selected integration checks: ' + ', '.join(failed + skipped))
        return 1
    if affected_plan and affected_plan.get('revisions') and not changes:
        # tools/land.py reuses this pass for a landing: the tree it ran on and the checks it selected with their inputs.
        tree_hash = subprocess.check_output(['git', 'rev-parse', 'HEAD^{tree}'], cwd=source, text=True).strip()
        selected = [c['check'] for c in affected_plan['checks']]
        (output / 'gate-record.json').write_text(json.dumps({'tree': tree_hash, 'base': affected_plan['revisions'][0], 'checks': {
            name: index[name].get('inputs', []) for name in selected if name in index}}, indent=1) + '\n')
    note('PASS selected integration checks; unselected behavior is not revalidated')
    if resume:
        note('retention: resumed gate preserves the original evidence and shared preparation artifacts')
        record_peak()
        return 0
    # A passing gate removes its own bulky outputs and those of earlier gates of the same landing (tools/clean-generated.py).
    record_peak()
    try:
        spec = importlib.util.spec_from_file_location('clean_generated', Path(__file__).with_name('clean-generated.py'))
        cleaner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cleaner)
        note('retention: freed %.1f MiB of bulky outputs' % (cleaner.slim_after_pass(output, args.label) / 2**20))
    except Exception as error:
        note('retention skipped: %s' % error)
    return 0

if __name__ == '__main__':
    sys.exit(main())
