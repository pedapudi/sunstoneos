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

  prepare-board-bundles.py --inputs inputs.json
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
# Objects of an image that passed on the board, by the record that shows it:
# the kernel check of ranks 4 and 5, rank-9-usb-v4 on port 3-3, and bundle 2's
# image with the monitor's counter option, which passed on port 3-3
# (board-session-20261023/port-3-3/bb2-run).
PASSED_ORIGINS = {'rank-4-kernel-check', 'rank-9-usb-v4', 'kernel-check-counter-option'}
PASSED_RECORDS = {'rank-4-kernel-check': 'the kernel check of ranks 4 and 5, `574245e2…`',
                  'rank-9-usb-v4': '`board-bundles-20261023/rank-9-usb-v4/`',
                  'kernel-check-counter-option': 'bundle 2, `a8e6ecac…`, passed as `bb2-run` on port 3-3'}

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
RETURNING_SOURCES = ['tools/prepare-board-bundles.py', 'checks/board-bundles-images.py', 'tools/check-a733-kernel.py',
                     *HOST_TOOLS, 'platform/a733/monitor.S', 'platform/a733/monitor.c', 'platform/a733/platform.c',
                     'platform/a733/link.ld', 'arch/aarch64/vectors.S', 'include/a733_fault.h', 'checks/kernel.c',
                     'checks/board-bundles-fault.c']
RETURNING_STOP = ('Stop at any nonzero exit. The runner keeps the A733 port lock and quarantines the instance after '
                  'any outcome that its bundle does not list as controlled; a hang ends at the monitor\'s 2 s '
                  'deadline, and a stalled bus access needs a power removal. Do not retry.')
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
NPU_CCU = (0x000, 0x0a0, 0x0a8, 0x0ac, 0x2a0, 0x2a4, 0x2a8, 0x2ac, 0x2b0, 0x500, 0x5c0, 0x5e0, 0xa80, 0xa88, 0xa8c,
           0xb00, 0xb04, 0xc00, 0xc0c, 0x1908, 0x1920, 0x1988, 0x19a0, 0x1b10, 0x1b14, 0x1b1c)


def returning_row(name, position, component, overlay, expect, controlled, expected, intro, admission=True, needs=None,
                  passes=True, sources=()):
    """passes: a status-0 run with the expect text is one of the bundle's outcomes, beside any controlled one."""
    return {'name': name, 'class': 'monitor', 'returning': True, 'position': position, 'component': component,
            'module': 'tools/prepare-board-bundles.py', 'function': 'prepare_returning',
            'runner': 'tools/check-a733-kernel.py', 'prerequisite': SUPERVISED_PREREQUISITE,
            'admission': {'schema': 'board-returning-bundle-v1', 'row': name} if admission else None,
            'overlay': overlay, 'expect': expect, 'controlled': controlled, 'intro': intro, 'passes': passes,
            'sources': RETURNING_SOURCES + list(sources), 'expected_result': expected,
            'stop_condition': RETURNING_STOP, 'needs': needs,
            'procedure': ['Run the cache-off helper and a survey of the current instance.',
                          'Claim this test, then run {bundle}/host/tools/check-a733-kernel.py once on '
                          '{bundle}/probe.bin with the DDR record and warm-return chain of this lifetime.']}


POWER_OVERLAY = ['apps/board-bundles-power/power.c.obj', 'apps/board-bundles-power/trace.c.obj',
                 'arch/aarch64/device_memory.c.obj', 'drivers/power/a733_power_domains.c.obj',
                 'drivers/power/a733_power_domains_native.c.obj']
