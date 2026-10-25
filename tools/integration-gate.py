#!/usr/bin/env python3
"""Run explicitly selected integration builds and checks, once each.

Host configuration stays in CACHE/bin/cfg.sh. This tool never launches a board.
Build directories belong to the resolved source checkout, so gates on different
checkouts never share a CMake cache; a lock refuses a second concurrent gate on
the same checkout. A source tree with uncommitted changes is refused unless
--allow-dirty is given, and the recorded identity then names those changes:
the tracked diff and every untracked file that Git does not ignore. The
gate stops if that state changes before it holds the lock.
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
}
DEPENDENCIES = {'kernel': 'kernel-check', 'soak8': 'smp8-soak-model',
                'mimus-smp': 'smp-mimus-model'}
CHECKS = (*DEPENDENCIES, 'shared-helpers', 'progress', 'check-index', 'pmic-tree',
          'power-domains-tree')


def tree_state(source, status):
    """The status text, and the SHA-256 of the tracked diff and of the untracked files.

    Each untracked, non-ignored path contributes its name and the SHA-256 of
    its contents (a symbolic link's target; a nested repository's name only).
    None stands for a value Git could not produce."""
    diff = subprocess.run(['git', 'diff', '--binary', 'HEAD'], cwd=source, capture_output=True)
    listed = subprocess.run(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=source, capture_output=True)
    untracked = None
    if listed.returncode == 0:
        names = sorted(name for name in listed.stdout.split(b'\0') if name)
        digest = hashlib.sha256()
        for name in names:
            path = source / os.fsdecode(name)
            if path.is_symlink():
                content = os.fsencode(os.readlink(path))
            elif path.is_dir():
                content = b''
            else:
                content = path.read_bytes()
            digest.update(name + b'\0' + hashlib.sha256(content).digest())
        untracked = (digest.hexdigest(), len(names))
    return (status,
            hashlib.sha256(diff.stdout).hexdigest() if diff.returncode == 0 else None, untracked)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('label', help='new evidence label; existing directories are refused')
    parser.add_argument('--reason', required=True, help='changed behavior and risk this selection checks')
    parser.add_argument('--target', action='append', default=[], help='central CMake target; repeat to add targets')
    parser.add_argument('--variant', choices=VARIANTS, action='append', default=[])
    parser.add_argument('--check', choices=CHECKS, action='append', default=[])
    parser.add_argument('--full', action='store_true', help='explicit broad integration milestone: all central targets, the nine OS variants and all listed checks')
    parser.add_argument('--cache', type=Path, default=Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos')))
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
    checks = list(CHECKS) if args.full else list(dict.fromkeys(args.check))
    for check in checks:
        if check in DEPENDENCIES and DEPENDENCIES[check] not in targets:
            targets.append(DEPENDENCIES[check])
    status = subprocess.run(['git', 'status', '--porcelain'], cwd=source, capture_output=True, text=True)
    if status.returncode != 0:
        parser.error('source must be a Git checkout: ' + status.stderr.strip())
    if status.stdout and not args.allow_dirty:
        parser.error('source has uncommitted changes; commit them or pass --allow-dirty to record them')
    state = tree_state(source, status.stdout)
    output = cache / 'build' / ('gate-' + args.label)
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error(f'evidence already exists: {output}; reuse its results or choose a new label for affected work')
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    env['PATH'] = str(cache / 'host/usr/lib/llvm-21/bin') + os.pathsep + env.get('PATH', '')
    # One build tree per resolved checkout: the name keeps it recognizable and
    # the path hash separates checkouts that share a name.
    tree = re.sub(r'[^A-Za-z0-9_.-]', '_', source.name) + '-' + hashlib.sha256(str(source).encode()).hexdigest()[:10]
    cfg, build = cache / 'bin/cfg.sh', cache / 'build' / ('gate-central-' + tree)
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
    identity = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=source, capture_output=True, text=True)
    note('revision: ' + (identity.stdout.strip() if identity.returncode == 0 else 'unavailable'))
    note('diff-sha256: ' + (state[1] or 'unavailable'))
    note('untracked-sha256: ' + ('%s (%d files)' % state[2] if state[2] else 'unavailable'))
    note('status: ' + ('clean' if not status.stdout else str(len(status.stdout.splitlines())) + ' changed paths'))
    if status.stdout:
        (output / 'status.txt').write_text(status.stdout)
    lock_path = cache / 'build' / ('gate-' + tree + '.lock')
    lock = lock_path.open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        note('FAIL another gate holds ' + str(lock_path) + ' for this checkout')
        return 1
    # The status, diff and untracked files recorded above must still describe
    # the tree that this gate builds while it holds the lock.
    current = subprocess.run(['git', 'status', '--porcelain'], cwd=source, capture_output=True, text=True)
    if current.returncode != 0 or tree_state(source, current.stdout) != state:
        note('FAIL the source changed between its status check and the lock; rerun with a new label')
        return 1
    if targets or args.full:
        if not run('central-configure', [cfg, source, build]):
            return 1
        if args.full:
            if not run('central-targets', ['cmake', '--build', build, '--target', 'help']):
                return 1
            excluded = {'all', 'clean', 'depend', 'edit_cache', 'rebuild_cache', 'install', 'list_install_components'}
            discovered = re.findall(r'^\.\.\. ([A-Za-z0-9_][A-Za-z0-9_.+-]*)\s*$',
                                    (output / 'central-targets.log').read_text(), re.M)
            targets = list(dict.fromkeys(targets + [t for t in discovered if t not in excluded and
                          not t.endswith(('.o', '.i', '.s', '.obj'))]))
            if not discovered:
                note('FAIL central-targets: unsupported target listing; select targets explicitly')
                return 1
        # One invocation lets CMake reuse shared dependencies across selected targets.
        if not run('central-build', ['cmake', '--build', build, '--target', *targets, '-j', '12']):
            return 1
    for variant in variants:
        directory = cache / 'build' / ('gate-os-' + variant + '-' + tree)
        if not run('os-' + variant + '-configure', [cfg, source, directory,
                   '-DSUNSTONE_MIMUS_BOARD_APPLICATION=ON', '-DSUNSTONE_A733_OS=ON', *VARIANTS[variant]]):
            return 1
        if not run('os-' + variant + '-build', ['cmake', '--build', directory, '--target', 'sunstone-a733-os', '-j', '12']):
            return 1
    commands = {
        'kernel': ['tools/check-kernel.py', build / 'kernel-check.elf', '--qemu', qemu, '--evidence', evidence / 'kernel'],
        'soak8': ['tools/check-smp.py', build / 'smp8-soak-model.elf', '--case', 'soak8', '--trials', str(args.trials), '--qemu', qemu, '--evidence', evidence / 'soak8'],
        'mimus-smp': ['tools/check-mimus-smp.py', '--image', build / 'smp-mimus-model.elf', '--bundle', cache / 'inputs/mimus-bench/inputs.bin', '--reference', cache / 'evidence/mimus-host.json', '--qemu', qemu, '--evidence', evidence / 'mimus-smp'],
        'shared-helpers': ['checks/shared-helpers.py', '--output', evidence / 'shared-helpers'],
        'progress': ['checks/progress.py'],
        'check-index': ['checks/check-index.py', '--verify'],
        'pmic-tree': ['checks/pmic-tree.py'],
        'power-domains-tree': ['checks/power-domains-tree.py'],
    }
    for check in checks:
        if not run('check-' + check, [python, *commands[check]]):
            return 1
    note('PASS selected integration checks; unselected behavior is not revalidated')
    return 0


if __name__ == '__main__':
    sys.exit(main())
