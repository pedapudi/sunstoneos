#!/usr/bin/env python3
"""Run every catalogued SunstoneOS test graph through LOAD and SUBMIT of the real OS NPU runtime and service on the host.

Uses checks/os-npu-service.c (case 22): the production wrapper, runtime, service, controller, model assembler and
loader admission, with the register file, interrupt route, startup sequencer and cache maintenance modeled. Each
graph must be admitted by its structure, placed, assembled and submitted; the modeled register file
completes jobs without computing them, so outputs are compared on the board, not here. The graphs are those of
tools/npu-graph-variants.py compiled by tools/npu-graph-compile.sh; --graphs names the directory that holds
artifacts.json (graph files, input and simulator reference tensors).
"""
import argparse, hashlib, importlib.util, json, os, re, subprocess, shutil, sys
from pathlib import Path

import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--output', '{output}', '--graphs', '{fixture:npu-graphs}']]
CACHE = Path.home() / '.cache/sunstoneos'
CHECK_INPUTS = ('tools/npu-fixtures.json', 'apps/os/npu.c', 'apps/os/npu-declaration.c', 'tools/npu-test-models.py', 'apps/npu-graphs/models-a1.json', 'apps/npu-graphs/models-a2.json', 'apps/npu-graphs/models-b.json', 'apps/npu-graphs/models-sh.json',
                'drivers/npu/a733_npu_runtime.c', 'drivers/npu/nbg_assembly.c',
                'checks/os-npu-service.c', 'checks/os-npu-graphs.py', 'tools/npu-fixtures.json')
sys.path.insert(0, str(ROOT / 'checks'))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--graphs', type=Path, default=CACHE / 'build/npu-graph-variants')
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
artifacts = json.loads((args.graphs / 'artifacts.json').read_text())
NETWORK = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
UNITS = ('checks/os-npu-service.c', 'kernel/umem/umem.c', 'drivers/npu/npu_declaration.c', 'drivers/npu/a733_npu_runtime.c', 'drivers/npu/a733_npu_runtime_interim.c', 'drivers/npu/npu_pool.c',
         'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_controller.c', 'drivers/npu/a733_npu_memory.c',
         'drivers/npu/a733_npu_mmu.c', 'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c',
         'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_relocation.c', 'drivers/storage/storage_digest.c',
         # checks/os-npu-service.c models the EL0 ring too (case 33), so its units are linked.
         'kernel/appmem/app_region.c', 'apps/os/npu-queue.c', 'runtime/npu_app_queue.c', 'drivers/npu/npu_queue.c')
DEFINES = ['SUNSTONE_A733_OS=1',
           'SUNSTONE_BOARD_APPLICATION=1', 'SUNSTONE_BOARD_SUPERVISED=1', 'OS_NPU_CORE=0u']
