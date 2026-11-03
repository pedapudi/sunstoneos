#!/usr/bin/env python3
"""Land a worker branch on bringup: merge, gate, validate, fast-forward and push.

  land.py WORKTREE --reason TEXT [--approved-by coordinator]

One lander runs at a time. The lock is held from the merge through the push, so
bringup cannot move under a gate. If bringup moves anyway, the tool merges again
and gates again. The tool never resolves a conflict, never deletes files, never
rewrites history, never forces a push and never pipes git merge output.
See docs/landing.md.
"""
import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_priority
import host_space

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos'))
BRINGUP_WORKTREE = Path.home() / 'git/sunstoneos-worktrees/bringup'
PROTECTED_LIST = 'tools/land-protected.txt'
ATTEMPTS = 3


class Refused(Exception):
    """A landing precondition failed; the message states which one."""


def git(cwd, *args, check=True):
    done = subprocess.run(['git', '-C', str(cwd), *args], capture_output=True, text=True)
    if check and done.returncode:
        raise Refused('git %s failed: %s' % (' '.join(args), (done.stderr or done.stdout).strip()))
    return done


def out(cwd, *args):
    return git(cwd, *args).stdout.strip()


def is_ancestor(cwd, older, newer):
    return git(cwd, 'merge-base', '--is-ancestor', older, newer, check=False).returncode == 0


def glob_regex(pattern):
    """Translate a path glob: * stays within a component, ** crosses components."""
    text, i = '', 0
    while i < len(pattern):
        if pattern.startswith('**', i):
            text += '.*'
            i += 2
        elif pattern[i] == '*':
            text += '[^/]*'
            i += 1
        elif pattern[i] == '?':
            text += '[^/]'
            i += 1
        else:
            text += re.escape(pattern[i])
            i += 1
    return re.compile(text + r'\Z')


def protected_patterns(text):
    return [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith('#')]


def protected_hits(paths, patterns):
    expressions = [glob_regex(p) for p in patterns]
    return sorted(p for p in paths if any(e.match(p) for e in expressions))


def require_clean(worktree, what):
    status = out(worktree, 'status', '--porcelain', '--untracked-files=all')
    if status:
        raise Refused('%s is not clean:\n%s' % (what, status))