POWER_SOURCES = ['apps/board-bundles-power/power.c', 'apps/board-bundles-power/power.h',
                 'apps/board-bundles-power/trace.c', 'apps/board-bundles-power/trace.h',
                 'drivers/power/a733_power_domains.c', 'drivers/power/a733_power_domains_native.c',
                 'drivers/power/a733_pmic.c', 'arch/aarch64/device_memory.c', 'checks/board-bundles-apps.py',
                 'checks/board-bundles-apps-model.c']

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
    returning_row('kernel-check-counter-option', 2, 'Kernel check built with the returning monitor\'s counter option',
                  ['platform/a733/monitor.c.obj'],
                  'PASS kernel: timer preemption, queue ordering, blocking wakeup, FP context; rounds=100', False,
                  'The kernel check\'s 100 rounds pass and the monitor returns to a responsive ROM.',
                  'The rank 4 kernel check with SUNSTONE_MONITOR_COUNTERS: the monitor writes MDCR_EL3, HSTR_EL2 and '
                  'MDCR_EL2.HPMN before EL1 entry. It is the equivalence base of the counter, walk and WFI image.'),
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
                  'PASS board bundle 4: event counters, walk A/B and WFI', True,
                  'Monitor status 0 with the three stage records and the PASS line, or the controlled outcome '
                  'wfi-stopped-waking: status 2, interrupt 29, after the WFI stage marker with fewer than 1,000 '
                  'wakes and the last wake more than one tick before the forced return.',
                  'Bundle 2\'s image with checks/kernel.c replaced by apps/board-bundles-cpu/main.c, the '
                  'single-core platform/a733/cpu_pmu.c and arch/aarch64/device_memory.c. One task runs three '
                  'stages, each with a log marker and a progress record at 0x42000080 (magic, stage, wakes, last '
                  'wake CNTPCT). Counters: 64 empty regions, a 1 ms CNTPCT loop, a refill read of a cleaned 64 KiB '
                  'buffer, PMCR_EL0 and PMCEID0/1_EL0, with IRQs masked around each region. Walk A/B: refused when '
                  'CCU +0x588 reads MBUS_DFS_EN set (bundle 3 read it clear on port 3-3 on 2026-10-24); otherwise '
                  'a discarded warm-up block and twelve blocks in ABBA order of 1,024 TLBI-and-load iterations, A '
                  'with TCR_EL1 0x200803019 (uncached walks) and B with 0x200803519, each after a calibration region '
                  'whose DTLB_WALK count the block must exceed by exactly 1,024. WFI: 1,000 ticks of '
                  'a733_cpu_idle() with IRQs unmasked, reading the counters after each wake outside any masked '
                  'region. Run it after bundle 2 and bundle 3 of the same lifetime; report each board separately.',
                  sources=['apps/board-bundles-cpu/main.c', 'platform/a733/cpu_pmu.c', 'arch/aarch64/device_memory.c',
                           'checks/board-bundles-apps.py', 'platform/a733/link.ld']),
    read_row('power-domain-words', 5, 'Policy and status words of power domains 1, 4 and 5',
             'Adoption state of the PCK600 domains, read only while the PPU clock is open',
             [word(PPU_CLOCK[0], 'r_ccu', note='PPU clock gate 0')] +
             [word(0x07060000 + 0x1000 * d + o, condition=PPU_CLOCK, note=f'domain {d} {name}')
              for d in (1, 4, 5) for o, name in ((0, 'policy'), (8, 'status'))],
             'Every word is read while R_CCU 0x1ac bit 0 reads 1, and the 128 scratch bytes are restored and equal.'),
    returning_row('pmic-power-domain-reads', 6, 'PMIC service task and power-domain owner, read-only',
                  POWER_OVERLAY + ['apps/board-bundles-power/bundle6.c.obj'],
                  'COMPLETE board bundle 6: PMIC service and domain adoption reads', False,
                  'Monitor status 0 with the S_TWI0 divisor and bit time, the supply verification bits, 32 DCDC2 '
                  'and DCDC4 snapshots, one STEP line per service step that completed a bus phase, the step '
                  'distribution against the 110% wire-time bound and the adoption reads of domains 1, 4 and 5.',
                  'rank-9-usb-v4\'s platform and PMIC provider objects with apps/board-bundles-power/ (bundle6.c, '
                  'power.c, trace.c), the power-domain owner, its native adapter and device_memory.c; no USB unit. '
                  'The service task verifies DCDC2 and DCDC4 once; a client then takes 32 snapshots, DCDC2 and '
                  'DCDC4 in turn, and makes the adoption reads: R_CCU 0x1ac, CCU 0x5c0 and 0x5e0, and the policy '
                  'and status of domains 1, 4 and 5 only while 0x1ac bit 0 reads 1. Each STEP line gives the step\'s '
                  'span, the scheduler ticks inside it, the phases it completed, the TWI interrupt flag (CNTR bit '
                  '3) at the first read of each byte phase with the interrupt enable (bit 7) clear, and its span in '
                  'thousandths of the wire time of those phases. No GIC register is read for the record, and no PPU '
                  'or PMIC register is written.',
                  sources=POWER_SOURCES + ['apps/board-bundles-power/bundle6.c']),
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
                  'read back to back with CNTPCT stamps until it reads ON or 10 ms pass. The client reads the same '
                  'words again. Domain 1 stays ON until power removal. A refusal (closed gate, unverified DCDC2 or '
                  'incoherent adoption) writes nothing and is inconclusive: bundle 8 then finds domain 1 not ON.',
                  sources=POWER_SOURCES + ['apps/board-bundles-power/bundle7.c']),
    returning_row('dram-bus-priority', 8, 'DRAM bus priority reads and table write',
                  ['apps/board-bundles-dram/main.c.obj', 'apps/board-bundles-dram/sequence.c.obj',
                   'arch/aarch64/device_memory.c.obj', 'drivers/dram/a733_dram.c.obj'],
                  'COMPLETE board bundle 8:', True,
                  'Monitor status 0 with COMPLETE board bundle 8: table written, the inherited and final fields of '
                  'ports 2, 14 and 15, the USB_PCIE fields unchanged and every access with its CNTPCT span; or '
                  'status 0 with COMPLETE board bundle 8: inconclusive and the skipped stage; or a controlled '
                  'outcome: an NSI synchronous external abort or a readback mismatch.',
                  'rank-9-usb-v4\'s platform objects and the provider\'s tasks.c and queue.c with '
                  'apps/board-bundles-dram/ (main.c, sequence.c), drivers/dram/a733_dram.c in table mode and '
                  'device_memory.c. It reads R_CCU 0x1ac, the domain 1 status only while 0x1ac bit 0 reads 1, and '
                  'CCU +0x580, +0x584 and +0x588. Only while domain 1 reads ON (8) and NSI_CLK bits 31 and 30 and '
                  'NSI_BGR bits 16 and 0 read 1 does it make the first NSI accesses: the +0x10, +0x14 and +0x18 '
                  'words of ports 2, 14 and 15, those of the USB_PCIE port 13, a733_dram_priority() in table mode '
                  '(writes and readbacks of ports 2, 14 and 15 only), and port 13 again, which must read back '
                  'unchanged. Otherwise it makes no NSI access and ends inconclusive, which admits no NSI '
                  'comparison in bundle 10. Inherited fields that read zero on all three ports are recorded as '
                  'possibly protected; the readback separates that case. Run it after bundle 7 of the same '
                  'lifetime.',
                  sources=['apps/board-bundles-dram/main.c', 'apps/board-bundles-dram/sequence.c',
                           'apps/board-bundles-dram/sequence.h', 'drivers/dram/a733_dram.c', 'include/a733_dram.h',
                           'arch/aarch64/device_memory.c', 'checks/board-bundles-apps.py',
                           'checks/board-bundles-apps-model.c']),
    read_row('npu-clock-words', 9, 'Speed-bin word and the CCU words of the NPU register pass',
             'The NPU speed bin and the CCU state that NPU startup admits, read before the NPU bundle',
             [word(0x0300624c, 'sid', note='SID speed-bin word')] +
             [word(CCU + o, 'ccu', note=f'CCU +0x{o:x}') for o in NPU_CCU],
             'Every word is read, and the 128 scratch bytes are restored and equal.'),
    {'name': 'npu-single-core', 'class': 'terminal', 'position': 10, 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'NPU single-core image with native USB takeover',
     'module': 'apps/npu-convolution/prepare-supervised.py', 'runner': 'tools/check-a733-npu-supervised.py',
     'sources': ['apps/npu-convolution/prepare-supervised.py', 'tools/check-a733-npu-supervised.py',
                 'drivers/npu/npu_service.c', 'tools/prepare-board-bundles.py', *TRANSPORT_SOURCES],
     'expected_result': 'Startup reaches READY and the outputs match the SDK simulator.' + TERMINAL_COUNTS,
     'needs': 'tools/check-a733-npu-supervised.py must declare and report COMPLETION_COUNTS, and the NPU owner must '
              'name admission values and record domain 1 and NSI state with the outputs.'},
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


