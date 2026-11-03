# Return to the boot ROM after a failure

Status: **implemented** and **host-tested** for every row below that returns;
**qualified on the board** for a forced application failure and an EL0 data
abort (records `1793579984604886693-7d81d397` and
`1793582514152160909-ecc5988b` in [application.jsonl](../evidence/application.jsonl)).
The other paths are executed only in host models and on the virtual board.

A returning SunstoneOS image ends a failed run at the boot ROM, with its
first-failure record and the DRAM that the host saved before launch, instead of
at a watchdog reset. A watchdog reset loses DRAM. After a reset the runner
often cannot prove DRAM ownership, quarantines the USB instance, and only a
physical replug recovers the port.

This applies to the CPU return lifetime (`SUNSTONE_A733_OS_CPU_RETURN`). The
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
| Device failure: PMIC, thermal, NPU, GPU, CPU clock, DMA, `board_fail` called by a driver | Level 0xb record | Watchdog: the owner retains its resources, and only a reset releases them; see [the audit](#device-owner-audit) |
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
as `ss_fail` does. It records group application with the detail 0xe00 plus the
ESR class and `ESR_EL1` bits 14:0 (for an abort, the write flag and the status
code) in site bits 30:16, the faulting address as the value (zero for classes
where `FAR_EL1` is undefined) and `ELR_EL1` as the caller. It then makes the
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

1. The CPU clock owner lowers CPU-B to the boot point, in whatever phase it was.
2. The thermal service stops its converter.
3. The DMA and NPU services report their own retirement. An application's
   NPU client must close itself when `ss_live()` turns -1; if it does not,
   step 5 applies.
4. The PMIC owner stops after its slots and locks are idle.
5. If the sequence has not finished 3 s after the claim
   (`OS_FAILURE_RETURN_US`), the supervisor ends the failure at the watchdog.
   The session deadline bounds it in any case.
6. The supervisor wakes one terminal task per core. Each runs at the highest
   priority, so it takes the core from any application task. The terminals
   clean the caches, restore the interrupt controller and power state, disarm
   the watchdog and return to the ROM, as for success.

Later application failures during the sequence leave the first record; a
device failure during it closes admission and ends at the watchdog.

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

Each `board_fail` caller in a device owner, and whether the owner could stop
through its normal stop path after the failure. The stop sequence needs the
owner's stop path to complete with no retained resource: `os_cpu_return_pmic`
refuses a held, retained or failed PMIC slot, the thermal stop refuses after
`cancel()`, and `os_npu_retirement_check` and `os_gpu_return_ready` refuse a
failed service. None of the callers qualifies, so every device failure ends at the
watchdog.

| Owner and caller | State after the failure | Stop path | Ending |
| --- | --- | --- | --- |
| PMIC task, `apps/board/main.c` `pmic` (authority or ledger fault) | Task parks with the slot ownership held; `pmic.failed` | `os_cpu_return_pmic` refuses (0x739) | Watchdog |
| Thermal owner, driver fault, lost owner, interrupt re-arm or mask failure (`apps/os/thermal.c` `fault`, `cancel`) | `thermal.failed` and `retaining` set; the clock token is retained | The stop requires `live()`, which `thermal.failed` clears | Watchdog |
| Thermal owner, critical temperature (`fault` after `A733_THERMAL_CRITICAL`) | The converter is healthy, but the owner takes the same state as above | `a733_thermal_stop` could stop it; the owner refuses. The watchdog ending is deliberate: the 0.5 s stop shortens the time at the critical temperature, and the return sequence can take 3 s | Watchdog |
| CPU clock owner refusal (`apps/os/cpu.c` `refused`, reached through the thermal owner's `fault`) | A retained or invalid driver state holds the clock, rail or request; a refusal at the start phase holds nothing but still fails the thermal owner | The clock return step refuses a retained or invalid driver (`REFUSED`) | Watchdog |
| DMA, `apps/os/dma.c` `fatal` through `platform_panic` | The pool service is retained | `os_cpu_return_panic` claims the return, `os_dma_return_ready` never becomes true, and the 3 s bound ends at the watchdog | Watchdog after 3 s |
| NPU, `apps/os/npu.c` `fail_at`, `apps/os/npu-client.c` `retained` | `npu.failed`; every NPU resource stays held | `os_npu_retirement_check` refuses | Watchdog |
| GPU, `apps/os/gpu.c` interrupt service failure and `gpu.failed` paths | The compute service is marked failed | `os_gpu_return_ready` and the GPU stop refuse | Watchdog |
| Supervisor, `apps/board/main.c` `supervise` | Admission was already closed or the deadline passed; this is a consequence, not a cause | Not applicable | Watchdog |

A device failure that the owner could stop after cleaning up needs the owner to
report a stoppable failure state to the claim, so that `os_cpu_return_failure`
can accept a device group. No owner provides one.

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

The breadcrumb record is level 0xb in the RTC fault word, with the site, caller
and value words; `tools/a733-crumbs.py` decodes it. The
DRAM record repeats the same site, value and caller, with the core, the failure
context (running, startup or fault) and the counter at the failure. It reaches
the host through the existing result read, so the runner reads it after the
return and before restoring DRAM. The runner also reads the breadcrumb words;
an image that does not link the failure return and returns with a failure
record in the breadcrumbs is still refused as an unresolved return.

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

The model does not execute FreeRTOS task switching, the service stop paths
themselves, or physical clocks and power. The board evidence is the two runs
named in the status line.
