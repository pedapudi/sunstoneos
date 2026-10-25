# Design: a CPU0 return deadline for the returning eight-core workload

Status: **implemented**; instruction-model and host checks pass. Nothing in
this document has run on a board. Section numbers refer to
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
generation, reservation). The boards differ in supply: port 3-3 is powered
from USB alone, port 3-1 has a separate 5 V, 3 A supply. Port 3-3 had also
run more images in its powered lifetime, among them the PMIC bundles, bundle
14 and 15 and the native USB image. Its 91-word power, CPU-configuration and
GIC capture after the thermal run equalled the clean capture.

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
| Tick programming | Linux `set_next_event` writes CVAL = counter + delta, then CTL with ENABLE set and IMASK clear, with an ISB after each write (`arm_arch_timer.c:741`, `arch_timer.h`). The handler acts only when CTL.ISTATUS is set and first sets IMASK (`arm_arch_timer.c:665`). FreeRTOS's SMP tick handler clears the source after `xTaskIncrementTick`. | `platform_tick_ack` adds a fixed 24,000 ticks to the previous CVAL (`platform.c:124`). The tick handler tail-calls it after restoring PMR and clearing DAIF.I (`0x41010bcc`–`0x41010be0`). The IRQ handler does not read ISTATUS. | A tick handled late leaves CVAL in the past, so the timer fires again at once until the schedule catches up, and each tick sends time-slice SGIs to all seven secondaries. A level still sampled at EOI counts an extra tick. Bounded; not a hang by itself. |
| Event stream and WFE waits | Linux enables a 10 kHz event stream in CNTKCTL (`arm_arch_timer.c:915`), so every WFE wait wakes periodically. | `CNTKCTL_EL1 = 3` (`continue.S:140`): counter access only. Lock waits rely on the releaser's `DSB SY; SEV` (`0x41010eac`). | Architecturally sufficient. A missed event would leave a core waiting forever with interrupts masked. |
| EL2 controls for an unused EL2 | TF-A sets CNTHCTL_EL2 EL1PCEN and EL1PCTEN, CNTVOFF 0, CNTHP_CTL_EL2.ENABLE 0 and VPIDR/VMPIDR from MIDR/MPIDR (`context_mgmt.c:1133`–`1208`). | CNTHCTL_EL2 = 3 and CNTVOFF = 0 (`continue.S:113`); CNTHP_CTL_EL2, VPIDR_EL2 and VMPIDR_EL2 keep their reset values. | The workload's drain refuses any pending PPI other than SGI 0 and PPI 30 (`all-core-first-task-probe.S:215`); an enabled EL2 timer would assert PPI 26 and park that core. Recorded EL1 MPIDR reads equal the hardware values and CPU0's ISPENDR0 read 0 in the ROM. |
| SGI send | Linux: `DSB ISHST`, write ICC_SGI1R_EL1, `ISB` (`irq-gic-v3.c:1428`–`1450`). | `DSB ISHST`, write ICC_SGI1R_EL1, `ERET` (`0x4101141c`–`0x41011438`), reached from the tick handler through a nested `SVC #117` at EL1 (`0x41010cb4`). | Equivalent ordering; the nested SVC is unusual but ran on port 3-1. |
| Acknowledge, priority, EOI | Linux EOImode 0 at EL1: EOIR1 then ISB after the handler (`irq-gic-v3.c:719`). | IAR1 at entry, handler, DAIF.I set, `DSB SY; ISB`, EOIR1 (`0x41011cc0`, `0x41011d24`–`0x41011d34`); ICC_CTLR_EL1 = 0. The tick and SGI handlers raise PMR to `0xb0` and clear DAIF.I as upstream FreeRTOS does; both sources have Nonsecure priority `0xe0`, so neither preempts the other. | No difference with an effect. GIC-600 erratum 1014611 (SGI/PPI deferred until deactivation) does not apply with combined EOI and deactivation. |
| FP/SIMD enable | TF-A CPTR_EL3 reset value; Linux `el2_setup.h` CPTR_EL2 with TFP clear. | CPTR_EL3 = 0, CPTR_EL2 = `0x33ff`, CPACR_EL1.FPEN = 3 on every core (`continue.S:111`, `:138`). FreeRTOS saves q0–q31, FPSR and FPCR on every IRQ and every task (`configUSE_TASK_FPU_SUPPORT 2`). | Same trap controls; eager saving instead of Linux's lazy saving. |
| CPU errata | TF-A applies Cortex-A76 r4p1 1946160, 2356586, 3888013 and 2743102, and the DSU conditions (`cortex_a76.S:429`–`488`). | `platform/a733/cpu_coherence.c` applies the same set. | None. |
| Power-on and GIC wake | The A733 TF-A port sets the hotplug power mode and enable, then clears GIC_WAKEUP_DISABLE (`sunxi_native_pm_ncat2.c`). | `cpu_continue.c` clears control bit 1 in the same register. | None. |

