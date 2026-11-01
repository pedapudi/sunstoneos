#!/usr/bin/env python3
"""Check range selection across deleted dependencies, renames and required inputs."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('checks/affected.py', 'checks/check-index.py', 'checks/harness.py')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
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
    (out / 'results.json').write_text(json.dumps({'paths': selected, 'range': ranged, 'removed': result}, indent=2) + '\n')
    print('PASS: declared inputs, include closure, renamed/deleted dependencies and pending inputs')


if __name__ == '__main__':
    main()
