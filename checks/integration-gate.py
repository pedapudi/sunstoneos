#!/usr/bin/env python3
"""Check the integration gate with fake tools and a scratch Git checkout.

The cases cover selection, failure handling, evidence preservation,
per-checkout build directories and the dirty-tree identity."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

if not __debug__:
    raise SystemExit('checks require assertions')
ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / 'tools/integration-gate.py'


class GateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.sandbox = Path(self.temp.name)
        self.calls = self.sandbox / 'calls.jsonl'
        self.env = dict(os.environ, GATE_TEST_CALLS=str(self.calls), PYTHONDONTWRITEBYTECODE='1')
        # These tools only record arguments. No build, emulator or device is used.
        stub = '''#!/usr/bin/env python3
import json, os, pathlib, sys
program, *args = sys.argv
name = pathlib.Path(program).name
with open(os.environ['GATE_TEST_CALLS'], 'a') as f:
    f.write(json.dumps([name, *args]) + '\\n')
if '--target' in args and 'help' in args:
    print('... all\\n... clean\\n... kernel-check\\n... other-image\\n... ignored.o')
if os.environ.get('GATE_TEST_FAIL') in args:
    print('host deadline exceeded')
    sys.exit(1)
'''
        for relative in ('bin/cfg.sh', 'venv/bin/python', 'host/usr/lib/llvm-21/bin/cmake'):
            path = self.sandbox / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(stub)
            path.chmod(0o755)
        self.source = self.checkout('source')

    def checkout(self, name):
        """A committed scratch checkout; the gate only reads its Git state."""
        source = self.sandbox / name
        source.mkdir()
        (source / 'CMakeLists.txt').write_text('project(gate-test)\n')
        git = ['git', '-c', 'user.name=gate', '-c', 'user.email=gate@example.invalid']
        for command in (['init', '-q'], ['add', 'CMakeLists.txt'], ['commit', '-q', '-m', 'Gate test']):
            subprocess.run(git + command, cwd=source, check=True, capture_output=True)
        return source

    def run_gate(self, *args, label='case', source=None):
        return subprocess.run([sys.executable, str(GATE), str(source or self.source), label, '--cache', str(self.sandbox),
                               *args], env=self.env, capture_output=True, text=True)

    def tree(self, source):
        return source.name + '-' + hashlib.sha256(str(source.resolve()).encode()).hexdigest()[:10]

    def records(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()] if self.calls.exists() else []

    def test_no_default_suite(self):
        result = self.run_gate('--reason', 'No selection')
        self.assertEqual(result.returncode, 2)
        self.assertIn('select --target', result.stderr)
        self.assertEqual(self.records(), [])
        self.assertFalse((self.sandbox / 'build/gate-case').exists())

    def test_full_requires_reason(self):
        self.assertEqual(self.run_gate('--full').returncode, 2)
        self.assertEqual(self.records(), [])

    def test_source_check_needs_no_build_and_deduplicates(self):
        result = self.run_gate('--reason', 'PMIC source ownership', '--check', 'pmic-tree', '--check', 'pmic-tree')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.records(), [['python', 'checks/pmic-tree.py']])

    def test_selected_builds_only(self):
        result = self.run_gate('--reason', 'Changed kernel', '--target', 'kernel-check', '--check', 'kernel')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        self.assertEqual([c[0] for c in calls], ['cfg.sh', 'cmake', 'python'])
        self.assertEqual(calls[1][calls[1].index('--target') + 1:], ['kernel-check', '-j', '12'])
        self.assertEqual(calls[2][1], 'tools/check-kernel.py')

    def test_selected_variant_only(self):
        result = self.run_gate('--reason', 'Resident audio wiring', '--variant', 'resaud')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        self.assertEqual(len(calls), 2)
        self.assertIn('-DSUNSTONE_A733_OS_AUDIO=ON', calls[0])
        self.assertNotIn('-DSUNSTONE_A733_OS_RADIO=ON', calls[0])
        self.assertEqual(calls[0][2], str(self.sandbox / 'build' / ('gate-os-resaud-' + self.tree(self.source))))

    def test_build_directory_follows_the_source(self):
        other = self.checkout('other')
        for source, label in ((self.source, 'first'), (other, 'second')):
            result = self.run_gate('--reason', 'Kernel build', '--target', 'kernel-check', label=label, source=source)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        configured = [c[2] for c in self.records() if c[0] == 'cfg.sh']
        self.assertEqual(configured, [str(self.sandbox / 'build' / ('gate-central-' + self.tree(s))) for s in (self.source, other)])
        self.assertNotIn('first', configured[0])

    def test_dirty_source_is_refused_unless_allowed(self):
        (self.source / 'CMakeLists.txt').write_text('project(changed)\n')
        (self.source / 'new.c').write_text('')
        refused = self.run_gate('--reason', 'Dashboard edit', '--check', 'progress')
        self.assertEqual(refused.returncode, 2)
        self.assertIn('uncommitted changes', refused.stderr)
        self.assertEqual(self.records(), [])
        allowed = self.run_gate('--reason', 'Dashboard edit', '--check', 'progress', '--allow-dirty', label='dirty')
        self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)
        diff = subprocess.run(['git', 'diff', '--binary', 'HEAD'], cwd=self.source, capture_output=True, check=True).stdout
        summary = (self.sandbox / 'build/gate-dirty/summary.txt').read_text()
        self.assertIn('diff-sha256: ' + hashlib.sha256(diff).hexdigest(), summary)
        self.assertIn('status: 2 changed paths', summary)
        self.assertIn('?? new.c', (self.sandbox / 'build/gate-dirty/status.txt').read_text())

    def untracked_identity(self, *names):
        """The gate's untracked-file digest, computed independently."""
        digest = hashlib.sha256()
        for name in sorted(names):
            digest.update(name.encode() + b'\0' + hashlib.sha256((self.source / name).read_bytes()).digest())
        return '%s (%d files)' % (digest.hexdigest(), len(names))

    def test_dirty_identity_names_untracked_files(self):
        (self.source / '.gitignore').write_text('ignored.o\n')
        (self.source / 'ignored.o').write_text('first')
        (self.source / 'sub').mkdir()
        (self.source / 'sub/new.c').write_text('int first;\n')
        summaries = []
        for label, content in (('first', 'int first;\n'), ('second', 'int second;\n')):
            (self.source / 'sub/new.c').write_text(content)
            (self.source / 'ignored.o').write_text(label)
            result = self.run_gate('--reason', 'Dashboard edit', '--check', 'progress', '--allow-dirty', label=label)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            summary = (self.sandbox / 'build' / ('gate-' + label) / 'summary.txt').read_text()
            # The tracked diff is empty; only the untracked files distinguish the two trees.
            self.assertIn('diff-sha256: ' + hashlib.sha256(b'').hexdigest(), summary)
            self.assertIn('untracked-sha256: ' + self.untracked_identity('.gitignore', 'sub/new.c'), summary)
            summaries.append(summary)
        self.assertNotEqual(summaries[0], summaries[1])

    def test_change_before_the_lock_stops_the_gate(self):
        # A git wrapper adds an untracked file just before the status check
        # that the gate repeats after taking the lock.
        real = subprocess.run(['sh', '-c', 'command -v git'], capture_output=True, text=True, check=True).stdout.strip()
        wrapper = self.sandbox / 'wrapper/git'
        wrapper.parent.mkdir()
        wrapper.write_text(f'''#!/bin/sh
if [ "$1" = status ]; then
  count=$(cat "{self.sandbox}/status-count" 2>/dev/null || echo 0)
  echo $((count + 1)) > "{self.sandbox}/status-count"
  [ "$count" = 1 ] && echo late > late.c
fi
exec {real} "$@"
''')
        wrapper.chmod(0o755)
        self.env['PATH'] = str(wrapper.parent) + os.pathsep + self.env['PATH']
        result = self.run_gate('--reason', 'Kernel build', '--target', 'kernel-check')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertTrue((self.source / 'late.c').exists())
        self.assertIn('FAIL the source changed between its status check and the lock',
                      (self.sandbox / 'build/gate-case/summary.txt').read_text())
        self.assertEqual(self.records(), [])

    def test_clean_source_records_its_state(self):
        self.assertEqual(self.run_gate('--reason', 'Dashboard edit', '--check', 'progress').returncode, 0)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('status: clean', summary)
        self.assertIn('diff-sha256: ' + hashlib.sha256(b'').hexdigest(), summary)
        self.assertIn('untracked-sha256: ' + self.untracked_identity(), summary)
        self.assertFalse((self.sandbox / 'build/gate-case/status.txt').exists())

    def test_failure_is_not_retried_and_later_checks_do_not_run(self):
        self.env['GATE_TEST_FAIL'] = 'tools/check-mimus-smp.py'
        result = self.run_gate('--reason', 'Recognition scheduling', '--check', 'mimus-smp', '--check', 'progress')
        self.assertEqual(result.returncode, 1)
        runs = [c for c in self.records() if c[0] == 'python']
        self.assertEqual(len(runs), 1)
        self.assertEqual(runs[0][1], 'tools/check-mimus-smp.py')
        self.assertIn('host deadline exceeded', (self.sandbox / 'build/gate-case/check-mimus-smp.log').read_text())

    def test_configure_failure_prevents_checks(self):
        self.env['GATE_TEST_FAIL'] = str(self.source)
        self.assertEqual(self.run_gate('--reason', 'Kernel build', '--check', 'kernel').returncode, 1)
        self.assertEqual([c[0] for c in self.records()], ['cfg.sh'])

    def test_reused_label_preserves_evidence(self):
        args = ('--reason', 'Dashboard edit', '--check', 'progress')
        self.assertEqual(self.run_gate(*args).returncode, 0)
        before = (self.sandbox / 'build/gate-case/summary.txt').read_bytes()
        self.assertEqual(self.run_gate(*args).returncode, 2)
        self.assertEqual(len(self.records()), 1)
        self.assertEqual((self.sandbox / 'build/gate-case/summary.txt').read_bytes(), before)

    def test_explicit_full_suite_and_declared_trials(self):
        result = self.run_gate('--reason', 'Shared toolchain changed', '--full', '--trials', '2')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        central = next(c for c in calls if c[0] == 'cmake' and 'kernel-check' in c)
        self.assertIn('other-image', central)
        self.assertNotIn('clean', central)
        self.assertNotIn('ignored.o', central)
        self.assertEqual(sum(c[0] == 'cfg.sh' for c in calls), 10)
        runs = [c for c in calls if c[0] == 'python']
        self.assertEqual(len(runs), 8)
        soak = next(c for c in runs if 'tools/check-smp.py' in c)
        self.assertEqual(soak[soak.index('--trials') + 1], '2')


if __name__ == '__main__':
    unittest.main()
