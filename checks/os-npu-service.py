#!/usr/bin/env python3
"""Run the OS NPU wrapper (apps/os/npu.c) against the real runtime and service on the host shim.

See checks/os-npu-service.c. The NPU register file, the interrupt route, the platform startup
sequencer and cache maintenance are modeled; the register model aborts on any register it does
not list. Everything else is the production source, including the published ShuffleNetV2 model.
The negative control builds the same fixture with the wrapper as it was before the OPEN fix,
which treated the booting state of the OPEN reply as final; both cases must then fail.
"""
import argparse
from pathlib import Path
import subprocess
import sys

import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
CHECK_INPUTS = ('apps/os/npu.c', 'apps/os/npu.h', 'apps/os/npu-catalogue.py', 'apps/os/npu-models.json',
                'drivers/npu/a733_npu_runtime.c', 'drivers/npu/a733_npu_runtime_interim.c',
                'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_controller.c', 'include/npu_arch.h',
                'checks/os-npu-service.c', 'tools/npu-session-catalogue.py')
UNITS = ('checks/os-npu-service.c', 'drivers/npu/a733_npu_runtime.c', 'drivers/npu/a733_npu_runtime_interim.c',
         'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_controller.c', 'drivers/npu/a733_npu_memory.c',
         'drivers/npu/a733_npu_mmu.c', 'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c',
         'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_relocation.c', 'drivers/storage/storage_digest.c')
DEFINES = ['A733_A76_ADMITTED=1', 'SUNSTONE_A733_OS=1', 'SUNSTONE_A733_OS_NPU=1', 'SUNSTONE_A733_OS_NPU_OBSERVE=1',
           'SUNSTONE_BOARD_APPLICATION=1', 'SUNSTONE_BOARD_SUPERVISED=1', 'SUNSTONE_A733_OS_CPU_CLOCK=1']
CASES = (20, 21)
ARTIFACTS = CACHE / 'build/npu-board/session-artifacts.json'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--no-negative-control', action='store_true')
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
python = sys.executable
generated = out / 'generated'; generated.mkdir()
subprocess.run([python, str(ROOT / 'apps/os/npu-catalogue.py'), '--artifacts', str(ARTIFACTS),
                '--models', str(ROOT / 'apps/os/npu-models.json'), '--model', '0', '--model-name',
                'shufflenet-v2-published', '--output', str(generated / 'npu-models.c')], check=True)
subprocess.run([python, str(ROOT / 'tools/npu-session-catalogue.py'), 'generate', '--application',
                str(ROOT / 'apps/os/npu-models.json'), '--output', str(generated / 'npu_admitted_models.c')], check=True)
NETWORK = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
includes = [ROOT / 'apps/os', generated, ROOT / 'apps/board', ROOT / 'platform/a733-storage-update',
            ROOT / 'platform/a733-smp-cached', ROOT / 'drivers/usb', ROOT / 'network/include',
            ROOT / 'platform/qemu-smp', NETWORK / 'include', NETWORK / 'portable/Compiler/GCC']


def build(name, wrapper=None, extra=()):
    defines = DEFINES + list(extra) + (['OS_NPU_SOURCE="%s"' % wrapper] if wrapper else [])
    sources = [*UNITS, generated / 'npu-models.c', generated / 'npu_admitted_models.c']
    return host_rtos.build(out / name, sources, 'os-npu-service', defines=defines, includes=includes,
                           flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'],
                           link=['-Wl,--gc-sections'])


def run(exe, case):
    return subprocess.run([str(exe), str(case)], capture_output=True, text=True, env=environment(), timeout=900)


exe = build('fixed')
for case in CASES:
    done = run(exe, case)
    (out / ('case-%d.log' % case)).write_text(done.stdout + done.stderr)
    assert done.returncode == 0, (case, done.returncode, done.stdout[-400:], done.stderr[-600:])
    print('case', case, ' '.join(done.stdout.split('\n')[:2]))
# Case 23 (the MMU exception record): a build whose first operation points its input at the unmapped device address
# 0x20000000 (SUNSTONE_NPU_UNMAPPED_*, drivers/npu/nbg_assembly.c). The register model faults the job when it finds that
# address in a descriptor; the wrapper must record the MMU status and the address. The ordinary build (the control)
# never finds one and must complete the job, which cases 20 and 21 above already show.
hooked = build('unmapped', extra=['SUNSTONE_NPU_UNMAPPED_SLOT=INPUT', 'SUNSTONE_NPU_UNMAPPED_ADDRESS=0x20000000u'])
done = run(hooked, 23)
(out / 'case-23.log').write_text(done.stdout + done.stderr)
assert done.returncode == 0, (23, done.returncode, done.stdout[-400:], done.stderr[-600:])
print('case 23', done.stdout.strip().splitlines()[-1])
if not args.no_negative_control:
    # The wrapper before the OPEN fix: the reply to OPEN must be READY.
    old = (ROOT / 'apps/os/npu.c').read_text()
    guard = 'reply.state!=expected&&!(op==NPU_SERVICE_OPEN&&reply.state==A733_NPU_BOOTING)'
    assert old.count(guard) == 1
    mutant = out / 'npu-before-open-fix.c'
    mutant.write_text(old.replace(guard, 'reply.state!=expected'))
    control = build('before-open-fix', wrapper=str(mutant))
    for case in CASES:
        done = run(control, case)
        assert done.returncode != 0, ('the wrapper before the OPEN fix passed case', case)
        print('case', case, 'fails on the wrapper before the OPEN fix, as required')
print('PASS OS NPU wrapper on the real runtime and service: %d cases, the MMU exception record and a negative control' % len(CASES))
