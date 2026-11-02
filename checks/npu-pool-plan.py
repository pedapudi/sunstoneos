#!/usr/bin/env python3
"""Check the NPU pool planner (tools/npu-pool-plan.py): the size follows the declaration and the place follows the layout.

The measurement of the runtime (checks/npu-pool-size.py, which needs the model artifacts) is replaced by a function, so
that this check needs nothing but the repository. It shows that

- the slot count and the bounds for graphs registered later come from the manifest's npu section, as apps/os/npu.c takes
  them at admission, and a build that keeps graph i in slot i sizes each slot by its own graph only when it can;
- the pool is the measured size rounded up to whole 2 MiB blocks, and at least 10 MiB;
- the pool lies at the top of the slot 1 area, below its end, and is refused when a region of the manifest reaches into it
  or when it is larger than the area allows;
and that each of these rules fails the check when a copy of the tool lacks it.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
MIB = 1 << 20


def load(path):
    spec = importlib.util.spec_from_file_location('plan_under_test', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def manifest_file(directory, regions, npu):
    path = Path(directory) / 'manifest.json'
    path.write_text(json.dumps({'name': 'pool-plan', 'devices': {'required': [], 'optional': []},
                                'cpu': {'cluster': 'a76', 'workers': 0, 'priority': 2, 'stack_bytes': 4096},
                                'regions': regions, 'npu': npu, 'output_limit': 64,
                                'deadlines': {'return_reserve_us': 1000000, 'progress_window_us': 1000000}}))
    return path


def exercise(plan_module):
    seen = []

    def measure(slots, bounds, uniform):
        seen.append((slots, bounds, uniform))
        return seen_bytes[0]
    with tempfile.TemporaryDirectory() as directory:
        models = Path(directory) / 'models.json'
        models.write_text(json.dumps({'npu_models': ['a', 'b', 'c']}))
        declared = {'slots': 2, 'in_flight': 4, 'graphs': 8, 'graph_bytes': 262144, 'tensor_bytes': 131072}
        none = {'slots': 3, 'in_flight': 4, 'graphs': 3}
        seen_bytes = [6 * MIB]
        # Bounds from the declaration: eight graphs declared, three linked, five registered later.
        manifest = manifest_file(directory, [], declared)
        result = plan_module.plan(manifest, models, False, measure)
        assert seen[-1] == (2, '5,262144,131072', True), seen[-1]
        assert result['pool_bytes'] == 10 * MIB and result['pool_base'] == 0x79000000 - 10 * MIB, result    # the minimum
        # A queue image with one slot for each linked graph and nothing registered later sizes each slot by its own graph.
        result = plan_module.plan(manifest_file(directory, [], none), models, True, measure)
        assert seen[-1] == (3, None, False), seen[-1]
        assert plan_module.plan(manifest_file(directory, [], none), models, False, measure) and seen[-1][2] is True
        # The size is the measurement rounded up to whole blocks.
        for measured, expected in ((11 * MIB, 12 * MIB), (12 * MIB, 12 * MIB), (97 * MIB + 1, 98 * MIB)):
            seen_bytes[0] = measured
            got = plan_module.plan(manifest_file(directory, [], none), models, False, measure)
            assert got['pool_bytes'] == expected and got['pool_base'] == 0x79000000 - expected, (measured, got)
        # A region in the slot 1 area below the pool leaves it in place; one that reaches into it is refused.
        seen_bytes[0] = 96 * MIB
        window = [{'name': 'w', 'kind': 'work', 'access': ['read', 'write'], 'bytes': 40 * MIB}]
        assert plan_module.plan(manifest_file(directory, window, none), models, False, measure)['pool_base'] == 0x79000000 - 96 * MIB
        big = [{'name': 'big', 'kind': 'work', 'access': ['read', 'write'], 'bytes': 600 * MIB},
               {'name': 'other', 'kind': 'work', 'access': ['read', 'write'], 'bytes': 160 * MIB}]
        try:
            plan_module.plan(manifest_file(directory, big, none), models, False, measure)
        except SystemExit as error:
            assert 'would start at' in str(error) and 'regions end' in str(error), error
        else:
            raise AssertionError('a pool over a manifest region was accepted')
        # Larger than the area allows.
        seen_bytes[0] = 800 * MIB
        try:
            plan_module.plan(manifest_file(directory, [], none), models, False, measure)
        except SystemExit as error:
            assert '768' in str(error), error
        else:
            raise AssertionError('a pool larger than the slot 1 area was accepted')
        try:
            plan_module.plan(manifest_file(directory, [], {}), models, False, measure)
        except (SystemExit, KeyError):
            pass
        else:
            raise AssertionError('a manifest without an npu section was planned')


MUTATIONS = {
    'overlap-not-refused': ("    if base < used:", "    if False:"),
    'no-rounding': ("pool = max(-(-needed // memory_layout.BLOCK) * memory_layout.BLOCK,", "pool = max(needed,"),
    'no-minimum': ("memory_layout.record()['npu_pool']['min'])\n    return {", "0)\n    return {"),
    'bounds-ignored': ("if declaration['graph_bytes'] and declaration['tensor_bytes'] and graphs > count:", "if False:"),
    'slots-assumed-one': ("needed = measure(slots=slots,", "needed = measure(slots=1,"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    exercise(load(ROOT / 'tools/npu-pool-plan.py'))
    print('npu-pool-plan: the planner places and sizes the pool as designed')
    source = (ROOT / 'tools/npu-pool-plan.py').read_text()
    for name, (old, new) in MUTATIONS.items():
        assert source.count(old) == 1, (name, 'the text to mutate is not found exactly once')
        copy = args.output / ('mutant-' + name + '.py')
        copy.write_text(source.replace(old, new).replace('ROOT = Path(__file__).resolve().parents[1]', 'ROOT = Path(%r)' % str(ROOT)))
        try:
            exercise(load(copy))
        except (AssertionError, SystemExit, KeyError):
            print('mutation %s: fails the check' % name)
        else:
            raise AssertionError('the mutation %s passed the check' % name)


if __name__ == '__main__':
    main()
