#!/usr/bin/env python3
"""Run explicitly selected integration builds and checks, once each.

Host configuration stays in CACHE/bin/cfg.sh. This tool never launches a board.
Build directories belong to the resolved source checkout, so gates on different
checkouts never share a CMake cache; a lock refuses a second concurrent gate on
the same checkout. The recorded source identity includes uncommitted changes:
the revision, the tracked diff and every untracked file that Git does not
ignore. The gate fails if that state cannot be read completely, or if it
changes before the gate holds the lock or by the end of the last step.
Use checks/check-index.py --changed PATH... to help choose focused checks; its
textual coverage is advisory. Component checks outside this gate run directly.
The full gate conditions hook fixtures on --affected or --changed scope. Its
default combines HEAD^..HEAD (all tracked files at a root commit) with staged,
unstaged and nonignored untracked changes in the checkout.
"""
import argparse
import fcntl
import hashlib
import importlib.util
import json
from pathlib import Path
from host_evidence import unique_directory
import os
import re
import shlex
import subprocess
import sys

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
                  '-DSUNSTONE_OS_NPU_QUEUE_MODELS=0,1',
                  '-DSUNSTONE_OS_NPU_CATALOGUE={cache}/' + NPU_CATALOGUE],
    # The performance-mode service (docs/designs/performance-modes.md) on the
    # supervised A76 CPU clock build, with the high boot mode; and with the
    # NPU profile and the low boot mode.
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


