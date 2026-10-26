#!/usr/bin/env python3
"""Prepare frozen board-test bundles and queue submissions for the single-core, memory, GPU and NPU paths.

The tool never contacts the board, never submits and never claims. It holds a
fixed table of rows (docs/designs/board-bundles.md and
docs/designs/board-bundles-next.md). For each row the caller
supplies the keyword arguments of the row's preparation module in one JSON
file, keyed by row name; the tool adds `output`. The outer process creates
~/.cache/sunstoneos/evidence/board-bundles-<date>-<n>/ exclusively and runs the
preparation step once inside a bubblewrap sandbox that has no USB device, no
/sys, no network and a read-only view of every other file. Inputs must lie
outside /tmp, /run and /sys, which the sandbox replaces with empty
directories.

For each row with inputs, the step refuses a Terminal row whose runner does
not declare COMPLETION_COUNTS. It calls the module's prepare(), which ends
with the module's own validate_bundle(), and refuses the row unless the
admitted value carries the row's admission fields. A Read row's bundle must
carry the fixed reader and a word list that the reader's own plan() admits.
It then scans a Monitor row's image.elf with nm, refuses a returning image
whose objects differ from the images that passed outside its declared overlay
or whose controlled outcomes the bundle's runner does not admit, removes write
permission from every bundle entry,
hashes each bundle file once into artifacts.json, derives a Terminal row's
queue timeout from the runner's own bounds and the bundle's file sizes, and a
returning row's from the FEL requests of the bundle's copy of its runner and
the churn guard's wait,
snapshots the row's sources with tools/evidence-snapshot.py, writes
submission.json after checking it with submission() from
tools/board-test-queue.py, and writes runbook.md in the format of
docs/board-runbooks/. run.json records every row's outcome. The exit status
is 0 when no row was refused.

A returning row may name a base row of the same table. Its image may then link
objects of the base row's image (origin set-base) besides those of images that
passed on the board; the tool prepares the row only when the base row was
prepared earlier in the same run, and requires each such object to equal the
base bundle's object of the same unit. A row's `after` list orders its
runbook only: the base and every row in that list must pass on the board
first, in the same lifetime. The operator applies these gates; the runner
does not check them.

A returning row's runbook runs the returning SRAM survey of SURVEY_BUILD with
the repository's tools/run-launch-survey.py before the image. Inputs with a
returning row are refused before any run directory exists unless that build's
manifest names the repository's runner and survey source, as the runner
itself requires, and the survey software record covers its binary.

With --lifetime, the runbooks name the board lifetime they run in: its DDR
record, the warm-return chain of the runs since then, and the order of this
run's rows and of external bundles (such as a native USB image) with the
record directory of each. An external entry names the rows it runs after,
and any entry may name controlled outcomes of earlier rows that stop it
(`stop_if`); each must be a controlled outcome of that row's bundle. Every
gate must name a row placed earlier in the order. --check-externals RUN_DIR
checks that each external runbook writes only record directories under its
entry's stem and names its run directory. Each
row's runbook then gives its full --warm-return list, and the run directory
receives runbook.md with the order, the gates and the chain.

  prepare-board-bundles.py --inputs inputs.json [--lifetime lifetime.json]
"""
import argparse
import ast
from datetime import datetime, timezone
import hashlib
import shutil
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
BWRAP = '/usr/bin/bwrap'
NM = '/usr/lib/llvm-21/bin/llvm-nm'
EVIDENCE = Path.home() / '.cache/sunstoneos/evidence'
BLOCK = 1 << 20
# The queue admits a peripheral test only with one of two recovery
# prerequisites. No record yet qualifies for this one, so the Monitor rows
# wait for it. Running them earlier needs a queue-defined monitor-timer
# prerequisite and the user's written acceptance of an uncovered bus stall.
MONITOR_PREREQUISITE = 'independent-watchdog-rom-return'
# One Monitor attempt: 615 s for the churn guard to admit two re-enumerations,
# 435 s of transaction and return bounds, and 150 s of margin.
MONITOR_TIMEOUT = 1200
TERMINAL_MARGIN = 60
QUEUE_TIMEOUT_LIMIT = 3600
# The churn guard admits one re-enumeration within its 600 s window plus 15 s
# spacing (usb_guard.reenumeration in run_session).
GUARD_WAIT = 615
# The supervised runners move every byte through this module's FEL requests.
SUPERVISED_TRANSPORT = 'tools/check-a733-smp.py'
# Per-job fields a Terminal runner must declare in COMPLETION_COUNTS and report
# in its result record before the tool prepares its row.
COUNT_FIELDS = ('interrupt_completions', 'fallback_completions', 'client_wakeups')
# USB stacks, the shared USB0 loop and secondary-core power-up.
FORBIDDEN_PREFIXES = ('dcd_', 'hcd_', 'tud_', 'tuh_', 'usb0_', 'a733_cpu_start_', 'a733_cpu_secondary_')
ENVIRONMENT = ('PATH', 'HOME', 'LANG', 'PYTHONDONTWRITEBYTECODE')
OBJCOPY = '/usr/lib/llvm-21/bin/llvm-objcopy'
# Read and returning rows run under the queue's supervised RAM recovery: the
# user removes power after a stall (docs/designs/board-bundles-next.md).
SUPERVISED_PREREQUISITE = 'supervised-ram-recovery'
READER = 'tools/board-bundles-next-read.py'
# A Read bundle makes no re-enumeration, so its attempt needs no guard wait:
# its 30 s FEL bound per command and the scratch save and restore fit 120 s.
READ_TIMEOUT = 120
HOST_TOOLS = ('tools/check-a733-smp.py', 'tools/usb-access.py')
# The returning SRAM survey that precedes each returning run. Its runner is the
# repository's tools/run-launch-survey.py, which refuses a build whose manifest
# names another runner or survey source. The build of launch-survey-20261017
# names the runner from before 575565f8; this build holds the same binary and
# names the current runner. SURVEY_SOFTWARE is the binary's instruction and
# return evidence.
SURVEY_RUNNER = 'tools/run-launch-survey.py'
SURVEY_SOURCE = 'boot/a733/launch-survey.S'
SURVEY_BUILD = EVIDENCE / 'bootstrap-retention-20261024/survey-build'
SURVEY_SOFTWARE = EVIDENCE / 'launch-survey-20261017/software.json'
# The coordinator's survey script gave the runner 240 s.
SURVEY_TIMEOUT = 240
# Objects of an image that passed on the board, by the record that shows it:
# the kernel check of ranks 4 and 5, rank-9-usb-v4 on port 3-3, and bundle 2's
# image with the monitor's counter option, which passed on port 3-3
# (board-session-20261023/port-3-3/bb2-run).
PASSED_ORIGINS = {'rank-4-kernel-check', 'rank-9-usb-v4', 'kernel-check-counter-option'}
PASSED_RECORDS = {'rank-4-kernel-check': 'the kernel check of ranks 4 and 5, `574245e2…`',
                  'rank-9-usb-v4': '`board-bundles-20261023/rank-9-usb-v4/`',
                  'kernel-check-counter-option': 'bundle 2, `a8e6ecac…`, passed as `bb2-run` on port 3-3'}
# Objects of the base row prepared in the same run, which has not run on a
# board when the set is prepared.
SET_BASE = 'set-base'

TERMINAL_PROCEDURE = [
    'Confirm that ranks 1 to 5 and rank 9 of docs/audit/board-test-plan.md passed earlier in this power cycle.',
    'Claim this test, then run {runner} on the frozen bundle {bundle} with this evidence directory.',
    'Save the per-job interrupt_completions, fallback_completions and client_wakeups from the runner\'s '
    'result record with the evidence; they are measurements, not pass conditions.',
    'Keep the port lock and confirm whole-board power removal before any other test; this test ends the '
    'board session.']
TERMINAL_STOP = ('Stop after the first failure. The runner keeps the A733 port lock after every outcome until '
                 'whole-board power removal is confirmed, so no other test runs in this board session.')
TERMINAL_COUNTS = (' The record also reports, per job, interrupt_completions, fallback_completions and '
                   'client_wakeups. These counts are measurements, not pass conditions; zero fallback '
                   'completions over the session is the evidence that removes the tick wait.')
TRANSPORT_SOURCES = ['tools/check-a733-smp-supervised.py', SUPERVISED_TRANSPORT]
OS_SOURCES = ['apps/os/prepare.py', 'tools/check-a733-os-supervised.py', 'apps/mimus-board/prepare.py',
              'apps/thermal/prepare.py', 'apps/board/prepare.py', 'tools/prepare-board-bundles.py',
              *TRANSPORT_SOURCES]

SINGLE_CORE_SOURCES = ['tools/prepare-board-bundles.py', READER, *HOST_TOOLS]
RETURNING_SOURCES = ['tools/prepare-board-bundles.py', 'checks/board-bundles-apps.py', 'tools/check-a733-kernel.py',
                     *HOST_TOOLS, 'platform/a733/monitor.S', 'platform/a733/monitor.c', 'platform/a733/platform.c',
                     'platform/a733/link.ld', 'arch/aarch64/vectors.S', 'include/a733_fault.h', 'checks/kernel.c',
                     'checks/board-bundles-fault.c']
RETURNING_STOP = ('Stop at any nonzero exit. The runner quarantines the instance after any outcome that its bundle '
                  'does not list as controlled. After a return to the ROM it then exits with the recorded result; '
                  'without a return it keeps the A733 port lock. A hang ends at the monitor\'s 2 s deadline, and a '
                  'stalled bus access needs a power removal. Do not retry.')
READ_STOP = ('Stop at any nonzero exit. The reader keeps the A733 port lock and quarantines the instance after a '
             'failed FEL command; a stalled or aborted read needs a power removal. Do not retry.')


def read_row(name, position, component, purpose, words, expected):
    return {'name': name, 'class': 'read', 'position': position, 'component': component,
            'module': 'tools/prepare-board-bundles.py', 'function': 'prepare_read', 'runner': READER,
            'admission': {'schema': 'board-read-bundle-v1', 'row': name},
            'sources': SINGLE_CORE_SOURCES, 'purpose': purpose, 'words': words,
            'expected_result': expected, 'stop_condition': READ_STOP,
            'procedure': ['Claim this test, then run the bundle\'s board-read.py on {bundle}/WORDS.json once.',
                          'Save session.json, scratch-before.bin and scratch-restored.bin with the evidence.']}


def word(address, block=None, condition=None, note=''):
    value = {'address': f'0x{address:08x}', 'note': note}
    if block:
        value['block'] = block
    else:
        value['condition'] = {'address': f'0x{condition[0]:08x}', 'mask': f'0x{condition[1]:08x}',
                              'value': f'0x{condition[2]:08x}'}
    return value


CCU = 0x02002000
R_CCU_RTC = (0x0701020c, 0x00010001, 0x00010001)  # RTC bus gate bit 0, reset bit 16
PPU_CLOCK = (0x070101ac, 1, 1)                    # R_CCU PPU clock gate
NPU_CCU = (0x000, 0x0a0, 0x0a8, 0x0ac, 0x2a0, 0x2a4, 0x2a8, 0x2ac, 0x2b0, 0x500, 0x580, 0x588, 0x5c0, 0x5e0, 0xa80,
           0xa88, 0xa8c, 0xb00, 0xb04, 0xc00, 0xc0c, 0x1908, 0x1920, 0x1988, 0x19a0, 0x1b10, 0x1b14, 0x1b1c)
NPU_SRAM_RESET = (CCU + 0xb04, 1 << 19, 1 << 19)  # NPU SRAM reset released (manual 4.1.6.120)
# SYSCTRL bit 1 routes the shared SRAM to the CPU (set by the boot ROM) or the NPU (cleared by U-Boot); its
# sram_ctrl node names no clock or reset (sun60iw2p1.dtsi lines 1451-1459), so the reader admits it only behind
# a condition word, the NPU SRAM reset (docs/reference-backsolve-npu.md).
SRAM_ROUTING = 0x03000008


def returning_row(name, position, component, overlay, expect, controlled, expected, intro, admission=True, needs=None,
                  passes=True, sources=(), base=None, after=(), failures=(), approval=None):
    """passes: a status-0 run with the expect text is one of the bundle's outcomes, beside any controlled one.
    base: the row whose image this row links objects of, prepared in the same run and run first; after: further
    rows of the same run that must pass first. failures: the image's own failure lines, which the runbook lists
    among the paths that end in a quarantine. approval: the schema of the reviewed approval record that a row
    changing a clock or voltage requires; the record names the row, the port and the image's SHA-256, and the
    bundle carries it as approval.json."""
    return {'name': name, 'class': 'monitor', 'returning': True, 'position': position, 'component': component,
            'base': base, 'after': list(after), 'failures': list(failures), 'approval': approval,
            'module': 'tools/prepare-board-bundles.py', 'function': 'prepare_returning',
            'runner': 'tools/check-a733-kernel.py', 'prerequisite': SUPERVISED_PREREQUISITE,
            'admission': {'schema': 'board-returning-bundle-v1', 'row': name} if admission else None,
            'overlay': overlay, 'expect': expect, 'controlled': controlled, 'intro': intro, 'passes': passes,
            'sources': RETURNING_SOURCES + list(sources), 'expected_result': expected,
            'stop_condition': RETURNING_STOP, 'needs': needs,
            'procedure': ['Run the cache-off helper and a survey of the current instance.',
                          'Claim this test, then run {bundle}/host/tools/check-a733-kernel.py once on '
                          '{bundle}/probe.bin with the DDR record and warm-return chain of this lifetime.']}


GPU_SOURCES = ['apps/gpu-power-id/prepare.py', 'apps/gpu-power-id/build.py', 'apps/gpu-power-id/check.py',
               'apps/gpu-power-id/main.c', 'apps/gpu-power-id/wrapper.c', 'apps/gpu-power-id/probe-read.c',
               'apps/gpu-power-id/fw-start.c', 'drivers/gpu/pvr_firmware.c', 'drivers/gpu/pvr_mips.c',
               'drivers/gpu/pvr_fw_state.c', 'drivers/gpu/pvr_ccb.c', 'drivers/gpu/pvr_gpu_start.c',
               'platform/a733/gpu_memory.c', 'include/pvr_gpu_start.h', 'include/a733_gpu_memory.h',
               'drivers/gpu/pvr_gpu_control.c', 'drivers/gpu/pvr_gpu_stop.c', 'include/pvr_gpu_control.h',
               'include/pvr_gpu_stop.h', 'include/pvr_ccb.h', 'apps/gpu-power-id/fw-job.inc',
               'drivers/gpu/pvr_compute_job.c', 'drivers/gpu/pvr_compute_programs.c', 'drivers/gpu/pvr_compute_context.c',
               'drivers/gpu/pvr_compute_package.c', 'drivers/gpu/pvr_cccb.c', 'drivers/gpu/pvr_gpu_vm.c',
               'platform/a733/gpu_vm_memory.c', 'include/pvr_cccb.h',
               'drivers/power/a733_pmic.c', 'drivers/power/a733_pmic_consumer.c',
               'platform/a733/monitor.c', 'include/a733_el3_security.h', 'include/a733_gpu_opp.h',
               'apps/gpu-provider-ready/main.c', 'apps/gpu-provider-ready/build.py',
               'apps/gpu-provider-ready/check.py', 'drivers/gpu/a733_gpu_power.c', 'drivers/gpu/a733_gpu_platform.c',
               'drivers/gpu/a733_gpu_platform_native.c', 'drivers/gpu/a733_gpu_resources.c',
               'drivers/gpu/a733_gpu_resources_native.c', 'drivers/power/a733_power_domains.c',
               'include/a733_gpu_power.h', 'checks/a733-gpu-core-owner.c', 'tools/prepare-board-bundles.py',
               'tools/check-a733-kernel.py', *HOST_TOOLS]
GPU_PREREQUISITES = ('Admission (apps/gpu-power-id/README.md, "Physical admission is separate"): a cold DDR start '
                     'of this lifetime; {history}; the coordinator '
                     'grants the 18 MiB RAM interval [0x41000000, 0x42200000), the PMIC inputs (S_TWI0 gate and '
                     'reset, APBS route, PL0/PL1, settled supplies, no other PMU writer) and, for the power rows, '
                     'exclusive GPU-local authority over the GPU CCU words, PLL_GPU0 (CCU 0x0e0) and the CORE6 power '
                     'policy unit at 0x07066000: its policy (+0x00) and PWCR (+0x20) writes and reads of its status '
                     '(+0x08), MISR (+0x14) and IDR0 (+0xfb0). The grant covers each CORE6 entry state the controller '
                     'admits: settled OFF (policy and status 0), settled ON (both 8) and the power-up state (policy '
                     '8, status 0, PWCR 0x101, the reset value). Any other entry is refused before a write. Voltage '
                     'and clock grant, approved by the user on 2026-10-25: before the first ON the GPU platform '
                     'raises DCDC4 (AXP8191 register 0x15) from 0x1e (800 mV) to 0x2e (960 mV) under an exclusive '
                     'DCDC4 lease, read back, and waits 1.2 ms; DCDC4 stays at 960 mV; each ON enables PLL_GPU0 and '
                     'sets it to 1008 MHz as the GPU core clock, and each OFF disables it, every PLL_GPU0 write read '
                     'back. Every image\'s EL3 monitor makes the vendor BL31\'s cold-boot security writes before EL1 '
                     'starts (include/a733_el3_security.h). The '
                     'client also reads, without writing, a 35-word snapshot before the first ON and after each ON '
                     'attempt: the CORE6 and TOP5 PPU rows, R_CCU 0x1ac and 0x250, and CCU 0x0e0, 0xb20 and 0xb24. '
                     'TOP5 is written only through the shared domain-5 row. No NPU image runs in this lifetime.')
GPU_FAILURES = ['`FAIL GPU ON begin`, `GPU ON deadline` or `GPU ON poll`, after the `GPU power phase=...` and `GPU '
                'CORE entry`, `inputs`, `after` and `GPU clock` lines of the failing cycle, which hold cached values '
                'only; `GPU ON poll` or `GPU ON deadline` also ends a CORE6 status poll that never read 8, and `GPU '
                'ON poll` an entry state that admission refuses, a DCDC4 raise that failed, or controller error 8 (a '
                'PLL_GPU0 write that did not read back), 9 (a lock wait that ended unlocked) or 10 (a GPU clock '
                'update bit that did not clear); `GPU ON poll` with `GPU power phase=5 error=5 step=51` or '
                '`step=52` and the gate word in `last` is a master gate that TOP5 ON did not reopen; `GPU SNAP` '
                'lines of the failed ON precede it',
                '`FAIL GPU snapshot mapping`: a snapshot page is not an identity Device-nGnRnE mapping; no snapshot '
                'word was read',
                '`FAIL GPU OFF begin`, `GPU OFF deadline`, `GPU OFF poll` or `GPU OFF ownership proof`',
                '`FAIL GPU provider park proof` or `GPU provider request retained`',
                'a `FAIL` line of the provider or service (`provider fault`, `provider startup and supply '
                'verification deadline`, `service park deadline`, `provider not quiescent at park`, `PMIC clock`)']


# What a GPU row's lifetime holds before it runs; the rows after a firmware
# run in the same lifetime name those runs instead.
GPU_FRESH = 'no GPU firmware or command has run since power was applied'


