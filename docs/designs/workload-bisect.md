# Design: a CPU0 return deadline for the returning eight-core workload

Status: **implemented**; instruction-model and host checks pass. The first
profile completed normally once on port 3-3, in a fresh lifetime on
2026-10-25 (below); its deadline return path has not run on a board.
Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

The returning eight-core workload runs the frozen native image
`762237cd…` with the workload overlay `a772ba03…`. Each core runs a worker
that holds a floating-point value across 32 one-tick delays, and a
background task that increments a counter whenever the worker blocks. CPU0
services the 1 kHz FreeRTOS tick; the tick wakes the secondaries' workers
with SGI 0. After the rounds, every core stops its timer and interrupt
delivery, the cores clean the owned writable interval
(`0x41200000`–`0x41291000`), and CPU0 returns to the boot ROM after the
seven secondaries reach OFF.

On 2026-10-25 this workload did not return on port 3-3: no new ROM instance
appeared in 6.7 hours. The same overlay and native image returned on port 3-1
on 2026-10-24 (`processor-completion-20261024/port-3-1/task-workload/`). That
run reports 32 rounds on every core, 32 ticks on CPU0 and 33 SGIs on each
secondary. The two launches differ only in the contract and request (epoch,
generation, reservation). Both boards take power and data from a single
USB-C cable. The failing run came late in a long port 3-3 lifetime, after
about twelve returning images: four PMIC bundle 6 runs, the native USB image
of rank 9, bundles 14 and 15, the rank 7 CPU0 preparation and pre-scheduler,
the first task, the task switch and the thermal reading. Its 91-word power,
CPU-configuration and GIC capture after the thermal run equalled the clean
capture.

In a fresh port 3-3 lifetime on 2026-10-25 (cold DDR, a 91-word capture
equal to the clean one, the cache-off helper and a survey), the first
profile below completed normally: 32 rounds on every core, 32 ticks on CPU0,
33 SGIs on each secondary, controls and DRAM restored and the ROM usable
(`board-session-20261025-1/port-3-3/tw-deadline-prepare/runtime`). Repeats
in the same lifetime, each followed by a capture, separate the first two
candidates below: failures that appear only as the lifetime grows point at
accumulated state, and isolated failures at a race.

## Every failure parks CPU0, so a non-return carries no information

Each check and each bounded wait in the overlay ends in a park when it
fails. CPU0 parks as well, so no failure of any kind reaches the ROM:

| Wait or check | Bound | On failure | CPU0 state afterwards |
| --- | --- | --- | --- |
| Kernel lock, `vPortRecursiveLock` (`SEVL`, `WFE`, `LDR` at `0x41010dfc`) | none | — | waits with interrupts masked by PMR |
| Worker `vTaskDelay(1)` per round (`check_fp_context`, `0x41007a58`) and CPU0's `vTaskDelay` while it waits for the others | none; needs CPU0's tick and the SGIs | — | runs the background task |
| IRQ handler deadline check (`platform.c:129`) | startup deadline (in this path the diagnostic deadline equals it) | `a733_smp_exit_privileged(3, irq)`, an SMC | `el1_return_checkpoint` refuses it and parks in EL3 (`native-el1-cache-probe.S:324`) |
| Arrival barrier `.Lworkload_background`, `.Lworkload_wait` (`all-core-first-task-probe.S:64`, `:84`) | startup deadline | `.Lworkload_park` | `WFE` at EL0, interrupts enabled |
| Completion barrier and drain `.Lwait_done`, `.Lworkload_rwp`, `.Lworkload_clear` (`:183`, `:207`, `:215`) | startup deadline; any unexpected pending or active PPI | `identity_park` | `WFI` at EL1, interrupts masked |
| Survey waits in `identity_primary_run` | startup deadline | returns 0 | `identity_park` at EL3 |
| A configASSERT or FreeRTOS abort | — | SMC through `svc #119` | parks in EL3 |

The startup deadline is about 1 s after launch; the workload needs about
35 ms after the scheduler starts. Only the IRQ handler's check can end a
stall, and only on a core that still takes an interrupt; it then parks too.