def prepare_returning(row, build, output):
    """A returning bundle from the image builder's build.json: image, binary, runner closure, equivalence and
    controlled outcomes."""
    record = json.loads(Path(build).read_text())
    output.mkdir()
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
    flat = output.parent / (output.name + '-objcopy.bin')
    result = subprocess.run([OBJCOPY, '-O', 'binary', str(output / 'image.elf'), str(flat)], capture_output=True,
                            text=True)
    require(result.returncode == 0, f'objcopy failed: {result.stderr.strip()}')
    same = flat.read_bytes() == (output / 'probe.bin').read_bytes()
    flat.unlink()
    require(same, 'probe.bin is not the loadable segments of image.elf.')
    equivalence = json.loads((output / 'equivalence.json').read_text())
    require(equivalence['image_sha256'] == sha256(output / 'image.elf'), 'image.elf is not the built image.')
    objects = equivalence['objects']
    require(objects and all(o['origin'] in PASSED_ORIGINS | {'new'} for o in objects.values()),
            'An object has no recorded origin.')
    new = sorted(rel for rel, o in objects.items() if o['origin'] == 'new')
    require(set(new) <= set(row['overlay']),
            'Objects differ from the images that passed outside the overlay: ' + ', '.join(new))
    value = {'schema': 'board-returning-bundle-v1', 'row': row['name'], 'new_objects': new,
             'passed_objects': len(objects) - len(new), 'image_sha256': equivalence['image_sha256']}
    if row['controlled']:
        runner = load('board_bundles_returning_runner', output / 'host/tools/check-a733-kernel.py')
        outcomes = runner.controlled_outcomes((output / 'controlled.json').read_bytes())
        value['controlled'] = [o['name'] for o in outcomes]
    return value