def gpu_row(name, position, component, variant, expect, expected, intro, after=(), failures=(), admission='',
            history=GPU_FRESH):
    """A returning GPU image of port 3-1, prepared by apps/gpu-power-id/prepare.py from its own build record.
    admission: text added to the shared GPU admission, such as a row's own exception. history: what GPU firmware
    has run earlier in the lifetime."""
    row = returning_row(name, position, component, [], expect, False, expected, intro, after=after,
                        failures=list(failures) + GPU_FAILURES)
    row.update(module='apps/gpu-power-id/prepare.py', function='prepare_bundle', variant=variant,
               admission={'schema': 'board-gpu-returning-bundle-v1', 'row': name}, sources=list(GPU_SOURCES),
               admission_text=GPU_PREREQUISITES.format(history=history) + (' ' + admission if admission else ''))
    return row


# The PMIC provider, its native adapter and consumer, and the GPU resource
# adapter, which recompile differently since the PMIC access path
# (docs/designs/pmic-access.md), and the counter clock, new since the
# lock-free clock (8431f4b0); checks/board-bundles-apps.py declares them.
PROVIDER_OVERLAY = ['drivers/gpu/a733_gpu_resources.c.obj', 'drivers/gpu/a733_gpu_resources_native.c.obj',
                    'drivers/power/a733_pmic.c.obj',
                    'drivers/power/a733_pmic_consumer.c.obj', 'drivers/power/a733_pmic_native.c.obj',
                    'platform/a733/counter_clock.c.obj']
POWER_OVERLAY = ['apps/board-bundles-power/power.c.obj', 'apps/board-bundles-power/trace.c.obj',
                 'arch/aarch64/device_memory.c.obj', 'drivers/power/a733_power_domains.c.obj',
                 'drivers/power/a733_power_domains_native.c.obj'] + PROVIDER_OVERLAY
POWER_SOURCES = ['apps/board-bundles-power/power.c', 'apps/board-bundles-power/power.h',
                 'apps/board-bundles-power/trace.c', 'apps/board-bundles-power/trace.h',
                 'drivers/power/a733_power_domains.c', 'drivers/power/a733_power_domains_native.c',
                 'drivers/power/a733_pmic.c', 'drivers/power/a733_pmic_native.c', 'drivers/power/a733_pmic_consumer.c',
                 'drivers/gpu/a733_gpu_resources_native.c', 'include/a733_pmic.h', 'include/a733_pmic_native.h',
                 'arch/aarch64/device_memory.c', 'checks/board-bundles-apps-model.c']
BASE_ROW = 'kernel-check-counter-option'
# Bundle numbers that unmerged branches hold. The bundle check requires the
# positioned rows and these numbers together to count 1, 2, 3, ... without a
# gap, so a dropped row is still caught; a merge moves a number from here
# into the table.
RESERVED_POSITIONS = {}

# The clock audit's dump (docs/clock-audit.md): every documented register of the CCU (manual 4.1.5 with the
# offsets of the board kernel's ccu-sun60iw2.c), of the PRCM below the reader's 0x340 bound (manual 4.2.4 with
# ccu-sun60iw2-r.c) and of the CPU PLL block below its 0x3034 bound (manual 5.1.5.5). The PRCM words at 0x360
# and above, including the crypt key at 0x3e4, and CPU_SLOW_CLK_SEL at 0x3034 lie outside the reader's ungated
# blocks and are not read. The RTC clock words follow the RTC bus gate and reset, as in bundles 3 and 14.
CCU_OFFSETS = (
    0x0, 0x20, 0x24, 0x28, 0x2c, 0x30, 0xa0, 0xa4, 0xa8, 0xac, 0xb0, 0xc0, 0xc4, 0xc8, 0xcc, 0xd0, 0xe0, 0xe4, 0xe8,
    0xec, 0xf0, 0x120, 0x124, 0x128, 0x12c, 0x130, 0x140, 0x144, 0x148, 0x14c, 0x150, 0x160, 0x164, 0x168, 0x16c,
    0x170, 0x220, 0x224, 0x228, 0x22c, 0x230, 0x240, 0x244, 0x248, 0x24c, 0x250, 0x260, 0x264, 0x268, 0x26c, 0x270,
    0x280, 0x284, 0x288, 0x28c, 0x290, 0x2a0, 0x2a4, 0x2a8, 0x2ac, 0x2b0, 0x2e0, 0x2e4, 0x2e8, 0x2ec, 0x2f0, 0x500,
    0x510, 0x518, 0x538, 0x540, 0x560, 0x568, 0x574, 0x580, 0x584, 0x588, 0x58c, 0x594, 0x59c, 0x5a4, 0x5b4, 0x5c0,
    0x5e0, 0x5e4, 0x704, 0x70c, 0x724, 0x744, 0x784, 0x78c, 0x7a4, 0x7ac, 0x800, 0x804, 0x808, 0x80c, 0x810, 0x814,
    0x818, 0x81c, 0x820, 0x824, 0x850, 0x880, 0xa00, 0xa04, 0xa20, 0xa24, 0xa40, 0xa44, 0xa60, 0xa64, 0xa6c, 0xa74,
    0xa80, 0xa88, 0xa8c, 0xac0, 0xac4, 0xb00, 0xb04, 0xb20, 0xb24, 0xc00, 0xc0c, 0xc80, 0xc84, 0xc8c, 0xd00, 0xd0c,
    0xd10, 0xd1c, 0xd20, 0xd2c, 0xd30, 0xd3c, 0xd80, 0xd84, 0xd8c, 0xd90, 0xe00, 0xe04, 0xe08, 0xe0c, 0xe10, 0xe14,
    0xe18, 0xe80, 0xe84, 0xe88, 0xe8c, 0xe90, 0xe94, 0xe98, 0xe9c, 0xea0, 0xea4, 0xea8, 0xeac, 0xeb0, 0xf00, 0xf04,
    0xf08, 0xf0c, 0xf10, 0xf14, 0xf18, 0xf1c, 0xf20, 0xf24, 0xf28, 0xf2c, 0xfc0, 0xfc4, 0xfe4, 0x1000, 0x1004,
    0x1008, 0x100c, 0x1024, 0x1060, 0x1064, 0x1080, 0x1084, 0x1200, 0x120c, 0x1210, 0x121c, 0x1220, 0x1224, 0x122c,
    0x1230, 0x123c, 0x1240, 0x124c, 0x1280, 0x1284, 0x128c, 0x12c0, 0x12cc, 0x1300, 0x1304, 0x1308, 0x130c, 0x1340,
    0x1348, 0x1350, 0x1354, 0x135c, 0x1360, 0x1364, 0x1380, 0x1384, 0x138c, 0x13c0, 0x13c4, 0x1400, 0x1410, 0x141c,
    0x1420, 0x142c, 0x1500, 0x1504, 0x1508, 0x150c, 0x1510, 0x1514, 0x1544, 0x154c, 0x1580, 0x1584, 0x1588, 0x158c,
    0x15c0, 0x15c4, 0x1604, 0x160c, 0x1640, 0x164c, 0x1680, 0x1684, 0x168c, 0x1690, 0x1694, 0x16c4, 0x16cc, 0x16e4,
    0x16ec, 0x1700, 0x1704, 0x1744, 0x1800, 0x1804, 0x1808, 0x1840, 0x1844, 0x1860, 0x1884, 0x1904, 0x1908, 0x190c,
    0x1910, 0x1914, 0x1918, 0x191c, 0x1920, 0x1928, 0x1984, 0x1988, 0x198c, 0x1990, 0x1994, 0x1998, 0x199c, 0x19a0,
    0x19a8, 0x1a00, 0x1b00, 0x1b04, 0x1b10, 0x1b14, 0x1b1c, 0x1b28, 0x1b30, 0x1b34, 0x1b38, 0x1c00, 0x1c04, 0x1f00,
    0x1f10, 0x1f20, 0x1f24, 0x1f28, 0x1f30, 0x1f34, 0x1f38, 0x1f3c, 0x1f50)
R_CCU_OFFSETS = (
    0x0, 0xc, 0x10, 0x100, 0x104, 0x108, 0x10c, 0x11c, 0x12c, 0x130, 0x13c, 0x150, 0x15c, 0x17c, 0x18c, 0x19c, 0x1ac,
    0x1b0, 0x1bc, 0x1c0, 0x1cc, 0x20c, 0x210, 0x21c, 0x22c, 0x250, 0x260, 0x290, 0x294, 0x320, 0x324, 0x328, 0x338,
    0x33c)
CPU_PLL_OFFSETS = (
    0x0, 0x4, 0x8, 0xc, 0x10, 0x1000, 0x1004, 0x1008, 0x100c, 0x1010, 0x1014, 0x1018, 0x101c, 0x1020, 0x1024, 0x1028,
    0x102c, 0x2000, 0x2004, 0x2008, 0x200c, 0x2010, 0x2014, 0x2018, 0x201c, 0x2020, 0x2024, 0x2028, 0x202c, 0x3000,
    0x3004, 0x3008, 0x300c, 0x3010, 0x3014, 0x3018, 0x301c, 0x3020, 0x3024, 0x3028, 0x302c)
RTC_CLOCK_OFFSETS = (0x000, 0x004, 0x060, 0x160, 0x16c)  # LOSC, auto switch, 32K fanout, DCXO, DCXO gating

# The CPU operating-point steps of docs/designs/cpu-top-opp.md: one source,
# apps/board-bundles-power/cpu-opp-step.c, built with -DOPP_BUNDLE=n.
OPP_STEP_OVERLAY = POWER_OVERLAY + ['apps/board-bundles-power/cpu-opp-step.c.obj', 'drivers/clock/a733_cpu_clock.c.obj',
                                    'drivers/thermal/a733_thermal.c.obj', 'drivers/thermal/a733_thermal_native.c.obj',
                                    'drivers/adc/a733_adc_native.c.obj', 'platform/a733/cpu_pmu.c.obj']
OPP_STEP_SOURCES = POWER_SOURCES + ['apps/board-bundles-power/cpu-opp-step.c', 'drivers/clock/a733_cpu_clock.c',
                                    'include/a733_cpu_clock.h', 'drivers/thermal/a733_thermal.c',
                                    'drivers/thermal/a733_thermal_native.c', 'drivers/adc/a733_adc_native.c',
                                    'platform/a733/cpu_pmu.c', 'include/a733_cpu_pmu.h', 'checks/cpu-clock-model.c',
                                    'checks/cpu-clock-check.py']
OPP_APPROVAL = 'cpu-operating-point-approval-v1'


def opp_row(name, position, component, text, lines, intro, after, entry=None, writes=True):
    """One CPU operating-point step after bundle 20: admission, thermal stop and end checks as the source states.
    entry names the admitted rails and CPU-B rate when they are not the boot values; a row without writes needs no
    approval record."""
    entry = entry or ('DCDC3 and DCDC5 at 800 mV, CPU-B at 1,014 MHz', 'dcdc5_uv=800000 dcdc3_uv=800000')
    return returning_row(
        name, position, component, OPP_STEP_OVERLAY, f'COMPLETE board bundle {position}: {text}', True,
        f'Monitor status 0 with TEMP set=0 below 70000 at level 0; the SUPPLY line ({entry[1]} sid=0x496 bin=4 '
        f'dcxo_hz=26000000); {lines}; the CPU-L, CPU-B and DSU after lines; a last '
        f'TEMP line below 70000; and COMPLETE board bundle {position}: {text}; then a responsive ROM.',
        intro + ' The image admits the step only at the NORMAL thermal level with every sensor below 70 C, on '
        f'vf0400, with {entry[0]}, CPU-L and the DSU on their PLLs and every '
        'pattern and LFM enable clear, and no planned voltage above the bin\'s top voltage for its rail (DCDC5 '
        '1000 mV, DCDC3 1050 mV in vf0400), and otherwise returns REFUSED before any write. ' +
        ('A sensor set follows each raise and at the end; a sensor at 70 C or more stops the step, which lowers what '
         'it raised (DSU 598 MHz, clusters 1,014 MHz, DCDC3 800 mV if it raised DCDC3) and returns STOPPED.' if writes
         else 'Sensor sets come at the start and the end; a sensor at 70 C or more at the end returns STOPPED.') +
        ' Both are controlled outcomes. At the end it requires the planned rates and rails.' + (' Approved by the user on 2026-10-24 (CPU clock and voltage); '
        'bundle/approval.json binds the values to port 3-3 and this image.' if writes else
        ' It writes no clock or PMIC register.'),
        sources=OPP_STEP_SOURCES, base=BASE_ROW, after=after, approval=OPP_APPROVAL if writes else None,
        failures=['a `FAIL` line of the provider, the thermal driver, the PMIC requests or the CPU clock driver, '
                  'after a `STEP` line naming the driver state; one after a park or a selector write leaves that '
                  'domain or rail where the log last shows it, which only a power removal restores'])

