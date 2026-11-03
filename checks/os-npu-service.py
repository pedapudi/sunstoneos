#!/usr/bin/env python3
"""The OS NPU runtime and service with a declaration of 400 graphs, some of 22 tensors, and the queue's open path, on the host.

Builds checks/os-npu-service.c with a catalogue of 400 entries that cycle through five graphs (add, chain A, relu, matrix
multiply and a graph of 12 inputs and 10 outputs made from add by checks/nbg_wide.py) and runs
  case 30: one slot for each graph (400 slots): the runtime prepares, every slot's regions are in the NPU page tables, the page
           after every region is not, and the tables of the declaration lie inside the reservation;
  case 33: the same with the queue's tables: the open path of apps/os/npu-queue.c loads all 400 graphs, links nothing, builds
           400 handles and claims the direct queue; then a SUBMIT of a command beyond the declared 400, and of another
           slot's command by a client that was not given it, are refused by the service.
The limits that remain are memory and the NPU's 4 GiB of device addresses; the pool is sized from the declaration.
"""
import argparse, json, os, re, shutil, subprocess, sys
from pathlib import Path

import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
CHECK_INPUTS = ('apps/os/npu.c', 'apps/os/npu-queue.c', 'apps/os/npu-catalogue.py', 'drivers/npu/a733_npu_runtime.c',
                'drivers/npu/npu_service.c', 'drivers/npu/nbg_assembly.c', 'runtime/npu_app_queue.c',
                'checks/os-npu-service.c', 'checks/os-npu-service.py', 'checks/nbg_wide.py')
sys.path.insert(0, str(ROOT / 'checks'))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--graphs', type=Path, default=CACHE / 'build/npu-graph-variants')
parser.add_argument('--count', type=int, default=400)
parser.add_argument('--artifacts', type=Path, help="run an image's own catalogue instead of the synthetic one (with --models; SUNSTONE_NPU_MODEL_DB names its database)")
parser.add_argument('--models', type=Path)
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
import nbg_wide
artifacts = json.loads((args.graphs / 'artifacts.json').read_text())
database = json.loads((ROOT / 'tools/npu-session-models.json').read_text())
by_name = {m['name']: m for m in database['models']}
NETWORK = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
# The wide graph: the add graph with 12 inputs and 10 outputs; each tensor of the catalogue entry is the add graph's own.
add = artifacts['graph-add']
wide_file = out / 'wide.nb'
wide_file.write_bytes(nbg_wide.widen(Path(add['model']).read_bytes(), 12, 10))
import hashlib
base = by_name['graph-add']
wide = json.loads(json.dumps(base))
wide['name'] = 'graph-wide'
wide['model'] = {'bytes': wide_file.stat().st_size, 'sha256': hashlib.sha256(wide_file.read_bytes()).hexdigest()}
wide['inputs'] = [dict(base['inputs'][-1]) for _ in range(12)]
wide['outputs'] = [dict(base['outputs'][-1]) for _ in range(10)]
for k, t in enumerate(wide['inputs'] + wide['outputs']):
    t['name'] = 'extra%d' % k
files = {}
for kind, count in (('in', 12), ('ref', 10)):
    for k in range(count):
        src = Path((add['inputs'][-1] if kind == 'in' else add['references'][-1]))
        f = out / ('wide-%s%d.bin' % (kind, k)); shutil.copy(src, f); files[(kind, k)] = str(f)
wide_artifact = {'model': str(wide_file), 'inputs': [files[('in', k)] for k in range(12)],
                 'references': [files[('ref', k)] for k in range(10)]}
sources = [('graph-add', by_name['graph-add'], artifacts['graph-add']), ('graph-chainA', by_name['graph-chainA'], artifacts['graph-chainA']),
           ('graph-relu', by_name['graph-relu'], artifacts['graph-relu']), ('graph-mm', by_name['graph-mm'], artifacts['graph-mm']),
           ('graph-wide', wide, wide_artifact)]
db = {'compiler_lock': database['compiler_lock'], 'models': []}
art, names = {}, []
for i in range(args.count):
    name, entry, files_of = sources[i % len(sources)]
    unique = 'n%03d-%s' % (i, name)
    e = json.loads(json.dumps(entry)); e['name'] = unique
    db['models'].append(e); art[unique] = files_of; names.append(unique)
(out / 'models-db.json').write_text(json.dumps(db))
(out / 'artifacts.json').write_text(json.dumps(art))
(out / 'list.json').write_text(json.dumps({'npu_models': names}))
EXPECT_WIDE = '1'
if args.artifacts:
    shutil.copy(args.artifacts, out / 'artifacts.json'); shutil.copy(args.models, out / 'list.json'); EXPECT_WIDE = ''
generated = out / 'generated'; generated.mkdir()
env = {**os.environ} if args.artifacts else {**os.environ, 'SUNSTONE_NPU_MODEL_DB': str(out / 'models-db.json')}
subprocess.run([sys.executable, str(ROOT / 'apps/os/npu-catalogue.py'), '--artifacts', str(out / 'artifacts.json'),
                '--models', str(out / 'list.json'), '--model', '0', '--all', '--output', str(generated / 'npu-models.c')],
               check=True, env=env)
