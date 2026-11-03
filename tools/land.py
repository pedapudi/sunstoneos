#!/usr/bin/env python3
"""Land a worker branch on bringup: merge, check by risk, fast-forward and push.

  land.py WORKTREE --reason TEXT [--approved-by coordinator]

A branch that changes only docs/ runs the evidence checks and the link check and no gate. Any other branch
runs the gate in its affected mode, in a shared integration worktree. After a code landing the full suite
starts on the bringup tip in the background (tools/full-gate.py) and blocks nothing. One lander runs at a time.
The lock is held from the merge through the push. If bringup moves during a gate, the tool merges again and
gates again. The tool never resolves a conflict, never deletes files, never rewrites history, never forces a
push and never pipes git merge output. See docs/landing.md.
"""
import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import signal
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
DOCS_ONLY = ['docs/**']


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


def start_step(name, command, cwd):
    """Start a check in the background; finish_step reports its failure."""
    return name, subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def finish_step(step):
    name, process = step
    text = process.communicate()[0]
    if process.returncode:
        raise Refused('%s failed (exit %d):\n%s' % (name, process.returncode, text[-2000:].strip()))


def run_step(name, command, cwd):
    finish_step(start_step(name, command, cwd))


def merge_bringup(worktree, branch):
    done = git(worktree, 'merge', '--no-edit', '-m', "Merge branch 'bringup' into %s" % branch, 'bringup', check=False)
    unmerged = out(worktree, 'diff', '--name-only', '--diff-filter=U')
    if unmerged:
        raise Refused('the merge of bringup has unmerged paths; resolve them in %s and run the tool again:\n%s'
                      % (worktree, unmerged))
    if done.returncode:
        raise Refused('the merge of bringup failed: %s' % (done.stderr or done.stdout).strip())


def docs_only(names):
    expressions = [glob_regex(p) for p in DOCS_ONLY]
    return all(any(e.match(n) for e in expressions) for n in names)


def label_for(args, branch):
    """A gate label that names no existing gate directory, so that a stale summary cannot pass a new gate."""
    base = 'land-%s-%s' % (re.sub(r'[^A-Za-z0-9._-]', '-', branch.replace('work/', '')),
                           datetime.datetime.now().strftime('%m%d-%H%M%S'))
    label, number = base, 1
    while (args.cache / 'build' / ('gate-' + label)).exists():
        number += 1
        label = '%s-%d' % (base, number)
    return label


def gate_argv(args, tree):
    """The command that starts the gate on TREE: the gate command given with --gate-command, or tools/integration-gate.py of TREE itself.

    The gate takes the host-wide gate lock (CACHE/gate.lock) itself, so it needs no wrapper."""
    return args.gate_command or [sys.executable, str(Path(tree) / 'tools/integration-gate.py')]


def integration_tree(args, ref):
    """The shared worktree where every code landing is gated, detached at ref.

    One path for every landing keeps its build directories, so a gate rebuilds only what changed,
    and a worker cannot move the tree under the gate."""
    tree = args.cache / 'land-integration'
    if tree.exists():
        require_clean(tree, 'the integration worktree')
        git(tree, 'checkout', '-q', '--detach', ref)
    else:
        git(args.bringup_worktree, 'worktree', 'add', '-q', '--detach', str(tree), ref)
    return tree


def gate_failures(summary):
    lines = summary.read_text(errors='replace').splitlines() if summary.is_file() else []
    return [l for l in lines if re.match(r'(FAIL|SKIP|INCOMPLETE)\b', l)]


def verify(args, worktree, base, tip, names, label):
    """Run the checks that the diff earns. Returns the gate label, or 'docs-only' when no gate ran."""
    if docs_only(names):
        for name, command in (('tools/evidence.py validate', args.evidence_command + ['validate']),
                              ('tools/evidence.py sync --check', args.evidence_command + ['sync', '--check']),
                              ('checks/docs-links.py', args.links_command)):
            run_step(name, command, worktree)
        return 'docs-only'
    tree = integration_tree(args, tip)
    # The gate starts beside the registry check, which refuses in seconds what the gate would refuse at its
    # end. If the registry check refuses, the gate is stopped by its own process group.
    command = gate_argv(args, tree) + [str(tree), label, '--reason', args.reason, '--affected', '%s..%s' % (base, tip), '--keep-going']
    gate = subprocess.Popen(command, cwd=tree, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        run_step('checks/gate-inputs.py', args.registry_command + ['--output', str(args.cache / 'build' / ('registry-' + label + '.json'))], tree)
        gate.wait()
        summary = args.cache / 'build' / ('gate-' + label) / 'summary.txt'
        if not gate_passes(summary):
            raise Refused('the gate did not pass; read %s\n%s' % (summary, '\n'.join(gate_failures(summary))[-1500:]))
    finally:
        if gate.poll() is None:
            os.killpg(gate.pid, signal.SIGTERM)
            gate.wait()
    return label


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
            label = verify(args, worktree, base, tip, names, label_for(args, branch))
            require_clean(worktree, 'the worktree after the checks')
            if out(worktree, 'rev-parse', 'HEAD') != tip:
                raise Refused('the branch moved during the checks')
            if out(worktree, 'rev-parse', 'bringup') != base:
                print('bringup moved during the checks (attempt %d); merging again' % (attempt + 1))
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
            if label != 'docs-only' and args.full_gate_command:
                # The full suite runs on the new tip in its own process and blocks nothing.
                subprocess.Popen(args.full_gate_command + ['--cache', str(cache), '--bringup-worktree', str(bringup)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
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
                        help='gate command; WORKTREE LABEL --reason TEXT --affected RANGE --keep-going are appended; '
                        'default: tools/integration-gate.py of the integration worktree')
    parser.add_argument('--evidence-command', default=os.environ.get('LAND_EVIDENCE_COMMAND'))
    parser.add_argument('--links-command', default=os.environ.get('LAND_LINKS_COMMAND'))
    parser.add_argument('--full-gate-command', default=os.environ.get('LAND_FULL_GATE_COMMAND'),
                        help='starts the full suite on the bringup tip after a code landing; empty: do not start it')
    parser.add_argument('--registry-command', default=os.environ.get('LAND_REGISTRY_COMMAND'))
    args = parser.parse_args(argv)
    try:
        host_space.preflight(args.cache / 'build')
        host_space.use_disk_tmp(args.cache / 'build')       # the gate, its compilers and the evidence tools inherit TMPDIR
    except host_space.SpaceShortage as error:
        print('REFUSED: %s' % error, file=sys.stderr)
        return 1
    args.gate_command = shlex.split(args.gate_command) if args.gate_command else None
    args.evidence_command = shlex.split(args.evidence_command or '%s tools/evidence.py' % sys.executable)
    args.links_command = shlex.split(args.links_command or '%s -B checks/docs-links.py' % sys.executable)
    args.full_gate_command = shlex.split(args.full_gate_command if args.full_gate_command is not None else '%s %s' % (sys.executable, ROOT / 'tools/full-gate.py'))
    args.registry_command = shlex.split(args.registry_command or '%s -B checks/gate-inputs.py' % sys.executable)
    try:
        land(args)
    except Refused as error:
        print('REFUSED: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