ROWS = [
    # The single-core bundles of docs/designs/board-bundles-next.md, in the
    # order of least recovery risk on port 3-3.
    returning_row('fault-capture', 1, 'Fault capture on core 0 of the returning platform',
                  ['checks/kernel.c.obj'], 'CHECK fault capture: load 0x400000000', True,
                  'Monitor status 1 with exactly one FAIL exception line: ESR_EL1 class 0x25 with a level-1 '
                  'translation fault, FAR_EL1 0x400000000 and ELR_EL1 inside board_bundles_fault_load, in task '
                  'fault, then a responsive ROM.',
                  'One task loads from 0x4_0000_0000, above every valid level-1 entry, so the load takes a '
                  'translation fault before any bus access; platform_exception() writes one FAIL exception line '
                  'and exits with monitor status 1, the bundle\'s one controlled outcome.', passes=False),
    returning_row(BASE_ROW, 2, 'Kernel check built with the returning monitor\'s counter option',
                  ['platform/a733/monitor.c.obj', 'platform/a733/platform.c.obj'],
                  'PASS kernel: timer preemption, queue ordering, blocking wakeup, FP context; rounds=100', False,
                  'The kernel check\'s 100 rounds pass and the monitor returns to a responsive ROM.',
                  'The rank 4 kernel check with SUNSTONE_MONITOR_COUNTERS, built from the source revision: the '
                  'monitor writes MDCR_EL3, HSTR_EL2 and MDCR_EL2.HPMN before EL1 entry. Against bundle 2, which '
                  'passed on port 3-3, platform/a733/platform.c and platform/a733/monitor.c differ: platform_init() '
                  'sets DAIF.I again after the local pool preparation, and the monitor reads back the interrupt '
                  'state it restores on return and, when that fails, publishes status 6 with the image\'s status in '
                  'replaced_status. Its pass makes it the equivalence base of the other returning images prepared '
                  'in the same run, which link its platform.c object and, when the monitor changed since '
                  'rank-9-usb-v4, its monitor object.'),
    read_row('clock-words', 3, 'Clock words for the CPU-L boot-clock report, MBUS and NSI',
             'CPU-L configured rate, MBUS_DFS_EN and the NSI gate and reset, read before any image needs them',
             [word(CCU + 0x580, 'ccu', note='NSI_CLK_REG: gate 31, reset 30'),
              word(CCU + 0x588, 'ccu', note='MBUS_CLK_REG: MBUS_DFS_EN 28'),
              word(R_CCU_RTC[0], 'r_ccu', note='RTC bus gate 0 and reset 16'),
              word(0x07090160, condition=R_CCU_RTC, note='DCXO register of the RTC clock driver'),
              word(0x08871000, 'cpu_pll', note='PLL_CPU_L'), word(0x0887101c, 'cpu_pll', note='CPU_L mux and P')],
             'Every word is read, the oscillator word only after its R_CCU gate reads open and its reset released, '
             'and the 128 scratch bytes are restored and equal.'),
    returning_row('cpu-counters-walk-wfi', 4, 'Event counters, core-0 walk A/B and idle WFI in one image',
                  ['apps/board-bundles-cpu/main.c.obj', 'arch/aarch64/device_memory.c.obj',
                   'platform/a733/cpu_pmu.c.obj'],
                  'COMPLETE board bundle 4:', True,
                  'Monitor status 0 with the three stage records and COMPLETE board bundle 4: counters, walk A/B '
                  'and WFI; or status 0 with COMPLETE board bundle 4: inconclusive when the WFI stage could not start '
                  'before 0.9 s after the monitor\'s entry; or the controlled outcome wfi-stopped-waking: status 2, '
                  'interrupt 29, after the WFI stage marker with fewer than 1,000 wakes and the last wake more than '
                  'two ticks before the forced return.',
                  'The base image with checks/kernel.c replaced by apps/board-bundles-cpu/main.c, the '
                  'single-core platform/a733/cpu_pmu.c and arch/aarch64/device_memory.c. One task runs three '
                  'stages, each with a log marker and a progress record at 0x42000080 (magic, stage, wakes, last '
                  'wake CNTPCT). Counters: 64 empty regions, 2,000 ALU instructions, a 1 ms CNTPCT loop, a refill '
                  'read of a cleaned 64 KiB buffer, PMCR_EL0 and PMCEID0/1_EL0, with IRQs masked around each region. '
                  'Walk A/B: refused when CCU +0x588 reads MBUS_DFS_EN set (bundle 3 read it clear on port 3-3 on '
                  '2026-10-24); otherwise a discarded warm-up block and twelve blocks in ABBA order of 1,024 '
                  'TLBI-and-load iterations, iteration k loading line k of block k, A with TCR_EL1 0x200803019 '
                  '(uncached walks) and B with 0x200803519; each block must count exactly 1,024 DTLB_WALK and 1,024 '
                  'ITLB_WALK, and a calibration region before it records the walks after a lone TLBI. WFI: '
                  'started only before 0.9 s after '
                  'the monitor\'s entry, 1,000 ticks of a733_cpu_idle() with IRQs unmasked, reading the counters '
                  'after each wake outside any masked region. Run it after bundle 2 and bundle 3 of the same '
                  'lifetime; report each board separately.',
                  sources=['apps/board-bundles-cpu/main.c', 'apps/board-bundles-cpu/walk.h', 'platform/a733/cpu_pmu.c',
                           'arch/aarch64/device_memory.c', 'platform/a733/link.ld'], base=BASE_ROW),
    read_row('power-domain-words', 5, 'Policy and status words of power domains 1, 4 and 5',
             'Adoption state of the PCK600 domains, read only while the PPU clock is open',
             [word(PPU_CLOCK[0], 'r_ccu', note='PPU clock gate 0')] +
             [word(0x07060000 + 0x1000 * d + o, condition=PPU_CLOCK, note=f'domain {d} {name}')
              for d in (1, 4, 5) for o, name in ((0, 'policy'), (8, 'status'))],
             'Every word is read while R_CCU 0x1ac bit 0 reads 1, and the 128 scratch bytes are restored and equal.'),
    returning_row('pmic-power-domain-reads', 6, 'PMIC service task and power-domain owner, read-only',
                  POWER_OVERLAY + ['apps/board-bundles-power/bundle6.c.obj'],
                  'COMPLETE board bundle 6: PMIC service and domain adoption reads', True,
                  'Monitor status 0 with the summary (S_TWI0 divisor and bit time, supply verification bits, the '
                  'provisional bound and its verdict, the byte time and the wire-time spin, the re-spins, the step '
                  'distribution against 1000, 1050, 1100 and 1268 permille of measured wire time, the access-cost windows with their '
                  'fastest and slowest sub-windows, the XADDR write, service-loop and whole-poll windows, the '
                  'domain-1 mapping and the adoption '
                  'reads of domains 1, 4 and 5), COMPLETE board bundle 6, and then, while they fit in the log, the '
                  'STEP lines, the 32 snapshots and the owner accesses; or the controlled outcome '
                  'provisional-step-bound-missed: status 1 after the same summary with BOUND pass=0 and FAILED '
                  'board bundle 6: provisional step bound.',
                  'The PMIC provider and adapter of the PMIC access path (docs/designs/pmic-access.md) with '
                  'apps/board-bundles-power/ (bundle6.c, power.c, trace.c), the power-domain owner, its native '
                  'adapter and device_memory.c, on rank-9-usb-v4\'s and the base\'s platform objects; no USB unit. '
                  'The service task verifies DCDC2 and DCDC4 once; a client then takes 32 snapshots, DCDC2 and '
                  'DCDC4 in turn, and makes the adoption reads: R_CCU 0x1ac, CCU 0x5c0 and 0x5e0, and the policy '
                  'and status of domains 1, 4 and 5 only while 0x1ac bit 0 reads 1. It records the owner adapter\'s '
                  'mapping result and the PAR_EL1 translation of the domain-1 block. Before the first snapshot the '
                  'service times one byte phase from its CNTR write to INT_FLAG set (that step is left out of the '
                  'step records), and every S_TWI0 write is timed in place. Each STEP line gives the step\'s CNTPCT '
                  'span, the scheduler ticks inside it, the phases it completed, the TWI interrupt flag at the first '
                  'read of each byte phase, the most polls of one phase and its re-spins: CNTR polls beyond the '
                  'second of a flag phase or a STOP wait, which are reported and do not gate the run. The wire time '
                  'of a step counts two bit times per START, repeated START or STOP and, per byte, nine bit times '
                  'or the measured byte when that is longer and at most 110% of nine bit times; the measured byte '
                  'is the lower bracket of the wire-time spin, which runs with IRQs masked. At the park, the '
                  'service, which is the provider task, times 256 iterations in eight sub-windows each of CNTR '
                  'reads with DSB SY on both sides, plain CNTR reads (issue throughput), dependent CNTR reads '
                  '(latency), the adapter\'s time and read operations, the task check with IRQs unmasked, and the '
                  'TWI observer, and prints each total with its fastest and slowest sub-window; it names its '
                  'core. Three windows follow there, also with IRQs masked: the window loop alone with a DSB SY end '
                  '(counter overhead); 256 plain writes of S_TWI0 XADDR (+0x04) with its own value, one DSB SY after '
                  'each sub-window of 32, run only when ADDR bits 7:3 are not 11110b, CNTR reads 0x40, STAT 0xf8 and '
                  'LCR both lines high with neither forced, and followed by the same reads and an XADDR and ADDR '
                  'readback (A733 User Manual V1.00 sections 18.11.10.1, 18.11.10.2 and 18.11.10.4: XADDR only '
                  'matches a 10-bit slave address, and the controller answers as a slave only with A_ACK set, '
                  'which the provider never writes); and the service loop\'s work between two polls '
                  '(a733_pmic_work, the time read, a733_pmic_work). Up to four of the last four client snapshots carry '
                  'the poll window, which stops at 256 samples, normally after the first of them: the service polls '
                  'a queued snapshot back to back without its spin and stamps each '
                  'a733_pmic_poll(); a poll that read only CNTR in a flag phase, wrote nothing and returned PENDING '
                  'is a whole poll with the flag clear, through the provider object\'s try-lock, its time read and '
                  'its flag read. 256 such samples, those with a scheduler tick left out and counted, are grouped '
                  'in eight sub-windows of 32, beside 256 samples of the stamp pair alone. Those snapshots leave '
                  'the step records: of the 34 snapshots, the wire-time spin step is excluded and the poll window '
                  'normally takes one, so STEPS normally reads recorded=32 and excluded=1, one fewer recorded for '
                  'each further snapshot the window takes. Pass: both supplies verified, every DCDC2 snapshot 0xff9e, every DCDC4 snapshot 0xff9e, or 0xffae (960 mV) when a GPU image of the same lifetime raised it, all equal, 34 snapshots counted, and the '
                  'provisional empirical bound of docs/designs/pmic-access.md: at CPU-L 494 MHz, the median step at most '
                  '1268 permille of its measured wire time and the first step at most that limit plus 40 us. A missed '
                  'bound, or a measured byte that is missing or above 110%, is the controlled outcome; a supply, '
                  'snapshot or record mismatch, an S_TWI0 value that changed across the write window or a provider '
                  'that was not idle in the loop window is not. A refused write window (TWI_WRITE before run=0) or '
                  'an empty poll window (TWI_POLL clear count=0) is recorded and does not fail the run. The summary '
                  'and outcome print before the detail lines, which stop '
                  'while 256 log bytes remain. No GIC register is read for the record, and no PPU or PMIC register '
                  'is written; the only S_TWI0 write outside the provider writes XADDR\'s own value.',
                  sources=POWER_SOURCES + ['apps/board-bundles-power/bundle6.c'], base=BASE_ROW,
                  failures=['`FAIL board bundle 6: supply verification or snapshot mismatch`',
                            '`FAIL board bundle 6: snapshot count or step record`',
                            '`FAIL board bundle 6: S_TWI0 state changed across the XADDR write window`',
                            '`FAIL board bundle 6: provider not idle in the loop window`',
                            '`FAIL board bundle 6: cost windows could not enter the I2C mutex` or `cost window '
                            'operation refused`',
                            'a `FAIL` line of the provider or service (`provider fault`, a deadline, `provider not '
                            'quiescent at park`)']),
    returning_row('power-domain-1-on', 7, 'Domain 1 ON through the power-domain owner',
                  POWER_OVERLAY + ['apps/board-bundles-power/bundle7.c.obj'], 'COMPLETE board bundle 7:', True,
                  'Monitor status 0 with COMPLETE board bundle 7: domain 1 ON, the owner\'s access trace and every '
                  'status value with its CNTPCT stamp; or status 0 with COMPLETE board bundle 7: inconclusive when '
                  'the owner refused the row before any PPU write; or the controlled outcome domain-1-failed.',
                  'The image of bundle 6 with bundle7.c in place of bundle6.c. After supply verification, a client '
                  'with a shared DCDC2 token reads R_CCU 0x1ac and, while it reads 1, the delay row (+0x170, '
                  '+0x174, +0xc00, +0xc04, +0xc10), policy and status of domain 1, then requests ON through the '
                  'domain-1 row. The owner checks CCU 0x5c0 bit 5, 0x5e0 bit 11 and R_CCU 0x1ac bit 0, adopts the '
                  'domain, writes and reads back each delay-row word and writes the ON command; the status is then '
                  'read back to back with CNTPCT stamps, each change with the stamp of the read before it, until it '
                  'reads ON or 10 ms pass. The owner starts its own 10 ms bound after that spin, so a domain that '
                  'never reaches ON fails up to 20 ms after the command. The client reads the same words again. '
                  'Domain 1 stays ON until power removal, so the bundle gives one sample per board lifetime. A '
                  'refusal (closed gate, unverified DCDC2 or incoherent adoption) writes nothing and is '
                  'inconclusive: bundle 8 then finds domain 1 not ON. It runs the PMIC provider of bundle 6, so it '
                  'runs only after bundle 6 of the same set passed in the same lifetime.',
                  sources=POWER_SOURCES + ['apps/board-bundles-power/bundle7.c'], base=BASE_ROW,
                  after=['pmic-power-domain-reads']),
    returning_row('dram-bus-priority', 8, 'DRAM bus priority reads and table write',
                  ['apps/board-bundles-dram/main.c.obj', 'apps/board-bundles-dram/sequence.c.obj',
                   'arch/aarch64/device_memory.c.obj', 'drivers/dram/a733_dram.c.obj'],
                  'COMPLETE board bundle 8:', True,
                  'Monitor status 0 with COMPLETE board bundle 8: table written, the inherited and final fields of '
                  'ports 2, 14 and 15, the USB_PCIE fields unchanged and every access with its CNTPCT span; or '
                  'status 0 with COMPLETE board bundle 8: inconclusive and the skipped stage; or a controlled '
                  'outcome: an NSI synchronous external abort or a readback mismatch.',
                  'rank-9-usb-v4\'s and the base\'s platform objects and the provider\'s tasks.c and queue.c with '
                  'apps/board-bundles-dram/ (main.c, sequence.c), drivers/dram/a733_dram.c in table mode and '
                  'device_memory.c. It reads R_CCU 0x1ac, the domain 1 status only while 0x1ac bit 0 reads 1, and '
                  'CCU +0x580, +0x584 and +0x588. Only while domain 1 reads ON (8) and NSI_CLK bits 31 and 30 and '
                  'NSI_BGR bits 16 and 0 read 1 does it make the first NSI accesses: the +0x10, +0x14 and +0x18 '
                  'words of ports 2, 14 and 15, those of the USB_PCIE port 13, a733_dram_priority() in table mode '
                  '(writes and readbacks of ports 2, 14 and 15 only), and port 13 again, which must read back '
                  'unchanged. Otherwise it makes no NSI access and ends inconclusive, which admits no NSI '
                  'comparison in bundle 10. Inherited fields that read zero on all three ports are recorded as '
                  'possibly protected; the readback separates that case. Each timed span holds one DSB SY-fenced '
                  'access and its barriers only, without the address translation and deadline checks of the '
                  'framebuffer\'s register path. Run it after bundle 7 of the same lifetime.',
                  sources=['apps/board-bundles-dram/main.c', 'apps/board-bundles-dram/sequence.c',
                           'apps/board-bundles-dram/sequence.h', 'drivers/dram/a733_dram.c', 'include/a733_dram.h',
                           'arch/aarch64/device_memory.c', 'checks/board-bundles-apps-model.c'], base=BASE_ROW,
                  after=['power-domain-1-on']),
    read_row('npu-clock-words', 9, 'Speed-bin word, the NPU register-pass CCU words, PPU domain 4 and the shared '
             'SRAM routing word',
             'The NPU speed bin, the CCU state that NPU startup admits, the PPU clock gate and domain-4 policy and '
             'status, and SYSCTRL 0x03000008, whose bit 1 routes the shared SRAM to the CPU or the NPU, read before '
             'the NPU bundle',
             [word(0x0300624c, 'sid', note='SID speed-bin word')] +
             [word(CCU + o, 'ccu', note=f'CCU +0x{o:x}') for o in NPU_CCU] +
             [word(PPU_CLOCK[0], 'r_ccu', note='PPU clock gate 0')] +
             [word(0x07064000 + o, condition=PPU_CLOCK, note=f'domain 4 {name}')
              for o, name in ((0, 'policy'), (8, 'status'))] +
             [word(SRAM_ROUTING, condition=NPU_SRAM_RESET, note='SYSCTRL shared SRAM routing, bit 1')],
             'Every word is read, the domain-4 words only after R_CCU 0x1ac bit 0 reads 1, the SYSCTRL word only '
             'after CCU +0xb04 bit 19 reads 1, and the 128 scratch bytes are restored and equal.'),
    {'name': 'npu-single-core', 'class': 'terminal', 'position': 10, 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'NPU single-core image with native USB takeover',
     'module': 'apps/npu-convolution/prepare-supervised.py', 'runner': 'tools/check-a733-npu-supervised.py',
     'sources': ['apps/npu-convolution/prepare-supervised.py', 'tools/check-a733-npu-supervised.py',
                 'drivers/npu/npu_service.c', 'tools/prepare-board-bundles.py', *TRANSPORT_SOURCES],
     'expected_result': 'Startup reaches READY and the outputs match the SDK simulator.' + TERMINAL_COUNTS,
     'needs': 'tools/check-a733-npu-supervised.py must declare and report COMPLETION_COUNTS, and the NPU owner must '
              'name admission values and record domain 1 and NSI state with the outputs.'},
    # The returning GPU images of port 3-1, in their run order.
    gpu_row('gpu-provider-ready', 11, 'GPU provider readiness before any GPU power request', 'provider-ready',
            'COMPLETE GPU provider readiness: supplies verified, parked, no GPU power request',
            'Monitor status 0 with COMPLETE GPU provider readiness: supplies verified, parked, no GPU power request, '
            'then a responsive ROM.',
            'The frozen provider-readiness checkpoint `3adfa4ad…` (apps/gpu-provider-ready/README.md): the GPU '
            'diagnostic\'s constructors, the PMIC service and the domain-5 row, which is never requested; the '
            'service verifies DCDC2 and DCDC4 and parks, and the client checks every owner record empty. No GPU or '
            'PCK register access. Its CPU monitor, the accepted restoration-readback monitor, is the one the two GPU '
            'power images reuse, so its return is their first board run of that monitor. It runs first on port 3-1, '
            'as apps/gpu-power-id/README.md orders it.'),
    gpu_row('gpu-power-only', 12, 'GPU TOP5 and CORE6 power without a GPU register access', 'power-only',
            'COMPLETE GPU power only: CORE status 8, OFF and provider parked',
            'Monitor status 0 with GPU power cycle=1 core_status=0x8, GPU power cycle=2 core_status=0x8 and '
            'COMPLETE GPU power only: CORE status 8, OFF and provider parked, then a responsive ROM. Record the '
            'GPU CORE entry, inputs and after lines of both cycles and every GPU SNAP line.',
            'apps/gpu-power-id built with --power-only. Two ON/OFF cycles of the default GPU power path '
            '(docs/designs/gpu-vendor-default.md) run before the provider park. Before the first ON, DCDC4 is '
            'raised to 960 mV. Each ON, under the exclusive LOCAL lease, captures CORE6 policy, status, IDR0, MISR '
            'and PWCR, admits settled OFF or ON or the power-up state (policy 8, status 0, PWCR 0x101), writes PWCR '
            '0, enables PLL_GPU0, releases the GPU reset, opens the bus and core clock gates, sets PLL_GPU0 to '
            '1008 MHz, then requests domain 5 (TOP5) ON through the shared row with the GPU clocked, requires the GPU '
            'AHB and MBUS master gates (CCU 0x5c0 bit 7, 0x5e0 bit 16) open, writes CORE6 policy 8 and requires the '
            'CORE6 status poll to read 8; OFF writes CORE6 policy 0 and requires status '
            '0, powers domain 5 OFF with the GPU still clocked, then closes the bus and core gates, disables '
            'PLL_GPU0 and asserts the reset: the vendor order of drivers/gpu/a733_gpu_power.c. The first OFF is '
            'a TOP5 OFF, which on board-session-20261025-7 closed the AHB master gate; the second ON is the first TOP5 '
            'power-up on a board and shows whether TOP5 ON reopens it and whether the MBUS gate was closed too. A GPU gates line after each ON and OFF '
            'gives both gate words at ON entry, after TOP5 ON, after TOP5 OFF and at the end of OFF. The '
            'preparation reads that order from the image\'s own on_steps and '
            'off_steps tables. A read-only snapshot of 35 CORE6, TOP5, R_CCU and CCU words precedes the first ON '
            'and follows each ON attempt. The image never reads or writes the GPU register window at 0x01800000: '
            'the identity reads and their mapping check are compiled out.',
            after=['gpu-provider-ready']),
    gpu_row('gpu-power-identity', 13, 'GPU power and hardware identity', 'identity',
            'COMPLETE GPU power identity: 36.56.104.183 in two ON/OFF cycles, provider parked',
            'Monitor status 0 with GPU identity cycle=1 and cycle=2 lines, each matches=1 for 36.56.104.183, and '
            'COMPLETE GPU power identity: 36.56.104.183 in two ON/OFF cycles, provider parked, then a responsive '
            'ROM.',
            'apps/gpu-power-id, identity variant: the power-only sequence with the GPU core identity read in each '
            'cycle between ON and OFF as the vendor\'s probe reads it, two 32-bit loads of 0x01800024 and then '
            '0x01800020, each after an identity Device-nGnRnE mapping check and a deadline check. No GPU register '
            'write. It runs only after gpu-power-only of this set returned with status 0 in the same lifetime and '
            'printed GPU power cycle=1 core_status=0x8 and GPU power cycle=2 core_status=0x8.',
            after=['gpu-power-only'],
            failures=['`FAIL GPU identity mapping`, `GPU identity ownership/access` or `GPU identity '
                      'deadline/clock`',
                      '`FAIL GPU identity not 36.56.104.183`, after the GPU identity lines and the provider park: '
                      'the core answered with another value in at least one cycle; record both lines']),
    read_row('cpu-b1-words', 14, 'Speed-bin word and the CPU-L, CPU-B and DSU clock words of the A76 record',
             'The read-only clock part of the B1 record of docs/designs/cpu-a76-dvfs.md: the SID speed-bin word, '
             'the oscillator, and the PLL, mux/P, pattern and LFM words that boot0 leaves',
             [word(0x0300624c, 'sid', note='SID speed-bin word'),
              word(R_CCU_RTC[0], 'r_ccu', note='RTC bus gate 0 and reset 16'),
              word(0x07090160, condition=R_CCU_RTC, note='DCXO register of the RTC clock driver')] +
             [word(base + o, 'cpu_pll', note=f'{name} {what}') for base, name in ((0x08871000, 'CPU-L'),
                                                                                  (0x08872000, 'CPU-B'))
              for o, what in ((0, 'PLL'), (0x4, 'pattern 0'), (0x8, 'pattern 1'), (0x18, 'LFM'), (0x1c, 'mux/P'))] +
             [word(0x08873000, 'cpu_pll', note='DSU PLL'), word(0x0887301c, 'cpu_pll', note='DSU mux/P')],
             'Every word is read, the oscillator word only after its R_CCU gate reads open and its reset released, '
             'and the 128 scratch bytes are restored and equal.'),
    returning_row('cpu-supply-reads', 15, 'DCDC3 and DCDC5 snapshots through the PMIC provider, read-only',
                  POWER_OVERLAY + ['apps/board-bundles-power/cpu-supply.c.obj'],
                  'COMPLETE board bundle 15: DCDC3 and DCDC5 snapshots, reaped and provider parked', False,
                  'Monitor status 0 with the raw and decoded DCDC3 and DCDC5 snapshot lines (page, chip, revision, '
                  'enable, selector, enabled, decoded and programmed microvolts) and COMPLETE board bundle 15, then '
                  'a responsive ROM.',
                  'The image of bundle 6 with apps/board-bundles-power/cpu-supply.c in place of bundle6.c and no '
                  'domain row, so the service makes no DCDC2 or DCDC4 verification and no PPU access. After provider '
                  'readiness and the 5,160 us supply settle, a client takes one shared token of DCDC3 and DCDC5, '
                  'submits one snapshot of each in turn, reaps it, parks the service and prints the values: the '
                  'supply part of the B1 record of docs/designs/cpu-a76-dvfs.md, whose DCDC3 selector is the value '
                  'that a DCDC3 selector write must find. No voltage, clock, PMIC or PPU register is written.',
                  sources=POWER_SOURCES + ['apps/board-bundles-power/cpu-supply.c'], base=BASE_ROW,
                  failures=['a `FAIL` line of the provider or service (`snapshot submit`, `snapshot result`, '
                            '`snapshot reap`, a deadline, `provider fault`, `provider not quiescent at park`)']),
    gpu_row('gpu-wrapper-read', 16, 'GPU wrapper reads after the default GPU ON', 'wrapper-read',
            'COMPLETE GPU wrapper read: CORE status 8, four wrapper words, OFF and provider parked',
            'Either monitor status 1 with the four GPU WRAPPER n=1 to n=4 lines, the decoded GPU WRAPPER qchan and '
            'delay lines and FAIL GPU wrapper read: CORE6 not ON, four wrapper words read, the expected result '
            'while CORE6 never reports ON; or status 0 with COMPLETE GPU wrapper read: CORE status 8, four wrapper '
            'words, OFF and provider parked. Either is followed by a responsive ROM. Record every line of kernel.log '
            'and the progress record at result.bin offset 128.',
            'apps/gpu-power-id, wrapper-read variant (apps/gpu-power-id/wrapper.c): the power-only ON sequence of '
            'the default GPU power path (DCDC4 960 mV, PLL_GPU0 at 1008 MHz, CORE6 policy 8 after the clocks), '
            'waited for at most 200 ms; then the read-only snapshot, the controller and CORE6 report lines, and only then four '
            '32-bit reads of the GPU wrapper in this order: 0x01880104 (pwr_qchan_cfg), 0x01880100 (pwr_cmp_dly), '
            '0x01880018 (mult_core_cnt) and 0x0188001c (mult_core_id), names from the vendor sunxi_dvfs_ctrl.c. '
            'Before each read the image writes the read number into the progress record at result offset 128 and '
            'prints a GPU WRAPPER reading line. The reads are refused unless the controller completed the clock and '
            'reset steps. No other GPU-window address is read and nothing in the window is written. It is the last '
            'step of its lifetime.',
            after=['gpu-provider-ready'],
            failures=['`FAIL GPU wrapper read: CORE6 not ON, four wrapper words read`, after the four `GPU WRAPPER '
                      'n=... a=... v=...` lines and the decoded `GPU WRAPPER qchan` and `delay` lines: the expected '
                      'result while CORE6 never reports ON; record it',
                      '`FAIL GPU wrapper read refused before clock and reset progress`, `GPU wrapper mapping` or `GPU '
                      'wrapper read time budget`: no wrapper word was read',
                      '`FAIL GPU wrapper read order`: an internal refusal before the read it names'],
            admission='GPU-window read exception: this row alone reads four words of the GPU wrapper, 0x01880104, '
                      '0x01880100, 0x01880018 and 0x0188001c, in that order, each a 32-bit Device-nGnRnE load after '
                      'an identity mapping check and a deadline check, after the default ON; it '
                      'makes no other GPU-window access and no GPU-window write. Risk: a wrapper read that never completes stops the CPU without an exception, the '
                      'monitor does not return, nothing reaches the host and only a power removal recovers the board. '
                      'A non-return is itself the result that the wrapper is unreachable in this state; the progress '
                      'record and log, which name each read before it is issued, survive only on a return.'),
    gpu_row('gpu-probe-read', 17, "GPU core identity read as the vendor's probe makes it, then after its secure configuration",
            'probe-read', 'COMPLETE GPU probe read: identity 36.56.104.183 before and after the secure configuration, '
            'restored, OFF and provider parked',
            'Status 0 with both GPU PROBE identity lines matches=1, GPU PROBE window serror_pending=0, GPU PROBE result '
            'restored=1 and COMPLETE GPU probe read: identity 36.56.104.183 before and after the secure configuration, '
            'restored, OFF and provider parked; or status 1 with FAIL GPU probe read: identity or restore not as expected, GPU '
            'left powered after those lines; or a fault line from a synchronous abort in the write window; or no '
            'return. A return is followed by a responsive ROM. Record every line of kernel.log and the progress record '
            'at result.bin offset 128, which holds the row in flight and every completed value.',
            'apps/gpu-power-id, probe-read variant (apps/gpu-power-id/probe-read.c): the power-only ON sequence of '
            'the default GPU power path (DCDC4 960 mV, PLL_GPU0 at 1008 MHz, TOP5 and CORE6 in the vendor order), '
            'which must reach CORE6 status 8 within 200 ms before any GPU-window access. Then snapshot 1, the controller and CORE6 lines, '
            'identity Device-nGnRnE mapping checks of the wrapper, SMC0 and GPU core pages, and these 32-bit accesses '
            'in order: reads of 0x01880014 (drm_ctrl), 0x01880010 (irq_ctrl) and 0x0a0000b0 (SMC0); the core identity '
            'as the vendor reads it at probe time, 0x01800024 and then 0x01800020; then, with D, A, I and F masked and '
            'restore vectors installed, 2 to 0x0a0000b0 and 0 to 0x01880014 (the vendor sunxi_secure_config, in its '
            'order), each read back, the identity again, and both words written back to their entry values, each read '
            'back. Then the OFF, the supply end and the provider park of the power-only image, so the lifetime can '
            'continue. Before each access the image writes the row number into the progress '
            'record at result offset 128 and prints a GPU PROBE reading or writing line. docs/gpu-power.md, "The '
            'probe-read probe makes the vendor\'s probe-time read, then the secure configuration", maps each result '
            'to its meaning. It runs after the cold DDR start and the provider-readiness row; '
            'docs/designs/gpu-firmware-start.md places it before the firmware start.',
            after=['gpu-provider-ready'],
            failures=['`FAIL GPU probe read: identity or restore not as expected, GPU left powered`, after the GPU PROBE '
                      'identity, window and result lines: record them',
                      'a fault line after a `GPU PROBE reading` line of rows 10 or 11: a synchronous abort on the '
                      'second identity read; the restore vectors wrote both words back before the fault report',
                      '`FAIL GPU ON deadline` (CORE6 not at 8 within 200 ms), `GPU probe read mapping` or `GPU '
                      'probe read time budget`: no wrapper, SMC0 or core word was accessed',
                      '`FAIL GPU probe read vector table` after the GPU PROBE identity before line: VBAR_EL1 was not '
                      'the image\'s table, so nothing was written',
                      'status 7 with `FAIL GPU probe read window refused: monitor deadline too near, nothing written`: '
                      'the write window would have ended less than 500 ms before the monitor\'s 2 s deadline'],
            admission='GPU-window and SMC0 exception: this row alone, after the default ON, reads the GPU core identity as the vendor probe does '
                      '(rgxbvnc.c:119 through OSReadHWReg64, two 32-bit reads of 0x01800024 and 0x01800020, '
                      'osfunc.h:1086-1095), then makes the two writes of the vendor sunxi_secure_config '
                      '(sunxi_platform.c:964-979) in its order and with its values, 2 to SMC0 0x0a0000b0 and 0 to the '
                      'GPU wrapper drm_ctrl 0x01880014, reads the identity again, and writes both words back to the '
                      'values it read before them. It also reads 0x01880010 and, before the writes, the two written '
                      'words; these reads and the readbacks are the probe\'s own. Each access is a 32-bit '
                      'Device-nGnRnE access after an identity mapping check of its page. The coordinator accepted the '
                      'SMC0 write as part of the GPU grant for this row, because the vendor driver makes it on every '
                      'GPU power-on. The A733 user manual places SMC0 at 0x0A000000-0x0A00FFFF in the memory '
                      'controller block (MEMC0, interrupt 213, MEMC0_SMC0) and describes none of its registers; the '
                      'name SMC_DRM_GPU_HW_RST of offset 0xb0 comes only from the vendor source. The GPU runs at '
                      '1008 MHz from PLL_GPU0 and 960 mV. Risk: a GPU core read that never completes stops the '
                      'CPU without an exception, the monitor does not return and only a power removal recovers the '
                      'board; after the writes such a stall leaves SMC0 at 2 and drm_ctrl at 0 until that removal. A '
                      'synchronous abort in the write window restores both words before the fault report; an '
                      'asynchronous abort stays pending until both are restored and is reported. The monitor\'s 2 s '
                      'recovery is an EL3 FIQ that the restores do not cover; the window starts only while at least '
                      '500 ms remain before it, and a recovery inside the window would leave both words written until '
                      'the power removal. What SMC0 +0xb0 = 2 does is unknown; if it cut non-secure DRAM access the '
                      'run would not return. The progress record survives only on a return.'),
    read_row('el3-security-switches', 18, 'The security words that every EL3 entry writes and the board can read back',
             'Board evidence of the EL3 security setup (include/a733_el3_security.h), whose writes are never '
             'restored: CCMU_SEC_SWITCH_REG and PRCM_SEC_SWITCH_REG (writes 14 and 13), which read 0 after a power '
             'removal and 7 after the setup; the three S_TZMA words (write 11), predicted 0 after the setup, which '
             'no board has read; and SPC 0x020540e0, port 14\'s status word (write 17), 0x1060 after a power '
             'removal and 0xfffffbff after the setup. This is bundle 18 from set board-bundles-20261025-10 on; in '
             'set board-bundles-20261025-4 the number named the removed gpu-vendor-clk row',
             [word(0x02003f00, 'ccu', note='CCMU_SEC_SWITCH_REG: 7 after write 14'),
              word(0x07010290, 'r_ccu', note='PRCM_SEC_SWITCH_REG: bits 2:0 set after write 13'),
              word(0x07003000, 's_tzma', note='S_TZMA +0x0: 0 after write 11'),
              word(0x07003004, 's_tzma', note='S_TZMA +0x4: 0 after write 11'),
              word(0x07003008, 's_tzma', note='S_TZMA +0x8: 0 after write 11'),
              word(0x020540e0, 'spc', note='SPC port 14 status word: 0xfffffbff after write 17')],
             'All six words are read and the 128 scratch bytes are restored and equal.'),
    gpu_row('gpu-fw-start', 19, 'GPU firmware start after the vendor secure configuration and timer record',
            'fw-start', 'COMPLETE GPU firmware start: started word 1 after the timer record and the secure '
            'configuration, GPU left running for the power removal',
            'Status 0 with GPU FWS start result=0 phase=3 error=0, GPU FWS shared started=0x1 and COMPLETE GPU '
            'firmware start: started word 1 after the timer record and the secure configuration, GPU left running '
            'for the power removal; or status 1 with GPU FWS start result=3 and FAIL GPU firmware start: started '
            'word not 1, GPU left running after the shared-state and register lines; or no return. Either return '
            'leaves the GPU powered with its firmware processor released; the lifetime ends there and a power '
            'removal follows. Record every line of kernel.log and the progress record at result.bin offset 128, '
            'which names the access in flight.',
            'apps/gpu-power-id, firmware-start variant (apps/gpu-power-id/fw-start.c), step 2 of '
            'docs/designs/gpu-firmware-start.md. Before any power change it prepares the pinned open firmware '
            '(sha256 1db1c399...): code, data and the MIPS page table in write-back image RAM, the nineteen shared '
            'resources in the image\'s Normal-NC block at 0x41c00000, system and OS init with the 1,008 MHz core '
            'clock, the queues and the 29-step start plan. Then the power-only ON of the default GPU power path, '
            'which must reach CORE6 status 8 within 200 ms, snapshot 1, mapping checks, and these 32-bit accesses '
            'in order: the identity 0x01800024 and 0x01800020, which must be 36.56.104.183; drm_ctrl 0x01880014 '
            'and SMC0 0x0a0000b0; RGX_CR_TIMER 0x01800164 and 0x01800160 for one timer-correlation record; 2 to '
            'SMC0 0x0a0000b0 and 0 to drm_ctrl, each read back as an observation and never written back; then the '
            'start of drivers/gpu/pvr_gpu_start.c without SOFT_RESET2, every 64-bit register as two 32-bit '
            'accesses (low word written first, high word read first), and a wait of at most 1 s for the 32-bit '
            'started word; then reads of SOFT_RESET and the MIPS wrapper registers. The start begins only while '
            'its 1 s bound and a 100 ms report fit before the 1.5 s session deadline, which keeps 500 ms before '
            'the monitor\'s 2 s recovery. Before each access the image writes the access number into the progress '
            'record and prints a GPU FWS reading or writing line. It runs after the cold DDR start and the '
            'provider-readiness row, in a lifetime of its own, and is the last step of that lifetime.',
            after=['gpu-provider-ready'],
            failures=['`FAIL GPU firmware start: started word not 1, GPU left running`, after the `GPU FWS start`, '
                      '`shared`, `state` and `register` lines: error 2 is the 1 s deadline, 4 a started word already '
                      'set before the start, 5 a word other than 0 or 1; the firmware processor stays released',
                      '`FAIL GPU firmware start: identity not 36.56.104.183, nothing written, GPU left powered`',
                      '`FAIL GPU firmware start preparation, nothing powered`, `GPU firmware start mapping` or '
                      '`GPU firmware start time budget`: no GPU, wrapper or SMC0 word was accessed',
                      'status 7 with `FAIL GPU firmware start refused: deadline too near, nothing written`'],
            admission='GPU firmware exception: this row alone starts the GPU firmware processor. After the default '
                      'ON it makes the vendor sunxi_secure_config writes (sunxi_platform.c:964-979), 2 to SMC0 '
                      '0x0a0000b0 and 0 to drm_ctrl 0x01880014, and does not write them back, as the vendor never '
                      'does; step 1 on port 3-1 (board-session-20261025-14) read SMC0 +0xb0 as 0 right after '
                      'writing 2 and drm_ctrl as 0 after writing 0. It then writes the GPU registers of the vendor '
                      'RGXStart for this core (rgxstartstop.c): SYS_BUS_SECURE 0xa100, SOFT_RESET 0x100, '
                      'SLC_CTRL_MISC 0x3800, the eleven MIPS wrapper registers and AXI_ACE_LITE 0x38c0, never '
                      'SOFT_RESET2, and releases the firmware processor, which then runs the pinned open firmware '
                      'from the image\'s own memory: 139,264 bytes of write-back RAM at the image\'s fw_memory and the '
                      'first 184,320 bytes of the Normal-NC block at 0x41c00000, inside the granted RAM interval. The '
                      'firmware\'s MIPS page table maps only those pages. The image does not stop the firmware or '
                      'power the GPU off, because the vendor does both only after the firmware\'s own power-off '
                      'handshake, which this image does not carry: the GPU stays on at 960 mV and 1008 MHz with the '
                      'firmware running until the power removal that ends the lifetime. Risk: a GPU register access '
                      'that never completes stops the CPU, the monitor does not return and only a power removal '
                      'recovers the board; the firmware may write its own pages after the image returns, which no '
                      'later step of the lifetime reads.'),
    returning_row('cpu-b-800mv-step', 20, 'CPU-B 1,014 -> 1,404 -> 1,014 MHz at 800 mV with cores 6 and 7 off',
                  OPP_STEP_OVERLAY,
                  'COMPLETE board bundle 20: CPU-B 1404 MHz and back to 1014 MHz at 800 mV, no PMIC write', True,
                  'Monitor status 0 with two TEMP lines at level 0, the SUPPLY line (dcdc5_uv=800000 dcdc3_uv=800000 '
                  'sid=0x496 bin=4 dcxo_hz=26000000), the CPU-L, DSU and CPU-B words before, CPU-B raised with '
                  'pll=0xf8803600 mux=0x3000000 mhz=1404, CPU-B restored and CPU-B after with pll=0xf8802700 '
                  'mux=0x3000000 mhz=1014, each STEP line with dcdc3_uv=800000, and COMPLETE board bundle 20, then a '
                  'responsive ROM.',
                  'The first CPU operating-point step of docs/designs/cpu-top-opp.md, on the image of bundle 15 '
                  '(apps/board-bundles-power/cpu-opp-step.c). With cores 6 and 7 powered off, the client starts the '
                  'thermal driver and the thermal policy, reads one full sensor set, inspects CPU-L, the DSU and '
                  'CPU-B read-only through the CPU clock driver (DCDC5 and DCDC3 snapshots; PLL, mux, pattern and LFM '
                  'words), and admits the step only at the NORMAL thermal level on speed bin vf0400 with DCDC3 and '
                  'DCDC5 at 800 mV, CPU-B at 1,014 MHz and every pattern and LFM enable clear. The driver then raises '
                  'CPU-B to 1,404 MHz and lowers it to 1,014 MHz. Each change parks the CPU-B mux on the 26 MHz '
                  'oscillator for 20 us, writes N with the update bit, waits for update and for three lock reads 3 us '
                  'apart, waits 20 us, selects the PLL and waits 20 us. Both points need 800 mV in vf0400, so no PMIC '
                  'register is written; after each change the image requires DCDC3 to have kept 800 mV. A second inspection of '
                  'the three domains must read every PLL and mux word and both rails as before, then a second sensor set '
                  'follows, the thermal converter is disabled and the PMIC service parked. Approved by the user on 2026-10-24 (CPU clock and voltage); '
                  'bundle/approval.json binds the values to port 3-3 and this image.',
                  sources=OPP_STEP_SOURCES, base=BASE_ROW, approval=OPP_APPROVAL,
                  failures=['a `FAIL` line of the provider, the thermal driver or the CPU clock driver, after a `STEP` '
                            'line naming the driver state: `transition` after the park leaves CPU-B on the oscillator '
                            'or the PLL, which only a power removal restores']),
    opp_row('dcdc3-1050mv-step', 21, 'DCDC3 800 -> 1,050 -> 800 mV with CPU-B held at 1,014 MHz',
            'DCDC3 1050 mV and back to 800 mV, CPU-B held at 1014 MHz',
            'VOLT dcdc3_selector=0xb7 dcdc3_uv=1050000, a TEMP line below 70000 at level 0, then VOLT '
            'dcdc3_selector=0x9e dcdc3_uv=800000; CPU-L, CPU-B and DSU after with the words they had before',
            'Step 3 of docs/designs/cpu-top-opp.md, the first DCDC3 selector write. Under an exclusive CPU-B clock '
            'lease, so CPU-B stays at 1,014 MHz, and an exclusive DCDC3 lease, the client reads DCDC3, writes '
            'selector 0x37 (1,050 mV) through the PMIC provider, which requires the inherited 0x1e first, keeps bit '
            '7 and reads the byte back, waits 1 ms and reads it again; after a sensor set it writes 0x1e (800 mV) the '
            'same way. 1,014 MHz needs 800 mV, so both voltages cover it. No clock register is written.', ()),
    opp_row('cpu-l-800mv-step', 22, 'CPU-L 494 -> 1,196 MHz and the DSU 494 -> 780 MHz at 800 mV',
            'CPU-L 1196 MHz and the DSU 780 MHz at 800 mV',
            'RATE cpu0_hz near 494000000, CPU-L set pll=0xf8802e00 mux=0x3000000 mhz=1196, RATE cpu0_hz near '
            '1196000000, a TEMP line below 70000, DSU set pll=0xf8801e00 mux=0x3000105 mhz=780, every STEP line with '
            'dcdc5_uv=800000 and dcdc3_uv=800000',
            'Step 2 of docs/designs/cpu-top-opp.md: the first park of the cluster that runs the image, as boot0 parks '
            'it. CPU0 measures its own rate with the cycle counter against CNTPCT over 1 ms before and after the '
            'change and requires it within 1% of the configured rate. The DSU then goes to 780 MHz, under three '
            'quarters of 1,196 MHz. Both points need 800 mV in vf0400, so no PMIC register is written. CPU-L and the '
            'DSU stay at their new rates.', ('dcdc3-1050mv-step',)),
    opp_row('cpu-b-top-step', 23, 'CPU-B to 2,002 MHz at 1,050 mV with the DSU at 910 MHz, then back',
            'CPU-B 2002 MHz at 1050 mV with the DSU at 910 MHz, then CPU-B 1014 MHz at 800 mV',
            'CPU-B set pll=0xf8804d00 mhz=2002 with STEP dcdc3_uv=1050000, a TEMP line below 70000, DSU set '
            'pll=0xf8802300 mux=0x3000105 mhz=910, a TEMP line, DSU set pll=0xf8802e00 mux=0x3010105 mhz=598, CPU-B '
            'set pll=0xf8802700 mhz=1014 with STEP dcdc3_uv=800000',
            'Step 4 of docs/designs/cpu-top-opp.md, with cores 6 and 7 off: the driver raises DCDC3 to 1,050 mV, '
            'waits 1 ms and relocks CPU-B at 2,002 MHz, the top point of vf0400; the DSU then goes to 910 MHz, the '
            'rate at which the vendor holds the DSU while CPU-B runs at 1,488 MHz or more, which the driver\'s '
            'three-quarters cap admits only after CPU-B is above 1,214 MHz. The DSU then goes to 598 MHz, the '
            'fastest 800 mV DSU row at or below three quarters of 1,014 MHz whatever CPU-L\'s rate, and CPU-B back '
            'to 1,014 MHz, after which the driver writes DCDC3 back to 800 mV. The DSU stays at 598 MHz.',
            ('cpu-l-800mv-step',)),
    opp_row('cpu-l-dsu-top-step', 24, 'CPU-L to 1,794 MHz at 1,000 mV, then the DSU to 1,274 MHz',
            'CPU-L 1794 MHz at 1000 mV and the DSU 1274 MHz',
            'RATE cpu0_hz near the entry rate, CPU-L set pll=0xf8804500 mux=0x3000000 mhz=1794 with STEP '
            'dcdc5_uv=1000000, RATE cpu0_hz near 1794000000, a TEMP line below 70000, DSU set pll=0xf8803100 '
            'mux=0x3000105 mhz=1274 with dcdc5_uv=1000000',
            'Step 5 of docs/designs/cpu-top-opp.md: the driver raises DCDC5, which feeds CPU-L and the DSU, to '
            '1,000 mV, waits 1 ms and relocks CPU-L at 1,794 MHz, measured by CPU0; the DSU then goes to 1,274 MHz '
            'with no voltage write. CPU-L, the DSU and DCDC5 stay at the top points of vf0400, so this step ends its '
            'lifetime.', ('cpu-b-top-step',)),
    gpu_row('gpu-fw-stop', 25, 'GPU firmware start, one health-check command, firmware stop and OFF',
            'fw-stop', 'COMPLETE GPU firmware stop: started, health check executed, forced idle and power off '
            'acknowledged, idle, OFF and provider parked',
            'Status 0 with GPU FWS start result=0, GPU FWS control result=0 phase=4 error=0 health=1 '
            'health_status=0x0, GPU FWS stop result=0 phase=10, GPU FWS power ack_idle=0x1 ack_off=0x1 '
            'state_idle=0x2 state_off=0x0 and COMPLETE GPU firmware stop: started, health check executed, '
            'forced idle and power off acknowledged, idle, OFF and provider parked; or one of the controlled '
            'failures listed below; or no return. Record every line of kernel.log and the progress record at '
            'result.bin offset 128, which names the access in flight and its step.',
            'apps/gpu-power-id, firmware-stop variant (apps/gpu-power-id/fw-start.c built with FWS_STOP), the '
            'combined steps (a) and (b) of docs/designs/gpu-firmware-stop.md. It runs the firmware start of bundle '
            '19, with the vendor 500 ms started-word bound. Then the firmware control: it waits for the firmware\'s '
            'compatibility record, starts the kernel CCB and submits one health-check command (type 115 of the '
            'open firmware), kicking MTS_SCHEDULE 0x01800b00 with 0 and reading it back, and waits for its return '
            'slot; it reads the MIPS wrapper interrupt status 0x018008a8 and clears its event bit at 0x018008b0. '
            'Then the stop: a forced-idle request and a forced power-off request, each acknowledged through the '
            'power-sync word with the firmware power state FORCED_IDLE and then OFF; the vendor RGXStop register '
            'sequence (interrupt acknowledgement, Sidekick and SLC idle polls, thread-0 DM association clears, BIF '
            'and SLC status polls, no SOFT_RESET); then the platform OFF, the supply end and the provider park, so '
            'the lifetime can continue. Every GPU-window access is a 32-bit access after its progress record. It '
            'runs after the cold DDR start and the provider-readiness row, in a lifetime of its own.',
            after=['gpu-provider-ready'],
            failures=['`FAIL GPU firmware control: compatibility or health check not complete, GPU left running`, '
                      'after the `GPU FWS control` and `compat` lines: error 4 an incompatible record, 3 the '
                      '200 ms deadline, 7 a firmware event; the firmware stays running and a power removal follows',
                      '`FAIL GPU firmware stop: not complete, GPU left powered`, after the `GPU FWS stop`, `power` '
                      'and `after` lines: a power request not acknowledged within 300 ms, an acknowledgement with '
                      'another power state, or a platform OFF fault; a power removal follows',
                      '`FAIL GPU firmware stop: idle check not met after the power-off acknowledgement, GPU OFF and '
                      'provider parked`: the vendor powers off in that case too; the GPU is off',
                      '`FAIL GPU firmware start: started word not 1, GPU left running` or a start refusal, as in '
                      'bundle 19'],
            admission='GPU firmware exception: this row starts the GPU firmware as bundle 19 does, then sends it '
                      'three kernel CCB commands, a health check and the forced-idle and power-off requests, each '
                      'in the shared queues of the image\'s own Normal-NC block, kicked through MTS_SCHEDULE '
                      '0x01800b00. It reads and clears the MIPS wrapper interrupt (0x018008a8, 0x018008b0), writes '
                      'the thread-0 DM associations 0x01800b40 and 0x01800b30 to 0, polls the Sidekick, SLC, BIF '
                      'and SLC status registers (0x018003c8, 0x01803898, 0x01801358, 0x01801350, 0x01801320, '
                      '0x01801338, 0x01803870), and powers the GPU off through the default path only after the '
                      'firmware acknowledged the power-off request. No GPU job, context or memory outside the '
                      'firmware\'s own mappings is created. Risk: a register access that never completes stops the '
                      'CPU and only a power removal recovers the board; a failure before the power-off '
                      'acknowledgement leaves the firmware running until the power removal.'),
    gpu_row('gpu-fw-stop-again', 26, 'The GPU firmware stop image again in the same lifetime, with no power removal',
            'fw-stop', 'COMPLETE GPU firmware stop: started, health check executed, forced idle and power off '
            'acknowledged, idle, OFF and provider parked',
            'Status 0 with GPU FWS start result=0, GPU FWS control result=0 phase=4 error=0 health=1 '
            'health_status=0x0, GPU FWS stop result=0 phase=10, GPU FWS power ack_idle=0x1 ack_off=0x1 '
            'state_idle=0x2 state_off=0x0 and COMPLETE GPU firmware stop: started, health check executed, '
            'forced idle and power off acknowledged, idle, OFF and provider parked; or one of the controlled '
            'failures listed below; or no return. Record every line of kernel.log and the progress record at '
            'result.bin offset 128, which names the access in flight and its step.',
            'Step (c) of docs/designs/gpu-firmware-stop.md: the image of bundle 25, the same build, run a second time '
            'in the lifetime right after bundle 25 passed, with no power removal between them, so that its start '
            'begins from the stopped firmware, the GPU OFF and DCDC4 already at 960 mV. A pass shows that the stop '
            'leaves a state from which the firmware starts again. As in bundle 25:'
            ' apps/gpu-power-id, firmware-stop variant (apps/gpu-power-id/fw-start.c built with FWS_STOP), the '
            'combined steps (a) and (b) of docs/designs/gpu-firmware-stop.md. It runs the firmware start of bundle '
            '19, with the vendor 500 ms started-word bound. Then the firmware control: it waits for the firmware\'s '
            'compatibility record, starts the kernel CCB and submits one health-check command (type 115 of the '
            'open firmware), kicking MTS_SCHEDULE 0x01800b00 with 0 and reading it back, and waits for its return '
            'slot; it reads the MIPS wrapper interrupt status 0x018008a8 and clears its event bit at 0x018008b0. '
            'Then the stop: a forced-idle request and a forced power-off request, each acknowledged through the '
            'power-sync word with the firmware power state FORCED_IDLE and then OFF; the vendor RGXStop register '
            'sequence (interrupt acknowledgement, Sidekick and SLC idle polls, thread-0 DM association clears, BIF '
            'and SLC status polls, no SOFT_RESET); then the platform OFF, the supply end and the provider park, so '
            'the lifetime can continue. Every GPU-window access is a 32-bit access after its progress record. It '
            'runs after the cold DDR start and the provider-readiness row, in a lifetime of its own.',
            after=['gpu-provider-ready', 'gpu-fw-stop'],
            history=('one complete GPU firmware cycle has run in this lifetime, bundle 25: firmware start, one '
                     'health check, the forced-idle and power-off requests, the stop and the OFF, with the GPU OFF '
                     'and the provider parked at its end'),
            failures=['`FAIL GPU firmware control: compatibility or health check not complete, GPU left running`, '
                      'after the `GPU FWS control` and `compat` lines: error 4 an incompatible record, 3 the '
                      '200 ms deadline, 7 a firmware event; the firmware stays running and a power removal follows',
                      '`FAIL GPU firmware stop: not complete, GPU left powered`, after the `GPU FWS stop`, `power` '
                      'and `after` lines: a power request not acknowledged within 300 ms, an acknowledgement with '
                      'another power state, or a platform OFF fault; a power removal follows',
                      '`FAIL GPU firmware stop: idle check not met after the power-off acknowledgement, GPU OFF and '
                      'provider parked`: the vendor powers off in that case too; the GPU is off',
                      '`FAIL GPU firmware start: started word not 1, GPU left running` or a start refusal, as in '
                      'bundle 19'],
            admission='GPU firmware exception: this row starts the GPU firmware as bundle 19 does, then sends it '
                      'three kernel CCB commands, a health check and the forced-idle and power-off requests, each '
                      'in the shared queues of the image\'s own Normal-NC block, kicked through MTS_SCHEDULE '
                      '0x01800b00. It reads and clears the MIPS wrapper interrupt (0x018008a8, 0x018008b0), writes '
                      'the thread-0 DM associations 0x01800b40 and 0x01800b30 to 0, polls the Sidekick, SLC, BIF '
                      'and SLC status registers (0x018003c8, 0x01803898, 0x01801358, 0x01801350, 0x01801320, '
                      '0x01801338, 0x01803870), and powers the GPU off through the default path only after the '
                      'firmware acknowledged the power-off request. No GPU job, context or memory outside the '
                      'firmware\'s own mappings is created. Risk: a register access that never completes stops the '
                      'CPU and only a power removal recovers the board; a failure before the power-off '
                      'acknowledgement leaves the firmware running until the power removal.'),
    read_row('clock-audit-words', 27, 'Every CCU, PRCM and CPU PLL clock register, the speed bin and the RTC clock words',
             'The clock state of the board, read only, for the rate audit of docs/clock-audit.md: each PLL, bus, '
             'module clock and gate word that the CCU, the PRCM and the CPU PLL block document, the SID speed-bin '
             'word, and the RTC oscillator words behind their bus gate',
             [word(0x0300624c, 'sid', note='SID speed-bin word')] +
             [word(CCU + o, 'ccu', note=f'CCU +0x{o:x}') for o in CCU_OFFSETS] +
             [word(0x07010000 + o, 'r_ccu', note=f'PRCM +0x{o:x}') for o in R_CCU_OFFSETS] +
             [word(0x08870000 + o, 'cpu_pll', note=f'CPU PLL +0x{o:x}') for o in CPU_PLL_OFFSETS] +
             [word(0x07090000 + o, condition=R_CCU_RTC, note=f'RTC +0x{o:x}') for o in RTC_CLOCK_OFFSETS],
             'Every word is read, the RTC words only after the R_CCU RTC gate reads open and its reset released, '
             'and the 128 scratch bytes are restored and equal.'),
    gpu_row('gpu-first-job', 28, 'The first GPU job: the fixed 32-lane compute job, then the firmware stop and OFF',
            'fw-job', 'COMPLETE GPU first job: 32 words as computed, context cleaned up, firmware stopped, OFF and '
            'provider parked',
            'Status 0 with GPU FWJ job complete=1 outcome=1 timeline=0x1, GPU FWJ events control=4 control_error=0 '
            'n=0, GPU FWJ hwr mmu_status=0x0 count=0, GPU FWJ output w0=0x2e5ca990 w1=0xd09b3063 w31=0x4775f521 '
            'guard=0x6d35a7c1 (expected0 and expected31 equal), GPU FWJ cleanup status=0x1, GPU FWS stop result=0 '
            'phase=10 and COMPLETE GPU first job: 32 words as computed, context cleaned up, firmware stopped, OFF and '
            'provider parked; or one of the controlled failures below; or no return. Record every line of kernel.log '
            'and the progress record at result.bin offset 128.',
            'apps/gpu-power-id, first-job variant (apps/gpu-power-id/fw-start.c built with FWS_STOP and FWS_JOB, with '
            'apps/gpu-power-id/fw-job.inc), docs/designs/gpu-first-job.md. The firmware lifecycle image of bundle 25 '
            'with the fixed numerical compute job between the health check and the stop, corrected after set 27 '
            '(docs/designs/gpu-first-job.md, "Set 27 post-mortem"): the job\'s PDS pointer constants are stored low '
            'word first, an MMUCACHE command precedes the KICK, no page is mapped GPU read-only, every firmware event '
            '101-108 is recorded without failing the control, and a stop the firmware does not acknowledge runs '
            'upstream\'s hard reset (the idle checks, the full soft reset, then the OFF). Before any power change it '
            'builds, in its own memory, the workload GPU VM (88 write-back pages, the output page bypassing the GPU '
            'system-level cache), the context programs, the compute context and the job (seed 0x12345678, factor '
            '0x9e3779b1), and maps the six firmware objects for the firmware at 0xc0800000 + n * 0x10000 in its '
            'Normal-NC block. After the health check it publishes the workload pages and tables, sends MMUCACHE and '
            'waits for its execution, formats the client CCB, writes a CDM command and a timeline UPDATE, kicks, '
            'waits at most 150 ms for the timeline word, checks all 32 words and both guards, sends the context '
            'cleanup, and then stops the firmware and powers the GPU off whatever the job\'s result. It runs first '
            'in a new lifetime, after the provider-readiness checkpoint.',
            after=['gpu-provider-ready'],
            failures=['`FAIL GPU first job: not complete, GPU OFF and provider parked`, after the `GPU FWJ job`, '
                      '`events`, `hwr`, `ring` and `output` lines: outcome 2 the timeline did not reach 1 within '
                      '150 ms, 3 an output word or guard differs, 4 a transport fault, 5 a firmware event (a `GPU FWJ '
                      'reset` line gives the reason, data master, page-fault flag and fault address); the firmware '
                      'acknowledged the stop, and the GPU is off',
                      '`FAIL GPU firmware stop: not acknowledged, idle checks met, GPU OFF and provider parked`: the '
                      'control failed or the firmware acknowledged neither power request; the stop ran upstream\'s '
                      'hard reset (`GPU FWS stop ... unacknowledged=1`) and the GPU is off',
                      '`FAIL GPU firmware stop: not acknowledged and the GPU not idle, GPU left powered`: an idle '
                      'check never matched; a power removal follows',
                      '`FAIL GPU first job: 32 words as computed, context cleanup not confirmed, GPU OFF and provider '
                      'parked`: the cleanup command returned no word of 1',
                      'every failure line of bundle 25'],
            admission='GPU job exception: this row runs one GPU compute job. Its data and programs are 88 write-back '
                      'pages of the image\'s own RAM, mapped only in the job\'s GPU VM, whose tables are also image '
                      'RAM; the firmware reaches only its own mappings plus the six job objects in the image\'s '
                      'Normal-NC block. The GPU reads the programs, control stream and input and writes only the '
                      '256-byte output page; no other memory is mapped for the GPU. It sends MMUCACHE, the KICK and '
                      'the context cleanup through the kernel CCB, besides bundle 25\'s commands. On set 27 the USC '
                      'program ran and faulted on its first load because its input pointer\'s halves were exchanged; '
                      'the GPU MMU refused the access and the firmware reset the context. Risk: as bundle 25; a GPU '
                      'that never reports idle after a failed job stays on until a power removal.'),
    gpu_row('gpu-job-session', 29, 'Eight timed GPU compute jobs on one context in one firmware session, then the '
            'firmware stop and OFF',
            'fw-session', 'COMPLETE GPU job session: 8 jobs as computed, context cleaned up, firmware stopped, OFF and '
            'provider parked',
            'Status 0 with GPU FWJ job complete=1 outcome=1 timeline=0x8 jobs=8, eight GPU FWJ run lines n=1 to 8 '
            'with write=0xb8 0x170 0x228 0x2e0 0x398 0xb8 0x170 0x228, GPU FWJ ring cccb_read=0x228 '
            'cccb_write=0x228, GPU FWJ output w0=0x2e5ca990 w1=0xd09b3063 w31=0x4775f521 guard=0x6d35a7c1, GPU FWJ '
            'cleanup status=0x1 kccb_done=0xb, GPU FWS stop result=0 phase=10 and COMPLETE GPU job session: 8 jobs '
            'as computed, context cleaned up, firmware stopped, OFF and provider parked; or one of the controlled '
            'failures below; or no return. Record every line of kernel.log and the progress record at result.bin '
            'offset 128.',
            'apps/gpu-power-id, session variant (apps/gpu-power-id/fw-start.c built with FWS_STOP, FWS_JOB and '
            'FWS_JOBS=8), docs/designs/gpu-job-session.md. The first-job image of bundle 28 (qualified on port 3-1 '
            'in set board-bundles-20261025-29), with seven more jobs on the same context by pvr_compute_job_repeat: '
            'each job refills and cleans the output, writes the CDM command and a timeline UPDATE to its number, '
            'and kicks; the client CCB wraps before job 6 with a padding command. Each job is timed in 24 MHz '
            'counter ticks from the MTS_SCHEDULE write of its KICK to the first read of its timeline value, by a '
            'spin that touches no register, and the image logs the ticks of its preparation, publication, '
            'MMUCACHE, jobs, cleanup and park. The GPU stays at 1,008 MHz with DCDC4 at 960 mV, the vendor top '
            'operating point. It runs first in a new lifetime, after the provider-readiness checkpoint.',
            after=['gpu-provider-ready'],
            failures=['`FAIL GPU job session: not complete, GPU OFF and provider parked`, after the `GPU FWJ job` '
                      'line with `jobs=` the completed jobs: outcome 2 a kicked job\'s timeline did not arrive '
                      'within 150 ms (its run line shows ticks=0; no cleanup), 3 an output word or guard differs, 4 a '
                      'transport fault or a job never kicked, 5 a firmware event (a `GPU FWJ reset` line gives the '
                      'reason, data master, page-fault flag and fault address), 6 too little of the wait left for '
                      'another job; the firmware acknowledged the stop, and the GPU is off',
                      '`FAIL GPU firmware stop: not acknowledged, idle checks met, GPU OFF and provider parked`: the '
                      'control failed or the firmware acknowledged neither power request; the stop ran upstream\'s '
                      'hard reset and the GPU is off',
                      '`FAIL GPU firmware stop: not acknowledged and the GPU not idle, GPU left powered`: an idle '
                      'check never matched; a power removal follows',
                      '`FAIL GPU job session: 8 jobs as computed, context cleanup not confirmed, GPU OFF and provider '
                      'parked`: the cleanup command returned no word of 1',
                      'every failure line of bundle 25'],
            admission='GPU job exception: this row runs eight GPU compute jobs on one context, the job of bundle 28 '
                      'repeated. Its data and programs are 88 write-back pages of the image\'s own RAM, mapped only in '
                      'the job\'s GPU VM, whose tables are also image RAM; the firmware reaches only its own mappings '
                      'plus the six job objects in the image\'s Normal-NC block. The GPU reads the programs, control '
                      'stream and input and writes only the 256-byte output page. It sends MMUCACHE, eight KICKs and '
                      'the context cleanup through the kernel CCB, besides bundle 25\'s commands. The client CCB '
                      'wraps for the first time on the board. Risk: as bundle 28; a GPU that never reports idle after '
                      'a failed job stays on until a power removal.'),
    opp_row('cpu-top-hold-step', 30, 'CPU-L 1,794 and the DSU 1,274 MHz with DCDC5 1,000 mV, then DCDC3 1,050 mV '
            'with CPU-B at 1,014 MHz, for the ROM return',
            'CPU-L 1794 MHz and DCDC5 1000 mV, the DSU 1274 MHz, DCDC3 1050 mV with CPU-B at 1014 MHz',
            'CPU-L set pll=0xf8804500 mux=0x3000000 mhz=1794 with STEP dcdc5_uv=1000000, a TEMP line below 70000, '
            'DSU set pll=0xf8803100 mux=0x3000105 mhz=1274, VOLT dcdc3_selector=0xb7 dcdc3_uv=1050000, a TEMP line',
            'Step 6 of docs/designs/cpu-top-opp.md, first image: with cores 6 and 7 off, the driver raises DCDC5 to '
            '1,000 mV and CPU-L to 1,794 MHz, then the DSU to 1,274 MHz; the image then raises DCDC3 to 1,050 mV '
            'with CPU-B held at 1,014 MHz, as step 3 did. The ROM return that follows resets CPU-L and the DSU and '
            'keeps the rails, which bundle 31 reads back; it does not reset CPU-B, which this image never changes. '
            'The workload relocks all three itself and lowers CPU-B to 1,014 MHz again before its return. Steps 1 '
            'to 5 passed on port 3-3 in earlier lifetimes. Set step6-set-5 ran this image as bundle 25, before the GPU '
            'and clock-audit rows took 25 to 29.', ()),
    opp_row('cpu-rails-held-read', 31, 'DCDC5 1,000 mV and DCDC3 1,050 mV after the ROM return, read only',
            'DCDC5 1000 mV and DCDC3 1050 mV kept across the ROM return, read only',
            'CPU-L before pll=0xf8802600 mux=0x3010000 mhz=494, CPU-B before pll=0xf8802700 mux=0x3000000 mhz=1014 '
            'and DSU before pll=0xf8802600 mux=0x3010105 mhz=494, which the image requires',
            'Step 6 of docs/designs/cpu-top-opp.md, the gate before the workload: after bundle 30 and one ROM return, '
            'the rails must still read DCDC5 1,000 mV and DCDC3 1,050 mV, the voltages that the workload\'s relock '
            'to the top points needs, and the three domains their boot PLL and mux words; a REFUSED line for the '
            'CPU supplies or the boot clock words ends the lifetime before the workload.',
            ('cpu-top-hold-step',), entry=('DCDC5 at 1,000 mV and DCDC3 at 1,050 mV, CPU-B at 1,014 MHz',
                                           'dcdc5_uv=1000000 dcdc3_uv=1050000'), writes=False),
    opp_row('cpu-after-workload-read', 32, 'Temperatures, rails and boot clock words after the top-point workload, '
            'read only',
            'temperatures, rails and boot clock words after the top-point workload, read only',
            'CPU-L before pll=0xf8802600 mux=0x3010000 mhz=494, CPU-B before pll=0xf8802700 mux=0x3000000 mhz=1014 '
            'and DSU before pll=0xf8802600 mux=0x3010105 mhz=494, which the image requires',
            'Step 6 of docs/designs/cpu-top-opp.md, after a completed workload: the workload lowered CPU-B to '
            '1,014 MHz before its return and the ROM return reset CPU-L and the DSU, so the three domains must read '
            'their boot words, with DCDC5 at 1,000 mV and DCDC3 at 1,050 mV; two temperature sets. A REFUSED line '
            'for the boot clock words or the CPU-B rate means CPU-B is still raised: remove power at once.',
            ('cpu-rails-held-read',), entry=('DCDC5 at 1,000 mV and DCDC3 at 1,050 mV, CPU-B at 1,014 MHz',
                                             'dcdc5_uv=1000000 dcdc3_uv=1050000'), writes=False),
    # `admission` maps fields of the value prepare() returns to the values that
    # identify this row's build. None refuses the row: no owning package has
    # named those values yet.
    {'name': 'memory-qualification', 'class': 'monitor', 'renewal_cap': True, 'admission': None,
     'component': 'DDR memory qualification, one run of the three-step map chain',
     'module': 'apps/memtest/check.py', 'runner': 'tools/a733-returning-host.py',
     'sources': ['apps/memtest/check.py', 'tools/a733-returning-host.py', 'tools/prepare-board-bundles.py'],
     'expected_result': 'An image-initiated exit with every record field present, an alias-probe pass, an '
                        'address-line pass, a page-signature match, no external abort and no expired DMA0 '
                        'deadline, over the tested range the contract names.',
     'procedure': [
         'Confirm that ranks 1 to 6 of docs/audit/board-test-plan.md and the clock-words Read bundle passed '
         'earlier in this power cycle, with rank 3 reporting dram_mib 12288; nothing else has run since the cold '
         'DDR start.',
         'Claim this test, then run the memory qualification profile through {runner} on the frozen bundle '
         '{bundle}.',
         'Save the run record in this evidence directory; the next run is prepared from it on the host.'],
     'stop_condition': 'Stop at any record that does not chain: anything other than an image-initiated exit with '
                       'every field present, alias-probe, address-line and page-signature passes, no external '
                       'abort and no expired DMA0 deadline. Such a record retains the board: no further upload of '
                       'any kind, including rank 8 and the Terminal rows, until the user reviews the record, and '
                       'after a DMA0 expiry not until a FEL read also shows DMA0 quiet.'},
    {'name': 'npu-service-os-image', 'class': 'terminal', 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'NPU service in the integrated OS image',
     'module': 'apps/os/prepare.py', 'runner': 'tools/check-a733-os-supervised.py',
     'sources': OS_SOURCES + ['drivers/npu/npu_service.c', 'apps/os/services.c'],
     'expected_result': 'One admitted model completes and its output matches its host reference.' + TERMINAL_COUNTS},
    {'name': 'gpu-service-os-image', 'class': 'terminal', 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'GPU service in the integrated OS image',
     'module': 'apps/os/prepare.py', 'runner': 'tools/check-a733-os-supervised.py',
     'sources': OS_SOURCES + ['drivers/gpu/pvr_gpu_service.c', 'apps/os/services.c'],
     'expected_result': 'Every submitted GPU job completes and its output matches its host reference.'
                        + TERMINAL_COUNTS},
    {'name': 'accelerator-arbiter', 'class': 'terminal', 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'GPU and NPU jobs under the memory and power arbiter',
     'module': 'apps/accelerators/prepare.py', 'runner': 'tools/check-a733-accelerators-supervised.py',
     'sources': ['apps/accelerators/prepare.py', 'tools/check-a733-accelerators-supervised.py',
                 'apps/npu-convolution/prepare-supervised.py', 'drivers/npu/npu_service.c',
                 'drivers/gpu/pvr_gpu_service.c', 'tools/prepare-board-bundles.py', *TRANSPORT_SOURCES],
     'expected_result': 'The GPU and NPU jobs complete under the arbiter and each output matches its host '
                        'reference.' + TERMINAL_COUNTS},
    {'name': 'recognition-dispatcher', 'class': 'terminal', 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'Recognition dispatcher placing mimus linear layers on the CPU or GPU in the OS image',
     'module': 'apps/os/prepare.py', 'runner': 'tools/check-a733-os-supervised.py',
     'sources': OS_SOURCES + ['apps/os/dispatch.c', 'apps/os/placement.py', 'drivers/gpu/pvr_gpu_service.c'],
     'expected_result': 'Recognition of the fixed inputs matches the host transcript, with each linear layer '
                        'on the processor the placement table names.' + TERMINAL_COUNTS},
    {'name': 'gpu-npu-hang-reset', 'class': 'excluded',
     'reason': 'An intentional GPU or NPU hang needs physical recovery.'},
    {'name': 'voltage-clock-changes', 'class': 'excluded',
     'reason': 'A voltage or clock change needs physical recovery and the user\'s approval.'},
    {'name': 'display-output', 'class': 'excluded',
     'reason': 'Display output needs a connected monitor or panel.'},
    {'name': 'power-domain-5', 'class': 'excluded',
     'reason': 'Domain 5 ON and OFF needs DCDC4 and the user\'s approval.'},
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def save(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def sandbox(run_dir):
    """The bubblewrap command prefix for one preparation step."""
    run_dir = str(run_dir)
    return [BWRAP, '--ro-bind', '/', '/', '--dev', '/dev', '--proc', '/proc', '--tmpfs', '/sys',
            '--tmpfs', '/tmp', '--tmpfs', '/run', '--bind', run_dir, run_dir, '--unshare-all',
            '--die-with-parent', '--clearenv', '--setenv', 'PATH', '/usr/local/bin:/usr/bin:/bin',
            '--setenv', 'HOME', str(Path.home()), '--setenv', 'LANG', 'C.UTF-8',
            '--setenv', 'PYTHONDONTWRITEBYTECODE', '1', '--chdir', run_dir]


def literals(path):
    """Module-level NAME = literal assignments of a Python source, read without executing it."""
    found = {}
    for node in ast.parse(Path(path).read_text()).body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    try:
                        found.setdefault(target.id, []).append(ast.literal_eval(node.value))
                    except (ValueError, TypeError):
                        pass
    return {name: values[0] for name, values in found.items() if len(values) == 1}


def runner_terms(path):
    """The runner's PREREQUISITES, its COMPLETION_COUNTS (None when absent) and its largest capture window."""
    values = literals(path)
    prerequisites = values.get('PREREQUISITES')
    seconds = [int(x) for x in re.findall(r'capture_seconds\s*=\s*(\d+)', Path(path).read_text())]
    require(isinstance(prerequisites, set) and prerequisites and seconds,
            f'Runner {path} has no PREREQUISITES literal or capture window.')
    counts = values.get('COMPLETION_COUNTS')
    return sorted(prerequisites), (sorted(counts) if isinstance(counts, (set, tuple, list)) else None), max(seconds)


def transport_bounds(path):
    """The FEL request bound, transfer rate and SRAM backup regions of the supervised transport."""
    values = literals(path)
    request, rate, regions = (values.get(k) for k in ('FEL_REQUEST_SECONDS', 'FEL_BYTES_PER_SECOND', 'REGIONS'))
    require(type(request) is int and request > 0 and type(rate) is int and rate > 0 and isinstance(regions, list)
            and regions and all(len(r) == 3 and type(r[2]) is int and r[2] > 0 for r in regions),
            f'Transport {path} has no FEL bounds or SRAM regions.')
    return {'request': request, 'rate': rate, 'regions': [r[2] for r in regions]}


def terms(row, root):
    """Prerequisites, capture window and transport bounds of one row; refuses a Terminal row without counts."""
    if row['class'] == 'read':
        return [SUPERVISED_PREREQUISITE], None, None
    if row['class'] == 'monitor':
        return [row.get('prerequisite', MONITOR_PREREQUISITE)], None, None
    prerequisites, counts, seconds = runner_terms(root / row['runner'])
    require(counts == sorted(COUNT_FIELDS),
            f'Runner {row["runner"]} does not declare COMPLETION_COUNTS {", ".join(COUNT_FIELDS)}; its record '
            'cannot carry the per-job completion and wake-up counts.')
    return prerequisites, seconds, transport_bounds(root / row['transport'])


def terminal_timeout(seconds, sizes, bounds):
    """One supervised attempt's worst case, in run_session order, plus margin.

    Every upload is the bytes of one bundle file, so each bundle file is
    counted as an upload: a watchdog readl, then one write and readback.
    """
    request, rate, regions = bounds['request'], bounds['rate'], bounds['regions']

    def transfer(moved):
        return request + -(-moved // rate)

    parts = {'guard_wait': GUARD_WAIT,
             'version_readl_exe': 3 * request,
             'sram_backup': sum(transfer(n) for n in regions),
             'uploads': sum(request + transfer(2 * n) for n in sizes),
             'capture': seconds,
             'refusal_probe': transfer(128) + request + sum(transfer(2 * n) for n in regions),
             'margin': TERMINAL_MARGIN}
    total = sum(parts.values())
    require(total <= QUEUE_TIMEOUT_LIMIT,
            f'One attempt can take {total} s, more than the queue\'s {QUEUE_TIMEOUT_LIMIT} s limit.')
    return total, parts


def returning_timeout(path):
    """One returning attempt's worst case: the churn guard's wait for the run's two re-enumerations, every
    default-bounded FEL request the runner makes, its default return window, and margin."""
    text = Path(path).read_text()
    bound = re.search(r'def transaction\(\*arguments, timeout=(\d+)\)', text)
    window = re.search(r'"--return-timeout", type=float, default=(\d+)', text)
    require(bound and window, f'Runner {path} has no FEL request bound or return window.')
    calls = [m for m in re.finditer(r'(?<!def )transaction\(([^()]|\([^()]*\))*\)', text)]
    # A request with its own timeout argument moves the optional RAM input,
    # which no returning bundle carries.
    counted = sum('timeout=' not in m.group(0) for m in calls)
    parts = {'guard_wait': GUARD_WAIT, 'fel_requests': counted * int(bound.group(1)),
             'return_window': int(window.group(1)), 'margin': TERMINAL_MARGIN}
    return sum(parts.values()), parts


def admit(row, admitted):
    """The admitted value names this row's build."""
    expected = row.get('admission')
    require(expected is not None, f'No owning package has named the admission values of {row["name"]}; '
                                  'the row is refused until it does.')
    require(isinstance(admitted, dict), 'prepare() returned no admission record.')
    wrong = sorted(k for k, v in expected.items() if admitted.get(k) != v)
    require(not wrong, 'The admitted bundle is not this row\'s build: ' +
            ', '.join(f'{k} is {admitted.get(k)!r}, not {expected[k]!r}' for k in wrong))


def monitor_scan(elf, nm, renewal_cap, returning=False):
    """The defined symbols of a Monitor image allow the forced return to the ROM."""
    result = subprocess.run([nm, '--defined-only', '-P', str(elf)], capture_output=True, text=True)
    require(result.returncode == 0, f'nm failed on {elf}: {result.stderr.strip()}')
    names = {line.split()[0] for line in result.stdout.splitlines() if line.strip()}
    require(names, 'Monitor image has no symbol table.')
    require('monitor_entry' in names, 'Monitor image does not define monitor_entry.')
    forbidden = sorted(n for n in names if n.startswith(FORBIDDEN_PREFIXES))
    require(not forbidden, 'Monitor image defines USB or secondary-core startup symbols: ' + ', '.join(forbidden[:8]))
    require(not renewal_cap or 'monitor_renewal_cap' in names,
            'Memory image does not define monitor_renewal_cap, the capped deadline renewal.')
    # monitor_sync renews the deadline without a cap; a returning image must
    # return within the monitor's 2 s deadline.
    require(not returning or 'monitor_sync' not in names, 'Returning image defines monitor_sync, the uncapped renewal.')


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_survey(build, repository, software=SURVEY_SOFTWARE):
    """The survey build passes every input check that the repository's survey runner makes before any device
    access (tools/run-launch-survey.py, run): the manifest's schema and physical_access, the runner and survey
    source it names, the SHA-256 of every frozen file it lists, and a software record of the instruction and
    return evidence that covers its binary."""
    build = Path(build)
    manifest = json.loads((build / 'manifest.json').read_text())
    require(manifest.get('schema') == 'a733-launch-survey-v1' and manifest.get('physical_access') is False,
            f'The survey build {build} is not a launch-survey-v1 build without physical access.')
    runner = sha256(Path(repository) / SURVEY_RUNNER)
    require(manifest.get('host_sha256') == runner,
            f'The survey build {build} names the runner {manifest.get("host_sha256")}, but '
            f'{Path(repository) / SURVEY_RUNNER} is {runner}; the runner would refuse it.')
    source = sha256(Path(repository) / SURVEY_SOURCE)
    require(manifest.get('source_sha256') == source,
            f'The survey build {build} names the source {manifest.get("source_sha256")}, but '
            f'{Path(repository) / SURVEY_SOURCE} is {source}; the runner would refuse it.')
    files = manifest.get('files')
    require(isinstance(files, dict) and 'launch-survey.bin' in files and
            all(Path(name).name == name and (build / name).is_file() and sha256(build / name) == digest
                for name, digest in files.items()),
            f'A frozen file of the survey build {build} differs from its manifest; the runner would refuse it.')
    record = json.loads(Path(software).read_text())
    require(record.get('schema') == 'a733-launch-survey-software-v1'
            and record.get('binary_sha256') == files['launch-survey.bin']
            and record.get('return_preserved') is True and record.get('physical_access') is False,
            f'{software} is not instruction and return evidence for the binary of the survey build {build}.')


def reader():
    """The fixed reader, loaded without running it; its plan() admits a word list."""
    return load('board_bundles_reader', ROOT / READER)


def copy_host_tools(output, extra=None):
    tools = output / 'host/tools'
    tools.mkdir(parents=True)
    for rel in HOST_TOOLS:
        shutil.copyfile(ROOT / rel, tools / Path(rel).name)
    for name, path in (extra or {}).items():
        shutil.copyfile(path, tools / name)


def prepare_read(row, output):
    """A Read bundle: the fixed reader, the row's word list and the reader's host modules."""
    output.mkdir()
    shutil.copyfile(ROOT / READER, output / 'board-read.py')
    (output / 'WORDS.json').write_text(json.dumps({'schema': 'board-read-words-v1', 'purpose': row['purpose'],
                                                   'words': row['words']}, indent=2) + '\n')
    copy_host_tools(output)
    return validate_read(row, output)


def validate_read(row, output):
    require(sha256(output / 'board-read.py') == sha256(ROOT / READER), 'The bundle\'s reader is not the fixed reader.')
    words = reader().plan(json.loads((output / 'WORDS.json').read_text()))
    return {'schema': 'board-read-bundle-v1', 'row': row['name'], 'words': len(words),
            'conditions': sum(1 for w in words if w[2] is not None),
            'reader_sha256': sha256(output / 'board-read.py'), 'words_sha256': sha256(output / 'WORDS.json')}


def prepare_returning(row, build, output, approval=None):
    """A returning bundle from the image builder's build.json: image, binary, runner closure, equivalence,
    controlled outcomes and, for a row that changes a clock or voltage, the approval record."""
    record = json.loads(Path(build).read_text())
    require((approval is None) == (row.get('approval') is None),
            'An approval record is required exactly for a row that names an approval schema.')
    if approval is not None:
        value = json.loads(Path(approval).read_text())
        require(value.get('schema') == row['approval'] and value.get('row') == row['name']
                and value.get('image_sha256') == record['elf_sha256'] and value.get('port') in ('3-1', '3-3')
                and isinstance(value.get('approval'), dict) and value['approval'].get('text')
                and value['approval'].get('date'),
                'The approval record does not name this row, its image, a port and the user\'s approval.')
    output.mkdir()
    if approval is not None:
        shutil.copyfile(approval, output / 'approval.json')
    shutil.copyfile(record['elf'], output / 'image.elf')
    shutil.copyfile(record['bin'], output / 'probe.bin')
    tools = dict(record['host_tools'])
    require(set(tools) == {'check-a733-kernel.py', 'check-a733-smp.py', 'usb-access.py'},
            'The build names another runner closure.')
    copy_host_tools(output, {'check-a733-kernel.py': tools['check-a733-kernel.py']})
    (output / 'equivalence.json').write_text(json.dumps({'objects': record['objects'], 'overlay': record['overlay'],
                                                         'image_sha256': record['elf_sha256']}, indent=2) + '\n')
    if row['controlled']:
        require(record['controlled'] is not None, 'The build lists no controlled outcome for this row.')
        (output / 'controlled.json').write_text(json.dumps(record['controlled'], indent=2) + '\n')
    else:
        require(record['controlled'] is None, 'The build lists controlled outcomes that this row does not take.')
    require(record['expect'] == row['expect'], 'The build\'s success text is not this row\'s.')
    return validate_returning(row, output)


def validate_returning(row, output):
    """The uploaded bytes are image.elf's loadable segments, and the equivalence and outcome files hold."""
    # A temporary directory, so that a frozen, read-only set can be validated again.
    with tempfile.TemporaryDirectory() as scratch:
        flat = Path(scratch) / 'objcopy.bin'
        result = subprocess.run([OBJCOPY, '-O', 'binary', str(output / 'image.elf'), str(flat)], capture_output=True,
                                text=True)
        require(result.returncode == 0, f'objcopy failed: {result.stderr.strip()}')
        same = flat.read_bytes() == (output / 'probe.bin').read_bytes()
    require(same, 'probe.bin is not the loadable segments of image.elf.')
    equivalence = json.loads((output / 'equivalence.json').read_text())
    require(equivalence['image_sha256'] == sha256(output / 'image.elf'), 'image.elf is not the built image.')
    objects = equivalence['objects']
    require(objects and all(o['origin'] in PASSED_ORIGINS | {'new', SET_BASE} for o in objects.values()),
            'An object has no recorded origin.')
    new = sorted(rel for rel, o in objects.items() if o['origin'] == 'new')
    require(set(new) <= set(row['overlay']),
            'Objects differ from the images that passed outside the overlay: ' + ', '.join(new))
    based = sorted(rel for rel, o in objects.items() if o['origin'] == SET_BASE)
    if based:
        # The base row's bundle of the same run, prepared before this row.
        require(row.get('base'), 'Objects name the set\'s base, but the row has no base row: ' + ', '.join(based))
        base = json.loads((output.parent.parent / row['base'] / 'bundle/equivalence.json').read_text())['objects']
        wrong = [rel for rel in based if base.get(rel, {}).get('sha256') != objects[rel]['sha256']]
        require(not wrong, 'Objects differ from the base bundle\'s object of the same unit: ' + ', '.join(wrong))
    value = {'schema': 'board-returning-bundle-v1', 'row': row['name'], 'new_objects': new, 'base_objects': based,
             'passed_objects': len(objects) - len(new) - len(based), 'image_sha256': equivalence['image_sha256']}
    if row['controlled']:
        runner = load('board_bundles_returning_runner', output / 'host/tools/check-a733-kernel.py')
        outcomes = runner.controlled_outcomes((output / 'controlled.json').read_bytes())
        value['controlled'] = [o['name'] for o in outcomes]
    return value


def lifetime_runs(lifetime, name):
    """This row's record stem and the warm-return records before it, or None without a lifetime order."""
    if not lifetime:
        return None
    order = lifetime['order']
    index = next((i for i, e in enumerate(order) if e.get('row') == name), None)
    if index is None:
        return None
    links = list(lifetime['chain']) + [e['run'] for e in order[:index]]
    return order[index], index + 1, links


def gates(row):
    """The rows of this set that must pass before a row: its base, then its `after` rows."""
    found = [(row['base'], 'its image links objects of that image')] if row.get('base') else []
    return found + [(name, 'it runs after that row in this set') for name in row.get('after', [])
                    if name != row.get('base')]


def external_gates(entry, lifetime):
    """The external entries of the order that must have completed before a row's order entry: (stem, name, run)."""
    externals = {e['stem']: e for e in (lifetime or {}).get('order', []) if 'external' in e}
    return [(stem, externals[stem]['external'], externals[stem]['run']) for stem in (entry or {}).get('after_external', [])]


def stops(entry):
    """The controlled outcomes of earlier rows that stop an order entry, as text."""
    return [f'`{name}` ended in `{outcome}`' for name, outcomes in (entry or {}).get('stop_if', {}).items()
            for outcome in outcomes]


def runbook(row, bundle, value, artifacts, prerequisites, timeout, lifetime=None, survey=None):
    """runbook.md for one prepared row, in the format of docs/board-runbooks/."""
    n = row['position']
    placed = lifetime_runs(lifetime, row['name'])
    entry, step, links = placed if placed else (None, None, None)
    stem = entry['stem'] if entry else f'bb{n}'
    lines = [f'# Bundle {n}: {row["component"]}', '',
             f'Status: **prepared** by `tools/prepare-board-bundles.py`; not run. Bundle {n} of '
             'docs/designs/board-bundles-next.md.' + (f' Step {step} of this set\'s order for port {lifetime["port"]} '
                                                      '(`../runbook.md`).' if entry else ''),
             f'Bundle: `{bundle}`.', '']
    if row['class'] == 'read':
        lines += [f'A Read bundle: `board-read.py` reads {value["words"]} words with 32-bit FEL `readl` commands, '
                  f'{value["conditions"]} of them only after their condition word matches. No board code runs '
                  'beyond the readl stub, which the reader confines to the saved and restored 128 scratch bytes '
                  'at 0x71000; the USB instance does not change.', '']
    else:
        lines += [row['intro'], '']
    required, stopped = gates(row), stops(entry)
    if required or stopped or external_gates(entry, lifetime):
        lines += ['## Run only after', '']
        passed = (lifetime or {}).get('passed', {})
        lines += [(f'- `{name}` passed earlier in this lifetime, from an earlier set, as `$S/{passed[name]}`: '
                   'status 0 with its success text. That run is the first link of the warm-return chain.'
                   if name in passed else
                   f'- `{name}` of this set passed in this lifetime ({reason}): status 0 with its success text. A '
                   'controlled outcome of that row is not a pass.') for name, reason in required]
        lines += [f'- Do not run it when {text}.' for text in stopped]
        lines += [f'- `{name}` (external entry `{stem}`) completed in this lifetime: `$S/{run}/session.json` has '
                  '`complete`, `rom_usable` and `dram_restored` true. Any other outcome of it ends the lifetime with '
                  'a power removal instead of this bundle.' for stem, name, run in external_gates(entry, lifetime)]
        lines += ['', 'The operator applies these gates; the runner does not check them.', '']
    lines += ['## Bundle', '', '| File | SHA-256 |', '| --- | --- |']
    lines += [f'| `{Path(a["path"]).relative_to(bundle)}` | `{a["sha256"]}` |' for a in artifacts]
    lines += ['']
    if row.get('admission_text'):
        lines += ['## Admission', '', row['admission_text'], '']
    if row.get('variant'):
        reused = {'gpu-provider-ready': 'the frozen provider-readiness image `3adfa4ad…`, which returned with '
                                        'status 0 on port 3-1 in two earlier lifetimes',
                  'gpu-power-id-fd3d5839': 'the power and identity image `fd3d5839…`, which launched on port 3-1 '
                                           'and did not return'}
        lines += [f'Equivalence: {value["reused_objects"]} linked objects are those of '
                  + ' and '.join(reused[x] for x in value['reused_from'])
                  + '; ' + str(len(value['new_objects'])) + ' are compiled from the source revision: '
                  + ', '.join(f'`{x}`' for x in value['new_objects'])
                  + '. No object of this image has a board pass of its own. `equivalence.json` lists every object '
                  'with its origin, and `build.json` and `host-check.json` are the build record and the '
                  f'{value["check_cases"]} passing host-check cases of this image.', '']
        if value.get('power_order'):
            o = value['power_order']
            lines += [f'Power order, read from the image\'s `on_steps` ({o["on_steps"]} steps) and `off_steps` '
                      f'({o["off_steps"]} steps), which is the vendor order: ON writes CORE6 PWCR 0 at step '
                      f'{o["on_pwcr_step"] + 1}, reads PLL_GPU0 back at 1008 MHz at step {o["on_pll_1008_step"] + 1}, '
                      f'requests domain 5 (TOP5) ON at step {o["on_domain_step"] + 1} with the GPU clocked, writes '
                      f'CORE6 policy 8 at step {o["on_policy8_step"] + 1} and ends with the CORE6 status poll for 8. '
                      f'OFF writes CORE6 policy 0 at step {o["off_policy0_step"] + 1}, requests domain 5 OFF at step '
                      f'{o["off_domain_step"] + 1} and stops the first GPU clock at step '
                      f'{o["off_first_clock_step"] + 1}.', '']
    elif row['class'] == 'monitor':
        origins = sorted({o['origin'] for o in json.loads((bundle / 'equivalence.json').read_text())['objects'].values()}
                         - {'new'})
        passed = sorted(set(origins) & set(PASSED_RECORDS))
        earlier = (lifetime or {}).get('passed', {})
        lines += [f'Equivalence: {value["passed_objects"]} linked objects are byte-identical to objects of images '
                  'that passed on the board (' + ', '.join(f'`{k}`: {PASSED_RECORDS[k]}' for k in passed) + ').'
                  + (f' {len(value["base_objects"])} equal objects of the image of `{row["base"]}`, the base of '
                     + (f'this set, which passed earlier in this lifetime as `$S/{earlier[row["base"]]}`: '
                        if row['base'] in earlier else 'this set, which has not run when the set is prepared: ')
                     + ', '.join(f'`{x}`' for x in value['base_objects']) + '.' if value.get('base_objects') else '')
                  + ' New: ' + ', '.join(f'`{x}`' for x in value['new_objects'])
                  + '. `equivalence.json` lists every object with its origin.', '']
    lines += ['## Commands', '',
              'Set the variables of docs/board-runbooks/README.md ("Names used in every runbook"), with `S` '
              f'naming this lifetime\'s session directory, and `B={bundle}`.', '']
    expected = row['expected_result'].replace('"', "'")
    if row['class'] == 'read':
        lines += ['1. Read the words once:', '', '   ```sh',
                  f'   $PY -B $V/common/bundle-run.py $PORT $S/{stem}-read {timeout} \\',
                  f'     "Board $PORT: {row["component"]}" \\', f'     "{expected}" \\',
                  f'     -- $PY -B $B/board-read.py $B/WORDS.json $S/{stem}-read', '   ```', '',
                  '   Success: `{"port": "PORT", "complete": true, "words": ' + str(value['words'])
                  + ', "skipped": 0}`. A nonzero `skipped` count is recorded with each skipped word and the '
                  'condition that stopped it; the bundle is then inconclusive and is not repeated.', '',
                  f'   Prerequisite: `{prerequisites[0]}`, written by `bundle-run.py` for the current instance. '
                  f'Queue timeout: {timeout} s.', '']
    else:
        controlled = ' --controlled $B/controlled.json' if row['controlled'] else ''
        lines += ['1. Turn the instruction cache off and survey the current instance:', '', '   ```sh',
                  f'   $PY -B $V/common/bundle-run.py $PORT $S/{stem}-cache-off 120 \\',
                  f'     "Board $PORT: returning instruction-cache disable before bundle {n}" \\',
                  '     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." '
                  '\\', f'     -- $PY -B $V/common/cache-off.py $S/{stem}-cache-off',
                  f'   $PY -B $V/common/bundle-run.py $PORT $S/{stem}-survey {SURVEY_TIMEOUT} \\',
                  f'     "Board $PORT: returning SRAM survey before bundle {n}" \\',
                  '     "Nonce-bound launcher controls, restored SRAM and a responsive same ROM." \\',
                  f'     --prerequisite launch-survey-software={survey["software"]} \\',
                  f'     --artifact {survey["build"]}/manifest.json --artifact {survey["build"]}/launch-survey.bin \\',
                  f'     -- $PY -B {survey["runner"]} run --fel $FEL --build {survey["build"]} \\',
                  f'        --software {survey["software"]} --database {{database}} --evidence $S/{stem}-survey \\',
                  '        --claim {claim} --owner {owner}', '   ```', '',
                  '   Each prints `"result": "passed"`. The survey build\'s manifest names the survey runner '
                  f'(`{survey["runner_sha256"]}`), which refuses a build that names another.', '']
        if links is None:
            lines += ['2. Run the image once. `D` is the DDR record of this lifetime\'s cold start; give one '
                      '`--warm-return` for each returning run since then, in order:', '', '   ```sh']
            tail = [f'        --ddr-evidence $D --platform-survey $S/{stem}-survey/session.json [--warm-return ...]']
        else:
            lines += [f'2. Run the image once, with the DDR record `{lifetime["ddr"]}` of this lifetime\'s cold start '
                      + (f'and one `--warm-return` for each of the {len(links)} returning '
                         + ('run' if len(links) == 1 else 'runs') + ' since then, in order:'
                         if links else 'and no `--warm-return`: no returning run precedes it in this lifetime:'),
                      '', '   ```sh']
            tail = [f'        --ddr-evidence $S/{lifetime["ddr"]}/session.json '
                    f'--platform-survey $S/{stem}-survey/session.json' + (' \\' if links else '')]
            tail += [f'        --warm-return $S/{x}/session.json' + (' \\' if i + 1 < len(links) else '')
                     for i, x in enumerate(links)]
        lines += [f'   $PY -B $V/common/bundle-run.py $PORT $S/{stem}-run {timeout} \\',
                  f'     "Board $PORT: {row["component"]}" \\', f'     "{expected}" \\',
                  '     -- $PY -B $B/host/tools/check-a733-kernel.py --fel $FEL --image $B/probe.bin --mode kernel '
                  '\\', f'        --expect "{row["expect"]}"{controlled} --evidence $S/{stem}-run \\', *tail,
                  '   ```', '',
                  f'   Prerequisite: `{prerequisites[0]}`. Queue timeout: {timeout} s, the runner\'s worst case '
                  'including the churn guard\'s 615 s wait.', '']
        passed = (f'`{row["expect"]}`, then `PASS: kernel check, FEL recovery, automatic USB access.`'
                  if row.get('passes', True) else None)
        if row['controlled']:
            outcomes = json.loads((bundle / 'controlled.json').read_text())['outcomes']
            names = ' or '.join(f'`CONTROLLED: {o["name"]}, status {o["status"]}, FEL recovery, automatic USB '
                                f'access.`' for o in outcomes)
            lines += ['   Success: ' + (f'{passed}; or a controlled outcome: ' if passed else '') + names
                      + f', with `controlled` in `$S/{stem}-run/session.json` holding the parsed EL1 values. A '
                      'controlled session is a valid `--warm-return` link for later bundles.', '',
                      '## Controlled outcomes', '',
                      'Each outcome in `controlled.json` is matched on the full result record and log; any other '
                      'nonzero status quarantines the instance:', '']
            lines += [f'- {outcome_text(o)}' for o in outcomes]
            lines += ['']
        else:
            lines += [f'   Success: {passed}', '']
    if row.get('returning'):
        lines += outcome_paths(row, bundle)
    lines += ['## Stop conditions', '', f'- {row["stop_condition"]}', '']
    if row['class'] == 'monitor':
        lines += ['## Runner', '',
                  'The bundle\'s `host/tools/check-a733-kernel.py` is `tools/check-a733-kernel.py` of the source '
                  'revision, which carries the warm-return admission, the controlled outcomes and their progress '
                  'record; it needs no patch.', '']
    return '\n'.join(lines)


def set_runbook(run_dir, lifetime, rows, table):
    """runbook.md of the run directory: the order of this lifetime, its gates and its warm-return chain."""
    names = {row['name']: row for row in table}
    lines = ['# Prepared set ' + run_dir.name + f' on port {lifetime["port"]}', '',
             'Status: **prepared** by `tools/prepare-board-bundles.py`; nothing here has run. Run the steps below '
             f'in order on port {lifetime["port"]}, in the lifetime that `$S/{lifetime["ddr"]}/` started. Each '
             'step runs once. A step that exits nonzero stops the steps after it. A step whose gate is not met is '
             'not run: a gate row passed only with status 0 and its success text, and a controlled outcome '
             '(which the runner reports as `CONTROLLED` and exits 0) is not a pass. The operator applies the '
             'gates; the runner does not check them.', '']
    if lifetime.get('start'):
        lines += ['## Start of the lifetime', ''] + list(lifetime['start']) + ['']
    lines += ['## Order', '', '| Step | Bundle | Records | Run only after |', '| --- | --- | --- | --- |']
    for i, entry in enumerate(lifetime['order'], 1):
        if 'row' in entry:
            row = names[entry['row']]
            status = rows.get(entry['row'], {}).get('status', 'not prepared')
            what = f'Bundle {row["position"]}, `{entry["row"]}/runbook.md` ({status})'
            after = [g for g, _ in gates(row)]
        else:
            what = f'`{entry["external"]}`, `{entry["runbook"]}`'
            after = entry.get('after', [])
        passed = lifetime.get('passed', {})
        gate = [f'`{a}` passed' + (f' (`{passed[a]}`, already in the chain)' if a in passed else '') for a in after]
        gate += [f'`{stem}` completed with DRAM restored' for stem, _, _ in external_gates(entry, lifetime)]
        gate += [f'not if {text}' for text in stops(entry)]
        lines.append(f'| {i} | {what} | `$S/{entry["stem"]}-*`; link `{entry["run"]}` | '
                     + ('; '.join(gate) or 'none') + ' |')
    lines += ['', '## Warm-return chain', '',
              f'Before step 1 the lifetime holds `{lifetime["ddr"]}` and '
              + ('these returning runs, in order: ' + ', '.join(f'`{x}`' for x in lifetime['chain'])
                 if lifetime['chain'] else 'no returning run') + '. Each step\'s runbook gives its complete '
              '`--warm-return` list: this chain, then the record of every earlier step of the order. A controlled '
              'outcome is a valid link; a step that did not return ends the lifetime.', '']
    for item in lifetime.get('state', []):
        lines += [f'- {item}']
    if lifetime.get('state'):
        lines += ['']
    return '\n'.join(lines)


def outcome_paths(row, bundle):
    """The outcome of every path of a returning run: a warm return or a recorded quarantine."""
    lines = ['## Outcome of each path', '', '| Path | Record | Ends in |', '| --- | --- | --- |']
    if row.get('passes', True):
        lines.append(f'| Pass | Status 0 and `{row["expect"]}` | Warm return: `PASS: kernel check, FEL recovery, '
                     'automatic USB access.`; the session is a `--warm-return` link |')
    if row['controlled']:
        for o in json.loads((bundle / 'controlled.json').read_text())['outcomes']:
            lines.append(f'| Controlled `{o["name"]}` | Status {o["status"]}, matched as the section "Controlled outcomes" '
                         'above lists it | Warm return: '
                         f'`CONTROLLED: {o["name"]}, ...`; a `--warm-return` link, not a pass of any gate |')
    for text in row.get('failures', []):
        lines.append(f'| Image failure | Status 1 with {text} | Quarantine |')
    lines += ['| Any other image failure | Status 1 with a `FAIL` line, or one `FAIL exception` line (a fault) | '
              'Quarantine |',
              '| Hang | The monitor\'s secure timer returns 2 s after entry: status 2, interrupt 29 | Quarantine |',
              '| Monitor cleanup failure | Status 6 (`replaced_status` holds the image\'s status) | Quarantine |',
              '| No return | No new FEL instance within the runner\'s return window: a stalled bus access or a '
              'monitor that never ran | Quarantine of the old instance; only whole-board power removal recovers it |',
              '| Transport failure | A FEL transfer, readback or identity check fails, before or after launch | '
              'Quarantine |',
              '| Runner refusal before launch | The runner refuses its inputs (the DDR record, the survey, a '
              'warm-return link, a file hash) before any device access | No device access and no recovery file; '
              '`bundle-run.py` prints `"result": "failed", "claim": "held"`. Repair the input and resume this '
              'step; the instance is not quarantined |',
              '| Host timeout | `bundle-run.py` reaches the queue timeout and kills the runner | No recovery file, '
              'and the port lock is released with the killed runner; the claim stays held. The board state is '
              'unknown: treat it as a quarantine and run nothing more on this board until power removal |', '',
              'Quarantine: the runner records the failure in `session.json` and writes '
              '`~/.cache/sunstoneos/recovery-required-<port>-<identity>.json` for the instance, which refuses '
              'further FEL requests to it. When the image returned to the ROM and its result was read, the '
              'runner then exits at once with that result; otherwise it keeps the port lock until recovery and '
              '`bundle-run.py` ends it at the queue timeout. The queue claim stays held, and `bundle-run.py` '
              'prints `"result": "failed", "claim": "held"`. No later step of this lifetime runs, and the board '
              'waits for power removal.', '']
    return lines


def outcome_text(o):
    """One controlled outcome as the runbook lists it."""
    parts = [f'`{o["name"]}`: status {o["status"]}']
    if o['interrupt'] is not None:
        parts.append(f'interrupt {o["interrupt"]}')
    parts.append(f'{o["fail_lines"]} FAIL exception line' + ('' if o['fail_lines'] == 1 else 's'))
    f = o['fault']
    if f:
        parts.append(f'ESR & `0x{f["esr_mask"]:08x}` = `0x{f["esr_value"]:08x}`, FAR `0x{f["far"]:x}`, ELR in '
                     f'`[0x{f["elr_low"]:x}, 0x{f["elr_high"]:x})`, task `{f["task"]}`')
    p = o.get('progress')
    if p:
        parts.append(f'progress record at result offset {p["offset"]} with magic `0x{p["magic"]:x}`, stage '
                     f'{p["stage"]}, count below {p["count_below"]} and a last stamp more than {p["age_above"]} '
                     'counter ticks before the monitor\'s end_counter')
    return ', '.join(parts) + ', log containing ' + ', '.join(f'`{t}`' for t in o['log_contains']) + '.'


def entries(bundle):
    """Every entry below bundle, parents first; only regular files and directories."""
    require(stat.S_ISDIR(os.lstat(bundle).st_mode), f'prepare() left no bundle directory at {bundle}.')
    found = [bundle]
    for directory, dirs, files in os.walk(bundle):
        dirs.sort()
        for name in sorted(dirs) + sorted(files):
            path = Path(directory) / name
            mode = os.lstat(path).st_mode
            require(stat.S_ISREG(mode) or stat.S_ISDIR(mode), f'Bundle entry is not a file or directory: {path}')
            found.append(path)
    return found


def freeze(bundle):
    for path in reversed(entries(bundle)):
        os.chmod(path, stat.S_IMODE(os.lstat(path).st_mode) & ~0o222)


def rchar():
    with open('/proc/self/io', 'rb', buffering=0) as stream:
        text = stream.read(4096)
    return int(re.search(rb'^rchar: (\d+)$', text, re.M).group(1)), len(text)


def artifact_pass(bundle):
    """Hash each regular file once; return the artifacts, each file's size, bytes read and writable entries."""
    found = entries(bundle)
    writable = sum(bool(os.lstat(p).st_mode & 0o222) for p in found)
    files = sorted(p for p in found if not p.is_dir())
    buffer = bytearray(BLOCK)
    view = memoryview(buffer)
    artifacts = []
    sizes = [os.lstat(p).st_size for p in files]
    size = sum(sizes)
    before, sample = rchar()
    for path in files:
        digest = hashlib.sha256()
        with open(path, 'rb', buffering=0) as stream:
            while count := stream.readinto(buffer):
                digest.update(view[:count])
        artifacts.append({'path': str(path), 'sha256': digest.hexdigest()})
    after, _ = rchar()
    return artifacts, sizes, {'bundle_bytes': size, 'read_bytes': after - before - sample,
                              'writable_entries': writable}


def changed_paths(status):
    """Paths named by `git status --porcelain=v1` output."""
    paths = set()
    for line in status.splitlines():
        for part in line[3:].split(' -> '):
            paths.add(part.strip('"'))
    return paths


def snapshot(row, row_dir, artifacts_file, repository):
    command = [sys.executable, str(ROOT / 'tools/evidence-snapshot.py'), '--repository', str(repository),
               '--output', str(row_dir / 'snapshot'), '--artifact', str(artifacts_file),
               '--note', f'Sources of the {row["name"]} board-test bundle; the artifact lists the frozen bundle files.']
    for source in row['sources']:
        command += ['--source', source]
    result = subprocess.run(command, capture_output=True, text=True)
    require(result.returncode == 0, 'Snapshot failed: ' + (result.stderr.strip() or result.stdout.strip()))
    manifest = json.loads((row_dir / 'snapshot/manifest.json').read_text())
    dirty = sorted(changed_paths(manifest['worktree_status_before_copy']) & set(row['sources']))
    require(not dirty, 'Uncommitted row sources: ' + ', '.join(dirty))
    return manifest


def paths(value):
    """Every string argument of the existing prepare() functions is a file path."""
    if isinstance(value, str):
        return Path(value)
    return [paths(v) for v in value] if isinstance(value, list) else value


def prepare_row(row, arguments, row_dir, request, queue, prepared=()):
    """Prepare, admit, scan, freeze, hash, snapshot and submit one row."""
    record = {'class': row['class']}
    stage = 'inputs'
    try:
        require(isinstance(arguments, dict) and 'output' not in arguments,
                'Row inputs must be an object of prepare() keyword arguments without output.')
        require(not row.get('base') or row['base'] in prepared,
                f'The base row {row.get("base")} is not prepared earlier in this run.')
        root = Path(request['repository'])
        module_path = root / row['module']
        require(module_path.is_file(), f'Preparation module {row["module"]} does not exist.')
        prerequisites, seconds, bounds = terms(row, root)
        row_dir.mkdir()
        bundle = row_dir / 'bundle'
        stage = 'prepare'
        module = load('board_bundle_' + row['name'].replace('-', '_'), module_path)
        kwargs = {k: paths(v) for k, v in arguments.items()}
        if row.get('function'):
            admitted = getattr(module, row['function'])(row, **kwargs, output=bundle)
        else:
            admitted = module.prepare(**kwargs, output=bundle)
        record['admission_schema'] = admitted.get('schema') if isinstance(admitted, dict) else None
        stage = 'admission'
        admit(row, admitted)
        if row['class'] == 'monitor':
            stage = 'monitor-scan'
            monitor_scan(bundle / 'image.elf', request['nm'], row.get('renewal_cap', False), row.get('returning', False))
        stage = 'freeze'
        freeze(bundle)
        stage = 'artifacts'
        artifacts, sizes, metrics = artifact_pass(bundle)
        record['metrics'] = metrics
        stage = 'timeout'
        if row['class'] == 'read':
            timeout = READ_TIMEOUT
        elif row.get('returning'):
            # The bundle's own copy of the runner is what the queue runs.
            timeout, record['timeout_terms'] = returning_timeout(bundle / 'host/tools/check-a733-kernel.py')
        elif row['class'] == 'monitor':
            timeout = MONITOR_TIMEOUT
        else:
            timeout, record['timeout_terms'] = terminal_timeout(seconds, sizes, bounds)
        artifacts_file = row_dir / 'artifacts.json'
        save(artifacts_file, artifacts)
        stage = 'snapshot'
        manifest = snapshot(row, row_dir, artifacts_file, root)
        stage = 'submission'
        names = {'runner': row['runner'], 'bundle': bundle}
        value = {'component': row['component'], 'kind': 'peripheral',
                 'sources': [{'repository': manifest['repository'], 'revision': manifest['revision']}],
                 'artifacts': artifacts, 'prerequisites': prerequisites,
                 'expected_result': row['expected_result'],
                 'procedure': [step.format(**names) for step in row.get('procedure', TERMINAL_PROCEDURE)],
                 'timeout_seconds': timeout, 'stop_condition': row.get('stop_condition', TERMINAL_STOP),
                 'evidence_directory': str(row_dir / 'evidence')}
        queue.submission(value)
        save(row_dir / 'submission.json', value)
        if 'position' in row:
            stage = 'runbook'
            with (row_dir / 'runbook.md').open('x') as stream:
                stream.write(runbook(row, bundle, admitted, artifacts, prerequisites, timeout,
                                     request.get('lifetime'), request.get('survey')) + '\n')
        record.update(status='prepared', submission=str(row_dir / 'submission.json'), revision=manifest['revision'],
                      timeout_seconds=timeout, prerequisites=prerequisites)
    except Exception as error:
        record.update(status='refused', stage=stage, message=f'{type(error).__name__}: {error}')
    return record


def step(run_dir):
    """The sandboxed preparation step: every row of request.json, in table order."""
    # bwrap always sets PWD after it changes directory; preparation and its
    # children see only the four variables the sandbox sets.
    os.environ.pop('PWD', None)
    request = json.loads((run_dir / 'request.json').read_text())
    queue = load('board_bundle_queue', ROOT / 'tools/board-test-queue.py')
    rows = {}
    for row in request['rows']:
        if row['class'] == 'excluded':
            rows[row['name']] = {'class': 'excluded', 'status': 'excluded', 'message': row['reason']}
        elif row['name'] not in request['inputs']:
            rows[row['name']] = {'class': row['class'], 'status': 'no-inputs'}
            if row.get('needs'):
                rows[row['name']]['needs'] = row['needs']
        else:
            prepared = {name for name, r in rows.items() if r['status'] == 'prepared'}
            rows[row['name']] = prepare_row(row, request['inputs'][row['name']], run_dir / row['name'], request,
                                            queue, prepared)
    value = {'schema': 'board-bundles-run-v1', 'physical_access': False, 'rows': rows}
    lifetime = request.get('lifetime')
    if lifetime:
        try:
            stop_outcomes(lifetime, run_dir, rows)
            with (run_dir / 'runbook.md').open('x') as stream:
                stream.write(set_runbook(run_dir, lifetime, rows, request['rows']) + '\n')
        except ValueError as error:
            value['lifetime_refused'] = str(error)
    save(run_dir / 'run.json', value)
    for name, record in rows.items():
        print(f'{name}: {record["status"]}' + (f' ({record["message"]})' if 'message' in record else ''))
    if 'lifetime_refused' in value:
        print('lifetime: refused (' + value['lifetime_refused'] + ')')
    return 1 if any(r['status'] == 'refused' for r in rows.values()) or 'lifetime_refused' in value else 0


def stop_outcomes(lifetime, run_dir, rows):
    """Every stop_if outcome is a controlled outcome of the named row's prepared bundle."""
    for entry in lifetime['order']:
        for name, outcomes in entry.get('stop_if', {}).items():
            require(rows.get(name, {}).get('status') == 'prepared', f'stop_if names {name}, which is not prepared.')
            path = run_dir / name / 'bundle/controlled.json'
            named = {o['name'] for o in json.loads(path.read_text())['outcomes']} if path.is_file() else set()
            unknown = sorted(set(outcomes) - named)
            require(not unknown, f'stop_if names outcomes that {name} does not list as controlled: '
                    + ', '.join(unknown))


def check_externals(run_dir):
    """Each external entry's runbook, read from the run directory, writes record directories under its own stem,
    names its run directory, and refers to no other record of the session directory than the DDR record, the
    chain and the runs of earlier entries."""
    lifetime = json.loads((run_dir / 'request.json').read_text()).get('lifetime')
    require(lifetime, f'{run_dir} was prepared without a lifetime.')
    earlier, result = [lifetime['ddr'], *lifetime['chain']], {}
    for entry in lifetime['order']:
        if 'external' in entry:
            text = (run_dir / entry['runbook']).read_text()
            named = sorted(set(re.findall(r'\$S/([A-Za-z0-9._-]+)', text)))
            own = [x for x in named if x not in earlier]
            wrong = [x for x in own if not x.startswith(entry['stem'] + '-')]
            require(not wrong, f'{entry["runbook"]} names record directories outside the stem {entry["stem"]}: '
                    + ', '.join(wrong))
            require(entry['run'] in own, f'{entry["runbook"]} never names its run directory {entry["run"]}.')
            result[entry['external']] = own
        earlier.append(entry['run'])
    return result


def check_lifetime(lifetime, names, inputs):
    """A lifetime file names its port, DDR record, warm-return chain and an order of distinct record stems in
    which every gate names a row placed earlier."""
    require(isinstance(lifetime, dict) and {'port', 'ddr', 'chain', 'order'} <= set(lifetime)
            and set(lifetime) <= {'port', 'ddr', 'chain', 'order', 'state', 'start', 'passed'},
            'The lifetime names port, ddr, chain and order, and optionally state, start and passed.')
    passed = lifetime.get('passed', {})
    require(isinstance(passed, dict) and all(name in names and run in lifetime['chain'] for name, run in passed.items()),
            'passed maps a row of the table to the chain record of its earlier pass in this lifetime.')
    require(all(isinstance(x, str) for x in lifetime.get('start', [])), 'start is a list of runbook lines.')
    require(all(isinstance(x, str) and x and '/' not in x for x in [lifetime['ddr'], *lifetime['chain']]),
            'The DDR record and the chain are record directory names of the session directory.')
    stems, runs, placed, placed_externals = set(), set(lifetime['chain']), list(lifetime.get('passed', {})), set()
    for entry in lifetime['order']:
        require(isinstance(entry, dict) and ('row' in entry) != ('external' in entry), 'Each order entry names a row '
                'or an external bundle.')
        keys = {'row', 'stem', 'run', 'stop_if', 'after_external'} if 'row' in entry else {
            'external', 'stem', 'run', 'runbook', 'after', 'stop_if'}
        require(set(entry) <= keys, f'Order entry {entry} has keys outside {sorted(keys)}; a row\'s gates are its '
                'base and after fields.')
        require('row' not in entry or (entry['row'] in inputs and names[entry['row']].get('returning')),
                f'Order entry {entry} names no returning row of these inputs.')
        require('external' not in entry or entry.get('runbook'), f'External entry {entry} names no runbook.')
        require(entry.get('stem') and entry['stem'] not in stems and entry.get('run') and entry['run'] not in runs,
                f'Order entry {entry} reuses a record stem or run directory.')
        require('row' not in entry or entry['run'] == entry['stem'] + '-run',
                f'A row\'s run directory is its stem followed by -run: {entry}')
        stop_if = entry.get('stop_if', {})
        require(isinstance(stop_if, dict) and all(isinstance(v, list) and v and all(isinstance(x, str) for x in v)
                                                  for v in stop_if.values()),
                f'Order entry {entry}: stop_if maps a row to a list of its controlled outcome names.')
        required = [g for g, _ in gates(names[entry['row']])] if 'row' in entry else list(entry.get('after', []))
        late = [g for g in required + list(stop_if) if g not in placed]
        require(not late, f'Order entry {entry} names gate rows that are not placed before it: ' + ', '.join(late))
        late = [g for g in entry.get('after_external', []) if g not in placed_externals]
        require(not late, f'Order entry {entry} names external entries that are not placed before it: '
                + ', '.join(late))
        if 'external' in entry:
            placed_externals.add(entry['stem'])
        stems.add(entry['stem'])
        runs.add(entry['run'])
        if 'row' in entry:
            placed.append(entry['row'])


def run(inputs, rows=ROWS, evidence=EVIDENCE, repository=ROOT, nm=NM, lifetime=None, run_dir=None,
        survey_build=SURVEY_BUILD):
    """Create the run directory, the next board-bundles-<date>-<n> under evidence or the new directory run_dir,
    and wait once on the sandboxed step; return (exit status, run directory). Inputs with a returning row
    first need a survey build that the repository's survey runner accepts."""
    names = {row['name']: row for row in rows}
    unknown = sorted(set(inputs) - set(names))
    require(not unknown, 'Inputs name unknown rows: ' + ', '.join(unknown))
    if lifetime is not None:
        check_lifetime(lifetime, names, inputs)
    excluded = sorted(n for n in inputs if names[n]['class'] == 'excluded')
    require(not excluded, 'Inputs name excluded rows: ' + ', '.join(excluded))
    require(Path(BWRAP).is_file(), f'{BWRAP} is missing; preparation needs the sandbox.')
    survey = None
    if any(names[n].get('returning') for n in inputs):
        check_survey(survey_build, repository)
        survey = {'build': str(Path(survey_build).resolve()), 'software': str(SURVEY_SOFTWARE.resolve()),
                  'runner': str((Path(repository) / SURVEY_RUNNER).resolve()),
                  'runner_sha256': sha256(Path(repository) / SURVEY_RUNNER)}
    if run_dir is not None:
        run_dir = Path(run_dir).resolve()
        require(Path(evidence).resolve() in run_dir.parents, f'{run_dir} is not under the evidence directory {evidence}.')
        run_dir.parent.mkdir(parents=True, exist_ok=True)
        run_dir.mkdir()
    else:
        evidence = Path(evidence)
        evidence.mkdir(parents=True, exist_ok=True)
        date = datetime.now(timezone.utc).strftime('%Y%m%d')
        n = 1
        while True:
            run_dir = evidence / f'board-bundles-{date}-{n}'
            try:
                run_dir.mkdir()
                break
            except FileExistsError:
                n += 1
    save(run_dir / 'request.json', {'rows': rows, 'inputs': inputs, 'repository': str(repository), 'nm': str(nm),
                                    'lifetime': lifetime, 'survey': survey})
    command = sandbox(run_dir) + [sys.executable, str(Path(__file__).resolve()), '--step', str(run_dir)]
    status = subprocess.run(command).returncode
    require((run_dir / 'run.json').is_file(), f'The sandboxed step stopped before recording a result ({status}).')
    return status, run_dir


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--inputs', type=Path, help='JSON object: row name to prepare() keyword arguments')
    parser.add_argument('--lifetime', type=Path, help='JSON object: the board lifetime and order the runbooks name')
    parser.add_argument('--evidence', type=Path, default=EVIDENCE, help='Directory that receives the run directory')
    parser.add_argument('--run-dir', type=Path, help='A new run directory under the evidence directory, in place of '
                        'the next board-bundles-<date>-<n>')
    parser.add_argument('--nm', default=NM)
    parser.add_argument('--check-externals', type=Path, metavar='RUN_DIR',
                        help='Check the external runbooks of a prepared run against its lifetime order')
    parser.add_argument('--step', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.step:
        return step(args.step)
    if args.check_externals:
        print(json.dumps(check_externals(args.check_externals), indent=1))
        return 0
    if args.inputs is None:
        parser.error('--inputs is required')
    lifetime = json.loads(args.lifetime.read_text()) if args.lifetime else None
    status, run_dir = run(json.loads(args.inputs.read_text()), evidence=args.evidence, nm=args.nm, lifetime=lifetime,
                          run_dir=args.run_dir)
    print(run_dir)
    return status


if __name__ == '__main__':
    sys.exit(main())
