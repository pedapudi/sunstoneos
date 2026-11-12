# Return to the boot ROM after a failure

Status: **implemented and host-tested**. Registered return steps cover every
session grant; [the owner audit](failure-return-audit.md) distinguishes complete
stop paths from retained faults. Selected GPU, NPU and storage-read failures
return to FEL without a watchdog reset; [the return evidence](../evidence/failure-return.jsonl) defines those
inputs and retirement proofs. USB and radio failure-injection stages await
reviewed USB launch admission facts.
Forced application and EL0-abort returns also have
[application evidence](../evidence/application.jsonl).

A returning SunstoneOS image ends a failed run at the boot ROM, with its
first-failure record and the DRAM that the host saved before launch, instead of
at a watchdog reset. A watchdog reset loses DRAM. After a reset the runner
often cannot prove DRAM ownership, quarantines the USB instance, and only a
physical replug recovers the port.

This applies to the returning lifetime (contract magic `A733_OS_RETURN_CONTRACT_MAGIC`). The
resident and supervised lifetimes keep their endings.

## Where failures end

An image records the first failure in the RTC breadcrumb words. A failure that
cannot return closes admission (`board_fail`, which sets `board.fatal`); the CPU0
supervisor then sees `board_live()` false and stops the watchdog restarts
through `platform_exit`, a supervisor call to EL3 (`a733_smp_stop`) that
shortens the watchdog to 0.5 s. The runner classifies that result
`watchdog-reset`.