## Reference comparison

The comparison covers each step the workload adds to the task-switch profile
that passed on port 3-3. Reference sources are pinned in
`~/.cache/sunstoneos/evidence/workload-reference-20261025/sources.json`:
Linux v6.17 (`e5f0a698`), Trusted Firmware-A (`0892ad4d`), the experimental
A733 TF-A port (`5bc9605c`) and the FreeRTOS kernel of `sources.lock.json`
(`97f50c0e`). Addresses refer to the frozen native image.

| Step | Reference | SunstoneOS | Difference and consequence |
| --- | --- | --- | --- |
| Tick programming | Linux `set_next_event` writes CVAL = counter + delta, then CTL with ENABLE set and IMASK clear; the accessor puts an ISB after the CTL write only (`arm_arch_timer.c:741`, `arch_timer.h:100`–`110`). The handler acts only when CTL.ISTATUS is set and first sets IMASK (`arm_arch_timer.c:665`). FreeRTOS's SMP tick handler clears the source after `xTaskIncrementTick`. | `platform_tick_ack` adds a fixed 24,000 ticks to the previous CVAL (`platform.c:124`). The tick handler tail-calls it after restoring PMR and clearing DAIF.I (`0x41010bcc`–`0x41010be0`). The IRQ handler does not read ISTATUS. | A tick handled late leaves CVAL in the past, so the timer fires again at once until the schedule catches up, and each tick sends time-slice SGIs to all seven secondaries. A level still sampled at EOI counts an extra tick. Bounded; not a hang by itself. |
| Event stream and WFE waits | Linux enables an event stream in CNTKCTL (`arm_arch_timer.c:915`–`956`) at `ARCH_TIMER_EVT_STREAM_FREQ`, 10 kHz (a 100 µs period, `include/clocksource/arm_arch_timer.h:68`–`70`), so every WFE wait wakes periodically. | `CNTKCTL_EL1 = 3` (`continue.S:140`): counter access only. Lock waits rely on the releaser's `DSB SY; SEV` (`0x41010eac`). | Architecturally sufficient. A missed event would leave a core waiting forever with interrupts masked. |
| EL2 controls for an unused EL2 | TF-A sets CNTHCTL_EL2 EL1PCEN and EL1PCTEN, CNTVOFF 0, CNTHP_CTL_EL2.ENABLE 0 and VPIDR/VMPIDR from MIDR/MPIDR (`context_mgmt.c:1133`–`1208`). | CNTHCTL_EL2 = 3 and CNTVOFF = 0 (`continue.S:113`); CNTHP_CTL_EL2, VPIDR_EL2 and VMPIDR_EL2 keep their reset values. | The workload's drain refuses any pending PPI other than SGI 0 and PPI 30 (`all-core-first-task-probe.S:215`); an enabled EL2 timer would assert PPI 26 and park that core. Recorded EL1 MPIDR reads equal the hardware values and CPU0's ISPENDR0 read 0 in the ROM. |
| SGI send | Linux: `DSB ISHST`, write ICC_SGI1R_EL1, `ISB` (`irq-gic-v3.c:1428`–`1450`). | `DSB ISHST`, write ICC_SGI1R_EL1, `ERET` (`0x4101141c`–`0x41011438`), reached from the tick handler through a nested `SVC #117` at EL1 (`0x41010cb4`). | Equivalent ordering; the nested SVC is unusual but ran on port 3-1. |
| Acknowledge, priority, EOI | Linux EOImode 0 at EL1: EOIR1 then ISB after the handler (`irq-gic-v3.c:719`). | IAR1 at entry, handler, DAIF.I set, `DSB SY; ISB`, EOIR1 (`0x41011cc0`, `0x41011d24`–`0x41011d34`); ICC_CTLR_EL1 = 0. The tick and SGI handlers raise PMR to `0xb0` and clear DAIF.I as upstream FreeRTOS does; both sources have Nonsecure priority `0xe0`, so neither preempts the other. | No difference with an effect. GIC-600 erratum 1014611 (SGI/PPI deferred until deactivation) does not apply with combined EOI and deactivation. |
| FP/SIMD enable | TF-A CPTR_EL3 reset value; Linux `el2_setup.h` CPTR_EL2 with TFP clear. | CPTR_EL3 = 0, CPTR_EL2 = `0x33ff`, CPACR_EL1.FPEN = 3 on every core (`continue.S:111`, `:138`). FreeRTOS saves q0–q31, FPSR and FPCR on every IRQ and every task (`configUSE_TASK_FPU_SUPPORT 2`). | Same trap controls; eager saving instead of Linux's lazy saving. |
| Cortex-A76 errata (cores 6 and 7, r4p1) | TF-A defines 1946160, 2356586 and 3888013 (`cortex_a76.S:429`–`488`) and applies each only when a build flag selects it; the pinned A733 port selects none of them (`allwinner-common.mk`, `allwinner-common-ncat2.mk`, `sun60i_a733/platform.mk`). 2743102 needs a DSB before the power-down ISB. The DSU errata 798953 and 936184 are at `cortex_a76.S:312`–`321`. | `platform/a733/cpu_coherence.c` applies 1946160, 2356586 and 3888013 on every A76 and the DSU conditions; the secondary shutdown has the 2743102 DSB. | SunstoneOS applies more than the A733 port. No missing workaround. |
| Cortex-A55 errata (cores 0–5, CPU0 included, r2p0) | TF-A's A55 reset workarounds (768277, 778703, 798797, 846532, 903758, 1221012) apply only to r1p0 and earlier; the A733 port sets `ERRATA_A55_1530923` (`allwinner-common-a55.mk`), a speculative address-translation erratum that generic code handles. | `cpu_coherence.c` applies none at r2p0 and requires the speculative-AT sequence that `el1_return_checkpoint` and the EL3 fault entry run. | None. |
| Power-on and GIC wake | The A733 TF-A port sets the hotplug power mode and enable, then clears GIC_WAKEUP_DISABLE (`sunxi_native_pm_ncat2.c`). | `cpu_continue.c` clears control bit 1 in the same register. | None. |