includes = [ROOT / 'apps/os', generated, ROOT / 'apps/board', ROOT / 'platform/a733-storage-update', ROOT / 'platform/a733-smp-cached',
            ROOT / 'drivers/usb', ROOT / 'network/include', ROOT / 'platform/qemu-smp', NETWORK / 'include', NETWORK / 'portable/Compiler/GCC']
UNITS = ('checks/os-npu-service.c', 'drivers/npu/npu_declaration.c', 'drivers/npu/a733_npu_runtime.c', 'drivers/npu/a733_npu_runtime_interim.c',
         'drivers/npu/npu_pool.c', 'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_controller.c', 'drivers/npu/a733_npu_memory.c',
         'drivers/npu/a733_npu_mmu.c', 'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_checksum.c',
         'drivers/npu/nbg_relocation.c', 'drivers/storage/storage_digest.c')
DEFINES = ['OS_NPU_META_BYTES=64ULL', 'A733_A76_ADMITTED=1', 'SUNSTONE_A733_OS=1', 'SUNSTONE_A733_OS_NPU=1', 'SUNSTONE_A733_OS_NPU_OBSERVE=1',
           'SUNSTONE_BOARD_APPLICATION=1', 'SUNSTONE_BOARD_SUPERVISED=1', 'SUNSTONE_A733_OS_CPU_CLOCK=1']
exe = host_rtos.build(out / 'exe', [*UNITS, generated / 'npu-models.c'], 'os-npu-service', defines=DEFINES, includes=includes,
                      flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'], link=['-Wl,--gc-sections'])
done = subprocess.run([str(exe), '30', str(out)], capture_output=True, text=True, env={**environment(), **({'EXPECT_WIDE': '1'} if EXPECT_WIDE else {})}, timeout=1800)
(out / 'case30.log').write_text(done.stdout + done.stderr)
assert done.returncode == 0, ('case 30', done.stdout[-600:], done.stderr[-1500:])
print(done.stdout.strip().splitlines()[-1])
# Mutation: the same declaration with the metadata allocator handing out the pool's own memory. The trust-boundary check (every
# table the EL1 code trusts is outside the pool EL0 can write, outside the buffers the NPU maps and outside its page tables)
# must refuse it, or it checks nothing.
bad = subprocess.run([str(exe), '30', str(out)], capture_output=True, text=True,
                     env={**environment(), 'EXPECT_WIDE': '1', 'MUTATE_META_IN_POOL': '1'}, timeout=1800)
assert bad.returncode != 0 and 'lies in the pool EL0 can write' in bad.stderr, ('mutation not refused', bad.stdout[-300:], bad.stderr[-600:])
print('PASS mutation: metadata placed in the pool is refused by the trust-boundary check')

# Case 33: the queue's open path. The queue units and the adapter of apps/os/npu-queue.c are compiled in; the CPU instructions
# of the adapter are no-ops on the host, and the worker, the runtime and the service are the production code on the modeled device.
queue_units = (*UNITS, 'checks/host/strict_mmio.c', 'kernel/appmem/app_region.c', 'apps/os/npu-queue.c', 'runtime/npu_app_queue.c', 'drivers/npu/npu_queue.c')
qexe = host_rtos.build(out / 'qexe', [*queue_units, generated / 'npu-models.c'], 'os-npu-service-queue',
                       defines=DEFINES + ['SUNSTONE_A733_OS_NPU_QUEUE=1', 'SUNSTONE_OS_NPU_QUEUE_DEPTH=64', 'OS_NPU_CORE=0u'], includes=[ROOT / 'checks/host/mmio', *includes],
                       flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'], link=['-Wl,--gc-sections'])
done = subprocess.run([str(qexe), '33', str(out)], capture_output=True, text=True, env={**environment(), **({'EXPECT_WIDE': '1'} if EXPECT_WIDE else {})}, timeout=3000)
(out / 'case33.log').write_text(done.stdout + done.stderr)
assert done.returncode == 0, ('case 33', done.stdout[-800:], done.stderr[-2500:])
print(done.stdout.strip().splitlines()[-1])

# Mutation: the application's tables do not map the pool. The queue's open must be refused, and the refusal must carry its own
# failure detail (0x70c, the read region of slot 0), so a record never says only "retained".
bad = subprocess.run([str(qexe), '33', str(out)], capture_output=True, text=True,
                     env={**environment(), 'MUTATE_POOL_UNMAPPED': '1', **({'EXPECT_WIDE': '1'} if EXPECT_WIDE else {})}, timeout=3000)
assert bad.returncode == 0 and 'PASS the open is refused' in bad.stdout, ('mutation not refused', bad.stdout[-400:], bad.stderr[-800:])
print('PASS mutation: a pool outside the application\'s mappings fails the queue open with its own reason')
