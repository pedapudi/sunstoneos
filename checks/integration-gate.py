#!/usr/bin/env python3
"""Check integration selection, failure handling and evidence preservation with fake tools."""
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

    def run_gate(self, *args, label='case'):
        return subprocess.run([sys.executable, str(GATE), str(ROOT), label, '--cache', str(self.sandbox),
                               *args], env=self.env, capture_output=True, text=True)

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
        self.assertTrue(calls[0][2].endswith('gate-os-resaud'))

    def test_failure_is_not_retried_and_later_checks_do_not_run(self):
        self.env['GATE_TEST_FAIL'] = 'tools/check-mimus-smp.py'
        result = self.run_gate('--reason', 'Recognition scheduling', '--check', 'mimus-smp', '--check', 'progress')
        self.assertEqual(result.returncode, 1)
        runs = [c for c in self.records() if c[0] == 'python']
        self.assertEqual(len(runs), 1)
        self.assertEqual(runs[0][1], 'tools/check-mimus-smp.py')
        self.assertIn('host deadline exceeded', (self.sandbox / 'build/gate-case/check-mimus-smp.log').read_text())

    def test_configure_failure_prevents_checks(self):
        self.env['GATE_TEST_FAIL'] = str(ROOT)
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
        self.assertEqual(sum(c[0] == 'cfg.sh' for c in calls), 6)
        runs = [c for c in calls if c[0] == 'python']
        self.assertEqual(len(runs), 8)
        soak = next(c for c in runs if 'tools/check-smp.py' in c)
        self.assertEqual(soak[soak.index('--trials') + 1], '2')


if __name__ == '__main__':
    unittest.main()
