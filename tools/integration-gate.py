#!/usr/bin/env python3
"""Run explicitly selected integration builds and checks, once each.

Host configuration stays in CACHE/bin/cfg.sh. This tool never launches a board.
Build directories belong to the resolved source checkout, so gates on different
checkouts never share a CMake cache; a lock refuses a second concurrent gate on
the same checkout. A source tree with uncommitted changes is refused unless
--allow-dirty is given, and the recorded identity then names those changes:
the revision, the tracked diff and every untracked file that Git does not
ignore. The gate fails if that state cannot be read completely, or if it
changes before the gate holds the lock or by the end of the last step.
Use checks/check-index.py --changed PATH... to help choose focused checks; its
textual coverage is advisory. Component checks outside this gate run directly.
"""
import argparse
import fcntl
import hashlib
from pathlib import Path
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
    # The supervised NPU profile. Its catalogue is a host input, as the mimus
    # inputs are: the OS build verifies every file it names against
    # tools/npu-session-models.json. {cache} is the --cache directory.
    'npu': ['-DSUNSTONE_A733_OS_NPU=ON', '-DSUNSTONE_OS_NPU_CATALOGUE={cache}/' + NPU_CATALOGUE],
    # The performance-mode service (docs/designs/performance-modes.md) on the
    # supervised A76 CPU clock build, with the high boot mode; and with the
    # NPU profile and the low boot mode.
    'perf': ['-DA733_A76_ADMITTED=ON', '-DSUNSTONE_A733_OS_CPU_CLOCK=ON', '-DSUNSTONE_A733_OS_PERF=ON'],
    'perf-npu-low': ['-DA733_A76_ADMITTED=ON', '-DSUNSTONE_A733_OS_CPU_CLOCK=ON', '-DSUNSTONE_A733_OS_PERF=ON',
                     '-DSUNSTONE_A733_OS_PERF_BOOT=low', '-DSUNSTONE_A733_OS_NPU=ON',
                     '-DSUNSTONE_OS_NPU_CATALOGUE={cache}/' + NPU_CATALOGUE],
}
# OS-core checks and the central target each needs. The core central build is
# configured without MIMUS_DIR, so these checks never read a mimus checkout.
DEPENDENCIES = {'kernel': 'kernel-check', 'soak8': 'smp8-soak-model'}
CORE_CHECKS = (*DEPENDENCIES, 'shared-helpers', 'progress', 'check-index', 'pmic-tree',
               'power-domains-tree')
