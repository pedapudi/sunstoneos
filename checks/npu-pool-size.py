#!/usr/bin/env python3
"""Bytes of the NPU pool that a model list needs, from the real runtime.

Builds the production runtime for the host with the listed models linked and runs `npu_runtime_reservation_with` through
the prepare path (checks/os-npu-service.c, case 30): one slot for each model, each sized by its own graph (the layout of a
queue image), or with --uniform every slot sized for the largest graph. The result is the value for the cmake parameter
SUNSTONE_OS_NPU_POOL_BYTES, rounded up to a multiple of 2 MiB it already is (the runtime places three groups on 2 MiB
boundaries). SUNSTONE_NPU_MODEL_DB selects the model database as for the image build.

  checks/npu-pool-size.py --artifacts ARTIFACTS.json --models LIST.json [--uniform] --output DIR
"""
import argparse, json, os, re, subprocess, sys, shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [
    ['--artifacts', '{cache}/build/npu-board/session-artifacts.json', '--models', 'apps/os/npu-models.json', '--output', '{output}'],
]
CACHE = Path.home() / '.cache/sunstoneos'
sys.path.insert(0, str(ROOT / 'checks'))
import host_rtos
from harness import environment

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--artifacts', type=Path, required=True)
parser.add_argument('--models', type=Path, required=True)
parser.add_argument('--uniform', action='store_true')
parser.add_argument('--slots', type=int, help='slot count (default: one for each model)')
parser.add_argument('--bounds', help='CAPACITY,GRAPH_BYTES,TENSOR_BYTES: graphs registered later, as the manifest declares them')
parser.add_argument('--select', nargs=argparse.REMAINDER, help='the catalogue tool\'s selection arguments, as the image build gives them (default: --model 0 --all)')
parser.add_argument('--queue', help='LINKS,IMAGE_LINKS: reserve the queue tables for the links an application may ask for and the image\'s constant edges')
parser.add_argument('--json', action='store_true', help='print the result as one JSON object')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
NETWORK = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
UNITS = ('checks/os-npu-service.c', 'drivers/npu/npu_declaration.c', 'drivers/npu/a733_npu_runtime.c', 'drivers/npu/a733_npu_runtime_interim.c', 'drivers/npu/npu_pool.c',
         'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_controller.c', 'drivers/npu/a733_npu_memory.c',
         'drivers/npu/a733_npu_mmu.c', 'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c',
         'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_relocation.c', 'drivers/storage/storage_digest.c')
DEFINES = ['OS_NPU_META_BYTES=64ULL', 'A733_A76_ADMITTED=1', 'SUNSTONE_A733_OS=1', 'SUNSTONE_A733_OS_NPU=1', 'SUNSTONE_A733_OS_NPU_OBSERVE=1',
           'SUNSTONE_BOARD_APPLICATION=1', 'SUNSTONE_BOARD_SUPERVISED=1', 'SUNSTONE_A733_OS_CPU_CLOCK=1']
generated = out / 'generated'; generated.mkdir()
subprocess.run([sys.executable, str(ROOT / 'apps/os/npu-catalogue.py'), '--artifacts', str(args.artifacts),
                '--models', str(args.models), *(args.select if args.select is not None else ['--model', '0', '--all']), '--output', str(generated / 'npu-models.c')], check=True)
includes = [ROOT / 'apps/os', generated, ROOT / 'apps/board', ROOT / 'platform/a733-storage-update',
            ROOT / 'platform/a733-smp-cached', ROOT / 'drivers/usb', ROOT / 'network/include', ROOT / 'platform/qemu-smp',
            NETWORK / 'include', NETWORK / 'portable/Compiler/GCC']
exe = host_rtos.build(out / 'exe', [*UNITS, generated / 'npu-models.c'], 'npu-pool-size', defines=DEFINES, includes=includes,
                      flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'], link=['-Wl,--gc-sections'])
inputs = out / 'inputs'; inputs.mkdir()
env = {**environment(), **({'UNIFORM': '1'} if args.uniform else {}), **({'NPU_SLOTS': str(args.slots)} if args.slots else {}), **({'NPU_SIZE_ONLY': '1'} if args.json else {}),
       **({'NPU_BOUNDS': args.bounds} if args.bounds else {}), **({'NPU_QUEUE': args.queue} if args.queue else {})}
done = subprocess.run([str(exe), '30', str(inputs)], capture_output=True, text=True, env=env, timeout=1800)
(out / 'run.log').write_text(done.stdout + done.stderr)
if done.returncode:
    sys.exit('the runtime refused the layout: ' + (done.stdout + done.stderr)[-400:])
line = [l for l in done.stdout.splitlines() if 'bytes reserved' in l][-1]
slots, reserved, regions = (int(x) for x in re.match(r'(\d+) slots: (\d+) bytes reserved, (\d+) bytes', line).groups())
meta = int(re.search(r'(\d+) bytes of EL1 metadata', line).group(1))
if args.json:
    print(json.dumps({'slots': slots, 'regions': regions, 'pool_bytes': reserved, 'meta_bytes': meta}))
else:
    print('%d slots, %d bytes in the slot regions, pool %d bytes (%.1f MiB), EL1 metadata %d bytes (the image build links exactly this: tools/npu-meta-header.py)' % (slots, regions, reserved, reserved / 1048576, meta))