| Path | Evidence the runner gets | Ending |
| --- | --- | --- |
| `ss_fail`, `ss_fail_value` after the scheduler started | Level 0xb record in the RTC words: site, value, caller | Failure return |
| `ss_fail` in an application's `init`, before the scheduler | Same | Failure return, all eight cores |
| `platform_panic` and `configASSERT` on a core that can still schedule | Level 1 fault word with status 4 and the line | Failure return, record as group `board` |
| `platform_panic` on core 0 during board startup, before the scheduler | Same, line numbers 1 to 12 | Failure return, all eight cores |
| Unexpected interrupt after the scheduler started | Level 1 fault word, detail interrupt number plus 0x100 | Failure return; the source is masked first |
| Synchronous exception from an application task at EL0 (data abort, instruction abort, undefined instruction, PC or SP alignment fault) on a core open to the scheduler | Level 0xb record: group application, detail 0xe00 plus the ESR class, value the faulting address, caller ELR | Failure return; the faulting task is suspended, see [EL0 exceptions](#exceptions-from-an-application-task-at-el0) |
| Device failure before a return claim | Level 0xb record | Watchdog: admission is closed and resources are retained |
| Registered owner fault during a return | Per-owner RAM record; first secondary RTC record at level 0xc after an application failure | Remaining stop steps run. Unproved ownership keeps terminals blocked until the shared timeout; see [the audit](failure-return-audit.md) |
| A check inside the return sequence itself (`cpu-return.c` `fail`, `bounded`) | Level 0xb record | Watchdog |
| Session deadline or return cutoff | Level 0xb record | Watchdog |
| `platform_panic` with interrupts or priorities masked, in a critical section or in an interrupt; stack overflow hook | Level 1 fault word | Watchdog: a terminal task cannot take the core |
| EL1 synchronous exception, SError, EL3 exception (vectors `a733_cached_cold_fault`, `a733_cached_el3_fault`) | Fault page and level 1 or 3 fault words | Watchdog |
| EL0 synchronous exception of another class, or from a task in a critical section or with interrupts masked, or after the last reserved stack | Fault page and level 1 fault words | Watchdog |
| Secondary-core fault at EL1 or EL3 | Same; the faulting secondary parks and core 0 stops | Watchdog |
| EL3 startup refusals (`a733_smp_stop` in `boot.c`, `a733_cached_smc` before the image runs) | Fault words | Watchdog |

Exception-vector faults taken at EL1 or EL3 end at the watchdog: the faulting
context's stack pointer and caches are unknown, and the vectors run before any
task exists to take over the core. An SError ends at the watchdog because it is
asynchronous and may report a lost write.

## Exceptions from an application task at EL0

The EL1 vector for a synchronous exception that is not a supervisor call
calls `os_cpu_return_el0_try` ([cpu-return-el0.S](../../apps/os/cpu-return-el0.S))
before it takes the stop. The routine admits the exception only if all of these
hold:

- `SPSR_EL1` names AArch64 EL0 with the I flag clear, so the task ran with
  interrupts open;
- `ICC_PMR_EL1` is at least 0xf0, above every API priority mask, so the task
  was not in a kernel critical section (the unmasked value 0xff reads back as
  0xf8 or 0xf0 when the controller implements fewer priority bits);
- the ESR class is 0x00 (undefined instruction), 0x20 (instruction abort),
  0x22 (PC alignment), 0x24 (data abort, including an alignment fault) or 0x26
  (SP alignment);
- one of eight reserved stacks of 2048 bytes is free.

The kernel stack is not used. The routine abandons the faulting context, whose
stack pointer and registers may be corrupt: it claims a stack with an exclusive
pair, sets `SP_EL0` to its top, loads `ESR_EL1`, `FAR_EL1` and `ELR_EL1` into
`x0` to `x2`, and returns to EL0 in `os_cpu_return_el0_fault`
([cpu-return.c](../../apps/os/cpu-return.c)). That function runs in the task,
as `ss_fail` does. It records group application with detail 0xe00 plus the ESR
class. Site bits 30:16 hold `ESR_EL1` bits 14:0: for an abort, the write flag
and status code. The value is the faulting address, or zero where `FAR_EL1` is
undefined; the caller is `ELR_EL1`. It then makes the
same claim as `ss_fail`, with the record context `fault`, and suspends the
task. The task never runs application code again.

The supervisor takes it from there as for any failure after the scheduler
started, so the fault may occur on any core, including a worker core. The
kernel's stack overflow check compares the task's stack pointer with the task's
own stack; `vApplicationStackOverflowHook` ignores a task that runs on a
reserved stack.

If the claim is refused, because a device failure closed admission or the
return window has passed, the function closes admission through `board_fail`
and the failure ends at the watchdog.

## The failure return

A failure that can return claims the return once. The claim is one
compare-and-swap on the same `closing` word that a successful `ss_finish` sets,
so a failure and a success cannot both proceed.

**After the scheduler started.** The claim publishes the failure record in the
OS result record (status `SS_RESULT_FAILED`, payload `struct ss_failure` in
`include/sunstone_session.h`), marks the application finished so that
`ss_live()` is -1 and `ss_finish` refuses, and leaves `board.fatal` clear.
The CPU0 supervisor then runs the same stop sequence as for a successful finish,
through each service's own stop path:

1. The supervisor requests registered application queues, then device owners,
   CPU clock lowering, thermal stop, PMIC stop and per-core terminals.
2. Each callback performs one bounded, non-blocking request or status operation.
   Hardware work stays on its authorized owner task. PENDING holds the next
   class; DONE requires that owner's retirement proof.
3. FAULT preserves resources and the owner's first diagnostic. The supervisor
   records the fault and continues later steps. Thermal sampling continues while
   a device remains unretired. A fault cannot authorize terminal release.
4. One three-second budget (`OS_FAILURE_RETURN_US`) bounds all classes in both
   normal and failure returns. An unfinished or unsafe return ends at the
   watchdog when this budget or the session deadline expires.
5. Once every owner reports DONE, the supervisor wakes one terminal task per
   core. Each runs at the highest priority, cleans caches, restores interrupt
   and power state, disarms the watchdog and returns to ROM.

An application's NPU producer closes its queue at a safe point after
`ss_live()` becomes negative. The GPU worker can finish accepted work after the
application is suspended; its failure receipt identifies abandoned completed
outputs. Storage finishes the issued transfer and closes only when its owner-held
record proves an untouched write session or a completed consistent transaction.
An unsafe filesystem phase immediately records the session ID and phase as FAULT;
return does not issue another metadata write, retry or recovery transaction.

The supervisor publishes the absolute return deadline before requesting owners.
The limit is three seconds from the return origin and ends before the session's
final 10 ms guard. Shared lock waiters recheck it during existing tick waits and after
acquisition. Shared pin reads and writes also check it before register access.
An earlier transaction deadline still applies; observing closing before the
shared deadline is published permits no new hardware work.

Later application failures preserve the first record. `ss_fail` and
`ss_fail_value` record their application detail once, including when a claim is
refused; their fallback does not add a board note. Registered device faults leave
`board.fatal` clear while the supervisor collects remaining owner results.

**Before the scheduler started.** Core 0 claims the return in
`ss_fail` or `platform_panic`, writes the record, marks the return ready and
enters the terminal itself. No service task has run, so no device was opened
and nothing needs stopping. Cores 1 to 7 wait in the start sequence for core 0
to finish its shared initialization; that wait also checks the claim and enters
the terminal. Only core 0 before it releases the others can claim.

**Software-detected faults** (`platform_panic`, an unexpected interrupt) record
group `board` and the line (or the interrupt number plus 0x800) as the site.
`platform_panic` claims only where a task can still run on the core:
interrupts and priority mask open. An unexpected interrupt is masked at the
interrupt controller before the claim, so that it cannot repeat.

## Device owner audit

[The grant audit](failure-return-audit.md) lists every owner, its admission
source, order, stop proof and remaining gap. Registration precedes startup;
closing refuses new registrations. The registry publishes all requests in an
order class before releasing that class to its tasks. This preserves shared
radio/headset ownership. A successful completion cannot overwrite FAULT.

USB host, partial radio startup, scan-only radio, I2S audio and closed DMA services
have native retirement proofs. Their callbacks report DONE only after those
proofs complete. Active or abandoned DMA work, uncertain filesystem state and
full-station CE ownership retain FAULT. Headset retirement requires protocol and
UART closure plus explicit version 4 module-reset authority; legacy profiles
retain FAULT ([Bluetooth return](bluetooth-return.md)).
The audit distinguishes implemented stop paths from admitted and physically
qualified compositions.

## NPU stop report when the service never ran

The runner requires the NPU stop report of an NPU image before it restores
DRAM. A failure before the scheduler, such as a refusal in `os_npu_start`,
leaves no NPU task and no report. `os_npu_never_ran` in
[npu.c](../../apps/os/npu.c) is true while the bootstrap task has not started
and no activation has happened, so nothing was powered, clocked or submitted.
In that state the claim publishes the report with `finished` 2
(`OS_NPU_REPORT_NOT_STARTED` in [npu.h](../../apps/os/npu.h)), `stopped` 1,
`retained` 0, every other field zero, and the epoch and generation of the
failure record. The claim does this only before the scheduler started: after
that, the bootstrap task may still run, and the client publishes the report
when it stops.

The host accepts `finished` 2 only for a failure return whose CPU return
report is valid, and only with the failure record's generation (zero when the
failure preceded the sealed session). A missing, stale, retained or
other-generation report is refused and the port is quarantined.

