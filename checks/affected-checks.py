#!/usr/bin/env python3
"""Check range selection across deleted dependencies, renames, configure-step CMake files and required inputs."""
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
        # It builds the OS image in the gate, so the configure step's CMake files are its inputs.
        'os-session-grants.py': ('CMakeLists.txt', 'cmake/a733-smp-cached.cmake', 'cmake/sunstone-os-core.cmake'),
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
    for name in ('sae-openssl.py',):
        shutil.copyfile(ROOT / 'checks' / name, source / 'checks' / name)
    conditional = select('--changed', 'checks/sae-openssl.py')
    pending = {check['check']: check for check in conditional['checks']}
    assert set(pending) == {'sae-openssl.py'}
    for check in pending.values():
        assert check['status'] == 'pending'
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
    # A check whose gate runs build an image covers each CMake file the configure step reads, through include() and
    # add_subdirectory(), under any option. A check that names only a fixture does not.
    write('CMakeLists.txt', 'include(cmake/one.cmake)\nif(OPTION)\n  add_subdirectory(apps/sub)\nendif()\n')
    write('cmake/one.cmake', 'include("${CMAKE_CURRENT_LIST_DIR}/two.cmake")\n')
    write('cmake/two.cmake', 'set(TWO 1)\n')
    write('cmake/unread.cmake', 'set(UNREAD 1)\n')
    write('apps/sub/CMakeLists.txt', 'include("${SUNSTONE_SOURCE_ROOT}/cmake/three.cmake")\n')
    write('cmake/three.cmake', 'set(THREE 1)\n')
    write('checks/os-image.py', '"""Image fixture."""\nGATE_RUNS = [["--build", "{build:sup}", "--output", "{output}"]]\n'
          'print("fixture")\n')
    write('checks/fixture-only.py', '"""Fixture-only fixture."""\nGATE_RUNS = [["--data", "{fixture:graphs}"]]\n'
          'print("fixture")\n')
    cmake_base = commit('Image check and the configure step')
    cmake_selected = {}
    for changed in ('CMakeLists.txt', 'cmake/two.cmake', 'cmake/three.cmake'):
        cmake_selected[changed] = {check['check'] for check in select('--changed', changed)['checks']}
        assert 'os-image.py' in cmake_selected[changed] and 'fixture-only.py' not in cmake_selected[changed], (changed, cmake_selected)
    cmake_selected['cmake/unread.cmake'] = {check['check'] for check in select('--changed', 'cmake/unread.cmake')['checks']}
    assert 'os-image.py' not in cmake_selected['cmake/unread.cmake'], cmake_selected
    write('cmake/two.cmake', 'set(TWO 2)\n')
    cmake_head = commit('Change an included CMake file')
    cmake_range = select(cmake_base + '..' + cmake_head)
    assert [check['check'] for check in cmake_range['checks']] == ['os-image.py'], cmake_range
    # Mutation: a branch that deletes or renames a file a document links to selects the link check, and the link check fails.
    shutil.copyfile(ROOT / 'checks/docs-links.py', source / 'checks/docs-links.py')
    write('apps/linked.py', 'print("linked")\n')
    write('apps/other.py', 'print("other")\n')
    write('docs/note.md', '# Note\n\nSee [the tool](../apps/linked.py) and [another](../apps/other.py).\n')
    links_base = commit('A document links two files')
    assert run(sys.executable, '-B', 'checks/docs-links.py').endswith('0 broken links')
    (source / 'apps/linked.py').unlink()
    (source / 'apps/other.py').rename(source / 'apps/renamed.py')
    links_head = commit('Delete and rename linked files')
    deleted = select(links_base + '..' + links_head)
    assert 'docs-links.py' in {check['check'] for check in deleted['checks']}, deleted
    broken = subprocess.run([sys.executable, '-B', 'checks/docs-links.py'], cwd=source, env=env, capture_output=True, text=True)
    assert broken.returncode != 0 and 'linked.py' in broken.stdout + broken.stderr, broken.stdout + broken.stderr
    write('apps/new.py', 'print("new")\n')
    added = commit('Add a file only')
    assert 'docs-links.py' not in {check['check'] for check in select(links_head + '..' + added)['checks']}
    (out / 'results.json').write_text(json.dumps({'current_inputs': current,
                                                'cmake_paths': {k: sorted(v) for k, v in cmake_selected.items()},
                                                'cmake_range': cmake_range, 'paths': selected, 'range': ranged, 'removed': result,
                                                'conditional_inputs': conditional, 'hook_native': hook_native,
                                                'hook_legacy_range': hook_range,
                                                'hook_retired_input': retired,
                                                'historical_consumer': consumer}, indent=2) + '\n')
    print('PASS: declared inputs, include closure, configure-step CMake files, renamed/deleted dependencies and pending inputs')


if __name__ == '__main__':
    main()