def main(argv=None):
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
    parser.add_argument('--allow-dirty', action='store_true', help='compatibility option; local changes are always recorded')
    args = parser.parse_args(argv)
    if not args.full and not (args.target or args.variant or args.check or args.affected or args.changed):
        parser.error('select --target, --variant or --check; --full is only for justified broad validation')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', args.label):
        parser.error('label must be a single filename beginning with a letter or digit')
    if args.trials < 1 or (args.trials != 1 and not args.full and 'soak8' not in args.check):
        parser.error('--trials must be positive and applies only to soak8')
    if any(not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.+-]*', t) or t in
           {'help', 'clean', 'install', 'edit_cache', 'rebuild_cache', 'depend'} for t in args.target):
        parser.error('--target must name a build target')
    source, cache = args.source.resolve(), args.cache.resolve()
    if not (source / 'CMakeLists.txt').is_file():
        parser.error('source must be a SunstoneOS checkout')
    targets = list(dict.fromkeys(args.target))
    variants = list(VARIANTS) if args.full else list(dict.fromkeys(args.variant))
    indexed, pending, affected_plan, hook_scope = [], [], None, None
    if args.full or args.affected or args.changed:
        try:
            spec = importlib.util.spec_from_file_location('affected', source / 'checks/affected.py')
            selector = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(selector)
            if args.full:
                index = selector.indexer.generate()
                indexed = [{'check': name, 'output': '--output' in entry['required_arguments']}
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
            else:
                affected_plan = selector.plan(args.affected, args.changed)
                indexed = [check for check in affected_plan['checks'] if check['status'] == 'runnable']
                pending = [check for check in affected_plan['checks'] if check['status'] == 'pending']
                variants = list(dict.fromkeys([*variants, affected_plan['baseline']]))
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            parser.error('cannot select indexed checks: ' + str(error))
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
    output = build_root / ('gate-' + args.label)
    output = unique_directory(output)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    env['SUNSTONEOS_DIR'] = str(source)
    env['PATH'] = str(cache / 'host/usr/lib/llvm-21/bin') + os.pathsep + env.get('PATH', '')
    # One build tree per resolved checkout: the name keeps it recognizable and
    # the path hash separates checkouts that share a name.
    tree = re.sub(r'[^A-Za-z0-9_.-]', '_', source.name) + '-' + hashlib.sha256(str(source).encode()).hexdigest()[:10]
    cfg, build = cache / 'bin/cfg.sh', build_root / ('gate-central-' + tree)
    application_builds = {name: build_root / ('gate-' + name + '-' + tree) for name in configured}
    python = cache / 'venv/bin/python'
    qemu = cache / 'host/usr/bin/qemu-system-aarch64'
    evidence = output / 'evidence'

    def note(message):
        print(message, flush=True)
        with (output / 'summary.txt').open('a') as stream:
            stream.write(message + '\n')

    def run(name, command):
        command = [str(value) for value in command]
        with (output / 'commands.txt').open('a') as stream:
            stream.write(name + ': ' + shlex.join(command) + '\n')
        with (output / (name + '.log')).open('w') as log:
            try:
                result = subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT)
                ok = result.returncode == 0
            except OSError as error:
                log.write(str(error) + '\n')
                ok = False
        note(('PASS ' if ok else 'FAIL ') + name)
        if not ok:
            note('Stopped at first failure; diagnose it before running only the affected steps with a new label.')
        return ok

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
    def discover(name, directory):
        """The build targets that CMake lists, or None after a failure."""
        if not run(name + '-targets', ['cmake', '--build', directory, '--target', 'help']):
            return None
        excluded = {'all', 'clean', 'depend', 'edit_cache', 'rebuild_cache', 'install', 'list_install_components'}
        discovered = re.findall(r'^\.\.\. ([A-Za-z0-9_][A-Za-z0-9_.+-]*)\s*$',
                                (output / (name + '-targets.log')).read_text(), re.M)
        if not discovered:
            note('FAIL ' + name + '-targets: unsupported target listing; select targets explicitly')
            return None
        return [t for t in discovered if t not in excluded and not t.endswith(('.o', '.i', '.s', '.obj'))]
    core_targets = []
    core_options = ['-DSUNSTONE_EXTERNAL_APPLICATIONS=', *['-D' + key + '=' for key in selectors]]
    if targets or args.full:
        # Override any external selections supplied by the host configuration.
        if not run('central-configure', [cfg, source, build, '-DSUNSTONE_APPLICATION=', *core_options]):
            return 1
        if args.full:
            core_targets = discover('central', build)
            if core_targets is None:
                return 1
            targets = list(dict.fromkeys(targets + core_targets))
        # One invocation lets CMake reuse shared dependencies across selected targets.
        if not run('central-build', ['cmake', '--build', build, '--target', *targets, '-j', '12']):
            return 1
    for variant in variants:
        directory = build_root / ('gate-os-' + variant + '-' + tree)
        options = [value.replace('{cache}', str(cache)) for value in VARIANTS[variant]]
        if not run('os-' + variant + '-configure', [cfg, source, directory,
                   *core_options, '-DSUNSTONE_APPLICATION=', '-DSUNSTONE_A733_OS=ON', *options]):
            return 1
        if not run('os-' + variant + '-build', ['cmake', '--build', directory, '--target', 'sunstone-a733-os', '-j', '12']):
            return 1
    for name in (configured if args.full else applications):
        directory, selected = application_builds[name], applications.get(name, [])
        selected = list(dict.fromkeys(['sunstone-a733-os', *selected]))
        if not run(name + '-configure', [cfg, source, directory, *core_options,
                   '-DSUNSTONE_EXTERNAL_APPLICATIONS=' + str(configured[name]['root']),
                   '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=' + name]):
            return 1
        if args.full:
            discovered = discover(name, directory)
            if discovered is None:
                return 1
            # The application build repeats the core targets; build only its own.
            selected = list(dict.fromkeys(selected + [t for t in discovered if t not in core_targets]))
        if selected and not run(name + '-build', ['cmake', '--build', directory, '--target', *selected, '-j', '12']):
            return 1
    commands = {
        'kernel': ['tools/check-kernel.py', build / 'kernel-check.elf', '--qemu', qemu, '--evidence', evidence / 'kernel'],
        'soak8': ['tools/check-smp.py', build / 'smp8-soak-model.elf', '--case', 'soak8', '--trials', str(args.trials), '--qemu', qemu, '--evidence', evidence / 'soak8'],
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
    for check in checks:
        if not run('check-' + check, [python, *commands[check]]):
            return 1
    for check in indexed:
        command = [python, 'checks/' + check['check']]
        if check['output']:
            command += ['--output', evidence / 'indexed' / Path(check['check']).stem]
        if not run('indexed-' + Path(check['check']).stem, command):
            return 1
    if not unchanged('during the selected steps'):
        return 1
    if affected_plan is not None and pending:
        note('INCOMPLETE affected coverage: selected checks still need the inputs listed above')
        return 2
    note('PASS selected integration checks; unselected behavior is not revalidated')
    return 0


if __name__ == '__main__':
    sys.exit(main())
