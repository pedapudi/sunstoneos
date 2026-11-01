#!/usr/bin/env python3
"""Check range selection across deleted dependencies, renames and required inputs."""
import argparse
import json
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('checks/affected.py', 'checks/check-index.py', 'checks/harness.py')


def current_inputs():
    """Check dynamically selected native sources against the current checkout."""
    spec = importlib.util.spec_from_file_location('current_check_index', ROOT / 'checks/check-index.py')
    indexer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(indexer)
    expected = {
        'gpu-linked-lifecycle.py': ('drivers/gpu/gpu_queue.c',),
        'gpu-peer-native.py': ('apps/gpu-queue-peer/main.c',),
        'os-audio-replay-linked.py': ('apps/os/capture.c', 'checks/os-audio-replay-application.c'),
        'isp0-replay.py': ('drivers/camera/a733_isp0.c', 'apps/isp0/native.c'),
        'isp0-irq-native.py': ('apps/isp0/native.c', 'apps/isp0/irq.h'),
        'pcie-link-decode.py': ('apps/pcie/observe.py', 'apps/pcie-test/observe.py',
                                'apps/pcie-nvme/observe.py'),
    }
    index = {}
    for name, paths in expected.items():
        inputs = indexer.covered(ROOT / 'checks' / name)
        assert set(paths) <= inputs, (name, sorted(set(paths) - inputs))
        index[name] = {'inputs': sorted(inputs), 'input_directories': []}
    selections = {}
    for name, paths in expected.items():
        for path in paths:
            selected = indexer.select(index, [path])
            assert name in selected, (path, name, selected)
            selections[path] = selected
    return selections


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    current = current_inputs()
    source = out / 'source'
    (source / 'checks').mkdir(parents=True)
    for name in ('affected.py', 'check-index.py', 'harness.py'):
        shutil.copyfile(ROOT / 'checks' / name, source / 'checks' / name)
    env = {**harness.environment(), 'GIT_CONFIG_GLOBAL': '/dev/null', 'GIT_CONFIG_NOSYSTEM': '1'}

    def run(*command):
        result = subprocess.run(command, cwd=source, env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout.strip()

    def write(name, text):
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def commit(message):
        run('git', 'add', '.')
        run('git', '-c', 'user.name=Check', '-c', 'user.email=check@example.invalid', 'commit', '-qm', message)
        return run('git', 'rev-parse', 'HEAD')

    def select(*args):
        return json.loads(run(sys.executable, '-B', 'checks/affected.py', *args, '--json'))

    run('git', 'init', '-q')
    write('include/device.h', '#define DEVICE 1\n')
    write('drivers/device.c', '#include "device.h"\n')
    write('checks/device.py', '"""Device fixture."""\nCHECK_INPUTS = ("drivers/device.c",)\nprint("fixture")\n')
    write('checks/image.py', '"""Image fixture."""\nimport argparse\np = argparse.ArgumentParser()\n'
          'p.add_argument("--image", required=True)\nCHECK_INPUTS = ("drivers/device.c",)\np.parse_args()\n')
    base = commit('Base inputs')
    selected = select('--changed', 'include/device.h')
    assert selected['baseline'] == 'sup'
    assert [c['check'] for c in selected['checks']] == ['device.py', 'image.py']
    assert selected['checks'][0]['status'] == 'runnable'
    assert selected['checks'][1]['status'] == 'pending' and '--image' in selected['checks'][1]['needs']
    # The new graph no longer mentions the old source or header. The base graph
    # must still select the surviving checks when that dependency disappears.
    (source / 'drivers/device.c').rename(source / 'drivers/renamed.c')
    (source / 'include/device.h').rename(source / 'include/retired.h')
    write('drivers/renamed.c', 'int device;\n')
    for name in ('device.py', 'image.py'):
        path = source / 'checks' / name
        path.write_text(path.read_text().replace('drivers/device.c', 'drivers/renamed.c'))
    head = commit('Rename dependencies')
    ranged = select(base + '..' + head)
    assert [c['check'] for c in ranged['checks']] == ['device.py', 'image.py']
    assert {'drivers/device.c', 'drivers/renamed.c', 'include/device.h'} <= set(ranged['changed'])
    # A removed check is required work, not a passing or silently omitted case.
    (source / 'checks/device.py').rename(out / 'removed-device.py')
    removed = commit('Remove a check')
    result = select(head + '..' + removed)
    assert result['checks'][0]['check'] == 'device.py'
    assert result['checks'][0]['status'] == 'pending'
    assert 'absent' in result['checks'][0]['needs'][0]
    # These scripts declare optional argparse flags that their default runtime
    # nevertheless requires. Classification must not execute them without inputs.
    for name in ('h264-input-host.py', 'sae-openssl.py'):
        shutil.copyfile(ROOT / 'checks' / name, source / 'checks' / name)
    conditional = select('--changed', 'checks/h264-input-host.py', 'checks/sae-openssl.py')
    pending = {check['check']: check for check in conditional['checks']}
    assert set(pending) == {'h264-input-host.py', 'sae-openssl.py'}
    for check in pending.values():
        assert check['status'] == 'pending'
    assert '--launcher' in ' '.join(pending['h264-input-host.py']['needs'])
    assert '--openssl' in ' '.join(pending['sae-openssl.py']['needs'])
    assert 'pinned source tree' in ' '.join(pending['sae-openssl.py']['needs'])
    # The real hook fixture inventories its inputs completely. Comments in the
    # shared harness name native headers but do not make them hook inputs.
    hook = (ROOT / 'checks/pre-push-hook.py').read_text()
    legacy = hook.replace("'checks/harness.py')", "'checks/harness.py', 'retired-hook-input.txt')")
    write('checks/pre-push-hook.py', legacy)
    write('retired-hook-input.txt', 'original\n')
    for name in ('sunstone_app.h', 'sunstone_crumb.h'):
        shutil.copyfile(ROOT / 'include' / name, source / 'include' / name)
    legacy_base = commit('Historical declared hook inputs')
    for name in ('sunstone_app.h', 'sunstone_crumb.h'):
        path = source / 'include' / name
        path.write_text(path.read_text() + '\n/* Native-only fixture change. */\n')
    legacy_head = commit('Unrelated native headers')
    write('retired-hook-input.txt', 'changed\n')
    retired_head = commit('Change a historical declared hook input')
    write('checks/pre-push-hook.py', hook)
    hook_native = select('--changed', 'include/sunstone_app.h', 'include/sunstone_crumb.h')
    hook_range = select(legacy_base + '..' + legacy_head)
    for selection in (hook_native, hook_range):
        assert 'pre-push-hook.py' not in {check['check'] for check in selection['checks']}
    retired = select(legacy_head + '..' + retired_head)
    assert 'pre-push-hook.py' in {check['check'] for check in retired['checks']}
    for changed in ('tools/git-hooks/pre-push', 'checks/harness.py', 'checks/pre-push-hook.py'):
        selection = select('--changed', changed)
        assert 'pre-push-hook.py' in {check['check'] for check in selection['checks']}
    # Additional declared fixture inputs remain authoritative, and the fixture
    # script itself is always an input even if omitted from its declaration.
    custom = hook.replace("'checks/pre-push-hook.py', ", "'custom-hook-input.txt', ")
    write('checks/pre-push-hook.py', custom)
    for changed in ('custom-hook-input.txt', 'checks/pre-push-hook.py'):
        selection = select('--changed', changed)
        assert 'pre-push-hook.py' in {check['check'] for check in selection['checks']}
    # Native checks retain inferred inputs from each historical endpoint, even
    # when a later checkout removes that dependency and declares no extra inputs.
    write('drivers/retired-input.c', 'original\n')
    write('checks/legacy-consumer.py', 'open("drivers/retired-input.c").read()\n')
    consumer_base = commit('Historical native consumer')
    write('drivers/retired-input.c', 'changed\n')
    consumer_head = commit('Change a historical native input')
    write('checks/legacy-consumer.py', 'CHECK_INPUTS = ()\nprint("no current input")\n')
    consumer = select(consumer_base + '..' + consumer_head)
    assert 'legacy-consumer.py' in {check['check'] for check in consumer['checks']}
    (out / 'results.json').write_text(json.dumps({'current_inputs': current, 'paths': selected, 'range': ranged, 'removed': result,
                                                'conditional_inputs': conditional, 'hook_native': hook_native,
                                                'hook_legacy_range': hook_range,
                                                'hook_retired_input': retired,
                                                'historical_consumer': consumer}, indent=2) + '\n')
    print('PASS: declared inputs, include closure, renamed/deleted dependencies and pending inputs')


if __name__ == '__main__':
    main()
