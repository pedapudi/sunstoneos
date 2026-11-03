#!/usr/bin/env python3
"""tools/land.py against a temporary bare origin: landing by risk, refusals, approval and the lock.

Covers a code landing gated in affected mode and the full suite starting after it, the docs-only landing that
runs the evidence and link checks and no gate, a landing after bringup moved during the gate, the unmerged-path,
registry, failed-gate, protected-path, dirty-worktree, nothing-to-land and concurrent-lander refusals, the
coordinator approval of a protected path, tools/full-gate.py, the union merge of the evidence logs, and the
host-wide gate lock of tools/integration-gate.py.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

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
    default_gate(root / 'defaultgate')


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
    """tools/integration-gate.py waits for the host-wide lock; the full suite takes its own."""
    source = root / 'src'
    source.mkdir(parents=True)
    (source / 'CMakeLists.txt').write_text('')
    cache = root / 'cache'
    (cache / 'build').mkdir(parents=True)
    gate = [sys.executable, str(ROOT / 'tools/integration-gate.py'), str(source), 'lock-check', '--cache', str(cache)]
    env = {**ENV, 'SUNSTONE_HOST_SPACE': 'off'}
    with open(cache / 'gate.lock', 'a+') as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        waiting = subprocess.Popen(gate + ['--check', 'kernel'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        try:
            line = waiting.stdout.readline()
            assert 'waiting for' in line and 'gate.lock' in line, line
            assert waiting.poll() is None, 'the gate ran past a held lock'
        finally:
            waiting.terminate()
            waiting.communicate()
        full = subprocess.run(gate + ['--full'], capture_output=True, text=True, env=env)
        assert 'waiting for' not in full.stdout + full.stderr, 'the full suite waited behind the landing gate'
    passed('every gate waits for the host-wide gate lock; the full suite has its own')


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