## The record

The first breadcrumb record is level 0xb in the RTC fault word, with the site, caller
and value words; `tools/a733-crumbs.py` decodes it. The
DRAM record repeats the same site, value and caller, with the core, the failure
context (running, startup or fault) and the counter at the failure. It reaches
the host through the existing result read, so the runner reads it after the
return and before restoring DRAM. The runner also reads the breadcrumb words;
an image that does not link the failure return and returns with a failure
record in the breadcrumbs is still refused as an unresolved return.

During the window, the supervisor writes the first registered secondary fault
at RTC level `0xc`. Its `QUIESCE` group names the owner and original group,
with the original code, value and caller. Each owner also retains its full first
record separately in RAM; [the audit](failure-return-audit.md) defines the layout
and the RTC slot's limits.

If a device failure already closed admission, the failure return does not
start, and the failure ends at the watchdog.

## Runner

`tools/board_images.py` marks an image that links `os_cpu_return_failure`.
For such an image `tools/board.py` accepts a normal return that carries a board
failure in its breadcrumbs, and the capture path decodes a failed result
instead of an application result. Retirement is validated, DRAM is
restored, and with `--restore-rails` the rail helper runs. The outcome is
`failure-return`: a failed result (the command exits 1) with the failure record
in `first_failure`. The port is not quarantined and is not paused; the three
same-site repeats rule still applies to repeated `failure-return` results. The
48-byte failure record (`struct ss_failure`) is recorded in `first_failure`.

## Checks

`checks/os-failure-return.py ELF [--baseline-elf ELF]` executes the linked image
in each context with modeled peripherals. It runs the real failure sites,
breadcrumb SVC, claim, result record, supervisor, secondary wait and the
EL1/EL3 terminal, and supplies the service completions, the task notifications
and the scheduler state words. It covers:

- a failure before the scheduler, which returns to the ROM from all eight cores
  with the RTC record, the DRAM record and every core restored;
- a failure while running: the claim, a record with admission left open, the
  supervisor notifying one terminal per core, and the ROM return;
- a later failure, which keeps the first record; a device failure during the
  return, an unfinished return, and a device failure that closed admission
  first, which end at the EL3 stop (the watchdog path);
- `platform_panic`, which returns on a core that can schedule and keeps the
  watchdog when masked, and an unexpected interrupt, which is masked and
  returns;
- exceptions from EL0 (data abort on core 6, instruction abort, undefined
  instruction, PC and SP alignment faults, a store on a secondary core), which
  enter the real vector, return to EL0 on a reserved stack with the arguments,
  record the failure and reach the ROM;
- a second fault on another core takes a
  second stack and keeps the first record;
- negative controls that keep the watchdog ending: an exception from EL1, an EL0
  exception with interrupts masked, with the priority mask raised to the API
  level, from AArch32, of an unlisted class, after the last stack, and after a
  device failure closed admission;
- for an NPU image, the never-started report after a startup failure, and the
  kept report of a service that ran;
- with `--baseline-elf`, an EL0 exception that ends at the watchdog, and a
  startup failure on an image without the failure return that does not reach
  the ROM.

`checks/qemu-os-boot.py` runs a case on the virtual board, where the MMU raises
a real translation fault. The application's store to an unmapped address reaches
the same vector source, the task continues on a reserved stack, the board
records group application with detail 0xe24, the faulting address and instruction. The task is suspended while the other cores run. The returning
lifetime is not linked on that board, so its `os_cpu_return_el0_fault` records
and suspends without the claim.

`checks/board-return-preservation.py` covers three runner cases: a failure
return (outcome `failure-return`, DRAM and rails restored, not quarantined), and a
failure record returned by an image that does not link the failure return (host
error, quarantined). Five NPU cases cover the never-started report: valid with a
failure return (restored, not quarantined), and refused for a normal return, a
stale epoch, a missing report, `retained` 1 and another generation.

The linked model executes the registry and supplies owner publications. It does
not execute FreeRTOS task switching or physical clocks and power. Focused owner
checks cover their stop paths separately. Physical evidence remains limited to
the named queue records.