def runbook(row, bundle, value, artifacts, prerequisites, timeout):
    """runbook.md for one prepared row, in the format of docs/board-runbooks/."""
    n = row['position']
    lines = [f'# Bundle {n}: {row["component"]}', '',
             f'Status: **prepared** by `tools/prepare-board-bundles.py`; not run. Position {n} of the single-core '
             'order of docs/audit/board-test-plan.md and docs/designs/board-bundles-next.md.',
             f'Bundle: `{bundle}`.', '']
    if row['class'] == 'read':
        lines += [f'A Read bundle: `board-read.py` reads {value["words"]} words with 32-bit FEL `readl` commands, '
                  f'{value["conditions"]} of them only after their condition word matches. No board code runs '
                  'beyond the readl stub, which the reader confines to the saved and restored 128 scratch bytes '
                  'at 0x71000; the USB instance does not change.', '']
    else:
        lines += [row['intro'], '']
    lines += ['## Bundle', '', '| File | SHA-256 |', '| --- | --- |']
    lines += [f'| `{Path(a["path"]).relative_to(bundle)}` | `{a["sha256"]}` |' for a in artifacts]
    lines += ['']
    if row['class'] == 'monitor':
        origins = sorted({o['origin'] for o in json.loads((bundle / 'equivalence.json').read_text())['objects'].values()}
                         - {'new'})
        lines += [f'Equivalence: {value["passed_objects"]} linked objects are byte-identical to objects of images '
                  'that passed on the board (' + ', '.join(f'`{o}`' for o in origins) + '; '
                  + ', '.join(f'{k}: {v}' for k, v in PASSED_RECORDS.items() if k in origins)
                  + '), listed per object in `equivalence.json`. New: '
                  + ', '.join(f'`{x}`' for x in value['new_objects']) + '.', '']
    lines += ['## Commands', '',
              'Set the variables of docs/board-runbooks/README.md ("Names used in every runbook"), with `S` '
              f'naming this lifetime\'s session directory, and `B={bundle}`.', '']
    expected = row['expected_result'].replace('"', "'")
    if row['class'] == 'read':
        lines += ['1. Read the words once:', '', '   ```sh',
                  f'   $PY -B $V/common/bundle-run.py $PORT $S/bb{n}-read {timeout} \\',
                  f'     "Board $PORT: {row["component"]}" \\', f'     "{expected}" \\',
                  f'     -- $PY -B $B/board-read.py $B/WORDS.json $S/bb{n}-read', '   ```', '',
                  '   Success: `{"port": "PORT", "complete": true, "words": ' + str(value['words'])
                  + ', "skipped": 0}`. A nonzero `skipped` count is recorded with each skipped word and the '
                  'condition that stopped it; the bundle is then inconclusive and is not repeated.', '',
                  f'   Prerequisite: `{prerequisites[0]}`, written by `bundle-run.py` for the current instance. '
                  f'Queue timeout: {timeout} s.', '']
    else:
        controlled = ' --controlled $B/controlled.json' if row['controlled'] else ''
        lines += ['1. Turn the instruction cache off and survey the current instance:', '', '   ```sh',
                  f'   $PY -B $V/common/bundle-run.py $PORT $S/bb{n}-cache-off 120 \\',
                  f'     "Board $PORT: returning instruction-cache disable before bundle {n}" \\',
                  '     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." '
                  '\\', f'     -- $PY -B $V/common/cache-off.py $S/bb{n}-cache-off',
                  f'   $PY -B $V/common/survey.py $PORT $S/bb{n}-survey "Board $PORT before bundle {n}"', '   ```', '',
                  '   Each prints `"result": "passed"`.', '',
                  '2. Run the image once. `D` is the DDR record of this lifetime\'s cold start; give one '
                  '`--warm-return` for each returning run since then, in order:', '', '   ```sh',
                  f'   $PY -B $V/common/bundle-run.py $PORT $S/bb{n}-run {timeout} \\',
                  f'     "Board $PORT: {row["component"]}" \\', f'     "{expected}" \\',
                  '     -- $PY -B $B/host/tools/check-a733-kernel.py --fel $FEL --image $B/probe.bin --mode kernel '
                  '\\', f'        --expect "{row["expect"]}"{controlled} --evidence $S/bb{n}-run \\',
                  f'        --ddr-evidence $D --platform-survey $S/bb{n}-survey/session.json [--warm-return ...]',
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
                      + f', with `controlled` in `$S/bb{n}-run/session.json` holding the parsed EL1 values. A '
                      'controlled session is a valid `--warm-return` link for later bundles.', '',
                      '## Controlled outcomes', '',
                      'Each outcome in `controlled.json` is matched on the full result record and log; any other '
                      'nonzero status quarantines the instance:', '']
            lines += [f'- {outcome_text(o)}' for o in outcomes]
            lines += ['']
        else:
            lines += [f'   Success: {passed}', '']
    lines += ['## Stop conditions', '', f'- {row["stop_condition"]}', '']
    if row['class'] == 'monitor':
        progress = row['controlled'] and any('progress' in o for o in json.loads(
            (bundle / 'controlled.json').read_text())['outcomes'])
        lines += ['## Without the patch', '',
                  'The bundle\'s `host/tools/check-a733-kernel.py` carries the warm-return admission and the '
                  'controlled outcomes that `bringup` merged, and the controlled-outcome progress record of the '
                  'bundle apps\' `primary.patch`. '
                  + ('Without that patch, the runner in `bringup` refuses this bundle\'s `controlled.json`, '
                     'whose outcome names a progress record.' if progress else
                     'This bundle\'s outcomes name no progress record, so the runner in `bringup` accepts them '
                     'as well.'), '']
    return '\n'.join(lines)


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


