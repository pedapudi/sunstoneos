#!/usr/bin/env python3
"""A change to a file a host model image links selects that image's check, and a missing kit function fails its link.

The latency, latency control, latency cost and CPU reliability models define the kit's task creation themselves, because none
of them links apps/os/application.c (commit cf1b288b7). The gate once missed the break because its index named those images'
CMake files but not the sources they compile. Cases, on a copy of this tree with those three definitions removed:
  selection  each path below selects the checks that build the images linking it, through the build graph
             (checks/check-index.py, tools/build_graph.py), and those checks declare gate runs, so the gate supplies them;
  failure    the images the selected checks build (latency-control-busy-model, latency-cost-model, cpu-reliability-model) fail to
             link with the undefined kit symbol, as the gate's build step would;
  control    the tree under test itself defines the kit function in all three places, so the gate's own builds of these images link.
Usage: gate-model-selection.py --output DIR
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_graph
import gate_images

SYMBOL = 'ss_task_create_static_affinity_set'
FORWARDS = ['apps/latency/model.c', 'apps/latency-cost/model.c', 'checks/cpu-reliability-model/application.c']
# path changed -> the checks it must select
SELECTED = {
    'include/sunstone_app.h': {'latency-control-integration.py', 'model-images.py'},
    'include/os_space.h': {'model-images.py'},          # the SMP platform of the CPU reliability model includes it
    'apps/os/application.c': {'model-images.py'},
    'apps/latency/workload.c': {'latency-control-integration.py', 'model-images.py'},
    'apps/latency/model.c': {'latency-control-integration.py', 'model-images.py'},
    'apps/latency/dma-model.c': {'model-images.py'},
    'apps/latency-cost/native.c': {'model-images.py'},
    'checks/cpu-reliability-model/application.c': {'model-images.py'},
}
FAILING = ['latency-control-busy-model', 'latency-cost-model', 'cpu-reliability-model']
DEFINITION = re.compile(r'(?:/\*(?:(?!\*/).)*?\*/\n)?void \*%s\(.*?\n\}\n' % SYMBOL, re.S)


def copy_tree(destination):
    names = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT).split(b'\0')
    for name in names:
        if not name:
            continue
        source = ROOT / name.decode()
        if source.is_file():
            target = destination / name.decode()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.read_bytes())
            target.chmod(source.stat().st_mode & 0o777)
    subprocess.run(['git', 'init', '-q'], cwd=destination, check=True)


def remove_forwards(tree):
    for name in FORWARDS:
        path = tree / name
        text = path.read_text()
        stripped, count = DEFINITION.subn('', text)
        assert count == 1, '%s: expected one definition of %s, found %d' % (name, SYMBOL, count)
        path.write_text(stripped)


def planned(tree, path):
    """The checks the tree's own affected selector picks for one changed path, with their gate runs declared."""
    result = subprocess.run([sys.executable, '-B', str(tree / 'checks/affected.py'), '--changed', path, '--json'],
                            cwd=tree, capture_output=True, text=True, check=True)
    return {entry['check'] for entry in json.loads(result.stdout)['checks']}


def declares_runs(tree):
    spec = importlib.util.spec_from_file_location('check_index', tree / 'checks/check-index.py')
    index = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(index)
    entries = index.generate()
    return {name for name, entry in entries.items() if entry['gate'].get('runs')}


def link(tree, image, output):
    recipe = gate_images.IMAGES[image]
    build = output / ('build-' + image)
    environment = dict(harness.environment(), PATH=str(harness.CACHE / 'host/usr/lib/llvm-21/bin') + ':' + harness.environment()['PATH'])
    configure = subprocess.run(build_graph.configure_command(tree, recipe, build), capture_output=True, text=True, env=environment)
    assert configure.returncode == 0, 'configure of %s failed:\n%s' % (image, configure.stderr[-2000:])
    targets = [recipe['target']] if isinstance(recipe['target'], str) else recipe['target']
    return subprocess.run(['cmake', '--build', str(build), '--target', *targets, '-j', '12'], capture_output=True, text=True, env=environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    tree = args.output / 'tree'
    copy_tree(tree)
    remove_forwards(tree)
    # control: the unmodified tree keeps every definition
    for name in FORWARDS:
        assert SYMBOL in (ROOT / name).read_text(), name + ': the tree under test lacks its own definition'
    runs = declares_runs(tree)
    for path, expected in SELECTED.items():
        got = planned(tree, path)
        assert expected <= got, '%s selects %s, not %s' % (path, sorted(got), sorted(expected - got))
        assert expected <= runs, '%s: %s declare no gate runs' % (path, sorted(expected - runs))
    for image in FAILING:
        build = link(tree, image, args.output)
        text = build.stdout + build.stderr
        assert build.returncode != 0, image + ' linked without its kit function'
        assert 'undefined symbol: ' + SYMBOL in text, '%s failed for another reason:\n%s' % (image, text[-2000:])
    print('PASS: %d paths select their model checks; %d model images fail to link without %s' % (len(SELECTED), len(FAILING), SYMBOL))


if __name__ == '__main__':
    main()
