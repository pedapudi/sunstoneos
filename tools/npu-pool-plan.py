#!/usr/bin/env python3
"""Plan the NPU pool of an image: its size from what the image declares, its place from the layout.

docs/designs/application-memory-layout.md, step 5. The pool holds the table arena and the buffers of every slot, so
its size is a function of the linked graphs and of the application's declaration; nothing about it is a number to
choose. This tool

  1. takes the slot count and the bounds for graphs registered later from the manifest's npu section, as
     apps/os/npu.c does at admission;
  2. runs the production runtime's own reservation (checks/npu-pool-size.py, which builds the runtime for the host)
     for the linked models, and rounds the result up to whole 2 MiB blocks, at least 10 MiB;
  3. places the pool at the top of the slot 1 area, below its end, and refuses a layout in which it would overlap a
     region the manifest tool placed there.

It prints the cmake arguments (SUNSTONE_OS_NPU_POOL_BYTES and SUNSTONE_OS_NPU_POOL_BASE), or a JSON object with --json.

  tools/npu-pool-plan.py --manifest MANIFEST.json --artifacts ARTIFACTS.json --models LIST.json --output DIR [--per-slot]

--per-slot is for the queue image, which keeps graph i resident in slot i and sizes each slot by its own graph.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import memory_layout

MIB = 1 << 20


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def place(pool_bytes, manifest):
    """The base of a pool of pool_bytes at the top of the slot 1 area, or SystemExit when a region is in the way."""
    low, high = memory_layout.record()['npu_pool']['ledger']
    block = memory_layout.BLOCK
    if pool_bytes % block or not memory_layout.record()['npu_pool']['min'] <= pool_bytes <= memory_layout.record()['npu_pool']['ledger_max']:
        raise SystemExit('npu-pool-plan: a pool of %d bytes is outside 10 MiB to %d MiB of whole blocks' %
                         (pool_bytes, memory_layout.record()['npu_pool']['ledger_max'] // MIB))
    base = high - pool_bytes
    used = max([r['address'] + r['bytes'] for r in manifest['regions'] if r['address'] >= low] + [low])
    if base < used:
        raise SystemExit('npu-pool-plan: the %d MiB pool would start at %#x, below %#x where the manifest\'s regions end in the '
                         'slot 1 area' % (pool_bytes // MIB, base, used))
    return base


def plan(manifest_path, pool_models, per_slot, measure):
    tool = load('plan_manifest', ROOT / 'tools/sunstone-manifest.py')
    manifest = tool.assign(tool.load(manifest_path))
    declaration = manifest.get('npu')
    if not declaration:
        raise SystemExit('npu-pool-plan: the manifest declares no NPU resources')
    slots, graphs = declaration['slots'], declaration['graphs']
    count = len(json.loads(Path(pool_models).read_text())['npu_models'])
    bounds = None
    if declaration['graph_bytes'] and declaration['tensor_bytes'] and graphs > count:
        bounds = '%d,%d,%d' % (graphs - count, declaration['graph_bytes'], declaration['tensor_bytes'])
    needed = measure(slots=slots, bounds=bounds, uniform=not (per_slot and slots == count and not bounds))
    pool = max(-(-needed // memory_layout.BLOCK) * memory_layout.BLOCK, memory_layout.record()['npu_pool']['min'])
    return {'slots': slots, 'bounds': bounds, 'needed_bytes': needed, 'pool_bytes': pool, 'pool_base': place(pool, manifest)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--models', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--per-slot', action='store_true')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()

    def measure(slots, bounds, uniform):
        directory = args.output / 'size'
        command = [sys.executable, str(ROOT / 'checks/npu-pool-size.py'), '--artifacts', str(args.artifacts.resolve()), '--models', str(args.models.resolve()),
                   '--slots', str(slots), '--json', '--output', str(directory.resolve())]
        if uniform:
            command.append('--uniform')
        if bounds:
            command += ['--bounds', bounds]
        done = subprocess.run(command, capture_output=True, text=True, cwd=ROOT / 'checks')
        if done.returncode:
            raise SystemExit('npu-pool-plan: the runtime refused the declaration:\n' + (done.stdout + done.stderr)[-600:])
        return json.loads(done.stdout.strip().splitlines()[-1])['pool_bytes']

    args.output.mkdir(parents=True, exist_ok=True)
    result = plan(args.manifest, args.models, args.per_slot, measure)
    if args.json:
        print(json.dumps(result))
    else:
        print('-DSUNSTONE_OS_NPU_POOL_BYTES=%d -DSUNSTONE_OS_NPU_POOL_BASE=%#x' % (result['pool_bytes'], result['pool_base']))


if __name__ == '__main__':
    main()
