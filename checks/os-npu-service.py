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
UNITS = ('checks/os-npu-service.c', 'drivers/npu/npu_declaration.c', 'drivers/npu/a733_npu_runtime.c', 'drivers/npu/a733_npu_runtime_interim.c',
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
NETWORK = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
includes = [ROOT / 'apps/os', generated, ROOT / 'apps/board', ROOT / 'platform/a733-storage-update',
            ROOT / 'platform/a733-smp-cached', ROOT / 'drivers/usb', ROOT / 'network/include',
            ROOT / 'platform/qemu-smp', NETWORK / 'include', NETWORK / 'portable/Compiler/GCC']


def build(name, wrapper=None):
    defines = DEFINES + (['OS_NPU_SOURCE="%s"' % wrapper] if wrapper else [])
    sources = [*UNITS, generated / 'npu-models.c']
    return host_rtos.build(out / name, sources, 'os-npu-service', defines=defines, includes=includes,
                           flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'],
                           link=['-Wl,--gc-sections'])


def run(exe, case, extra=None):
    return subprocess.run([str(exe), str(case)], capture_output=True, text=True, env={**environment(), **(extra or {})},
                          timeout=900)


exe = build('fixed')
for case in CASES:
    done = run(exe, case)
    (out / ('case-%d.log' % case)).write_text(done.stdout + done.stderr)
    assert done.returncode == 0, (case, done.returncode, done.stdout[-400:], done.stderr[-600:])
    print('case', case, ' '.join(done.stdout.split('\n')[:2]))
# Case 27: declarations and reservations.
done = run(exe, 27)
(out / 'case-27.log').write_text(done.stdout + done.stderr)
assert done.returncode == 0, (27, done.returncode, done.stdout[-400:], done.stderr[-600:])
print('case 27', done.stdout.strip().splitlines()[-1])
# Case 28: thirty-one slots.
done = run(exe, 28)
(out / 'case-28.log').write_text(done.stdout + done.stderr)
assert done.returncode == 0, (28, done.returncode, done.stdout[-400:], done.stderr[-600:])
print('case 28', done.stdout.strip().splitlines()[-1])
# Case 31: models given with their digests (not linked bytes): accepted with the right digest, refused with a wrong one.
done = run(exe, 31)
assert done.returncode == 0 and 'accepted at prepare and at LOAD' in done.stdout, (31, done.returncode, done.stdout[-400:], done.stderr[-600:])
bad = run(exe, 31, {'BAD_DIGEST': '1'})
assert bad.returncode == 0 and 'refused at its first LOAD' in bad.stdout, (31, bad.returncode, bad.stdout[-400:], bad.stderr[-600:])
print('case 31 models given with a digest: accepted at LOAD when it matches, refused at the first LOAD when one bit differs')
# Case 29: a graph that no list names, handed over after the scheduler started (the relu graph of the test set, which the
# catalogue of this image does not link): registered within the declared bounds, loaded and run; refused when damaged, too
# large or beyond the declared capacity; and a LOAD refused when the application changes the bytes it handed over.
variants = CACHE / 'build/npu-graph-variants/relu'
done = run(exe, 29, {'REG_MODEL': str(variants / 'export_nbg_unify/network_binary.nb'), 'REG_INPUT': str(variants / 'input0.bin')})
(out / 'case-29.log').write_text(done.stdout + done.stderr)
assert done.returncode == 0, (29, done.returncode, done.stdout[-400:], done.stderr[-600:])
print('case 29', done.stdout.strip().splitlines()[-1])
# Case 26: far more jobs than the former quota, in a session whose job field is 1. A session never fails because it has
# run a number of jobs.
done = run(exe, 26)
(out / 'case-26.log').write_text(done.stdout + done.stderr)
assert done.returncode == 0, (26, done.returncode, done.stdout[-400:], done.stderr[-600:])
print('case 26', done.stdout.strip().splitlines()[-1])
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
    # The wrapper with the lifetime quota of the session's job field: case 26 must fail.
    quota = (ROOT / 'apps/os/npu.c').read_text()
    guard = '!s||!claim())return 0;'
    assert quota.count(guard) == 1
    quota_wrapper = out / 'npu-with-quota.c'
    quota_wrapper.write_text(quota.replace(guard, '!s||npu.jobs>=s->npu.jobs||!claim())return 0;'))
    quota_control = build('with-quota', wrapper=str(quota_wrapper))
    assert run(quota_control, 26).returncode != 0, 'the wrapper with the job quota passed case 26'
    print('case 26 fails on the wrapper with the job quota, as required')
print('PASS OS NPU wrapper on the real runtime and service: %d cases and a negative control' % len(CASES))