No difference explains a deterministic failure of bytes that returned on
port 3-1. The first three rows remain candidates because their effect depends
on timing or on reset state. All reference files, including the event-stream
header and the A733 port's A55 fragment, are pinned in that `sources.json`.

## Candidate causes, most likely first

1. **State left by earlier images in the lifetime** outside the 91
   captured words. The failing run followed about twelve returning images;
   the passing run on port 3-1 and the passing run in a fresh port 3-3
   lifetime followed far fewer.
2. **An intermittent race.** The tick programming (first table row) is the
   leading example: a late tick leaves CVAL behind the counter and produces
   back-to-back ticks, each with seven SGIs and seven context switches under
   the kernel locks. A lost wake in a kernel-lock WFE (second row) is the
   other: it needs a missed event and would leave a core, possibly CPU0,
   waiting with interrupts masked.
3. **An unexpected pending PPI at the drain** (third row), which parks one
   core in `identity_park` and then CPU0 at its completion barrier. It
   depends on reset or inherited state, so it can combine with candidate 1.
4. **Sustained eight-core load on USB power.** This is the first profile
   that keeps all eight cores busy: the background loop
   (`diagnostic.c:8`–`14`, `0x41001424`–`0x4100143c`) never waits, for at
   least 32 ms, on both A76 cores as well. Both boards share the same
   USB-C supply arrangement, so this is not a difference between them, and
   the same load completed in the fresh lifetime.

## Two returning profiles

Both profiles keep the complete workload and add the same CPU0 return
deadline. The builder option `--return-deadline` of
`tools/prepare-secondary-identity-probe.py` (with `--task-workload`) adds
`boot/a733/workload-deadline-probe.S` and two native patches:

