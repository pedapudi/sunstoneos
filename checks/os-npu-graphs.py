#!/usr/bin/env python3
"""Run every catalogued SunstoneOS test graph through LOAD and SUBMIT of the real OS NPU runtime and service on the host.

Uses checks/os-npu-service.c (case 22): the production wrapper, runtime, service, controller, model assembler and
loader admission, with the register file, interrupt route, startup sequencer and cache maintenance modeled. Each
graph must be admitted by the reviewed catalogue, placed, assembled and submitted; the modeled register file
completes jobs without computing them, so outputs are compared on the board, not here. The graphs are those of
tools/npu-graph-variants.py compiled by tools/npu-graph-compile.sh; --graphs names the directory that holds
artifacts.json (graph files, input and simulator reference tensors).
"""
import argparse, json, subprocess, shutil, sys
from pathlib import Path

import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
CHECK_INPUTS = ('apps/os/npu.c', 'apps/os/npu-catalogue.py', 'apps/npu-graphs/models-a1.json', 'apps/npu-graphs/models-a2.json', 'apps/npu-graphs/models-b.json',
                'drivers/npu/a733_npu_runtime.c', 'drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_sp_programs.inc',
                'checks/os-npu-service.c', 'checks/os-npu-graphs.py', 'tools/npu-session-models.json')
sys.path.insert(0, str(ROOT / 'checks'))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--graphs', type=Path, default=CACHE / 'build/npu-graph-variants')
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
artifacts = json.loads((args.graphs / 'artifacts.json').read_text())
NETWORK = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
UNITS = ('checks/os-npu-service.c', 'drivers/npu/a733_npu_runtime.c', 'drivers/npu/a733_npu_runtime_interim.c',
         'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_controller.c', 'drivers/npu/a733_npu_memory.c',
         'drivers/npu/a733_npu_mmu.c', 'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c',
         'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_relocation.c', 'drivers/storage/storage_digest.c')
DEFINES = ['A733_A76_ADMITTED=1', 'SUNSTONE_A733_OS=1', 'SUNSTONE_A733_OS_NPU=1', 'SUNSTONE_A733_OS_NPU_OBSERVE=1',
           'SUNSTONE_BOARD_APPLICATION=1', 'SUNSTONE_BOARD_SUPERVISED=1', 'SUNSTONE_A733_OS_CPU_CLOCK=1']
ran = []
for label, listing in (('a1', 'models-a1.json'), ('a2', 'models-a2.json'), ('b', 'models-b.json')):
    work = out / label
    work.mkdir()
    generated = work / 'generated'; generated.mkdir()
    models = ROOT / 'apps/npu-graphs' / listing
    (work / 'artifacts.json').write_text(json.dumps(artifacts))
    subprocess.run([sys.executable, str(ROOT / 'apps/os/npu-catalogue.py'), '--artifacts', str(work / 'artifacts.json'),
                    '--models', str(models), '--model', '0', '--all', '--output', str(generated / 'npu-models.c')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'tools/npu-session-catalogue.py'), 'generate', '--application', str(models),
                    '--output', str(generated / 'npu_admitted_models.c')], check=True)
    names = json.loads(models.read_text())['npu_models']
    inputs = work / 'inputs'; inputs.mkdir()
    for number, name in enumerate(names):
        for k, path in enumerate(artifacts[name]['inputs']):
            shutil.copy(path, inputs / ('%02d-%d.bin' % (number, k)))
    includes = [ROOT / 'apps/os', generated, ROOT / 'apps/board', ROOT / 'platform/a733-storage-update',
                ROOT / 'platform/a733-smp-cached', ROOT / 'drivers/usb', ROOT / 'network/include', ROOT / 'platform/qemu-smp',
                NETWORK / 'include', NETWORK / 'portable/Compiler/GCC']
    exe = host_rtos.build(work / 'exe', [*UNITS, generated / 'npu-models.c', generated / 'npu_admitted_models.c'],
                          'os-npu-graphs', defines=DEFINES, includes=includes,
                          flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'], link=['-Wl,--gc-sections'])
    done = subprocess.run([str(exe), '22', str(inputs)], capture_output=True, text=True, env=environment(), timeout=1800)
    (work / 'run.log').write_text(done.stdout + done.stderr)
    assert done.returncode == 0, (label, done.stdout[-600:], done.stderr[-800:])
    ran.append((label, len(names)))
    print('set', label, ':', done.stdout.strip().splitlines()[-1])
print('PASS %d graphs through the real OS NPU runtime and service (%s)' % (sum(n for _, n in ran), ', '.join('%s: %d' % r for r in ran)))