class Lock:
    def __init__(self, path, wait):
        self.path, self.wait, self.handle = path, wait, None

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = open(self.path, 'a+')
        deadline = time.monotonic() + self.wait
        while True:
            try:
                fcntl.flock(self.handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    self.handle.seek(0)
                    raise Refused('another lander holds %s (%s)' % (self.path, self.handle.read().strip() or 'unknown'))
                time.sleep(0.2)
        self.handle.seek(0)
        self.handle.truncate()
        self.handle.write('pid %d since %s\n' % (os.getpid(), datetime.datetime.now(datetime.timezone.utc).isoformat()))
        self.handle.flush()
        return self

    def __exit__(self, *exc):
        self.handle.close()


def gate_passes(summary):
    if not summary.is_file():
        return False
    lines = summary.read_text(errors='replace').splitlines()
    return any('PASS selected' in l for l in lines) and not any(re.search(r'\bFAIL\b', l) for l in lines)


def run_step(name, command, cwd):
    done = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    if done.returncode:
        raise Refused('%s failed (exit %d):\n%s' % (name, done.returncode, (done.stdout + done.stderr)[-2000:].strip()))


def merge_bringup(worktree, branch):
    done = git(worktree, 'merge', '--no-edit', '-m', "Merge branch 'bringup' into %s" % branch, 'bringup', check=False)
    unmerged = out(worktree, 'diff', '--name-only', '--diff-filter=U')
    if unmerged:
        raise Refused('the merge of bringup has unmerged paths; resolve them in %s and run the tool again:\n%s'
                      % (worktree, unmerged))
    if done.returncode:
        raise Refused('the merge of bringup failed: %s' % (done.stderr or done.stdout).strip())


def land(args):
    worktree = args.worktree.resolve()
    cache = args.cache
    bringup = args.bringup_worktree
    if args.approved_by not in (None, 'coordinator'):
        raise Refused('--approved-by accepts only: coordinator')
    if not (worktree / '.git').exists():
        raise Refused('%s is not a git worktree' % worktree)
    branch = out(worktree, 'rev-parse', '--abbrev-ref', 'HEAD')
    if branch in ('HEAD', 'bringup', 'main'):
        raise Refused('the worktree is on %s, not a work branch' % branch)
    require_clean(worktree, 'the worktree')
    with Lock(cache / 'land.lock', args.wait):
        for attempt in range(ATTEMPTS):
            git(bringup, 'fetch', args.remote, 'bringup')
            if not is_ancestor(bringup, 'FETCH_HEAD', 'bringup'):
                raise Refused('local bringup does not contain %s/bringup' % args.remote)
            if out(bringup, 'rev-parse', '--abbrev-ref', 'HEAD') != 'bringup':
                raise Refused('%s is not on bringup' % bringup)
            require_clean(bringup, 'the bringup worktree')
            base = out(worktree, 'rev-parse', 'bringup')
            if not is_ancestor(worktree, base, 'HEAD'):
                merge_bringup(worktree, branch)
                require_clean(worktree, 'the worktree after the merge')
            tip = out(worktree, 'rev-parse', 'HEAD')
            if not is_ancestor(worktree, base, tip):
                raise Refused('the merge result does not descend from bringup %s' % base[:8])
            if tip == base:
                raise Refused('the branch has nothing to land beyond bringup')
            names = out(worktree, 'diff', '--name-only', base, tip).splitlines()
            listing = git(worktree, 'show', 'bringup:' + PROTECTED_LIST, check=False)
            patterns = protected_patterns(listing.stdout if listing.returncode == 0
                                          else (worktree / PROTECTED_LIST).read_text())
            hits = protected_hits(names, patterns)
            if hits and args.approved_by != 'coordinator':
                raise Refused('the diff touches protected paths; the coordinator must land it with '
                              '--approved-by coordinator:\n' + '\n'.join(hits))
            label = 'land-%s-%s' % (re.sub(r'[^A-Za-z0-9._-]', '-', branch.replace('work/', '')),
                                    datetime.datetime.now().strftime('%m%d-%H%M%S'))
            gate = args.gate_command + [str(worktree), label, '--reason', args.reason, '--full']
            subprocess.run(gate, cwd=worktree, capture_output=True, text=True)
            summary = cache / 'build' / ('gate-' + label) / 'summary.txt'
            if not gate_passes(summary):
                raise Refused('the gate did not pass; read %s' % summary)
            run_step('tools/evidence.py validate', args.evidence_command + ['validate'], worktree)
            progress = cache / 'build' / ('land-progress-' + label)
            run_step('checks/progress.py', args.progress_command + ['--output', str(progress)], worktree)
            require_clean(worktree, 'the worktree after the gate')
            if out(worktree, 'rev-parse', 'HEAD') != tip:
                raise Refused('the branch moved during the gate')
            if out(worktree, 'rev-parse', 'bringup') != base:
                print('bringup moved during the gate (attempt %d); merging again' % (attempt + 1))
                continue
            if not is_ancestor(bringup, 'bringup', tip):
                raise Refused('landing %s would not fast-forward bringup' % tip[:8])
            git(bringup, 'merge', '--ff-only', tip)
            pushed = git(bringup, 'push', args.remote, 'bringup', check=False)
            if pushed.returncode:
                raise Refused('bringup advanced to %s locally but the push failed; push it by hand:\n%s'
                              % (tip[:8], (pushed.stderr or pushed.stdout).strip()))
            record = {'time': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
                      'branch': branch, 'tip': tip, 'gate': label, 'reason': args.reason,
                      'approved_by': args.approved_by}
            with open(cache / 'landings.jsonl', 'a') as log:
                log.write(json.dumps(record) + '\n')
            print('landed %s (%s) on bringup; gate %s' % (tip, branch, label))
            return tip
        raise Refused('bringup kept moving; %d attempts used' % ATTEMPTS)


def main(argv=None):
    host_priority.lower()      # the landing gate and its builds yield the disk and CPUs to the board runner (tools/host_priority.py)
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('worktree', type=Path)
    parser.add_argument('--reason', required=True)
    parser.add_argument('--approved-by')
    parser.add_argument('--wait', type=float, default=0, help='seconds to wait for the lock (default: refuse at once)')
    parser.add_argument('--cache', type=Path, default=CACHE)
    parser.add_argument('--bringup-worktree', type=Path, default=BRINGUP_WORKTREE)
    parser.add_argument('--remote', default='origin')
    # The commands are replaceable so that checks/land.py can run without a real gate.
    parser.add_argument('--gate-command', default=os.environ.get('LAND_GATE_COMMAND'),
                        help='gate command; WORKTREE LABEL --reason TEXT --full are appended')
    parser.add_argument('--evidence-command', default=os.environ.get('LAND_EVIDENCE_COMMAND'))
    parser.add_argument('--progress-command', default=os.environ.get('LAND_PROGRESS_COMMAND'))
    args = parser.parse_args(argv)
    try:
        host_space.preflight(args.cache / 'build')
        host_space.use_disk_tmp(args.cache / 'build')       # the gate, its compilers and the evidence tools inherit TMPDIR
    except host_space.SpaceShortage as error:
        print('REFUSED: %s' % error, file=sys.stderr)
        return 1
    args.gate_command = shlex.split(args.gate_command or str(args.cache / 'bin/integration-gate.sh'))
    args.evidence_command = shlex.split(args.evidence_command or '%s tools/evidence.py' % sys.executable)
    args.progress_command = shlex.split(args.progress_command or '%s checks/progress.py' % sys.executable)
    try:
        land(args)
    except Refused as error:
        print('REFUSED: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
