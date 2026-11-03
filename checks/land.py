#!/usr/bin/env python3
"""tools/land.py against a temporary bare origin: landing, refusals, approval and the lock.

Covers a clean fast-forward landing, a landing after bringup moved during the gate,
the unmerged-path, failed-gate, protected-path, dirty-worktree, nothing-to-land and
concurrent-lander refusals, and the coordinator approval of a protected path.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys

if not __debug__:
    raise SystemExit('checks require assertions')
ROOT = Path(__file__).resolve().parents[1]
LAND = ROOT / 'tools/land.py'
ENV = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_SYSTEM': os.devnull,
       'SUNSTONE_HOST_SPACE': 'off',     # the fake cache is in a scratch directory (tools/host_space.py)
       'GIT_AUTHOR_NAME': 'check', 'GIT_AUTHOR_EMAIL': 'check@example.invalid',
       'GIT_COMMITTER_NAME': 'check', 'GIT_COMMITTER_EMAIL': 'check@example.invalid'}

# Stub gate: records a run, writes the summary the real gate writes, and can fail or move bringup.
GATE = '''import os, sys
from pathlib import Path
cache = Path(os.environ["LAND_CHECK_CACHE"])
worktree, label = sys.argv[1], sys.argv[2]
runs = cache / "gate-runs"
runs.write_text(runs.read_text() + label + "\\n" if runs.exists() else label + "\\n")
(cache / "build" / ("gate-" + label)).mkdir(parents=True)
failed = (cache / "gate-fail").exists()
summary = "FAIL kernel\\n" if failed else "PASS selected\\n"
(cache / "build" / ("gate-" + label) / "summary.txt").write_text(summary)
advance = cache / "advance"
if advance.exists():
    advance.unlink()
    import subprocess
    bringup = os.environ["LAND_CHECK_BRINGUP"]
    Path(bringup, "moved.txt").write_text("moved\\n")
    subprocess.run(["git", "-C", bringup, "add", "moved.txt"], check=True)
    subprocess.run(["git", "-C", bringup, "commit", "-qm", "moved during the gate"], check=True)
sys.exit(1 if failed else 0)
'''


def git(cwd, *args):
    done = subprocess.run(['git', '-C', str(cwd), *args], capture_output=True, text=True, env=ENV)
    assert done.returncode == 0, (args, done.stderr)
    return done.stdout.strip()


def commit(cwd, name, text, message):
    Path(cwd, name).parent.mkdir(parents=True, exist_ok=True)
    Path(cwd, name).write_text(text)
    git(cwd, 'add', name)
    git(cwd, 'commit', '-qm', message)


class Fixture:
    def __init__(self, root):
        self.root = root
        self.cache = root / 'cache'
        (self.cache / 'build').mkdir(parents=True)
        (root / 'ok.py').write_text('')
        (root / 'unregistered.py').write_text('import sys\nprint("checks with no registry answer: new-check.py")\nsys.exit(1)\n')
        (root / 'gate.py').write_text(GATE)
        self.origin = root / 'origin.git'
        git(root, 'init', '-q', '--bare', str(self.origin))
        self.bringup = root / 'bringup'
        git(root, 'clone', '-q', str(self.origin), str(self.bringup))
        git(self.bringup, 'checkout', '-qb', 'bringup')
        commit(self.bringup, 'tools/land-protected.txt', (ROOT / 'tools/land-protected.txt').read_text(), 'protected list')
        commit(self.bringup, 'shared.txt', 'base\n', 'base')
        git(self.bringup, 'push', '-q', 'origin', 'bringup')
        self.count = 0

    def worker(self, name):
        path = self.root / name
        git(self.bringup, 'worktree', 'add', '-q', '-b', 'work/' + name, str(path), 'bringup')
        return path

    def land(self, worker, *extra, reason='check'):
        env = {**ENV, 'LAND_CHECK_CACHE': str(self.cache), 'LAND_CHECK_BRINGUP': str(self.bringup)}
        command = [sys.executable, str(LAND), str(worker), '--reason', reason, '--cache', str(self.cache),
                   '--bringup-worktree', str(self.bringup),
                   '--gate-command', '%s %s' % (sys.executable, self.root / 'gate.py'),
                   '--evidence-command', '%s %s' % (sys.executable, self.root / 'ok.py'),
                   '--progress-command', '%s %s' % (sys.executable, self.root / 'ok.py'),
                   '--registry-command', '%s %s' % (sys.executable, self.root / 'ok.py'), *extra]
        return subprocess.run(command, capture_output=True, text=True, env=env)

    def gate_runs(self):
        runs = self.cache / 'gate-runs'
        return len(runs.read_text().split()) if runs.exists() else 0

    def origin_tip(self):
        return git(self.origin, 'rev-parse', 'bringup')


def passed(name):
    print('PASS', name)


def run(root):
    fx = Fixture(root)

    # A clean landing: bringup moved after the branch was cut, so the tool merges first.
    w = fx.worker('clean')
    commit(w, 'feature.txt', 'feature\n', 'feature')
    commit(fx.bringup, 'other.txt', 'other\n', 'other')
    git(fx.bringup, 'push', '-q', 'origin', 'bringup')
    result = fx.land(w)
    assert result.returncode == 0, result.stderr
    tip = git(w, 'rev-parse', 'HEAD')
    assert git(fx.bringup, 'rev-parse', 'bringup') == tip == fx.origin_tip()
    assert git(fx.bringup, 'rev-parse', 'HEAD') == tip
    assert tip in result.stdout and 'land-clean-' in result.stdout, result.stdout
    assert (fx.bringup / 'feature.txt').exists() and (fx.bringup / 'other.txt').exists()
    record = json.loads((fx.cache / 'landings.jsonl').read_text().splitlines()[-1])
    assert record['tip'] == tip and record['branch'] == 'work/clean' and record['reason'] == 'check'
    assert record['approved_by'] is None and record['gate'].startswith('land-clean-')
    assert fx.gate_runs() == 1
    passed('clean fast-forward landing')

    # Nothing left to land.
    result = fx.land(w)
    assert result.returncode and 'nothing to land' in result.stderr, result.stderr
    passed('nothing-to-land refusal')

    # Unmerged paths: refused, merge state left in place, bringup unchanged.
    w = fx.worker('conflict')
    commit(w, 'shared.txt', 'worker\n', 'worker edit')
    commit(fx.bringup, 'shared.txt', 'bringup\n', 'bringup edit')
    git(fx.bringup, 'push', '-q', 'origin', 'bringup')
    before, runs = fx.origin_tip(), fx.gate_runs()
    result = fx.land(w)
    assert result.returncode and 'unmerged paths' in result.stderr and 'shared.txt' in result.stderr, result.stderr
    assert git(w, 'diff', '--name-only', '--diff-filter=U') == 'shared.txt'
    assert (Path(git(w, 'rev-parse', '--absolute-git-dir')) / 'MERGE_HEAD').exists()
    assert fx.origin_tip() == before and fx.gate_runs() == runs
    passed('unmerged-path refusal leaves the merge state')

    # Dirty worktree.
    w = fx.worker('dirty')
    commit(w, 'dirty.txt', 'x\n', 'dirty work')
    (w / 'untracked.txt').write_text('stray\n')
    result = fx.land(w)
    assert result.returncode and 'not clean' in result.stderr, result.stderr
    assert fx.origin_tip() == before and fx.gate_runs() == runs
    passed('dirty worktree refusal')

    # Failed gate.
    w = fx.worker('failing')
    commit(w, 'failing.txt', 'x\n', 'failing work')
    (fx.cache / 'gate-fail').write_text('')
    result = fx.land(w)
    assert result.returncode and 'gate did not pass' in result.stderr, result.stderr
    assert fx.origin_tip() == before and fx.gate_runs() == runs + 1
    (fx.cache / 'gate-fail').unlink()
    passed('failed gate refusal')

    # A check without a registry answer: refused after the merge and before the gate.
    w = fx.worker('unregistered')
    commit(w, 'unregistered.txt', 'x\n', 'unregistered check')
    runs = fx.gate_runs()
    result = fx.land(w, '--registry-command', '%s %s' % (sys.executable, fx.root / 'unregistered.py'))
    assert result.returncode and 'checks/gate-inputs.py failed' in result.stderr and 'new-check.py' in result.stderr, result.stderr
    assert fx.origin_tip() == before and fx.gate_runs() == runs
    passed('unregistered check refused before the gate')

    # Protected path: refused before the gate, then landed with the coordinator approval.
    w = fx.worker('protected')
    commit(w, 'tools/board.py', '# changed\n', 'touch the board runner')
    runs = fx.gate_runs()
    result = fx.land(w)
    assert result.returncode and 'tools/board.py' in result.stderr and 'protected' in result.stderr, result.stderr
    assert fx.origin_tip() == before and fx.gate_runs() == runs
    result = fx.land(w, '--approved-by', 'coordinator')
    assert result.returncode == 0, result.stderr
    assert fx.origin_tip() == git(w, 'rev-parse', 'HEAD')
    assert json.loads((fx.cache / 'landings.jsonl').read_text().splitlines()[-1])['approved_by'] == 'coordinator'
    passed('protected path refusal and coordinator approval')

    # A branch that edits the protected list is refused; its own edit does not widen the list.
    w = fx.worker('listedit')
    commit(w, 'tools/land-protected.txt', '# emptied\n', 'empty the list')
    commit(w, 'tools/land.py', '# changed\n', 'change the tool')
    result = fx.land(w)
    assert result.returncode and 'protected' in result.stderr and 'tools/land.py' in result.stderr, result.stderr
    passed('the list and the tool are protected')

    # Concurrent lander: the lock refuses a second lander.
    w = fx.worker('locked')
    commit(w, 'locked.txt', 'x\n', 'locked work')
    before, runs = fx.origin_tip(), fx.gate_runs()
    with open(fx.cache / 'land.lock', 'a+') as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        result = fx.land(w)
    assert result.returncode and 'another lander' in result.stderr, result.stderr
    assert fx.origin_tip() == before and fx.gate_runs() == runs
    passed('concurrent lander blocked by the lock')

    # Bringup moves during the gate: merge again and gate again.
    (fx.cache / 'advance').write_text('')
    result = fx.land(w)
    assert result.returncode == 0, result.stderr
    assert fx.gate_runs() == runs + 2, fx.gate_runs()
    assert 'merging again' in result.stdout
    tip = git(w, 'rev-parse', 'HEAD')
    assert fx.origin_tip() == tip
    assert (fx.bringup / 'moved.txt').exists() and (fx.bringup / 'locked.txt').exists()
    passed('bringup moved during the gate: merged and gated again')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', type=Path, required=True, help='new scratch directory')
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True)
    run(arguments.output.resolve())
