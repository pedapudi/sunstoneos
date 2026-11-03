#!/usr/bin/env python3
"""Build and run the NPU pool isolation check (checks/npu-pool-isolation.c) against the production memory layer.

The check lays a pool out as the runtime does, builds the NPU's page tables at several pool addresses and walks them
independently: the device reaches exactly the declared buffers and nothing else (docs/designs/application-memory-layout.md,
step 5). The script also builds copies of the table builder with one isolation rule removed and requires
the check to fail on each (--no-mutations skips them).
"""
import argparse, shutil, subprocess
from pathlib import Path
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
# Each mutation removes one rule that keeps a device out of memory it was not given.
MUTATIONS = {
    'maps-one-page-past-each-buffer': [('drivers/npu/a733_npu_mmu.c',
        'for (uint64_t offset = 0; offset < r->bytes; offset += page) {',
        'for (uint64_t offset = 0; offset < r->bytes + page; offset += page) {')],
    'every-page-writable': [('drivers/npu/a733_npu_mmu.c', '(r->writable << 2) | 3u;', '(1u << 2) | 3u;')],
    'high-physical-bits-dropped': [('drivers/npu/a733_npu_mmu.c', '(uint32_t)((pa >> 32) << 4) |', '0 |')],
    # The memory layer and the table builder each refuse a buffer that overlaps the tables; only both removed expose them.
    'tables-may-be-mapped-as-a-buffer': [
        ('drivers/npu/a733_npu_memory.c',
         'a733_extents_intersect(b->cpu, b->bytes, (uintptr_t)arena, capacity) ||\n            a733_extents_intersect(b->cpu, b->bytes, (uintptr_t)tables, table_bytes) ||\n            a733_extents_intersect(b->physical, b->bytes, physical, capacity) ||', ''),
        ('drivers/npu/a733_npu_mmu.c',
         '            a733_extents_intersect(r->physical, r->bytes, physical, capacity)) return 0;', '            0) return 0;')],
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--no-mutations', dest='mutations', action='store_false')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)


def build(name, sources, include_root):
    exe = args.output / name
    subprocess.run([str(CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
                    '-I' + str(include_root), '-I' + str(ROOT / 'include'), str(ROOT / 'checks/npu-pool-isolation.c'),
                    *map(str, sources), '-o', str(exe)], check=True, env=environment())
    return exe


def run(exe):
    return subprocess.run([str(exe)], capture_output=True, text=True, env=environment())


done = run(build('npu-pool-isolation', [ROOT / 'drivers/npu/a733_npu_memory.c', ROOT / 'drivers/npu/a733_npu_mmu.c'], ROOT / 'include'))
assert done.returncode == 0, done.stdout + done.stderr[-800:]
print(done.stdout.strip())
if args.mutations:
    for name, edits in MUTATIONS.items():
        copy = args.output / ('mutant-' + name)
        copy.mkdir()
        bodies = {source: (ROOT / source).read_text() for source in ('drivers/npu/a733_npu_memory.c', 'drivers/npu/a733_npu_mmu.c')}
        for path, old, new in edits:
            assert bodies[path].count(old) == 1, (name, 'the text to mutate is not found exactly once')
            bodies[path] = bodies[path].replace(old, new)
        sources = []
        for source, body in bodies.items():
            (copy / Path(source).name).write_text(body)
            sources.append(copy / Path(source).name)
        mutant = run(build('mutant-' + name + '.exe', sources, ROOT / 'include'))
        assert mutant.returncode != 0, 'the mutation %s passed the check' % name
        print('mutation %s: fails the check' % name)