- **Arm.** The BL at `a733_cached_enter_el1+0x34`, which replaces
  `MSR ICC_IGRPEN0_EL1, XZR`, runs on every core at EL3. On CPU0 only it
  makes PPI 29 (the EL3 physical timer, CNTPS) Group 0 at priority 0 in
  CPU0's redistributor, enables it, sets CNTPS_CVAL to the startup deadline
  plus 12,000,000 ticks (0.5 s) and enables Group 0. With SCR_EL3.FIQ set,
  the timer interrupt is taken to EL3 whatever masks, locks or waits CPU0 is
  in at EL0 or EL1.
- **Interrupt.** The EL3 vector for an FIQ from a lower exception level
  (`a733_cached_el3_vectors+0x500`) branches to the capture on CPU0 and to
  the production fault path on any other core.
- **Other CPU0 stops.** Every SMC from EL1 first passes the deadline unit.
  CPU0's completion SMC disarms the timer and Group 0 and continues the
  unchanged return. Any other CPU0 SMC (the IRQ handler's deadline exit, a
  refusal, a panic) is captured. A failed survey at EL3 and a refused
  completion SMC are captured instead of parking.
- **Capture and return.** The capture acknowledges the interrupt, stops the
  timer, turns the EL3 MMU off and writes the record below. It then returns
  to CPU0 EL1 with interrupts masked, where a helper cleans the owned
  writable interval (9,280 lines) through the Nonsecure mapping that the
  writers used, and issues `SMC #0x736`. EL3 restores the SCR_EL3 value that
  `native_el3_cache_probe` saved, as the exercised return does, and returns
  to the ROM through the base overlay's RVBAR/RMR sequence.

| Profile | Added to the workload | What it separates | Deadline |
| --- | --- | --- | --- |
| `all-core-task-workload-deadline-v1` | The return deadline only. | Whether CPU0 still executes, and where every core stopped: the record gives CPU0's interrupted context, its tick timer, every core's pending, active and enabled SGI/PPI words and power status; the owned-interval snapshot gives each core's rounds, background progress, SGI and tick counts, arrival and completion records, current task and the kernel lock words. A return with reason 2 and detail 30 or 0 means CPU0 still took interrupts; reason 1 means it did not. No return is inconclusive (see below). | Startup deadline + 0.5 s, about 1.5 s after launch; the runner waits 10 s. |
| `all-core-task-workload-quiet-deadline-v1` | Also replaces the background loop's `BL ucPortGetCoreID` (`0x41001434`) with `WFE` (`--quiet-background`). Each background task increments once per wake-up, and its core otherwise waits. | Lower sustained load, together with other changes (see below). | Same. |

The quiet profile does not isolate the sustained load. Before the core-index
check, `ucPortGetCoreID` (`0x410109b8`) issues `SVC #0x6d` on every loop
iteration, so the replacement also removes that supervisor-call traffic on
all eight cores. That changes when interrupts arrive and the lock traffic
around them, and it removes the check that a background task has not moved
to another core (W0 keeps the first result). A completed quiet run is
therefore consistent with the race candidates as well as with candidate 4.

Profiles that disable the tick, the SGIs or the floating-point rounds are not
built. Each round's floating-point check sleeps for one tick
(`check_fp_context`, `0x41007a58`), and every secondary worker is woken by
an SGI from CPU0's tick, so removing either source needs scheduler changes
that would no longer be the run under test. The record and snapshot report
the tick and SGI counts of every core directly. Two one-word follow-ups
remain available if the first profile's record points at them: programming
the tick from the current counter (replacing the CVAL load in
`platform_tick_ack`) and enabling the CNTKCTL event stream, both for
candidate 2.

### Record at `0x4201b200`

The record occupies the first 512 bytes of the area that a completed survey
fills with CPU0's EL1 record page, so its magic number cannot appear after a
completed run. Fields are 64-bit words at the byte offsets given.