ran = []
for label, listing in (('a1', 'models-a1.json'), ('a2', 'models-a2.json'), ('b', 'models-b.json'), ('sh', 'models-sh.json')):
    work = out / label
    work.mkdir()
    generated = work / 'generated'; generated.mkdir()
    models = ROOT / 'apps/npu-graphs' / listing
    (work / 'artifacts.json').write_text(json.dumps(artifacts))
    subprocess.run([sys.executable, str(ROOT / 'tools/npu-test-models.py'), '--artifacts', str(work / 'artifacts.json'),
                    '--models', str(models), '--model', '0', '--all', '--output', str(generated / 'npu-models.c')], check=True)
    names = json.loads(models.read_text())['npu_models']
    inputs = work / 'inputs'; inputs.mkdir()
    for number, name in enumerate(names):
        for k, path in enumerate(artifacts[name]['inputs']):
            shutil.copy(path, inputs / ('%02d-%d.bin' % (number, k)))
    includes = [ROOT / 'apps/os', ROOT / 'checks', generated, ROOT / 'apps/board', ROOT / 'platform/a733-storage-update',
                ROOT / 'platform/a733-smp-cached', ROOT / 'drivers/usb', ROOT / 'network/include', ROOT / 'platform/qemu-smp',
                NETWORK / 'include', NETWORK / 'portable/Compiler/GCC']
    exe = host_rtos.build(work / 'exe', [*UNITS, generated / 'npu-models.c'],
                          'os-npu-graphs', defines=DEFINES, includes=includes,
                          flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'], link=['-Wl,--gc-sections'])
    # The reservation also covers graphs registered later (7 more of 262144 bytes, tensors of 262144 bytes, as the manifest of
    # the graphs application declares): small graphs make those bounds the size of every region, whole pages.
    done = subprocess.run([str(exe), '22', str(inputs)], capture_output=True, text=True,
                          env={**environment(), 'NPU_BOUNDS': '7,262144,262144'}, timeout=1800)
    (work / 'run.log').write_text(done.stdout + done.stderr)
    assert done.returncode == 0, (label, done.stdout[-600:], done.stderr[-800:])
    ran.append((label, len(names)))
    if label == 'a1':
        # Case 30: one slot for each of the set's graphs. Sized by its own graph each slot holds less than when every slot is
        # sized for the largest graph; both layouts are prepared, registered and walked by the check.
        totals = {}
        for mode in ('own', 'uniform'):
            env = {**environment(), **({'UNIFORM': '1'} if mode == 'uniform' else {})}
            r = subprocess.run([str(exe), '30', str(inputs)], capture_output=True, text=True, env=env, timeout=1800)
            assert r.returncode == 0, (mode, r.stdout[-600:], r.stderr[-800:])
            line = [l for l in r.stdout.splitlines() if 'bytes in the slots' in l][-1]
            totals[mode] = tuple(int(t) for t in re.findall(r'(\d+) bytes', line))
        assert totals['own'][1] < totals['uniform'][1] and totals['own'][0] <= totals['uniform'][0], totals
        print('per-slot sizing: %d bytes in the slot regions against %d with one size for all slots (reserved %d against %d)' %
              (totals['own'][1], totals['uniform'][1], totals['own'][0], totals['uniform'][0]))
    print('set', label, ':', done.stdout.strip().splitlines()[-1])
# An unseen graph: the relu graph with one coefficient byte changed, so that its digest is in no table, described by an
# entry of its own in a copy of the model list. The runtime must admit, place, assemble and submit it.
admission = importlib.util.spec_from_file_location('nbg_graph_admission', ROOT / 'checks/nbg-graph-admission.py')
admission_module = importlib.util.module_from_spec(admission); admission.loader.exec_module(admission_module)
work = out / 'unseen'; work.mkdir()
native = admission_module.build(work, 'fixed')
relu = Path(artifacts['graph-relu']['model']).read_bytes()
graph = admission_module.Graph(relu, native).with_bytes(23, 0, bytes([relu[admission_module.Graph(relu, native).section(23).offset] ^ 0xff]))
assert graph.data != relu
(work / 'unseen.nb').write_bytes(graph.data)
database = json.loads((ROOT / 'tools/npu-fixtures.json').read_text())
entry = json.loads(json.dumps(next(m for m in database['models'] if m['name'] == 'graph-relu')))
entry['name'] = 'graph-relu-unseen'
entry['model'] = {'bytes': len(graph.data), 'sha256': hashlib.sha256(graph.data).hexdigest()}
database['models'].append(entry)
(work / 'models-db.json').write_text(json.dumps(database))
(work / 'unseen-models.json').write_text(json.dumps({'npu_models': ['graph-relu-unseen']}))
unseen = dict(artifacts['graph-relu']); unseen['model'] = str(work / 'unseen.nb')
(work / 'artifacts.json').write_text(json.dumps({'graph-relu-unseen': unseen}))
generated = work / 'generated'; generated.mkdir()
env = {**os.environ, 'SUNSTONE_NPU_MODEL_DB': str(work / 'models-db.json')}
subprocess.run([sys.executable, str(ROOT / 'tools/npu-test-models.py'), '--artifacts', str(work / 'artifacts.json'),
                '--models', str(work / 'unseen-models.json'), '--model', '0', '--all', '--output', str(generated / 'npu-models.c')],
               check=True, env=env)
inputs = work / 'inputs'; inputs.mkdir()
for k, path in enumerate(unseen['inputs']):
    shutil.copy(path, inputs / ('00-%d.bin' % k))
includes = [ROOT / 'apps/os', ROOT / 'checks', generated, ROOT / 'apps/board', ROOT / 'platform/a733-storage-update',
            ROOT / 'platform/a733-smp-cached', ROOT / 'drivers/usb', ROOT / 'network/include', ROOT / 'platform/qemu-smp',
            NETWORK / 'include', NETWORK / 'portable/Compiler/GCC']
exe = host_rtos.build(work / 'exe', [*UNITS, generated / 'npu-models.c'], 'os-npu-graphs', defines=DEFINES, includes=includes,
                      flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'], link=['-Wl,--gc-sections'])
done = subprocess.run([str(exe), '22', str(inputs)], capture_output=True, text=True, env=environment(), timeout=1800)
(work / 'run.log').write_text(done.stdout + done.stderr)
assert done.returncode == 0, ('unseen', done.stdout[-600:], done.stderr[-800:])
print('unseen graph (digest in no table):', done.stdout.strip().splitlines()[-1])
# The queue study's receipt (apps/npu-graphs, NPU_GRAPHS_QUEUE): the decoder and the expectation agree on a receipt of eight rows and one link row.
import struct
hostspec = importlib.util.spec_from_file_location('npu_graphs_host', ROOT / 'apps/npu-graphs/host.py')
graphs_host = importlib.util.module_from_spec(hostspec); hostspec.loader.exec_module(graphs_host)
depths = [1, 2, 4, 8, 16, 32, 64, 128]
words = [0] * 216
words[0:9] = [graphs_host.MAGIC, 1, graphs_host.BYTES, 9, 9, 9, 1, 100, 24 * 8000]
for i, k in enumerate(depths):
    words[16 + 8 * i:24 + 8 * i] = [i, k, 1000, 0, 24 * 50000, 3, 0, 24 * 50]
words[16 + 64:24 + 64] = [8, 1, 1, 0, 24 * 20, 0, 0, 24 * 20]   # the link row
rows = graphs_host.decode(struct.pack('<216Q', *words), sizes=[(k, 1000) for k in depths] + [(1, 1)], queue=True)
assert rows['passed'] and [r.get('k') for r in rows['queue'][:8]] == depths and rows['queue'][0]['jobs_per_s'] == 20000
assert rows['clock']['before']['little_mhz'] == 0 and rows['clock']['after']['core'] == 0
assert rows['queue'][8] == {'link': True, 'pair_us': 20.0, 'wrong_output_bytes': 0, 'status': 'match'}
expected = graphs_host.queue_expectation(depths, 1000)['words']
def reads(words, raw):
    got = struct.unpack('<%dI' % (len(raw) // 4), raw)
    return len(got) == len(words) and all(g == w if isinstance(w, int) else w['min'] <= g <= w['max'] for g, w in zip(got, words))


# A receipt of an image with measured single-job latencies, and one whose graph differs from the simulator by one step.
receipt = bytearray(struct.pack('<216Q', *words))
struct.pack_into('<I', receipt, 1280, 100)
struct.pack_into('<100I', receipt, 1284, *[24 * (100 + i) for i in range(100)])
assert not reads(graphs_host.queue_expectation(depths, 1000)['words'], bytes(receipt))
assert reads(graphs_host.queue_expectation(depths, 1000, latency=100)['words'], bytes(receipt))
assert graphs_host.decode(bytes(receipt), sizes=[(k, 1000) for k in depths] + [(1, 1)], queue=True)['latency'] == {
    'count': 100, 'min_us': 100.0, 'p50_us': 150.0, 'p99_us': 199.0, 'max_us': 199.0}
off_by_one = bytearray(receipt)
struct.pack_into('<Q', off_by_one, 8 * 5, 8)                       # one graph no longer matches
struct.pack_into('<Q', off_by_one, 8 * (16 + 8 * 2 + 3), 1)        # its status reads mismatch
struct.pack_into('<I', off_by_one, 1152 + 8, 29)                   # 29 differing bytes
struct.pack_into('<I', off_by_one, 1216 + 8, 1)                    # of one 16-bit step
assert not reads(graphs_host.queue_expectation(depths, 1000, latency=100)['words'], bytes(off_by_one))
assert reads(graphs_host.queue_expectation(depths, 1000, latency=100, engine_tolerance=2)['words'], bytes(off_by_one))
struct.pack_into('<I', off_by_one, 1216 + 8, 3)                    # three steps is a failure even with the tolerance
assert not reads(graphs_host.queue_expectation(depths, 1000, latency=100, engine_tolerance=2)['words'], bytes(off_by_one))
assert len(expected) == 2 * 216 and expected[0:2] == [graphs_host.MAGIC & 0xffffffff, graphs_host.MAGIC >> 32]
# The chain study's result (apps/npu-graphs, NPU_GRAPHS_CHAIN): the decoder reads the statistics, the times and the tokens.
chain = bytearray(graphs_host.CHAIN_BYTES)
struct.pack_into('<8Q', chain, 0, graphs_host.CHAIN_MAGIC, 1, 18, 2, 1, 4, 100, 100 + 24 * 50000)
for k in range(2):
    struct.pack_into('<Q', chain, 112 + 8 * k, 24 * (1000 + k))                     # record ticks
struct.pack_into('<5q', chain, 112 + 8 * 4 * graphs_host.CHAIN_MAX_RECORDS, 1 << 40, 1 << 40, 1 << 40, 3 << 8, 8)
tokens_at = 112 + 8 * 4 * graphs_host.CHAIN_MAX_RECORDS + 40 * graphs_host.CHAIN_MAX_STAT_RECORDS * graphs_host.CHAIN_MAX_GRAPHS
struct.pack_into('<4H', chain, tokens_at, 5, 5, 1024, 7)
decoded = graphs_host.chain_decode(bytes(chain))
assert decoded['us']['record'] == [1000.0, 1001.0] and decoded['stats'][0][0]['cosine'] == 1.0 and decoded['stats'][0][0]['max_abs'] == 3.0
assert decoded['tokens'][0] == [5, 5, 1024, 7] and len(graphs_host.chain_expectation()['words']) == graphs_host.CHAIN_BYTES // 4
print('PASS %d graphs through the real OS NPU runtime and service (%s)' % (sum(n for _, n in ran), ', '.join('%s: %d' % r for r in ran)))