No difference explains a deterministic failure of bytes that returned on
port 3-1. The first three rows remain candidates because their effect depends
on timing or on reset state.

## Candidate causes, most likely first

1. **Sustained eight-core load on a USB-only supply.** This is the first
   profile that keeps all eight cores busy: the background loop
   (`diagnostic.c:8`–`14`, `0x41001424`–`0x4100143c`) never waits, for at
   least 32 ms, on both A76 cores as well. The earlier profiles leave the
   secondaries mostly idle. The identical bytes returned on the board with
   the separate supply. No reference difference is involved.
2. **Tick backlog and extra ticks under lock contention** (first table row).
   A late tick produces back-to-back ticks, each with seven SGIs and seven
   context switches under the kernel locks.
3. **A lost wake in a kernel-lock WFE** (second row). It needs a missed
   event and would leave a core, possibly CPU0, waiting with interrupts
   masked.
4. **An unexpected pending PPI at the drain** (third row), which parks one
   core in `identity_park` and then CPU0 at its completion barrier.
5. **State left by earlier images in port 3-3's lifetime** outside the 91
   captured words.

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
  writers used, and issues `SMC #0x736`. EL3 then returns to the ROM through
  the base overlay's RVBAR/RMR sequence.

| Profile | Added to the workload | What it separates | Deadline |
| --- | --- | --- | --- |
| `all-core-task-workload-deadline-v1` | The return deadline only. | Whether CPU0 still executes, and where every core stopped: the record gives CPU0's interrupted context, its tick timer, every core's pending, active and enabled SGI/PPI words and power status; the owned-interval snapshot gives each core's rounds, background progress, SGI and tick counts, arrival and completion records, current task and the kernel lock words. A return with reason 2 and detail 30 or 0 means CPU0 still took interrupts; reason 1 means it did not. No return means CPU0 could not take an EL3 interrupt (candidate 1, or an EL3 park the unit does not cover). | Startup deadline + 0.5 s, about 1.5 s after launch; the runner waits 10 s. |
| `all-core-task-workload-quiet-deadline-v1` | Also replaces the background loop's `BL ucPortGetCoreID` (`0x41001434`) with `WFE` (`--quiet-background`). Each background task increments once per wake-up, and its core otherwise waits. | Whether the sustained load is required for the failure (candidate 1). Run it after the first profile fails without return or with several cores stopped at unrelated addresses. | Same. |

Profiles that disable the tick, the SGIs or the floating-point rounds are not
built. Each round's floating-point check sleeps for one tick
(`check_fp_context`, `0x41007a58`), and every secondary worker is woken by
an SGI from CPU0's tick, so removing either source needs scheduler changes
that would no longer be the run under test. The record and snapshot report
the tick and SGI counts of every core directly. Two one-word follow-ups
remain available if the first profile's record points at them: programming
the tick from the current counter (replacing the CVAL load in
`platform_tick_ack`) for candidate 2, and enabling the CNTKCTL event stream
for candidate 3.

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
| 48, 56, 64 | ELR_EL3, SPSR_EL3, ESR_EL3 |
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
stage, interrupt, cleaned lines, deadline against the contract). The runner
then reads the owned writable interval (`0x91000` bytes) into
`owned-writable-after.bin`, writes nothing, keeps the claim and quarantines
the instance. The secondaries were not stopped: they may still run FreeRTOS
from DRAM with their GIC and power state unrestored, so the board needs a
power removal before its next run. A completed run is validated exactly as
the workload, and the runner restores DRAM as before.

## Safety review

- **Exclusion of other interrupts.** Only CPU0's PPI 29 is Group 0; no SPI
  is enabled. Nonsecure reads of Group 0 bits return zero, so the workload's
  own drain checks do not see PPI 29.
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

## Budgets and measurements

- **Default images.** With the new options absent, the identity, first-task,
  task-switch and task-workload builds of this tree equal the builds of
  `79b54841` in all 24 compared files (overlay, three payload segments,
  `identity.bin`, `identity.elf`).
- **Overlay space.** The extension uses 12,248 of its 12,288 bytes.
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
  checkpoint, take 8 s and 10 s. Host cases (`checks/workload-bisect-host.py`) run the
  actual runner with modeled FEL for a completed return, a transport
  failure, an invalid report, a deadline return that restores nothing,
  damaged records and changed inputs.