def prepare_row(row, arguments, row_dir, request, queue):
    """Prepare, admit, scan, freeze, hash, snapshot and submit one row."""
    record = {'class': row['class']}
    stage = 'inputs'
    try:
        require(isinstance(arguments, dict) and 'output' not in arguments,
                'Row inputs must be an object of prepare() keyword arguments without output.')
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
                stream.write(runbook(row, bundle, admitted, artifacts, prerequisites, timeout) + '\n')
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
            rows[row['name']] = prepare_row(row, request['inputs'][row['name']], run_dir / row['name'], request,
                                            queue)
    save(run_dir / 'run.json', {'schema': 'board-bundles-run-v1', 'physical_access': False, 'rows': rows})
    for name, record in rows.items():
        print(f'{name}: {record["status"]}' + (f' ({record["message"]})' if 'message' in record else ''))
    return 1 if any(r['status'] == 'refused' for r in rows.values()) else 0


def run(inputs, rows=ROWS, evidence=EVIDENCE, repository=ROOT, nm=NM):
    """Create the run directory and wait once on the sandboxed step; return (exit status, run directory)."""
    names = {row['name']: row for row in rows}
    unknown = sorted(set(inputs) - set(names))
    require(not unknown, 'Inputs name unknown rows: ' + ', '.join(unknown))
    excluded = sorted(n for n in inputs if names[n]['class'] == 'excluded')
    require(not excluded, 'Inputs name excluded rows: ' + ', '.join(excluded))
    require(Path(BWRAP).is_file(), f'{BWRAP} is missing; preparation needs the sandbox.')
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
    save(run_dir / 'request.json', {'rows': rows, 'inputs': inputs, 'repository': str(repository), 'nm': str(nm)})
    command = sandbox(run_dir) + [sys.executable, str(Path(__file__).resolve()), '--step', str(run_dir)]
    status = subprocess.run(command).returncode
    require((run_dir / 'run.json').is_file(), f'The sandboxed step stopped before recording a result ({status}).')
    return status, run_dir


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--inputs', type=Path, help='JSON object: row name to prepare() keyword arguments')
    parser.add_argument('--evidence', type=Path, default=EVIDENCE, help='Directory that receives the run directory')
    parser.add_argument('--nm', default=NM)
    parser.add_argument('--step', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.step:
        return step(args.step)
    if args.inputs is None:
        parser.error('--inputs is required')
    status, run_dir = run(json.loads(args.inputs.read_text()), evidence=args.evidence, nm=args.nm)
    print(run_dir)
    return status


if __name__ == '__main__':
    sys.exit(main())