# Applications, each with its own central build and checks. A check names its
# central target and its command; {build}, {cache}, {qemu} and {evidence} are
# replaced. Application sources and checks belong to the external checkout.
APPLICATIONS = {
    'mimus': {
        'checks': {'mimus-smp': ('smp-mimus-model', [
            '{mimus}/sunstoneos/tools/check-mimus-smp.py', '--image', '{build}/smp-mimus-model.elf',
            '--bundle', '{cache}/inputs/mimus-bench/inputs.bin', '--reference', '{cache}/evidence/mimus-host.json',
            '--qemu', '{qemu}', '--evidence', '{evidence}/mimus-smp'])},
    },
}
APPLICATION_CHECKS = {check: name for name, application in APPLICATIONS.items() for check in application['checks']}
CHECKS = (*CORE_CHECKS, *APPLICATION_CHECKS)


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
    parser.add_argument('label', help='new evidence label; existing directories are refused')
    parser.add_argument('--reason', required=True, help='changed behavior and risk this selection checks')
    parser.add_argument('--target', action='append', default=[], help='OS-core central CMake target; repeat to add targets')
    parser.add_argument('--variant', choices=VARIANTS, action='append', default=[])
    parser.add_argument('--check', choices=CHECKS, action='append', default=[])
    parser.add_argument('--full', action='store_true', help='explicit broad integration milestone: all core and application central targets, every OS variant and all listed checks')
    parser.add_argument('--cache', type=Path, default=Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos')))
    parser.add_argument('--build-root', type=Path, help='build and gate evidence directory; defaults to CACHE/build')
    parser.add_argument('--mimus-dir', type=Path, default=os.environ.get('MIMUS_DIR'),
                        help='optional external mimus checkout; defaults to MIMUS_DIR')
    parser.add_argument('--trials', type=int, default=1, help='predeclared soak8 trials, not failure retries (default: 1)')
    parser.add_argument('--allow-dirty', action='store_true', help='build a tree with uncommitted changes; the status and diff hash are recorded')
    args = parser.parse_args(argv)
    if not args.reason.strip():
        parser.error('--reason must describe the validation risk')
    if not args.full and not (args.target or args.variant or args.check):
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
    if any('npu' in v for v in variants) and not (cache / NPU_CATALOGUE).is_file():
        parser.error('the npu variant needs the NPU catalogue map ' + str(cache / NPU_CATALOGUE))
    mimus = args.mimus_dir.resolve() if args.mimus_dir else None
    if mimus and not (mimus / 'sunstoneos/CMakeLists.txt').is_file():
        parser.error('mimus must provide sunstoneos/CMakeLists.txt')
    checks = list(CHECKS if mimus else CORE_CHECKS) if args.full else list(dict.fromkeys(args.check))
    if not mimus and any(check in APPLICATION_CHECKS for check in checks):
        parser.error('mimus checks require --mimus-dir or MIMUS_DIR')
    for check in checks:
        if check in DEPENDENCIES and DEPENDENCIES[check] not in targets:
            targets.append(DEPENDENCIES[check])
    # Each selected application and the central targets of its selected checks.
    applications = {}
    for check in checks:
        if check in APPLICATION_CHECKS:
            name = APPLICATION_CHECKS[check]
            target = APPLICATIONS[name]['checks'][check][0]
            applications.setdefault(name, [])
            if target not in applications[name]:
                applications[name].append(target)
    status = subprocess.run(['git', 'status', '--porcelain'], cwd=source, capture_output=True, text=True)
    if status.returncode != 0:
        parser.error('source must be a Git checkout: ' + status.stderr.strip())
    if status.stdout and not args.allow_dirty:
        parser.error('source has uncommitted changes; commit them or pass --allow-dirty to record them')
    state = tree_state(source)
    if state[1] and not args.allow_dirty:
        parser.error('source has uncommitted changes; commit them or pass --allow-dirty to record them')
    build_root = args.build_root.resolve() if args.build_root else cache / 'build'
    output = build_root / ('gate-' + args.label)
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error(f'evidence already exists: {output}; reuse its results or choose a new label for affected work')
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    env['SUNSTONEOS_DIR'] = str(source)
    env['PATH'] = str(cache / 'host/usr/lib/llvm-21/bin') + os.pathsep + env.get('PATH', '')
    # One build tree per resolved checkout: the name keeps it recognizable and
    # the path hash separates checkouts that share a name.
    tree = re.sub(r'[^A-Za-z0-9_.-]', '_', source.name) + '-' + hashlib.sha256(str(source).encode()).hexdigest()[:10]
    cfg, build = cache / 'bin/cfg.sh', build_root / ('gate-central-' + tree)
    application_builds = {name: build_root / ('gate-' + name + '-' + tree) for name in APPLICATIONS}
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
    revision, changes, diff, untracked = state
    note('revision: ' + (revision or 'unavailable'))
    note('diff-sha256: ' + (diff or 'unavailable'))
    note('untracked-sha256: ' + ('%s (%d files)' % untracked if untracked else 'unavailable'))
    note('status: ' + ('unavailable' if changes is None else 'clean' if not changes else
                       str(len(changes.splitlines())) + ' changed paths'))
    if changes:
        (output / 'status.txt').write_text(changes)
    mimus_state = tree_state(mimus) if mimus else None
    if mimus:
        if None in mimus_state or (mimus_state[1] and not args.allow_dirty):
            note('FAIL external mimus state is unreadable or dirty; pass --allow-dirty to record local changes')
            return 1
        note('mimus-source: ' + str(mimus))
        note('mimus-revision: ' + mimus_state[0])
        note('mimus-diff-sha256: ' + mimus_state[2])
        note('mimus-untracked-sha256: %s (%d files)' % mimus_state[3])
        if mimus_state[1]:
            (output / 'mimus-status.txt').write_text(mimus_state[1])
    if any('npu' in v for v in variants):
        note('npu-catalogue-sha256: ' + hashlib.sha256((cache / NPU_CATALOGUE).read_bytes()).hexdigest())

    def unchanged(when):
        """The recorded state must be complete and still describe the tree."""
        if None in state or tree_state(source) != state:
            note('FAIL the source revision, status, diff or untracked files changed or could not be read '
                 + when + '; rerun with a new label')
            return False
        if mimus and tree_state(mimus) != mimus_state:
            note('FAIL external mimus changed or could not be read ' + when)
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
    if targets or args.full:
        # The empty MIMUS_DIR overrides the one cfg.sh passes.
        if not run('central-configure', [cfg, source, build, '-DSUNSTONE_APPLICATION=', '-DMIMUS_DIR=']):
            return 1
        if args.full:
            core_targets = discover('central', build)
            if core_targets is None:
                return 1
            targets = list(dict.fromkeys(targets + core_targets))
        # One invocation lets CMake reuse shared dependencies across selected targets.
        if not run('central-build', ['cmake', '--build', build, '--target', *targets, '-j', '12']):
            return 1
    for name in (APPLICATIONS if args.full and mimus else applications):
        directory, selected = application_builds[name], applications.get(name, [])
        if not run(name + '-configure', [cfg, source, directory, '-DMIMUS_DIR=' + str(mimus),
                   '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=mimus']):
            return 1
        if args.full:
            discovered = discover(name, directory)
            if discovered is None:
                return 1
            # The application build repeats the core targets; build only its own.
            selected = list(dict.fromkeys(selected + [t for t in discovered if t not in core_targets]))
        if selected and not run(name + '-build', ['cmake', '--build', directory, '--target', *selected, '-j', '12']):
            return 1
    for variant in variants:
        directory = build_root / ('gate-os-' + variant + '-' + tree)
        options = [value.replace('{cache}', str(cache)) for value in VARIANTS[variant]]
        if not run('os-' + variant + '-configure', [cfg, source, directory,
                   '-DMIMUS_DIR=', '-DSUNSTONE_APPLICATION=', '-DSUNSTONE_A733_OS=ON', *options]):
            return 1
        if not run('os-' + variant + '-build', ['cmake', '--build', directory, '--target', 'sunstone-a733-os', '-j', '12']):
            return 1
    commands = {
        'kernel': ['tools/check-kernel.py', build / 'kernel-check.elf', '--qemu', qemu, '--evidence', evidence / 'kernel'],
        'soak8': ['tools/check-smp.py', build / 'smp8-soak-model.elf', '--case', 'soak8', '--trials', str(args.trials), '--qemu', qemu, '--evidence', evidence / 'soak8'],
        'shared-helpers': ['checks/shared-helpers.py', '--output', evidence / 'shared-helpers'],
        'progress': ['checks/progress.py'],
        'check-index': ['checks/check-index.py', '--verify'],
        'pmic-tree': ['checks/pmic-tree.py'],
        'power-domains-tree': ['checks/power-domains-tree.py'],
    }
    for check, name in APPLICATION_CHECKS.items():
        places = {'build': application_builds[name], 'cache': cache, 'qemu': qemu, 'evidence': evidence,
                  'mimus': mimus}
        commands[check] = [value.format(**places) for value in APPLICATIONS[name]['checks'][check][1]]
    for check in checks:
        if not run('check-' + check, [python, *commands[check]]):
            return 1
    if not unchanged('during the selected steps'):
        return 1
    note('PASS selected integration checks; unselected behavior is not revalidated')
    return 0


if __name__ == '__main__':
    sys.exit(main())