| Offset | Content |
| --- | --- |
| 0, 8 | Magic `0x413733444c4e5231`, version 1 |
| 16 | Reason: 1 deadline interrupt, 2 CPU0 exit SMC before completion, 3 failed survey at EL3, 4 refused completion SMC |
| 24 | Stage: 1 captured, 3 captured and cleaned |
| 32, 40 | Counter at capture; armed deadline |
| 48, 56, 64 | ELR_EL3, SPSR_EL3, ESR_EL3 (meaningful only for an SMC, reasons 2 and 4) |
| 72, 80 | x0 and x1 at entry (status and detail of an exit SMC) |
| 88–128 | ELR_EL1, SPSR_EL1, ESR_EL1, FAR_EL1, SP_EL0, SP_EL1 |
| 136, 144 | x29 and x30 at entry |
| 152 | Acknowledged Group 0 interrupt (29, or 1023 for other reasons) |
| 160, 168 | CPU0 CNTP_CTL_EL0 and CNTP_CVAL_EL0 (the FreeRTOS tick) |
| 176, 184 | ICC_PMR_EL1 and ICC_RPR_EL1 |
| 200, 208 | Lines cleaned by the EL1 helper; counter after cleaning |
| 216, 224 | Survey state and completed mask from the identity report header |
| 232 + 32·n | Core n: GICR_ISPENDR0, GICR_ISACTIVER0, GICR_ISENABLER0, power status |
| 496 | 1 once CPU0 starts the ROM return |

### Host behaviour after a deadline return

`tools/workload-bisect-host.py` checks the record (nonce, version, reason,
stage, interrupt, cleaned lines, deadline against the contract) and reports a
failed check in the record's `inconsistent` field. Any report that carries
the magic number, consistent or not, is treated as a deadline return: a
fault in the cleaning helper re-enters the capture and returns at stage 1,
with the secondaries still running. The runner then reads the owned writable interval (`0x91000` bytes) into
`owned-writable-after.bin`, writes nothing, keeps the claim and quarantines
the instance. The secondaries were not stopped: they may still run FreeRTOS
from DRAM with their GIC and power state unrestored, so the board needs a
power removal before its next run. A completed run is validated exactly as
the workload, and the runner restores DRAM as before.

## The deadline return itself is unproven on a board

The return path, a CPU0 warm reset through RVBAR and RMR while seven
coherent secondaries keep running, has never run on a board; only the
instruction model has executed it. Once CPU0 leaves, nothing takes the
secondaries' own deadline exit, because CPU0's tick drives every interrupt
they receive: in the first profile their background loops keep running at
full load. A run with no return is therefore inconclusive: either CPU0
could not take the deadline interrupt, or the reset and return themselves
hung with the secondaries running.

The follow-up is a forced-stall proof variant: the first profile with one
secondary deliberately stopped before its completion record, as in the
modeled stalled-core case, run once in a lifetime that has nothing else to
test. Its expected outcome is a deadline return with reason 2. A return
proves the path on that board; no return isolates the path itself.

## State the deadline return leaves changed

The capture and return (`workload-deadline-probe.S:115`–`135`,
`:238`–`254`) restore only SCR_EL3. They leave:

- TCR_EL3, TTBR0_EL3 and MAIR_EL3 with the production cached values, and
  SCTLR_EL3 with M, C and I cleared; the completed path restores all of
  them in `native_el3_cache_probe` (`native-el3-cache-probe.S:110`–`117`);
- VBAR_EL3 pointing at the production EL3 vectors;
- PPI 29 in CPU0's redistributor as Group 0, priority 0 and enabled, with
  CNTPS_CTL disabled but CNTPS_CVAL still at the deadline;
- every secondary running or parked, with its GIC and power state as the
  workload left it.

This is acceptable because the return is a warm reset of CPU0, which resets
its system registers, and because a deadline return always ends in a power
removal before the board's next run; the runner writes nothing after the
launch.

## Safety review

- **Exclusion of other interrupts.** Only CPU0's PPI 29 is Group 0; no SPI
  is enabled. Nonsecure reads of Group 0 bits return zero, so the workload's
  own drain checks do not see PPI 29. GIC-600 erratum 2384374 concerns SPI
  and LPI packets; SPIs and LPIs stay disabled, so the added Group 0 PPI
  does not meet its conditions.
