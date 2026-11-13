#!/usr/bin/env python3
"""Check the actual native GPU thermal caller (apps/os/gpu.c) on the real FreeRTOS kernel.

The caller runs as a task under the host shim (checks/host/): ticks, time and its event waits
are the kernel's and the shim's, and the wrong-client cases call from a second task. The GPU
service replies and publications, the board lifetime and the thermal throttle input are
modeled. Each case runs in its own process.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
NETWORK = Path.home() / '.cache/sunstoneos/sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
DEFINES = ['SUNSTONE_A733_FAULT_PAGE=1', 'SUNSTONE_A733_OS=1', 'SUNSTONE_A733_OS_GPU=1',
           'SUNSTONE_BOARD_APPLICATION=1', 'SUNSTONE_BOARD_SUPERVISED=1', 'SUNSTONE_BOARD_USB2_DUAL=1', 'PVR_GPU_PERFORMANCE=1']
INCLUDES = [ROOT / 'apps/os', ROOT / 'apps/board', ROOT / 'platform/a733-smp-cached', ROOT / 'drivers/usb',
            ROOT / 'network/include', NETWORK / 'include', NETWORK / 'portable/Compiler/GCC']
SOURCES = ['checks/os-gpu-thermal.c', 'apps/os/quiesce.c']
CASES = ['repeated-park-reopen', 'throttle-during-reopen', 'critical-during-reopen',
         'close-observation-deadline', 'wrong-client', 'expired-lifetime', 'stale-open-epoch',
         'terminal-retire', 'retire-close-busy', 'retire-deadline', 'retire-critical',
         'retire-wrong-client', 'retire-pending-ticket', 'retire-stale-stopped-epoch',
         'close-shared-status-budget', 'owed-job-prevents-thermal-close']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, help='accepted for compatibility; the host build needs no OS build')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--lld', type=Path, help='accepted for compatibility')
parser.add_argument('--cases', type=int, nargs='+', choices=range(16), default=list(range(16)))
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)


def build(name, source):
    return host_rtos.build(out / name, SOURCES, 'os-gpu-thermal', defines=DEFINES,
                           includes=INCLUDES, flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'],
                           link=['-Wl,--gc-sections', '-Wl,--wrap=vTaskDelay'])


def run(exe, mode):
    return subprocess.run([str(exe), str(mode)], capture_output=True, text=True, env=environment())


exe = build('fixed', ROOT / 'apps/os/gpu.c')
identity = hashlib.sha256(exe.read_bytes()).hexdigest()
results = {}
for mode, name in enumerate(CASES):
    if mode not in args.cases: continue
    done = run(exe, mode)
    (out / (name + '.json')).write_text(json.dumps(dict(status=done.returncode, image_sha256=identity,
        physical_access=False)) + '\n')
    assert done.returncode == 0, (name, done.returncode, done.stderr[-400:])
    results[name] = True
# Negative control: a thermal park that does not wait for the client's completions closes the
# GPU under an owed job; the owed-job case must fail.
controls = {}
if 15 in args.cases:
    mutant = host_rtos.mutated(out, 'apps/os/gpu.c', 'if(pvr_compute_queue_clients_idle(gpu.component))break;',
                               'break;', 'gpu-close-ignores-clients.c')
    exe_mutant = host_rtos.build(out / 'control', SOURCES, 'os-gpu-thermal',
        defines=DEFINES + ['GPU_SOURCE="%s"' % mutant], includes=INCLUDES,
        flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'],
        link=['-Wl,--gc-sections', '-Wl,--wrap=vTaskDelay'])
    assert run(exe_mutant, 15).returncode != 0, 'control passed: close ignores owed jobs'
    controls['close-ignores-clients'] = 'fails'
(out / 'results.json').write_text(json.dumps(dict(cases=results, negative_controls=controls, elf_sha256=identity,
    scope='Actual native thermal caller and its event waits on the real kernel; service replies and publications, board lifetime and throttle input modeled.'),
    indent=2) + '\n')
print(f'PASS: {len(results)} native GPU thermal lifecycle cases; no physical access.')
