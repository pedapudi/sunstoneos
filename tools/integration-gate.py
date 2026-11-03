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
"""
import argparse
import fcntl
import hashlib
import importlib.util
import json
from pathlib import Path
from host_evidence import unique_directory
import host_priority
import gate_images
import host_space
import os
import re
import shlex
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

# The NPU catalogue map of the npu variant, relative to the --cache directory.
NPU_CATALOGUE = 'build/npu-board/session-artifacts.json'
VARIANTS = {
    'sup': [],
    'res': ['-DSUNSTONE_A733_OS_RESIDENT=ON'],
    'resaud': ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DSUNSTONE_A733_OS_AUDIO=ON'],
    'resradio': ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DSUNSTONE_A733_OS_RADIO=ON'],
    'resradioaud': ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DSUNSTONE_A733_OS_RADIO=ON', '-DSUNSTONE_A733_OS_AUDIO=ON'],
    'headset': ['-DSUNSTONE_A733_OS_HEADSET=ON'],
    'resheadset': ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DSUNSTONE_A733_OS_HEADSET=ON'],
    'radio-headset': ['-DSUNSTONE_A733_OS_RADIO=ON', '-DSUNSTONE_A733_OS_HEADSET=ON'],
    'resradio-headset': ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DSUNSTONE_A733_OS_RADIO=ON',
                         '-DSUNSTONE_A733_OS_HEADSET=ON'],
    # The supervised NPU profile. The OS build verifies its host inputs against
    # tools/npu-session-models.json. {cache} is the --cache directory.
    'npu': ['-DSUNSTONE_A733_OS_NPU=ON', '-DSUNSTONE_OS_NPU_CATALOGUE={cache}/' + NPU_CATALOGUE],
    'npu-queue': ['-DSUNSTONE_A733_OS_NPU=ON', '-DSUNSTONE_A733_OS_NPU_QUEUE=ON',
                  '-DSUNSTONE_OS_NPU_CLIENT_MODEL=0',
                  '-DSUNSTONE_OS_NPU_QUEUE_MODELS=0,1',
                  '-DSUNSTONE_OS_NPU_CATALOGUE={cache}/' + NPU_CATALOGUE],
    # The performance-mode service (docs/designs/performance-modes.md) on the
    # supervised A76 CPU clock build, with the high boot mode; and with the
    # NPU profile and the low boot mode.
    # The supervised A76 CPU clock build without the performance-mode service (checks/os-cpu-clock-native.py).
    'cpu-clock': ['-DA733_A76_ADMITTED=ON', '-DSUNSTONE_A733_OS_CPU_CLOCK=ON'],
    'perf': ['-DA733_A76_ADMITTED=ON', '-DSUNSTONE_A733_OS_CPU_CLOCK=ON', '-DSUNSTONE_A733_OS_PERF=ON'],
    'perf-npu-low': ['-DA733_A76_ADMITTED=ON', '-DSUNSTONE_A733_OS_CPU_CLOCK=ON', '-DSUNSTONE_A733_OS_PERF=ON',
                     '-DSUNSTONE_A733_OS_PERF_BOOT=low', '-DSUNSTONE_A733_OS_NPU=ON',
                     '-DSUNSTONE_OS_NPU_CATALOGUE={cache}/' + NPU_CATALOGUE],
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
    gate = {name: entry['gate'] for name, entry in index.items()}
    return {'images': gate_images.IMAGES, 'bundles': gate_images.BUNDLES,
            'checks': {n: {'runs': [{'args': a} for a in g['runs']]} for n, g in gate.items() if 'runs' in g},
            'board': [n for n, g in gate.items() if 'board' in g],
            'waived': {n: g['waived'] for n, g in gate.items() if 'waived' in g}}


def classify_affected(entries, index, inputs):
    """Sort the affected checks that are not runnable into the groups the gate reports.

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
    """The (kind, name) pairs a registered command asks for: {elf:IMAGE}, {build:IMAGE}, {session:IMAGE} and {bundle:NAME}."""
    return sorted({(m.group(1), m.group(2)) for part in command
                   for m in re.finditer(r'\{(elf|build|session|bundle):([A-Za-z0-9_-]+)\}', part)})


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
    # One gate at a time on the host, whoever starts it: a direct call waits behind a landing's gate instead of running beside
    # it. The full suite takes its own lock so that it blocks no landing. The lock is held until the process exits.
    gate_lock = take_gate_lock(cache, 'gate-full.lock' if args.full else 'gate.lock')
    targets = list(dict.fromkeys(args.target))
    variants = list(VARIANTS) if args.full else list(dict.fromkeys(args.variant))
    indexed, pending, affected_plan, hook_scope = [], [], None, None
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
                # The checks this change affects that the pure-host run does not cover: the full list stays informational,
                # these must be supplied, board-bound or waived.
                affected_pending = {c['check']: c for c in scope['checks'] if c['status'] == 'pending'}
                if dirty_paths:
                    affected_pending.update({c['check']: c for c in local['checks'] if c['status'] == 'pending'})
                affected_entries = list(affected_pending.values())
            else:
                index = selector.indexer.generate()
                affected_plan = selector.plan(args.affected, args.changed, index=index)
                indexed = [check for check in affected_plan['checks'] if check['status'] == 'runnable']
                pending = [check for check in affected_plan['checks'] if check['status'] == 'pending']
                affected_entries = pending
                variants = list(dict.fromkeys([*variants, affected_plan['baseline']]))
            inputs = load_gate_inputs(index)
            classified = classify_affected(affected_entries, index, inputs)
        except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
            parser.error('cannot select indexed checks: ' + str(error))
        for name in classified['supplied']:
            for run_ in inputs['checks'][name]['runs']:
                for kind, image in placeholders(run_['args']):
                    if kind == 'bundle':
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
    if any('npu' in v for v in variants) and not (cache / NPU_CATALOGUE).is_file():
        parser.error('the npu variant needs the NPU catalogue map ' + str(cache / NPU_CATALOGUE))
    selectors = compatibility_variables(source)
    paths = list(args.application)
    paths += [p for p in os.environ.get('SUNSTONE_EXTERNAL_APPLICATIONS', '').split(';') if p]
    paths += [os.environ[key] for key in selectors if os.environ.get(key)]
    try:
        configured = external_applications(source, paths)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error('invalid external application: ' + str(error))
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

    fail_fast = args.fail_fast or not (args.full or args.keep_going)
    jobs = 1 if fail_fast else args.jobs
    # Every check, build and configure step runs with this environment; it is
    # recorded in environment.txt. Only the configure steps use cfg.sh.
    results, timings = {}, []
    guard = threading.Lock()

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

    def run(name, command, script=None, timeout=None):
        """Run one step; the result is recorded as PASS, WARN or FAIL."""
        command = [str(value) for value in command]
        with guard:
            with (output / 'commands.txt').open('a') as stream:
                stream.write(name + ': ' + shlex.join(command) + '\n')
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
    if any('npu' in v for v in variants):
        note('npu-catalogue-sha256: ' + hashlib.sha256((cache / NPU_CATALOGUE).read_bytes()).hexdigest())

    def unchanged(when):
        """The recorded state must be complete and still describe the tree."""
        if None in state or tree_state(source) != state:
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
    tree_key = hashlib.sha256(repr(state).encode()).hexdigest()[:16]
    recipes, bundles, sessions = {}, {}, []
    for check in classified['supplied']:
        for run_ in inputs['checks'][check]['runs']:
            for kind, image in placeholders(run_['args']):
                if kind == 'bundle':
                    bundles[image] = inputs['bundles'][image]
                elif image in inputs['images']:
                    recipes[image] = inputs['images'][image]
                if kind == 'session' and image not in sessions:
                    sessions.append(image)

    def mark_built(directory):
        directory.mkdir(parents=True, exist_ok=True)
        (directory / '.gate-tree-key').write_text(tree_key)

    def built_for_tree(directory, artifact):
        marker = Path(directory) / '.gate-tree-key'
        return marker.is_file() and marker.read_text() == tree_key and Path(artifact).is_file()

    def image_directory(image):
        return build_root / (('gate-os-' if image in VARIANTS else 'gate-img-') + image + '-' + tree)

    def image_file(image):
        directory = image_directory(image)
        return directory / ('sunstone-a733-os.elf' if image in VARIANTS else inputs['images'][image]['elf'])

    def bundle_file(name):
        return build_root / ('gate-bundle-' + name + '-' + tree) / ('out-' + tree_key) / 'build/probe.elf'

    def session_file(image):
        return evidence / 'sessions' / (image + '.bin')

    def resolve(argument):
        def one(match):
            kind, name = match.group(1), match.group(2)
            return str({'elf': image_file, 'build': image_directory, 'bundle': bundle_file, 'session': session_file}[kind](name))
        return re.sub(r'\{(elf|build|session|bundle):([A-Za-z0-9_-]+)\}', one, argument).replace('{cache}', str(cache)).replace('{source}', str(source))

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
        core_options = ['-DSUNSTONE_EXTERNAL_APPLICATIONS=', *['-D' + key + '=' for key in selectors]]
        if targets_to_build or args.full or bundles:
            # Override any external selections supplied by the host configuration. The bundle builders read the compile
            # flags of this build (tools/returning_build.py).
            if run('central-configure', [cfg, source, build_dir, '-DSUNSTONE_APPLICATION=', *core_options,
                                         *(['-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'] if bundles else [])]):
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
            if built_for_tree(directory, directory / 'sunstone-a733-os.elf'):
                results['os-' + variant + '-configure'] = results['os-' + variant + '-build'] = 'PASS'
                note('CACHED os-' + variant + ': built for this tree state (' + tree_key + ')')
                continue
            if run('os-' + variant + '-configure', [cfg, source, directory,
                   *core_options, '-DSUNSTONE_APPLICATION=', '-DSUNSTONE_A733_OS=ON',
                   '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', *options]):    # the image checks read each build's compile flags
                if run('os-' + variant + '-build', ['cmake', '--build', directory, '--target', 'sunstone-a733-os', '-j', '12']):
                    mark_built(directory)
            else:
                skip('os-' + variant + '-build', 'os-' + variant + '-configure did not pass')
        for image, recipe in recipes.items():
            directory = image_directory(image)
            if built_for_tree(directory, image_file(image)):
                results['img-' + image + '-configure'] = results['img-' + image + '-build'] = 'PASS'
                note('CACHED img-' + image + ': built for this tree state (' + tree_key + ')')
                continue
            flags = [value.replace('{source}', str(source)).replace('{cache}', str(cache)) for value in recipe['configure']]
            if run('img-' + image + '-configure', [cfg, source, directory, *core_options, '-DSUNSTONE_A733_OS=ON',
                                                   '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', *flags]):
                if run('img-' + image + '-build', ['cmake', '--build', directory, '--target', recipe['target'], '-j', '12']):
                    mark_built(directory)
            else:
                skip('img-' + image + '-build', 'img-' + image + '-configure did not pass')
        for name, bundle_args in bundles.items():
            output_dir = bundle_file(name).parents[1]
            if bundle_file(name).is_file():
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
                                                   '--platform-build', build_dir, '--evidence', output_dir])
        for image in sessions:
            if results.get('img-' + image + '-build') == 'PASS':
                run('session-' + image, [python, 'tools/gate_session.py', image_file(image), session_file(image)])
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
                needs = [('bundle-%s-build' % image) if kind == 'bundle' else ('session-' + image) if kind == 'session' else
                         ('os-' + image if image in VARIANTS else 'img-' + image) + '-build'
                         for kind, image in placeholders(run_['args'])]
                tasks.append({'name': 'image-' + stem, 'command': command, 'script': check,
                              'needs': list(dict.fromkeys(needs)), 'serial': check in serial})

        def start(task):
            missing = [need for need in task['needs'] if results.get(need) != 'PASS']
            if missing:
                skip(task['name'], ', '.join(missing) + ' did not pass')
            else:
                run(task['name'], task['command'], task['script'], args.check_timeout)

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

    (output / 'environment.txt').write_text(
        ''.join(key + '=' + env[key] + '\n' for key in ('PATH', 'SUNSTONEOS_DIR', 'PYTHONDONTWRITEBYTECODE', 'TMPDIR') if key in env) +
        'cwd=' + str(source) + '\nstdin=/dev/null\nconfigure steps run ' + str(cfg) + '; checks do not source it\n')
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
    note('PASS selected integration checks; unselected behavior is not revalidated')
    return 0

if __name__ == '__main__':
    sys.exit(main())