- **Snapshot timing.** The cleaning reaches every core's dirty lines through
  the Inner Shareable mapping, but the secondaries keep writing afterwards;
  the ROM's uncached reads see the cleaned values plus any later eviction,
  not one instant.
- **Return with running secondaries.** CPU0 warm-resets while the
  secondaries are coherent, may still send SGIs and keep entry addresses in
  DRAM. No architectural statement covers the ROM's behaviour in that state;
  the host reads only, and power removal ends it.
- **Restoration on the completed path.** The completion SMC disables CNTPS
  and Group 0 before the unchanged restoration; `native_el3_cache_probe`
  restores the saved IPRIORITYR7 word, and `restore_local` disables all
  local sources and restores the group word. The modeled success case
  requires every GIC word restored.
- **Speculative-AT workaround.** The capture skips the block-walk sequence
  of `el1_return_checkpoint`. EL3 does not change the EL1 translation
  context on this path; it returns to the same EL1 regime and then resets.
- **Not covered.** A stall in an EL3 path with FIQ masked (the production
  EL3 fault handler, `a733_smp_stop`) and a stalled bus access on CPU0 still
  give no return.

## Merging changes shared files

Besides its new files, the change edits files that other returning
profiles share: `boot/a733/native-el1-cache-probe.S`,
`boot/a733/secondary-identity-probe.S`,
`tools/prepare-secondary-identity-probe.py`,
`tools/run-native-startup-probe.py` and the host modules
`tools/secondary-identity-host.py`, `tools/all-core-first-task-host.py` and
`tools/all-core-task-workload-host.py`. The default builds stay
byte-identical, but the source and host-evidence digests change, so frozen
returning bundles fail their source checks against the merged tree. Each
frozen bundle still runs from its own source copy; a refreeze of the rank 7
and rank 8 bundles is planned at merge.

Follow-up work, deferred to a later change: the replacement-word
computation for the deadline patches exists in both the builder and
`tools/secondary-identity-host.py`, and the record address, magic number
and deadline constant are repeated in the assembly, the builder, the host
module and the model. Both should move to one definition.

## Budgets and measurements

- **Default images.** With the new options absent, the identity, first-task,
  task-switch and task-workload builds of this tree equal the builds of
  `79b54841` in all 24 compared files (overlay, three payload segments,
  `identity.bin`, `identity.elf`).
- **Overlay space.** The extension uses 12,272 of its 12,288 bytes.
- **Completed-path cost (counted).** CPU0 executes 22 more instructions once
  at EL1 entry and 17 more at its completion SMC; each secondary executes 3
  more at entry and 4 more per SMC. The workload loops are unchanged.
- **Capture cost (estimated).** About 150 instructions at EL3 and 9,280
  `DC CIVAC`, well under 1 ms.
- **Instruction model** (`checks/workload-bisect-probe.py`). The deadline
  profile completes with the deadline armed and disarmed before it fires.
  With core 5 stopped before its completion record, CPU0 returns through
  its IRQ handler's deadline exit (reason 2, detail 30); with CPU0's tick
  also stopped, the deadline interrupt returns it (reason 1). The quiet
  profile completes. Every core reports 32 rounds in all three completed
  runs (default, deadline, quiet). The extra entry instructions shift the
  model's interleaving, so CPU0 counts 524 ticks with the deadline against
  581 without it; the secondaries' background counts are unchanged, and the
  quiet profile's fall from 1,069–1,864 to 279–490 per secondary is the
  intended reduction. The model runs take 105 s (default), 120 s (deadline)
  and 113 s (quiet); the two stall cases, resumed from the completed-worker
  checkpoint, take 8 s and 10 s. Host cases
  (`checks/workload-bisect-host.py`) run the actual runner with a modeled
  FEL for a completed return, a transport failure, an invalid report, and a
  deadline return and a damaged deadline record that both restore nothing;
  further cases damage records and change inputs.
