#!/usr/bin/env python3
"""tools/land.py against a temporary bare origin: landing by risk, refusals, approval and the lock.

Covers a code landing gated in affected mode and the full suite starting after it, the docs-only landing that
runs the evidence and link checks and no gate, a landing after bringup moved during the gate, the unmerged-path,
registry, failed-gate, protected-path, dirty-worktree, nothing-to-land and concurrent-lander refusals, the
coordinator approval of a protected path, tools/full-gate.py, the union merge of the evidence logs, and the
gate slots of tools/integration-gate.py.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time
GATE_RUNS = [['--output', '{output}']]

if not __debug__:
    raise SystemExit('checks require assertions')
ROOT = Path(__file__).resolve().parents[1]
LAND = Path(os.environ.get('LAND_UNDER_TEST', ROOT / 'tools/land.py'))   # an older copy shows a new case failing
ENV = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_SYSTEM': os.devnull,
       'SUNSTONE_HOST_SPACE': 'off',     # the fake cache is in a scratch directory (tools/host_space.py)
       'GIT_AUTHOR_NAME': 'check', 'GIT_AUTHOR_EMAIL': 'check@example.invalid',
       'GIT_COMMITTER_NAME': 'check', 'GIT_COMMITTER_EMAIL': 'check@example.invalid'}

# Stub gate: records a run, writes the summary the real gate writes, and can fail or move bringup.
GATE = '''import os, sys
from pathlib import Path
cache = Path(os.environ["LAND_CHECK_CACHE"])
worktree, label = sys.argv[1], sys.argv[2]
with open(cache / "gate-args", "a") as log:
    log.write(" ".join(sys.argv[3:]) + "\\n")
runs = cache / "gate-runs"
runs.write_text(runs.read_text() + label + "\\n" if runs.exists() else label + "\\n")
(cache / "build" / ("gate-" + label)).mkdir(parents=True)
if (cache / "record-on").exists() and "--affected" in sys.argv:    # the record of a passing affected gate on a clean tree
    import json, subprocess
    git = lambda *a: subprocess.run(["git", "-C", worktree, *a], capture_output=True, text=True).stdout.strip()
    base = git("rev-parse", sys.argv[sys.argv.index("--affected") + 1].split("..")[0])
    (cache / "build" / ("gate-" + label) / "gate-record.json").write_text(json.dumps({"tree": git("rev-parse", "HEAD^{tree}"), "base": base, "checks": {}}))
failed = (cache / "gate-fail").exists() or (Path(worktree) / "bad.txt").exists()
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


# Stub of checks/affected.py --changed PATH... --json: a path under tools/input selects one check.
AFFECTED = '''import json, sys
paths = sys.argv[sys.argv.index("--changed") + 1:-1]
print(json.dumps({"checks": [{"check": "x.py"}] if any(p.startswith("tools/input") for p in paths) else []}))
'''

STEP = '''import os, sys
from pathlib import Path
with open(Path(os.environ["LAND_CHECK_CACHE"]) / "steps", "a") as log:
    log.write(" ".join(["TAG"] + sys.argv[1:2]) + "\\n")
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
        for tag in ('registry', 'evidence', 'links'):          # stubs that log the step they stand for
            (root / (tag + '.py')).write_text(STEP.replace('TAG', tag))
        (root / 'unregistered.py').write_text('import sys\nprint("checks with no registry answer: new-check.py")\nsys.exit(1)\n')
        (root / 'gate.py').write_text(GATE)
        (root / 'affected.py').write_text(AFFECTED)
        (root / 'full.py').write_text('import sys\nfrom pathlib import Path\nPath(sys.argv[sys.argv.index("--cache") + 1], "full-started").write_text(" ".join(sys.argv[1:]))\n')
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

    def land(self, worker, *extra, reason='check', full_gate=''):
        env = {**ENV, 'LAND_CHECK_CACHE': str(self.cache), 'LAND_CHECK_BRINGUP': str(self.bringup)}
        command = [sys.executable, str(LAND), str(worker), '--reason', reason, '--cache', str(self.cache),
                   '--bringup-worktree', str(self.bringup),
                   '--gate-command', '%s %s' % (sys.executable, self.root / 'gate.py'),
                   '--evidence-command', '%s %s' % (sys.executable, self.root / 'evidence.py'),
                   '--links-command', '%s %s' % (sys.executable, self.root / 'links.py'),
                   '--registry-command', '%s %s' % (sys.executable, self.root / 'registry.py'),
                   '--affected-command', '%s %s' % (sys.executable, self.root / 'affected.py'),
                   '--full-gate-command', full_gate, *extra]
        return subprocess.run(command, capture_output=True, text=True, env=env)

    def gate_runs(self):
        runs = self.cache / 'gate-runs'
        return len(runs.read_text().split()) if runs.exists() else 0

    def gate_args(self):
        path = self.cache / 'gate-args'
        return path.read_text().splitlines() if path.exists() else []

    def steps(self):
        path = self.cache / 'steps'
        return path.read_text().splitlines() if path.exists() else []

    def records(self):
        return [json.loads(line) for line in (self.cache / 'landings.jsonl').read_text().splitlines()]

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
    assert '--full' not in fx.gate_args()[0] and '--affected' in fx.gate_args()[0], fx.gate_args()
    assert '--keep-going' in fx.gate_args()[0]
    assert fx.steps() == ['registry --output'], fx.steps()          # the evidence and link checks belong to the docs path
    passed('code landing gated in affected mode')

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

    # A gate that exits at argument validation writes no summary: the refusal quotes the end of the gate's log.
    w = fx.worker('early-exit')
    commit(w, 'early.txt', 'x\n', 'early exit work')
    (fx.root / 'early.py').write_text('import sys\nprint("line1")\nfor n in range(40):\n    print("noise", n)\nsys.stdout.flush()\nprint("error: names the unknown image npu-graphs", file=sys.stderr)\nsys.exit(2)\n')
    before, runs = fx.origin_tip(), fx.gate_runs()
    result = fx.land(w, '--gate-command', '%s %s' % (sys.executable, fx.root / 'early.py'))
    assert result.returncode and 'names the unknown image npu-graphs' in result.stderr, result.stderr
    assert 'wrote no summary' in result.stderr and 'exit 2' in result.stderr and 'line1' not in result.stderr, result.stderr
    assert any('land-early-exit-' in p.name and p.name.endswith('-gate.log') for p in (fx.cache / 'build').iterdir())
    assert fx.origin_tip() == before
    passed('a gate that exits early is refused with the end of its log')

    # A check without a registry answer: refused after the merge and before the gate.
    w = fx.worker('unregistered')
    commit(w, 'unregistered.txt', 'x\n', 'unregistered check')
    runs = fx.gate_runs()
    result = fx.land(w, '--registry-command', '%s %s' % (sys.executable, fx.root / 'unregistered.py'))
    assert result.returncode and 'checks/gate-inputs.py failed' in result.stderr and 'new-check.py' in result.stderr, result.stderr
    assert fx.origin_tip() == before          # the gate may have started beside the registry check; it is stopped and unread
    passed('unregistered check refused')

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

    docs_only(fx)
    background_full_gate(fx)
    full_gate(fx)
    union_merge(root / 'union')
    gate_lock(root / 'gatelock')
    reuse_gate(fx)
    stale_guard(fx)
    default_gate(root / 'defaultgate')


def reuse_gate(fx):
    """A landing reuses a passing affected gate: no rerun when nothing it covered changed, a partial rerun when a merged path
    affects a check, a full affected run when no prior gate exists."""
    def agent_gate(worker):
        env = {**ENV, 'LAND_CHECK_CACHE': str(fx.cache), 'LAND_CHECK_BRINGUP': str(fx.bringup)}
        done = subprocess.run([sys.executable, str(fx.root / 'gate.py'), str(worker), 'agent-' + worker.name, '--reason', 'x',
                               '--affected', 'bringup..HEAD'], env=env, cwd=worker, capture_output=True, text=True)
        assert done.returncode == 0, done.stderr
    def move_bringup(name):
        commit(fx.bringup, name, 'x\n', 'bringup moves: ' + name)
        git(fx.bringup, 'push', '-q', 'origin', 'bringup')
    git(fx.bringup, 'rm', '-q', 'bad.txt')            # the full-gate case left a commit that fails the stub gate
    git(fx.bringup, 'commit', '-qm', 'remove bad.txt')
    git(fx.bringup, 'push', '-q', 'origin', 'bringup')
    w = fx.worker('noprior')
    commit(w, 'noprior.txt', 'x\n', 'branch work')
    runs = fx.gate_runs()
    result = fx.land(w)
    assert result.returncode == 0, result.stderr
    assert fx.gate_runs() == runs + 1 and '--affected' in fx.gate_args()[-1], fx.gate_args()[-2:]
    passed('no prior gate: the affected gate runs in full')
    (fx.cache / 'record-on').write_text('')
    w = fx.worker('reuse')
    commit(w, 'reuse.txt', 'x\n', 'branch work')
    agent_gate(w)
    move_bringup('unrelated.txt')
    runs, steps = fx.gate_runs(), len(fx.steps())
    result = fx.land(w)
    assert result.returncode == 0, result.stderr
    assert fx.gate_runs() == runs and 'reusing gate' in result.stdout, result.stdout
    assert fx.records()[-1]['gate'] == 'reused-gate-agent-reuse' and fx.steps()[steps:] == ['registry --output']
    assert fx.origin_tip() == git(w, 'rev-parse', 'HEAD')
    passed('a passing gate is reused when the merge affects no check')
    w = fx.worker('partial')
    commit(w, 'partial.txt', 'x\n', 'branch work')
    agent_gate(w)
    move_bringup('tools/input.py')
    runs = fx.gate_runs()
    result = fx.land(w)
    assert result.returncode == 0, result.stderr
    args = fx.gate_args()[-1]
    assert fx.gate_runs() == runs + 1 and '--changed tools/input.py' in args and '--affected' not in args, args
    assert fx.records()[-1]['gate'].startswith('land-partial-')
    passed('a merged path that affects a check reruns only the affected checks')


def stale_guard(fx):
    """A landing that would restore an older version of a file bringup has since changed is refused; a new edit passes, and a
    deliberate revert passes with --restores."""
    for name in ('one.txt', 'two.txt'):
        commit(fx.bringup, 'series/' + name, 'v1 ' + name + '\n', 'series v1')
    first = git(fx.bringup, 'rev-parse', 'HEAD')
    for name in ('one.txt', 'two.txt'):
        commit(fx.bringup, 'series/' + name, 'v2 ' + name + '\n', 'series v2')
    second = git(fx.bringup, 'rev-parse', 'HEAD')
    git(fx.bringup, 'push', '-q', 'origin', 'bringup')
    # The stale copy: the branch sets the files back to the content they had before bringup's series landed.
    w = fx.worker('stale')
    for name in ('one.txt', 'two.txt'):
        commit(w, 'series/' + name, 'v1 ' + name + '\n', 'stale copy')
    commit(w, 'mine.txt', 'x\n', 'real work')
    before, runs = fx.origin_tip(), fx.gate_runs()
    result = fx.land(w)
    assert result.returncode and 'restore older versions' in result.stderr, result.stderr
    assert 'series/one.txt restores the version of %s' % git(fx.bringup, 'rev-parse', '--short=12', first + '^{commit}') in result.stderr or \
        'series/one.txt restores the version of ' in result.stderr, result.stderr
    assert 'mine.txt' not in result.stderr and fx.origin_tip() == before and fx.gate_runs() == runs
    passed('a landing that restores older versions of files is refused, naming the paths')
    # The same branch lands when the revert is deliberate and named.
    result = fx.land(w, '--restores', 'series/one.txt', '--restores', 'series/two.txt')
    assert result.returncode == 0, result.stderr
    assert fx.origin_tip() == git(w, 'rev-parse', 'HEAD') and (fx.bringup / 'series/one.txt').read_text() == 'v1 one.txt\n'
    passed('a deliberate revert lands with --restores')
    # A genuine new edit passes: content no earlier version had.
    w = fx.worker('edit')
    commit(w, 'series/one.txt', 'v3 one.txt\n', 'a new edit')
    result = fx.land(w)
    assert result.returncode == 0, result.stderr
    passed('a genuine new edit passes the stale-copy guard')
    # Speed: 167 stale paths are found by one bounded history walk.
    for number in range(167):
        commit(fx.bringup, 'bulk/f%03d.txt' % number, 'old %d\n' % number, 'bulk old')
    for number in range(167):
        commit(fx.bringup, 'bulk/f%03d.txt' % number, 'new %d\n' % number, 'bulk new')
    git(fx.bringup, 'push', '-q', 'origin', 'bringup')
    w = fx.worker('bulk')
    for number in range(167):
        (w / 'bulk').mkdir(exist_ok=True)
        (w / ('bulk/f%03d.txt' % number)).write_text('old %d\n' % number)
    git(w, 'add', '-A')
    git(w, 'commit', '-qm', 'stale bulk')
    started = time.monotonic()
    result = fx.land(w)
    elapsed = time.monotonic() - started
    assert result.returncode and result.stderr.count('restores the version of') == 167, result.stderr[-500:]
    print('measured: 167 stale paths refused in %.2f s including the merge' % elapsed)
    passed('167 stale paths are refused in %.1f s' % elapsed)


def docs_only(fx):
    """A branch that changes only docs/ runs the evidence and link checks and no gate; anything else outside docs/ gates."""
    w = fx.worker('docs')
    commit(w, 'docs/note.md', 'note\n', 'a note')
    commit(w, 'docs/evidence/x.jsonl', '{}\n', 'a record')
    runs, steps = fx.gate_runs(), len(fx.steps())
    result = fx.land(w, full_gate='%s %s' % (sys.executable, fx.root / 'full.py'))
    assert result.returncode == 0, result.stderr
    assert fx.gate_runs() == runs, 'a docs-only landing ran the gate'
    assert fx.steps()[steps:] == ['evidence validate', 'evidence sync', 'links'], fx.steps()
    assert fx.records()[-1]['gate'] == 'docs-only' and fx.origin_tip() == git(w, 'rev-parse', 'HEAD')
    assert not (fx.cache / 'full-started').exists(), 'a docs-only landing started the full suite'
    # A Markdown file outside docs/ is not documentation for this purpose, and one code file brings the gate back.
    for name, path in (('readme', 'README.md'), ('mixed', 'tools/helper.py')):
        w = fx.worker('docs-' + name)
        commit(w, 'docs/note-%s.md' % name, 'note\n', 'a note')
        commit(w, path, '# x\n', 'outside docs')
        result = fx.land(w)
        assert result.returncode == 0, result.stderr
        runs += 1
        assert fx.gate_runs() == runs
    passed('docs-only landing runs the evidence and link checks and no gate')


def background_full_gate(fx):
    """A code landing starts the full suite on the new tip and does not wait for it."""
    w = fx.worker('full-start')
    commit(w, 'full-start.txt', 'x\n', 'code')
    start = time.monotonic()
    result = fx.land(w, full_gate='%s %s' % (sys.executable, fx.root / 'full.py'))
    assert result.returncode == 0, result.stderr
    assert time.monotonic() - start < 5, 'the landing waited for the full suite'
    for _ in range(100):
        if (fx.cache / 'full-started').exists():
            break
        time.sleep(0.1)
    started = (fx.cache / 'full-started').read_text().split()
    assert 'bringup' in git(fx.bringup, 'rev-parse', '--abbrev-ref', 'HEAD') and str(fx.cache) in started, started
    passed('a code landing starts the full suite in the background')


def full_gate(fx):
    """The full gate on the bringup tip records its result; a failure names the failing steps and the commit range since the last pass."""
    def full():
        env = {**ENV, 'LAND_CHECK_CACHE': str(fx.cache), 'LAND_CHECK_BRINGUP': str(fx.bringup)}
        return subprocess.run([sys.executable, str(ROOT / 'tools/full-gate.py'), '--cache', str(fx.cache),
                               '--bringup-worktree', str(fx.bringup),
                               '--gate-command', '%s %s' % (sys.executable, fx.root / 'gate.py')],
                              capture_output=True, text=True, env=env)
    result = full()
    assert result.returncode == 0 and 'PASSED' in result.stdout, result.stdout + result.stderr
    assert '--full' in fx.gate_args()[-1] and '--affected' not in fx.gate_args()[-1]
    passing = git(fx.bringup, 'rev-parse', 'bringup')
    good = fx.worker('after-pass')
    commit(good, 'after-pass.txt', 'x\n', 'a landing after the pass')
    assert fx.land(good).returncode == 0
    commit(fx.bringup, 'bad.txt', 'x\n', 'a commit that breaks the gate')
    tip = git(fx.bringup, 'rev-parse', 'bringup')
    result = full()
    assert result.returncode == 1 and 'FAILED' in result.stdout, result.stdout + result.stderr
    assert 'failing steps: FAIL kernel' in result.stdout, result.stdout
    assert '%s..%s' % (passing[:8], tip[:8]) in result.stdout, result.stdout
    assert (fx.cache / 'full-gate.txt').read_text() == result.stdout
    lines = (fx.cache / 'full-gate.jsonl').read_text().splitlines()
    assert [json.loads(l)['passed'] for l in lines] == [True, False] and json.loads(lines[-1])['commit'] == tip
    # A second run while one holds the lock exits at once.
    with open(fx.cache / 'full-gate.lock', 'a+') as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        runs = fx.gate_runs()
        result = full()
    assert result.returncode == 0 and 'another full gate' in result.stdout and fx.gate_runs() == runs
    passed('full gate on bringup records the result and names the commit range since the last pass')


def gate_lock(root):
    """tools/integration-gate.py waits for a free gate slot; a landing may take slot 0; the full suite takes its own lock."""
    import select
    sys.path.insert(0, str(ROOT / 'tools'))
    import gate_slots
    source = root / 'src'
    source.mkdir(parents=True)
    (source / 'CMakeLists.txt').write_text('')
    cache = root / 'cache'
    (cache / 'build').mkdir(parents=True)
    gate = [sys.executable, str(ROOT / 'tools/integration-gate.py'), str(source), 'lock-check', '--cache', str(cache)]
    env = {**ENV, 'SUNSTONE_HOST_SPACE': 'off'}
    slots = gate_slots.SlotQueue(cache).slots
    handles = [open(cache / ('gate.lock.%d' % index), 'a+') for index in range(slots)]
    try:
        for handle in handles[1:]:                      # every slot a worker may use is held; slot 0 is the landings'
            fcntl.flock(handle, fcntl.LOCK_EX)
        waiting = subprocess.Popen(gate + ['--check', 'kernel'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        try:
            line = waiting.stdout.readline()
            assert 'waiting for a gate slot' in line, line
            assert waiting.poll() is None, 'the gate ran past held slots'
        finally:
            waiting.terminate()
            waiting.communicate()
        landing = subprocess.Popen(gate + ['--check', 'kernel'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                   env={**env, 'SUNSTONE_GATE_PRIORITY': 'landing'})
        try:
            ready, _, _ = select.select([landing.stdout], [], [], 20)
            first = landing.stdout.readline() if ready else ''
            assert 'waiting for' not in first, 'a landing waited behind the workers: ' + first
        finally:
            landing.terminate()
            landing.communicate()
        for handle in handles[:1]:
            fcntl.flock(handle, fcntl.LOCK_EX)
        full = subprocess.run(gate + ['--full'], capture_output=True, text=True, env=env)
        assert 'waiting for' not in full.stdout + full.stderr, 'the full suite waited behind the landing gate'
    finally:
        for handle in handles:
            handle.close()
    passed('every gate waits for a free slot; a landing takes slot 0; the full suite has its own lock')


def default_gate(root):
    """With no --gate-command the landing and the full gate start tools/integration-gate.py of the worktree they gate, with no wrapper."""
    import importlib.util
    import types
    spec = importlib.util.spec_from_file_location('land_default_gate', LAND)
    land = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(land)
    tree = root / 'tree'
    assert land.gate_argv(types.SimpleNamespace(gate_command=None), tree) == [sys.executable, str(tree / 'tools/integration-gate.py')]
    assert land.gate_argv(types.SimpleNamespace(gate_command=['gate.py']), tree) == ['gate.py']
    for name in ('tools/land.py', 'tools/full-gate.py'):
        assert 'integration-gate.sh' not in (ROOT / name).read_text(), name
    passed('the default gate command is tools/integration-gate.py of the gated worktree, not a wrapper script')


def union_merge(root):
    """Two branches that each append a record to one evidence log merge without a conflict, and the log validates."""
    root.mkdir()
    repo = root / 'repo'
    git(root, 'init', '-q', '-b', 'bringup', str(repo))
    for name in ('tools/evidence.py', 'docs/evidence/schema.json', '.gitattributes'):
        Path(repo, name).parent.mkdir(parents=True, exist_ok=True)
        Path(repo, name).write_text((ROOT / name).read_text())
    record = {"id": "base", "subsystem": "gpu", "hypothesis": "h", "outcome": "returned", "verdict": "passed",
              "measurements": [], "conditions": {}, "evidence": [], "narrative": "n"}
    log = repo / 'docs/evidence/gpu.jsonl'
    log.write_text(json.dumps(record) + '\n')
    git(repo, 'add', '-A')
    git(repo, 'commit', '-qm', 'base')
    for name in ('one', 'two'):
        git(repo, 'checkout', '-qb', name, 'bringup')
        with open(log, 'a') as stream:
            stream.write(json.dumps({**record, 'id': name}) + '\n')
        git(repo, 'commit', '-qam', name)
        git(repo, 'checkout', '-q', 'bringup')
    git(repo, 'merge', '-q', '--no-edit', 'one')
    merged = subprocess.run(['git', '-C', str(repo), 'merge', '--no-edit', 'two'], capture_output=True, text=True, env=ENV)
    assert merged.returncode == 0, 'the evidence logs conflicted: ' + merged.stdout
    assert [json.loads(l)['id'] for l in log.read_text().splitlines()] == ['base', 'one', 'two']
    done = subprocess.run([sys.executable, str(repo / 'tools/evidence.py'), '--root', str(repo), 'validate'],
                          capture_output=True, text=True, env=ENV)
    assert done.returncode == 0, done.stdout + done.stderr
    passed('union merge of two appended evidence records validates')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', type=Path, required=True, help='new scratch directory')
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True)
    run(arguments.output.resolve())
