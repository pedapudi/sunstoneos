# Return to the boot ROM after a failure

A returning SunstoneOS image ends a failed run at the boot ROM, with its
first-failure record and the DRAM that the host saved before launch, instead of
at a watchdog reset. A watchdog reset loses DRAM. After a reset the runner
often cannot prove DRAM ownership, quarantines the USB instance, and only a
physical replug recovers the port.

This applies to the CPU return lifetime (`SUNSTONE_A733_OS_CPU_RETURN`). The
resident and supervised lifetimes keep their endings.

## Where failures end

Before this change, every row below ended at the independent watchdog. An image
recorded the first failure in the RTC breadcrumb words, then closed admission
(`board_fail`, which sets `board.fatal`). The CPU0 supervisor then saw
`board_live()` false and stopped the watchdog restarts through `platform_exit`.
That is a supervisor call to EL3 (`a733_smp_stop`), which shortens the watchdog
to 0.5 s. The runner classified the result `watchdog-reset`.

| Path | Evidence the runner gets | Ending now |
| --- | --- | --- |
| `ss_fail`, `ss_fail_value` after the scheduler started | Level 0xb record in the RTC words: site, value, caller | Failure return |
| `ss_fail` in an application's `init`, before the scheduler | Same | Failure return, all eight cores |
| `platform_panic` and `configASSERT` on a core that can still schedule | Level 1 fault word with status 4 and the line | Failure return, record as group `board` |
| `platform_panic` on core 0 during board startup, before the scheduler | Same, line numbers 1 to 12 | Failure return, all eight cores |
| Unexpected interrupt after the scheduler started | Level 1 fault word, detail interrupt number plus 0x100 | Failure return; the source is masked first |
| Device failure: PMIC, thermal, NPU, GPU, CPU clock, `board_fail` called by a driver | Level 0xb record | Watchdog: the owner retains its resources, and only a reset releases them |
| A check inside the return sequence itself (`cpu-return.c` `fail`, `bounded`) | Level 0xb record | Watchdog |
| Session deadline or return cutoff | Level 0xb record | Watchdog |
| `platform_panic` with interrupts or priorities masked, in a critical section or in an interrupt; stack overflow hook | Level 1 fault word | Watchdog: a terminal task cannot take the core |
| EL1 synchronous exception, SError, EL3 exception (vectors `a733_cached_cold_fault`, `a733_cached_el3_fault`) | Fault page and level 1 or 3 fault words | Watchdog |
| Secondary-core fault | Same; the faulting secondary parks and core 0 stops | Watchdog |
| EL3 startup refusals (`a733_smp_stop` in `boot.c`, `a733_cached_smc` before the image runs) | Fault words | Watchdog |

Exception-vector faults keep the watchdog. At such a fault the faulting
context's stack pointer and caches are unknown, and the vectors run before any
task exists to take over the core. A safe return needs a per-vector decision
whether the stack is usable, and one physical run per exception class to
confirm it. The CPU can make progress after a fault from an application at EL0,
but no recovery has been built for it; the entry stays on the watchdog path.

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
   (`OS_FAILURE_RETURN_US`), the supervisor ends the failure at the watchdog,
   as before. The session deadline bounds it in any case.
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
to finish its shared initialization; that wait now also checks the claim and
enters the terminal. Only core 0 before it releases the others can claim.

**Software-detected faults** (`platform_panic`, an unexpected interrupt) record
group `board` and the line (or the interrupt number plus 0x800) as the site.
`platform_panic` claims only where a task can still run on the core:
interrupts and priority mask open. An unexpected interrupt is masked at the
interrupt controller before the claim, so that it cannot repeat.

## The record

The breadcrumb record is unchanged: level 0xb in the RTC fault word, with the
site, caller and value words. `tools/a733-crumbs.py` decodes it as before. The
DRAM record repeats the same site, value and caller, with the core, the failure
context (running, startup or fault) and the counter at the failure. It reaches
the host through the existing result read, so the runner reads it after the
return and before restoring DRAM. The runner also reads the breadcrumb words;
an image that does not link the failure return and returns with a failure
record in the breadcrumbs is still refused as an unresolved return.

If a device failure already closed admission, the failure return does not
start, and the failure keeps its watchdog ending.

## Runner

`tools/board_images.py` marks an image that links `os_cpu_return_failure`.
For such an image `tools/board.py` accepts a normal return that carries a board
failure in its breadcrumbs, and the capture path decodes a failed result
instead of an application result. Retirement is still validated, DRAM is
restored, and with `--restore-rails` the rail helper still runs. The outcome is
`failure-return`: a failed result (the command exits 1) with the failure record
in `first_failure`. The port is not quarantined and is not paused; the three
same-site repeats rule still applies to repeated `failure-return` results. An
`--expect` file, when given, is compared with the 48-byte failure record
(`struct ss_failure`).

## Checks

`checks/os-failure-return.py ELF [--baseline-elf ELF]` executes the linked image
in each context with modeled peripherals. It runs the real failure sites,
breadcrumb SVC, claim, result record, supervisor, secondary wait and the
EL1/EL3 terminal. It supplies the service completions, the task notifications
and the scheduler state words, and stubs three kernel entry points. It checks:

- startup: a failure before the scheduler returns to the ROM from all eight
  cores, with the RTC record, the DRAM record and every core restored.
- running: the claim, a record with admission left open, the supervisor
  notifying one terminal per core, and the ROM return.
- a later failure keeps the first record; a device failure during the return,
  an unfinished return, and a device failure that closed admission first all
  end at the EL3 stop, which is the watchdog path.
- `platform_panic` returns on a core that can schedule and keeps the watchdog
  when masked; an unexpected interrupt is masked and returns.
- with `--baseline-elf`, the same startup failure on an image without the
  failure return does not reach the ROM.

`checks/board-return-preservation.py` adds three runner cases: a failure return
(outcome `failure-return`, DRAM and rails restored, not quarantined), the same
with a differing expectation (`result-mismatch`, still restored), and a failure
record returned by an image that does not link the failure return (host error,
quarantined, as before).

The model does not execute FreeRTOS task switching, the service stop paths
themselves, or physical clocks and power. The physical evidence is the single
run recorded in [the CPU status](../status/cpu.md).
