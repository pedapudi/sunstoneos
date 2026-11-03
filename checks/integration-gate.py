#!/usr/bin/env python3
"""Check the integration gate with fake tools and a scratch Git checkout.

The cases cover selection, failure handling, evidence preservation,
per-checkout build directories and the dirty-tree identity."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import importlib.util

if not __debug__:
    raise SystemExit('checks require assertions')
ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / 'tools/integration-gate.py'
OUTPUT = None


class GateTests(unittest.TestCase):
    def setUp(self):
        self.sandbox = Path(tempfile.mkdtemp(prefix='gate-', dir=OUTPUT))
        self.calls = self.sandbox / 'calls.jsonl'
        self.env = dict(os.environ, GATE_TEST_CALLS=str(self.calls), GATE_TEST_TIMES=str(self.sandbox / 'times.jsonl'),
                        PYTHONDONTWRITEBYTECODE='1', SUNSTONE_HOST_SPACE='off')   # a sandbox in a scratch directory (tools/host_space.py)
        self.env.pop('EXAMPLE_DIR', None)
        self.env.pop('SUNSTONE_EXTERNAL_APPLICATIONS', None)
        # These tools only record arguments. No build, emulator or device is used.
        stub = '''#!/usr/bin/env python3
import json, os, pathlib, sys, time
program, *args = sys.argv
name = pathlib.Path(program).name
with open(os.environ['GATE_TEST_CALLS'], 'a') as f:
    f.write(json.dumps([name, *args]) + '\\n')
began = time.time()
if os.environ.get('GATE_TEST_SLEEP') and args and args[0].startswith(('checks/', 'tools/')):
    time.sleep(float(os.environ['GATE_TEST_SLEEP']))
    with open(os.environ['GATE_TEST_TIMES'], 'a') as f:
        f.write(json.dumps([args[0], began, time.time()]) + '\\n')
if '--target' in args and 'help' in args:
    print('... all\\n... clean\\n... kernel-check\\n... other-image\\n... ignored.o')
if os.environ.get('GATE_TEST_TOUCH'):
    pathlib.Path(os.environ['GATE_TEST_TOUCH']).write_text('written during a step\\n')
if os.environ.get('GATE_TEST_FAIL') in args or any(x in args for x in os.environ.get('GATE_TEST_FAIL_ALL', '').split(',') if x):
    print(os.environ.get('GATE_TEST_OUTPUT', 'host deadline exceeded'))
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
        record = source / 'apps/example/source.json'
        record.parent.mkdir(parents=True)
        record.write_text('{"application_directory": "sunstoneos"}\n')
        selector = source / 'checks/affected.py'
        selector.parent.mkdir()
        selector.write_text('''from types import SimpleNamespace
import subprocess
from pathlib import Path
import json, os
indexer = SimpleNamespace(PROSE_CHECKS=json.loads(os.environ.get('GATE_TEST_PROSE', '{}')),
                          SERIAL_CHECKS=set(os.environ.get('GATE_TEST_SERIAL', '').split(',')), generate=lambda: {
    'focused.py': {'pure_host': True, 'required_arguments': ['--output']},
    'progress.py': {'pure_host': True, 'required_arguments': []},
    'pre-push-hook.py': {'pure_host': True, 'required_arguments': ['--output']},
    'image.py': {'pure_host': False, 'library': False, 'required_arguments': ['--image']}})
def needs(entry):
    return ['--image']
def plan(value=None, changed=None, index=None):
    if value == 'HEAD^..HEAD':
        changed = subprocess.check_output(['git', 'diff', '--name-only', 'HEAD^', 'HEAD'],
                                          cwd=Path(__file__).resolve().parents[1], text=True).splitlines()
    checks = [{'check': 'focused.py', 'status': 'runnable', 'needs': [], 'output': True},
              {'check': 'progress.py', 'status': 'runnable', 'needs': [], 'output': False}]
    if changed == ['pending']:
        checks.append({'check': 'image.py', 'status': 'pending', 'needs': ['--image'], 'output': False})
    if set(changed or []) & {'tools/git-hooks/pre-push', 'checks/harness.py', 'checks/pre-push-hook.py', 'untracked-hook-input.txt'}:
        checks.append({'check': 'pre-push-hook.py', 'status': 'runnable', 'needs': [], 'output': True})
    return {'baseline': 'sup', 'range': value, 'changed': changed, 'checks': checks}
''')
        (selector.parent / 'gate-inputs.json').write_text('{"images": {}, "bundles": {}, "checks": {}, "board": [], "waived": {}}\n')
        (selector.parent / 'pre-push-hook.py').write_text('CHECK_INPUTS = ("untracked-hook-input.txt",)\nprint("fake hook fixture")\n')
        hook = source / 'tools/git-hooks/pre-push'
        hook.parent.mkdir(parents=True)
        hook.write_text('# fake hook\n')
        git = ['git', '-c', 'user.name=gate', '-c', 'user.email=gate@example.invalid']
        for command in (['init', '-q'], ['add', '.'], ['commit', '-q', '-m', 'Gate test']):
            subprocess.run(git + command, cwd=source, check=True, capture_output=True)
        return source

    def run_gate(self, *args, label='case', source=None):
        return subprocess.run([sys.executable, str(GATE), str(source or self.source), label, '--cache', str(self.sandbox),
                               '--min-available-gb', '0', *args], env=self.env, capture_output=True, text=True)

    def tree(self, source):
        return source.name + '-' + hashlib.sha256(str(source.resolve()).encode()).hexdigest()[:10]

    def enable_example(self):
        self.example = self.checkout('example')
        port = self.example / 'sunstoneos'
        port.mkdir()
        (port / 'CMakeLists.txt').write_text('add_sunstone_application(example)\n')
        (port / 'gate.json').write_text(json.dumps({'name': 'example', 'checks': {'example-smp': ['smp-example-model', [
            '{application}/sunstoneos/tools/check-example-smp.py', '--image', '{build}/smp-example-model.elf',
            '--qemu', '{qemu}', '--evidence', '{evidence}/example-smp']]}}))
        subprocess.run(['git', 'add', '.'], cwd=self.example, check=True, capture_output=True)
        subprocess.run(['git', '-c', 'user.name=gate', '-c', 'user.email=gate@example.invalid',
                        'commit', '-q', '-m', 'Add application'], cwd=self.example, check=True, capture_output=True)
        self.env['EXAMPLE_DIR'] = str(self.example)
        return self.example / 'sunstoneos/tools/check-example-smp.py'

    def records(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()] if self.calls.exists() else []

    def test_no_default_suite(self):
        result = self.run_gate('--reason', 'No selection')
        self.assertEqual(result.returncode, 2)
        self.assertIn('select --target', result.stderr)
        self.assertEqual(self.records(), [])
        self.assertFalse((self.sandbox / 'build/gate-case').exists())

    def test_reason_is_optional_metadata(self):
        result = self.run_gate('--check', 'progress')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.records(), [['python', 'checks/progress.py', '--output',
                                         str(self.sandbox / 'build/gate-case/evidence/progress')]])

    def test_source_check_needs_no_build_and_deduplicates(self):
        result = self.run_gate('--reason', 'PMIC source ownership', '--check', 'pmic-tree', '--check', 'pmic-tree')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.records(), [['python', 'checks/pmic-tree.py']])

    def test_selected_builds_only(self):
        result = self.run_gate('--reason', 'Changed kernel', '--target', 'kernel-check', '--check', 'kernel')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        self.assertEqual([c[0] for c in calls], ['cfg.sh', 'cmake', 'python'])
        # The OS-core build overrides the EXAMPLE_DIR that cfg.sh passes.
        self.assertEqual(calls[0][-1], '-DEXAMPLE_DIR=')
        self.assertEqual(calls[1][calls[1].index('--target') + 1:], ['kernel-check', '-j', '12'])
        self.assertEqual(calls[2][1], 'tools/check-kernel.py')

    def test_application_check_uses_its_own_build(self):
        checker = self.enable_example()
        result = self.run_gate('--reason', 'Application scheduling', '--check', 'kernel', '--check', 'example-smp')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        self.assertEqual([c[0] for c in calls], ['cfg.sh', 'cmake', 'cfg.sh', 'cmake', 'python', 'python'])
        core = self.sandbox / 'build' / ('gate-central-' + self.tree(self.source))
        example = self.sandbox / 'build' / ('gate-example-' + self.tree(self.source))
        self.assertEqual(calls[0][1:], [str(self.source), str(core), '-DSUNSTONE_APPLICATION=', '-DSUNSTONE_EXTERNAL_APPLICATIONS=', '-DEXAMPLE_DIR='])
        self.assertEqual(calls[1][calls[1].index('--target') + 1:], ['kernel-check', '-j', '12'])
        self.assertEqual(calls[2][1:], [str(self.source), str(example), '-DSUNSTONE_EXTERNAL_APPLICATIONS=', '-DEXAMPLE_DIR=',
                                      '-DSUNSTONE_EXTERNAL_APPLICATIONS=' + str(self.example),
                                      '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=example'])
        self.assertEqual(calls[3][calls[3].index('--target') + 1:],
                         ['sunstone-a733-os', 'smp-example-model', '-j', '12'])
        self.assertEqual(calls[4][1], 'tools/check-kernel.py')
        self.assertEqual(calls[5][calls[5].index('--image') + 1], str(example / 'smp-example-model.elf'))
        self.assertEqual(calls[5][1], str(checker))

    def test_provenance_record_is_not_an_application_alias(self):
        record = self.source / 'apps/pcie-test/source.json'
        record.parent.mkdir(parents=True)
        record.write_text('{"repository": "https://example.invalid/reference"}\n')
        subprocess.run(['git', 'add', '.'], cwd=self.source, check=True, capture_output=True)
        subprocess.run(['git', '-c', 'user.name=gate', '-c', 'user.email=gate@example.invalid',
                        'commit', '-q', '-m', 'Add provenance record'], cwd=self.source, check=True, capture_output=True)
        self.env['PCIE_TEST_DIR'] = str(self.sandbox / 'not-an-application')
        result = self.run_gate('--reason', 'Application alias admission', '--check', 'kernel')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(any('PCIE_TEST_DIR' in arg for call in self.records() for arg in call))

    def test_multiple_external_applications_keep_separate_builds(self):
        self.enable_example()
        second = self.checkout('second')
        port = second / 'sunstoneos'
        port.mkdir()
        record = json.loads((self.example / 'sunstoneos/gate.json').read_text())
        record = json.loads(json.dumps(record).replace('example', 'second'))
        (port / 'gate.json').write_text(json.dumps(record))
        subprocess.run(['git', 'add', '.'], cwd=second, check=True, capture_output=True)
        subprocess.run(['git', '-c', 'user.name=gate', '-c', 'user.email=gate@example.invalid',
                        'commit', '-q', '-m', 'Add second application'], cwd=second, check=True, capture_output=True)
        self.env.pop('EXAMPLE_DIR')
        self.env['SUNSTONE_EXTERNAL_APPLICATIONS'] = str(self.example) + ';' + str(second)
        result = self.run_gate('--reason', 'Separate external applications',
                               '--check', 'example-smp', '--check', 'second-smp')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        configurations = [call for call in calls if call[0] == 'cfg.sh']
        self.assertEqual(len(configurations), 2)
        self.assertNotEqual(configurations[0][2], configurations[1][2])
        for call, name, checkout in zip(configurations, ('example', 'second'), (self.example, second)):
            self.assertIn('-DSUNSTONE_APPLICATION=' + name, call)
            self.assertIn('-DSUNSTONE_EXTERNAL_APPLICATIONS=' + str(checkout), call)
        scripts = [call[1] for call in calls if call[0] == 'python']
        self.assertEqual(scripts, [str(self.example / 'sunstoneos/tools/check-example-smp.py'),
                                  str(second / 'sunstoneos/tools/check-second-smp.py')])

    def test_malformed_application_is_refused_before_any_step(self):
        self.enable_example()
        (self.example / 'sunstoneos/gate.json').write_text('{"name": "../outside"}')
        result = self.run_gate('--reason', 'External manifest admission', '--check', 'kernel')
        self.assertEqual(result.returncode, 2)
        self.assertIn('application names', result.stderr)
        self.assertEqual(self.records(), [])

    def test_application_check_requires_external_checkout(self):
        result = self.run_gate('--reason', 'Application scheduling', '--check', 'example-smp')
        self.assertEqual(result.returncode, 2)
        self.assertIn('external checks require', result.stderr)
        self.assertEqual(self.records(), [])

    def test_os_variant_build_precedes_external_application(self):
        self.enable_example()
        result = self.run_gate('--reason', 'OS and optional application composition',
                               '--variant', 'sup', '--check', 'example-smp')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        configured = [call for call in calls if call[0] == 'cfg.sh']
        self.assertEqual(len(configured), 2)
        self.assertIn('-DEXAMPLE_DIR=', configured[0])
        self.assertIn('-DSUNSTONE_APPLICATION=', configured[0])
        self.assertIn('-DSUNSTONE_APPLICATION=example', configured[1])

    def test_selected_variant_only(self):
        result = self.run_gate('--reason', 'Resident audio wiring', '--variant', 'resaud')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        self.assertEqual(len(calls), 2)
        self.assertIn('-DSUNSTONE_A733_OS_AUDIO=ON', calls[0])
        self.assertNotIn('-DSUNSTONE_A733_OS_RADIO=ON', calls[0])
        self.assertIn('-DEXAMPLE_DIR=', calls[0])
        self.assertIn('-DSUNSTONE_APPLICATION=', calls[0])
        self.assertEqual(calls[0][2], str(self.sandbox / 'build' / ('gate-os-resaud-' + self.tree(self.source))))

    def test_affected_runs_one_baseline_and_deduplicates_core_checks(self):
        result = self.run_gate('--affected', 'HEAD~1..HEAD', '--check', 'progress')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        self.assertEqual(sum(call[0] == 'cfg.sh' for call in calls), 1)
        self.assertIn('-DSUNSTONE_A733_OS=ON', calls[0])
        runs = [call for call in calls if call[0] == 'python']
        self.assertEqual([call[1] for call in runs], ['checks/progress.py', 'checks/focused.py'])
        self.assertIn('--output', runs[1])
        self.assertTrue((self.sandbox / 'build/gate-case/affected.json').is_file())

    def test_pending_affected_checks_are_reported_and_incomplete(self):
        result = self.run_gate('--changed', 'pending')
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('PENDING checks/image.py: --image', result.stdout)
        self.assertIn('INCOMPLETE affected coverage', result.stdout)
        self.assertFalse(any('checks/image.py' in call for call in self.records()))

    def commit_source(self, name):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('changed\n')
        subprocess.run(['git', 'add', '.'], cwd=self.source, check=True, capture_output=True)
        subprocess.run(['git', '-c', 'user.name=gate', '-c', 'user.email=gate@example.invalid',
                        'commit', '-qm', 'Change ' + name], cwd=self.source, check=True, capture_output=True)

    def test_full_hook_fixture_uses_explicit_changed_inputs(self):
        catalogue = self.sandbox / 'build/npu-board/session-artifacts.json'
        catalogue.parent.mkdir(parents=True, exist_ok=True)
        catalogue.write_text('{}\n')
        for number, (changed, expected) in enumerate((('drivers/ordinary.c', False),
                                                     ('tools/git-hooks/pre-push', True),
                                                     ('checks/harness.py', True),
                                                     ('checks/pre-push-hook.py', True))):
            with self.subTest(changed=changed):
                before = len(self.records())
                result = self.run_gate('--full', '--changed', changed, label='scope-' + str(number))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                runs = [call for call in self.records()[before:] if call[0] == 'python']
                self.assertEqual(sum('checks/pre-push-hook.py' in call for call in runs), int(expected))
                self.assertEqual(sum('checks/focused.py' in call for call in runs), 1)
                self.assertEqual(sum('checks/progress.py' in call for call in runs), 1)
                self.assertEqual(len(runs), 8 + int(expected))

    def test_full_hook_fixture_default_scope_and_explicit_range(self):
        catalogue = self.sandbox / 'build/npu-board/session-artifacts.json'
        catalogue.parent.mkdir(parents=True, exist_ok=True)
        catalogue.write_text('{}\n')
        # A root commit selects its tracked hook fixture; later defaults use the
        # latest first-parent change instead of treating every full check as changed.
        for label, changed, expected, explicit in (('root', None, True, False),
                                                  ('driver', 'drivers/ordinary.c', False, False),
                                                  ('harness', 'checks/harness.py', True, True)):
            with self.subTest(case=label):
                if changed:
                    self.commit_source(changed)
                before = len(self.records())
                selection = ['--full', *(['--affected', 'HEAD^..HEAD'] if explicit else [])]
                result = self.run_gate(*selection, label=label)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                runs = [call for call in self.records()[before:] if call[0] == 'python']
                self.assertEqual(sum('checks/pre-push-hook.py' in call for call in runs), int(expected))
                self.assertEqual(len(runs), 8 + int(expected))
                scope = json.loads((self.sandbox / 'build' / ('gate-' + label) / 'hook-scope.json').read_text())
                self.assertEqual(scope['selected'], expected)
                if label == 'root':
                    self.assertEqual(scope['basis'], 'all tracked files at the root commit')
                else:
                    self.assertEqual(scope['plan']['range'], 'HEAD^..HEAD')

    def test_full_default_scope_includes_local_hook_inputs(self):
        catalogue = self.sandbox / 'build/npu-board/session-artifacts.json'
        catalogue.parent.mkdir(parents=True, exist_ok=True)
        catalogue.write_text('{}\n')
        self.commit_source('drivers/ordinary.c')
        for number, (changed, expected) in enumerate((('unrelated-note.txt', False),
                                                     ('tools/git-hooks/pre-push', True),
                                                     ('checks/pre-push-hook.py', True),
                                                     ('untracked-hook-input.txt', True))):
            with self.subTest(changed=changed):
                path = self.source / changed
                original = path.read_text() if path.exists() else None
                path.write_text('local fixture change\n')
                before = len(self.records())
                result = self.run_gate('--full', label='local-' + str(number))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                runs = [call for call in self.records()[before:] if call[0] == 'python']
                self.assertEqual(sum('checks/pre-push-hook.py' in call for call in runs), int(expected))
                self.assertEqual(len(runs), 8 + int(expected))
                output = self.sandbox / 'build' / ('gate-local-' + str(number))
                scope = json.loads((output / 'hook-scope.json').read_text())
                self.assertIn(changed, scope['dirty_paths'])
                self.assertEqual(scope['selected'], expected)
                if original is not None:
                    path.write_text(original)

    def test_full_default_scope_keeps_the_original_renamed_hook_path(self):
        catalogue = self.sandbox / 'build/npu-board/session-artifacts.json'
        catalogue.parent.mkdir(parents=True, exist_ok=True)
        catalogue.write_text('{}\n')
        self.commit_source('drivers/ordinary.c')
        old, new = 'tools/git-hooks/pre-push', 'tools/git-hooks/renamed-hook'
        subprocess.run(['git', 'mv', old, new], cwd=self.source, check=True, capture_output=True)
        result = self.run_gate('--full')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        runs = [call for call in self.records() if call[0] == 'python']
        self.assertEqual(sum('checks/pre-push-hook.py' in call for call in runs), 1)
        self.assertEqual(len(runs), 9)
        scope = json.loads((self.sandbox / 'build/gate-case/hook-scope.json').read_text())
        self.assertTrue({old, new} <= set(scope['dirty_paths']))
        self.assertTrue(scope['selected'])

    def test_external_name_collision_keeps_the_indexed_os_check(self):
        checker = self.enable_example()
        record = self.example / 'sunstoneos/gate.json'
        app = json.loads(record.read_text())
        app['checks']['focused'] = app['checks'].pop('example-smp')
        app['checks']['focused'][1][-1] = '{evidence}/focused'
        record.write_text(json.dumps(app))
        catalogue = self.sandbox / 'build/npu-board/session-artifacts.json'
        catalogue.parent.mkdir(parents=True, exist_ok=True)
        catalogue.write_text('{}\n')
        for label, selection in (('affected', ['--affected', 'HEAD~1..HEAD', '--check', 'focused']),
                                 ('full', ['--full'])):
            with self.subTest(selection=label):
                before = len(self.records())
                result = self.run_gate(*selection, label=label)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                runs = [call for call in self.records()[before:] if call[0] == 'python']
                self.assertEqual(sum('checks/focused.py' in call for call in runs), 1)
                self.assertEqual(sum(str(checker) in call for call in runs), 1)
                output = self.sandbox / 'build' / ('gate-' + label)
                self.assertTrue((output / 'check-focused.log').is_file())
                self.assertTrue((output / 'indexed-focused.log').is_file())
                indexed = next(call for call in runs if 'checks/focused.py' in call)
                external = next(call for call in runs if str(checker) in call)
                self.assertEqual(indexed[-1], str(output / 'evidence/indexed/focused'))
                self.assertEqual(external[-1], str(output / 'evidence/focused'))

    def test_npu_variant_names_the_cached_catalogue(self):
        refused = self.run_gate('--reason', 'OS NPU profile', '--variant', 'npu')
        self.assertEqual(refused.returncode, 2)
        self.assertIn('NPU catalogue map', refused.stderr)
        self.assertEqual(self.records(), [])
        catalogue = self.sandbox / 'build/npu-board/session-artifacts.json'
        catalogue.parent.mkdir(parents=True, exist_ok=True)
        catalogue.write_text('{}\n')
        result = self.run_gate('--reason', 'OS NPU profile', '--variant', 'npu', label='npu')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('npu-catalogue-sha256: ' + hashlib.sha256(b'{}\n').hexdigest(),
                      (self.sandbox / 'build/gate-npu/summary.txt').read_text())
        calls = self.records()
        self.assertEqual(len(calls), 2)
        self.assertIn('-DSUNSTONE_A733_OS_NPU=ON', calls[0])
        self.assertIn('-DSUNSTONE_OS_NPU_CATALOGUE=' + str(self.sandbox / 'build/npu-board/session-artifacts.json'),
                      calls[0])

    def test_build_directory_follows_the_source(self):
        other = self.checkout('other')
        for source, label in ((self.source, 'first'), (other, 'second')):
            result = self.run_gate('--reason', 'Kernel build', '--target', 'kernel-check', label=label, source=source)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        configured = [c[2] for c in self.records() if c[0] == 'cfg.sh']
        self.assertEqual(configured, [str(self.sandbox / 'build' / ('gate-central-' + self.tree(s))) for s in (self.source, other)])
        self.assertNotIn('first', configured[0])

    def test_dirty_source_is_recorded_without_permission_flag(self):
        (self.source / 'CMakeLists.txt').write_text('project(changed)\n')
        (self.source / 'new.c').write_text('')
        allowed = self.run_gate('--reason', 'Dashboard edit', '--check', 'progress', label='dirty')
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

    def wrap_git(self, command, index, action):
        """Put a git wrapper first on the gate's PATH that runs the shell
        action, in the source checkout, just before the index-th (from 0)
        invocation of the git subcommand command. $REAL names the real git."""
        real = subprocess.run(['sh', '-c', 'command -v git'], capture_output=True, text=True, check=True).stdout.strip()
        wrapper = self.sandbox / 'wrapper/git'
        wrapper.parent.mkdir()
        counter = self.sandbox / ('count-' + command)
        wrapper.write_text(f'''#!/bin/sh
REAL={real}
if [ "$1" = {command} ]; then
  count=$(cat "{counter}" 2>/dev/null || echo 0)
  echo $((count + 1)) > "{counter}"
  if [ "$count" = {index} ]; then {action}; fi
fi
exec "$REAL" "$@"
''')
        wrapper.chmod(0o755)
        self.env['PATH'] = str(wrapper.parent) + os.pathsep + self.env['PATH']

    def assert_changed(self, result, when):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('FAIL the source revision, status, diff or untracked files changed or could not be read '
                      + when, summary)
        self.assertNotIn('PASS selected integration checks', summary)

    def test_change_before_the_lock_stops_the_gate(self):
        # Status runs before the refusal, in the recorded state and again after the lock.
        self.wrap_git('status', 2, 'echo late > late.c')
        result = self.run_gate('--reason', 'Kernel build', '--target', 'kernel-check')
        self.assertTrue((self.source / 'late.c').exists())
        self.assert_changed(result, 'between the status check and the lock')
        self.assertEqual(self.records(), [])

    def test_head_move_before_the_lock_stops_the_gate(self):
        # A new commit leaves the tree clean; only the revision changes.
        before = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=self.source, capture_output=True, text=True).stdout
        self.wrap_git('rev-parse', 1,
                      '"$REAL" -c user.name=gate -c user.email=gate@example.invalid commit -q --allow-empty -m moved')
        result = self.run_gate('--reason', 'Kernel build', '--target', 'kernel-check')
        after = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=self.source, capture_output=True, text=True).stdout
        self.assertNotEqual(before, after)
        self.assertIn('revision: ' + before.strip(), (self.sandbox / 'build/gate-case/summary.txt').read_text())
        self.assert_changed(result, 'between the status check and the lock')
        self.assertEqual(self.records(), [])

    def test_change_during_the_steps_fails_the_gate(self):
        # The fake build writes an untracked file into the checkout it builds.
        self.env['GATE_TEST_TOUCH'] = str(self.source / 'generated.c')
        result = self.run_gate('--reason', 'Kernel build', '--target', 'kernel-check')
        self.assertEqual([c[0] for c in self.records()], ['cfg.sh', 'cmake'])
        self.assert_changed(result, 'during the selected steps')

    def test_unreadable_untracked_file_fails_the_gate(self):
        if os.geteuid() == 0:
            self.skipTest('root reads every file')
        hidden = self.source / 'hidden.c'
        hidden.write_text('int hidden;\n')
        hidden.chmod(0)
        self.addCleanup(hidden.chmod, 0o644)
        result = self.run_gate('--reason', 'Dashboard edit', '--check', 'progress', '--allow-dirty')
        self.assertIn('untracked-sha256: unavailable', (self.sandbox / 'build/gate-case/summary.txt').read_text())
        self.assert_changed(result, 'between the status check and the lock')
        self.assertEqual(self.records(), [])

    def test_clean_source_records_its_state(self):
        self.assertEqual(self.run_gate('--reason', 'Dashboard edit', '--check', 'progress').returncode, 0)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('status: clean', summary)
        self.assertIn('diff-sha256: ' + hashlib.sha256(b'').hexdigest(), summary)
        self.assertIn('untracked-sha256: ' + self.untracked_identity(), summary)
        self.assertFalse((self.sandbox / 'build/gate-case/status.txt').exists())

    def test_failure_is_not_retried_and_later_checks_do_not_run(self):
        checker = str(self.enable_example())
        self.env['GATE_TEST_FAIL'] = checker
        result = self.run_gate('--reason', 'Application scheduling', '--check', 'example-smp', '--check', 'progress')
        self.assertEqual(result.returncode, 1)
        runs = [c for c in self.records() if c[0] == 'python']
        self.assertEqual(len(runs), 1)
        self.assertEqual(runs[0][1], checker)
        self.assertIn('host deadline exceeded', (self.sandbox / 'build/gate-case/check-example-smp.log').read_text())

    def test_configure_failure_prevents_checks(self):
        self.env['GATE_TEST_FAIL'] = str(self.source)
        self.assertEqual(self.run_gate('--reason', 'Kernel build', '--check', 'kernel').returncode, 1)
        self.assertEqual([c[0] for c in self.records()], ['cfg.sh'])

    def test_reused_label_preserves_evidence(self):
        args = ('--reason', 'Dashboard edit', '--check', 'progress')
        self.assertEqual(self.run_gate(*args).returncode, 0)
        before = (self.sandbox / 'build/gate-case/summary.txt').read_bytes()
        self.assertEqual(self.run_gate(*args).returncode, 0)
        self.assertEqual(len(self.records()), 2)
        children = list((self.sandbox / 'build/gate-case').glob('*/summary.txt'))
        self.assertEqual(len(children), 1)
        self.assertIn('PASS selected integration checks', children[0].read_text())
        self.assertEqual((self.sandbox / 'build/gate-case/summary.txt').read_bytes(), before)

    def test_explicit_full_suite_and_declared_trials(self):
        self.enable_example()
        catalogue = self.sandbox / 'build/npu-board/session-artifacts.json'
        catalogue.parent.mkdir(parents=True, exist_ok=True)
        catalogue.write_text('{}\n')
        result = self.run_gate('--reason', 'Shared toolchain changed', '--full', '--trials', '2')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        central = next(c for c in calls if c[0] == 'cmake' and 'kernel-check' in c)
        self.assertIn('other-image', central)
        self.assertNotIn('clean', central)
        self.assertNotIn('ignored.o', central)
        self.assertEqual(sum(c[0] == 'cfg.sh' for c in calls), 16)  # core, example and fourteen OS variants
        application = next(c for c in calls if c[0] == 'cmake' and 'smp-example-model' in c)
        # The fake lists the same targets in both builds; example builds only its own.
        self.assertEqual(application[application.index('--target') + 1:],
                         ['sunstone-a733-os', 'smp-example-model', '-j', '12'])
        runs = [c for c in calls if c[0] == 'python']
        self.assertEqual(len(runs), 10)
        self.assertEqual(sum('checks/focused.py' in call for call in runs), 1)
        self.assertEqual(sum('checks/progress.py' in call for call in runs), 1)
        soak = next(c for c in runs if 'tools/check-smp.py' in c)
        self.assertEqual(soak[soak.index('--trials') + 1], '2')

    def test_full_without_example_builds_only_os(self):
        catalogue = self.sandbox / 'build/npu-board/session-artifacts.json'
        catalogue.parent.mkdir(parents=True, exist_ok=True)
        catalogue.write_text('{}\n')
        result = self.run_gate('--reason', 'Independent OS milestone', '--full')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.records()
        self.assertFalse(any('smp-example-model' in call for call in calls))
        self.assertEqual(sum(c[0] == 'cfg.sh' for c in calls), 15)  # core and fourteen OS variants
        self.assertTrue(all('-DEXAMPLE_DIR=' in c for c in calls if c[0] == 'cfg.sh'))

    def test_external_change_during_steps_fails(self):
        self.enable_example()
        self.env['GATE_TEST_TOUCH'] = str(self.example / 'changed.cc')
        result = self.run_gate('--reason', 'External application changed', '--check', 'example-smp')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('FAIL external example changed', result.stdout)

    def test_build_root_keeps_new_directories_together(self):
        directory = self.sandbox / 'task-builds'
        result = self.run_gate('--reason', 'Isolated build', '--target', 'kernel-check',
                               '--build-root', str(directory))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((directory / 'gate-case/summary.txt').is_file())
        self.assertEqual(self.records()[0][2], str(directory / ('gate-central-' + self.tree(self.source))))


    def full_catalogue(self):
        catalogue = self.sandbox / 'build/npu-board/session-artifacts.json'
        catalogue.parent.mkdir(parents=True, exist_ok=True)
        catalogue.write_text('{}\n')

    def intervals(self):
        path = self.sandbox / 'times.jsonl'
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    @staticmethod
    def overlap(a, b):
        return a[1] < b[2] and b[1] < a[2]

    def test_full_gate_keeps_going_and_reports_every_failure(self):
        self.full_catalogue()
        self.env['GATE_TEST_FAIL_ALL'] = 'checks/pmic-tree.py,checks/check-index.py'
        result = self.run_gate('--reason', 'Two failures', '--full')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('mode: keep-going', summary)
        self.assertIn('FAIL check-pmic-tree', summary)
        self.assertIn('FAIL check-check-index', summary)
        self.assertIn('PASS check-power-domains-tree', summary)
        self.assertIn('PASS indexed-focused', summary)
        self.assertIn('steps:', summary)
        self.assertIn('FAIL selected integration checks: check-check-index, check-pmic-tree', summary)
        self.assertNotIn('PASS selected', summary)
        self.assertNotIn('Stopped at first failure', summary)
        self.assertTrue((self.sandbox / 'build/gate-case/timing.txt').is_file())

    def test_fail_fast_stops_at_the_first_failure_in_order(self):
        self.full_catalogue()
        self.env['GATE_TEST_FAIL_ALL'] = 'checks/pmic-tree.py,checks/check-index.py'
        result = self.run_gate('--reason', 'Stop early', '--full', '--fail-fast')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('mode: fail-fast', summary)
        self.assertIn('FAIL check-check-index', summary)
        self.assertIn('Stopped at first failure', summary)
        self.assertNotIn('check-pmic-tree', summary)
        self.assertNotIn('indexed-focused', summary)

    def test_targeted_selection_can_keep_going(self):
        self.env['GATE_TEST_FAIL_ALL'] = 'checks/pmic-tree.py'
        result = self.run_gate('--check', 'pmic-tree', '--check', 'power-domains-tree', '--keep-going')
        self.assertEqual(result.returncode, 1)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('FAIL check-pmic-tree', summary)
        self.assertIn('PASS check-power-domains-tree', summary)

    def test_failed_build_skips_only_the_checks_that_need_it(self):
        self.full_catalogue()
        self.env['GATE_TEST_FAIL'] = 'kernel-check'
        result = self.run_gate('--reason', 'Central build fails', '--full')
        self.assertEqual(result.returncode, 1)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('FAIL central-build', summary)
        self.assertIn('SKIP check-kernel: central-build did not pass', summary)
        self.assertIn('SKIP check-soak8: central-build did not pass', summary)
        self.assertIn('PASS check-pmic-tree', summary)
        self.assertIn('PASS os-headset-build', summary)
        self.assertFalse(any('tools/check-kernel.py' in call for call in self.records()))

    def test_checks_run_in_parallel_and_qemu_checks_run_alone(self):
        self.full_catalogue()
        self.env['GATE_TEST_SLEEP'] = '0.4'
        result = self.run_gate('--reason', 'Parallel', '--full', '--jobs', '4')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        spans = self.intervals()
        serial = [s for s in spans if s[0] in ('tools/check-kernel.py', 'tools/check-smp.py')]
        others = [s for s in spans if s not in serial]
        self.assertEqual(len(serial), 2)
        self.assertTrue(any(self.overlap(a, b) for a in others for b in others if a is not b))
        self.assertFalse(any(self.overlap(a, b) for a in serial for b in spans if a is not b))
        environment = (self.sandbox / 'build/gate-case/environment.txt').read_text()
        self.assertIn('PATH=' + str(self.sandbox / 'host/usr/lib/llvm-21/bin'), environment)

    def test_single_worker_never_overlaps_and_index_names_serial_checks(self):
        self.full_catalogue()
        self.env['GATE_TEST_SLEEP'] = '0.2'
        self.env['GATE_TEST_SERIAL'] = 'focused.py'
        result = self.run_gate('--reason', 'Serial', '--full', '--jobs', '1')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        spans = self.intervals()
        self.assertFalse(any(self.overlap(a, b) for a in spans for b in spans if a is not b))
        self.env['GATE_TEST_SERIAL'] = 'focused.py'
        result = self.run_gate('--reason', 'Serial index', '--full', '--jobs', '4', label='second')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        spans = [s for s in self.intervals()][len(spans):]
        focused = [s for s in spans if s[0] == 'checks/focused.py']
        self.assertEqual(len(focused), 1)
        self.assertFalse(any(self.overlap(focused[0], s) for s in spans if s is not focused[0]))

    def test_memory_guard_starts_no_check_while_memory_is_low(self):
        self.full_catalogue()
        result = self.run_gate('--reason', 'Low memory', '--full', '--min-available-gb', '1000000', '--memory-wait', '1')
        self.assertEqual(result.returncode, 1)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('WAIT ', summary)
        self.assertIn('not started; MemAvailable stayed below 1e+06 GiB', summary)
        self.assertFalse(any(call[0] == 'python' for call in self.records()))

    def test_prose_checks_warn_unless_documentation_is_strict(self):
        self.env['GATE_TEST_PROSE'] = json.dumps({'focused.py': True})
        self.env['GATE_TEST_FAIL'] = 'checks/focused.py'
        warned = self.run_gate('--changed', 'anything')
        self.assertEqual(warned.returncode, 0, warned.stdout + warned.stderr)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('WARN indexed-focused', summary)
        self.assertIn('PASS selected', summary)
        strict = self.run_gate('--changed', 'anything', '--strict-docs', label='strict')
        self.assertEqual(strict.returncode, 1)
        self.assertIn('FAIL indexed-focused', (self.sandbox / 'build/gate-strict/summary.txt').read_text())

    def test_partly_prose_check_warns_only_for_its_prose_cases(self):
        self.env['GATE_TEST_PROSE'] = json.dumps({'focused.py': ['test_records']})
        self.env['GATE_TEST_FAIL'] = 'checks/focused.py'
        self.env['GATE_TEST_OUTPUT'] = 'FAIL: test_records (__main__.Check)'
        self.assertEqual(self.run_gate('--changed', 'anything').returncode, 0)
        self.env['GATE_TEST_OUTPUT'] = 'FAIL: test_records (__main__.Check)\nERROR: test_parser (__main__.Check)'
        code = self.run_gate('--changed', 'anything', label='code')
        self.assertEqual(code.returncode, 1)
        self.env['GATE_TEST_OUTPUT'] = 'Traceback: import failed'
        self.assertEqual(self.run_gate('--changed', 'anything', label='crash').returncode, 1)

    def test_prose_failure_does_not_stop_a_fail_fast_gate(self):
        self.env['GATE_TEST_PROSE'] = json.dumps({'focused.py': True})
        self.env['GATE_TEST_FAIL'] = 'checks/focused.py'
        result = self.run_gate('--changed', 'anything', '--fail-fast')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        summary = (self.sandbox / 'build/gate-case/summary.txt').read_text()
        self.assertIn('WARN indexed-focused', summary)
        self.assertIn('PASS indexed-progress', summary)

    def test_real_index_runs_host_only_usb_named_checks_in_the_gate(self):
        spec = importlib.util.spec_from_file_location('real_index', ROOT / 'checks/check-index.py')
        indexer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(indexer)
        index = indexer.generate()
        for name, option in (('board-runner.py', '--out'), ('usb-disconnect.py', '--output'),
                             ('usb-access-discover.py', '--output')):
            self.assertTrue(index[name]['pure_host'], name)
            self.assertFalse(index[name]['usb'], name)
            self.assertEqual(index[name]['output_option'], option)
        # Checks outside HOST_ONLY keep their device-access classification.
        real = [n for n, e in index.items() if e['usb'] and not e['pure_host'] and n not in indexer.HOST_ONLY]
        self.assertIn('usb-session.py', real)
        self.assertFalse(set(indexer.HOST_ONLY) & set(real))
        self.assertEqual(index['docs-links.py']['prose'], True)
        self.assertTrue(index['pcie-qemu.py']['serial'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    options, remaining = parser.parse_known_args()
    OUTPUT = options.output.resolve()
    OUTPUT.mkdir(parents=True, exist_ok=False)
    unittest.main(argv=['integration-gate.py', *remaining])
