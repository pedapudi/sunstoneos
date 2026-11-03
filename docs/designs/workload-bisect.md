# Design: a CPU0 return deadline for the returning eight-core workload

Status: the deadline unit is **qualified on the board** for its failed-survey path (reason 3); the FIQ entry and the EL1 cleaning helper (reasons 1 and 2) are **host-tested** only. The RWP waits and the status-bit masks that its first board run motivated are **host-tested**. Section numbers refer to [performance-architecture.md](../performance-architecture.md).

The returning eight-core workload runs the frozen native image with the workload overlay. Each core runs a worker that holds a floating-point value across 32 one-tick delays, and a background task that increments a counter whenever the worker blocks. CPU0 services the 1 kHz FreeRTOS tick; the tick wakes the secondaries' workers with SGI 0. After the rounds, every core stops its timer and interrupt delivery, the cores clean the owned writable interval (`0x41200000`–`0x41291000`), and CPU0 returns to the boot ROM after the seven secondaries reach OFF.

Every failing check or bounded wait in the overlay ends in a park, and CPU0 parks as well, so without a deadline no failure of any kind reaches the ROM and a non-return carries no information. The deadline unit gives CPU0 a return path that records where every core stopped. Run history, record decodes and the reference comparison are in `docs/evidence/workload-bisect.jsonl`.

## Failure modes the unit locates

- **GIC restoration compare.** CPU0's `restore_local` rewrites its SGI/PPI group, modifier, configuration and priority words and compares a snapshot with the original. GICR_CTLR bit 3 (RWP) and bit 31 (UWP) are read-only status that hardware changes on its own, and RWP was observed set again after a completed wait, with no write between two reads. A comparison that includes them fails intermittently.
- **Other candidates, not observed:** state left by earlier images in a long lifetime; a late tick that leaves the compare value behind the counter and produces back-to-back ticks; a lost wake in a kernel-lock `WFE`; an unexpected pending PPI at the drain; sustained eight-core load.

## Corrections

- **Wait for RWP.** CPU0's `restore_local` waits for RWP to clear within the existing deadline before its snapshot (timeout: cleanup error 4 in CPU0's record), and each secondary's `cleanup` waits straight after its four restoring stores and before the sleep request (timeout: cleanup error 7). The secondaries' separate readback of the four restored words (former cleanup error 4) is removed; the final snapshot comparison (error 6) covers the same words. Linux `gic_cpu_init` and TF-A poll RWP in the same places.
- **Mask status bits.** All comparisons of GICR_CTLR ignore bits 31 and 3: CPU0's baseline check and restoration comparison in `boot/a733/primary-local-cache-probe.c`, the secondaries' baseline check and final comparison in `boot/a733/secondary-local-probe.c`, and the host decoders (`tools/all-core-pre-scheduler-host.py`, `tools/secondary-local-host.py`). The waits stay, as in Linux. The records keep the raw values. GICR_WAKER, GICR_ISPENDR0 and GICR_PWRR status bits are masked the same way ([multicore-correctness-1025.md](multicore-correctness-1025.md), item 1).
- **The runner exits after a deadline return.** A deadline return is not an unknown outcome: the ROM has answered, and the runner has read the record and the owned memory and quarantined the instance. The runner (`tools/workload-bisect-host.py`) raises `DeadlineRetained` for that outcome and passes a no-op hold, so it journals, quarantines and exits nonzero, releasing the board lock; the queue claim stays held because the run failed, and the quarantine refuses the instance until a power removal. The hold is skipped only after the quarantine is recorded: if the journal save or the quarantine write raises, the runner holds the lock. Every other post-launch failure keeps the indefinite hold. The host cases `deadline-return-retained` and `deadline-quarantine-failure-held` check both.

## Two returning profiles

Both profiles keep the complete workload and add the same CPU0 return deadline. The builder option `--return-deadline` of `tools/prepare-secondary-identity-probe.py` (with `--task-workload`) adds `boot/a733/workload-deadline-probe.S` and two native patches:

- **Arm.** The BL at `a733_cached_enter_el1+0x34`, which replaces `MSR ICC_IGRPEN0_EL1, XZR`, runs on every core at EL3. On CPU0 only it makes PPI 29 (the EL3 physical timer, CNTPS) Group 0 at priority 0 in CPU0's redistributor, enables it, sets CNTPS_CVAL to the startup deadline plus 12,000,000 ticks (0.5 s) and enables Group 0. With SCR_EL3.FIQ set, the timer interrupt is taken to EL3 whatever masks, locks or waits CPU0 is in at EL0 or EL1.
- **Interrupt.** The EL3 vector for an FIQ from a lower exception level (`a733_cached_el3_vectors+0x500`) branches to the capture on CPU0 and to the production fault path on any other core.
- **Other CPU0 stops.** Every SMC from EL1 first passes the deadline unit. CPU0's completion SMC disarms the timer and Group 0 and continues the unchanged return. Any other CPU0 SMC (the IRQ handler's deadline exit, a refusal, a panic) is captured. A failed survey at EL3 and a refused completion SMC are captured instead of parking.
- **Capture and return.** The capture acknowledges the interrupt, stops the timer, turns the EL3 MMU off and writes the record below. It returns to CPU0 EL1 with interrupts masked, where a helper cleans the owned writable interval (9,280 lines) through the Nonsecure mapping that the writers used, and issues `SMC #0x736`. EL3 restores the SCR_EL3 value that `native_el3_cache_probe` saved and returns to the ROM through the base overlay's RVBAR/RMR sequence.

| Profile | Added to the workload | What it separates |
| --- | --- | --- |
| `all-core-task-workload-deadline-v1` | The return deadline only. | Whether CPU0 still executes, and where every core stopped. The record gives CPU0's interrupted context, its tick timer, every core's pending, active and enabled SGI/PPI words and power status; the owned-interval snapshot gives each core's rounds, background progress, SGI and tick counts, arrival and completion records, current task and the kernel lock words. Reason 2 with detail 30 or 0 means CPU0 still took interrupts; reason 1 means it did not. The deadline is the startup deadline plus 0.5 s, about 1.5 s after launch; the runner waits 10 s. |
| `all-core-task-workload-quiet-deadline-v1` | Also replaces the background loop's `BL ucPortGetCoreID` with `WFE` (`--quiet-background`). Each background task increments once per wake-up. | Lower sustained load, together with removing the supervisor-call traffic of `ucPortGetCoreID` (`SVC #0x6d` on every loop iteration) and its core-migration check, so a completed quiet run does not isolate sustained load. |

The `--top-opp` profile builds on the return-deadline workload ([cpu-top-opp.md](cpu-top-opp.md)). Profiles that disable the tick, the SGIs or the floating-point rounds are not built. Each round's check sleeps for one tick and every secondary worker is woken by an SGI from CPU0's tick, so removing either needs scheduler changes that would no longer be the run under test.

### Record at `0x4201b200`

The record occupies the first 512 bytes of the area that a completed survey fills with CPU0's EL1 record page, so its magic number cannot appear after a completed run. Fields are 64-bit words at the byte offsets given.

| Offset | Content |
| --- | --- |
| 0, 8 | Magic `0x413733444c4e5231`, version 1 |
| 16 | Reason: 1 deadline interrupt, 2 CPU0 exit SMC before completion, 3 failed survey at EL3 (`identity_primary_run` returned 0; x30 is then the gate's return address), 4 refused completion SMC |
| 24 | Stage: 1 captured, 3 captured and cleaned. Reasons 3 and 4 always end at stage 1: CPU0 has left EL1, and with it the cleaning helper's translation regime |
| 32, 40 | Counter at capture; armed deadline |
| 48, 56, 64 | ELR_EL3, SPSR_EL3, ESR_EL3 (meaningful only for an SMC, reasons 2 and 4) |
| 72, 80 | x0 and x1 at entry (status and detail of an exit SMC) |
| 88–128 | ELR_EL1, SPSR_EL1, ESR_EL1, FAR_EL1 (0), SP_EL0, SP_EL1 |
| 136, 144 | x29 and x30 at entry |
| 152 | Acknowledged Group 0 interrupt (29, or 1023 for other reasons) |
| 160, 168 | CPU0 CNTP_CTL_EL0 and CNTP_CVAL_EL0 (the FreeRTOS tick) |
| 176, 184 | ICC_PMR_EL1 and ICC_RPR_EL1 (0) |
| 200, 208 | Lines cleaned by the EL1 helper; counter after cleaning |
| 216, 224 | Survey state and completed mask from the identity report header |
| 232 + 32·n | Core n: GICR_ISPENDR0, GICR_ISACTIVER0, GICR_ISENABLER0, power status |
| 496 | 1 once CPU0 starts the ROM return |

The overlay extension uses 12,112 of its 12,288 bytes.

### Host behaviour after a deadline return

`tools/workload-bisect-host.py` checks the record (nonce, version, reason, stage, interrupt, cleaned lines, deadline against the contract) and reports a failed check in the record's `inconsistent` field. Any report that carries the magic number, consistent or not, is treated as a deadline return: a fault in the cleaning helper re-enters the capture and returns at stage 1, with the secondaries still running. The runner then reads the owned writable interval (`0x91000` bytes) into `owned-writable-after.bin`, writes nothing, quarantines the instance and exits. The secondaries were not stopped and may still run FreeRTOS from DRAM with their GIC and power state unrestored, so the board needs a power removal before its next run. A completed run is validated exactly as the workload, and the runner restores DRAM as before.

## Invariants and safety

- **Other interrupts.** Only CPU0's PPI 29 is Group 0; no SPI is enabled. Nonsecure reads of Group 0 bits return zero, so the workload's drain checks do not see PPI 29. GIC-600 erratum 2384374 concerns SPI and LPI packets, which stay disabled.
- **State left changed.** The capture and return restore only SCR_EL3. They leave TCR_EL3, TTBR0_EL3 and MAIR_EL3 at the production cached values, SCTLR_EL3 with M, C and I cleared, VBAR_EL3 at the production EL3 vectors, PPI 29 enabled as Group 0 at priority 0 with CNTPS_CTL disabled, and every secondary running or parked. This is acceptable because the return is a warm reset of CPU0 and a deadline return always ends in a power removal before the next run.
- **Snapshot timing.** The cleaning reaches every core's dirty lines through the Inner Shareable mapping, but the secondaries keep writing afterwards; the ROM's uncached reads see the cleaned values plus any later eviction, not one instant.
- **Running secondaries.** CPU0 warm-resets while the secondaries are coherent and may still send SGIs; no architectural statement covers the ROM's behaviour in that state.
- **Completed path.** The completion SMC disables CNTPS and Group 0 before the unchanged restoration; `native_el3_cache_probe` restores the saved IPRIORITYR7 word, and `restore_local` disables all local sources and restores the group word.
- **Speculative-AT workaround.** The capture skips the block-walk sequence of `el1_return_checkpoint`; EL3 does not change the EL1 translation context on this path.
- **Not covered.** A stall in an EL3 path with FIQ masked (the production EL3 fault handler, `a733_smp_stop`) and a stalled bus access on CPU0 still give no return.

## Shared files

The change edits files that other returning profiles share: `boot/a733/native-el1-cache-probe.S`, `secondary-identity-probe.S`, `primary-local-cache-probe.c`, `secondary-local-probe.c`, `tools/prepare-secondary-identity-probe.py`, `tools/run-native-startup-probe.py` and the host modules for the identity, first-task and workload profiles. The RWP corrections change the overlay of every profile from the secondary local profile onwards; the identity survey overlay and every native payload segment are unchanged except patched branch words. Source and host-evidence digests change, so frozen returning bundles fail their source checks against a merged tree and need a refreeze ([native-refreeze-1025.md](native-refreeze-1025.md)). `patch_plan` defines the replacement words of every profile, and the record address, magic number and deadline constant are defined once in `boot/a733/secondary-local-probe.h` ([multicore-correctness-1025.md](multicore-correctness-1025.md), item 4).

## Qualified and checked figures

Records: `docs/evidence/workload-bisect.jsonl`.

| Result | Value | Record |
| --- | --- | --- |
| Port 3-3 fresh lifetime, deadline workload, runs completed of five | 4; run 5 returned through the deadline unit about 12 s after launch with reason 3 | `workload-bisect-run5-gic-restore` |
| Profile with the RWP waits, first run | Returned about 5 s after launch with reason 3, stage 1 (RWP read set again) | `workload-bisect-rwp-set-after-wait` |
| Completed-path cost | CPU0 22 more instructions at EL1 entry and 17 at its completion SMC; each secondary 3 at entry and 4 per SMC | `workload-bisect-model-and-host-checks` |
| Capture cost (estimated) | About 150 instructions at EL3 and 9,280 `DC CIVAC`, under 1 ms | `workload-bisect-model-and-host-checks` |

## Limitations and open work

- Open: a board run of the profiles with the status-bit masks. The masks and waits pass the instruction models (no host check (evidence: docs/evidence/workload-bisect.jsonl)) and host cases (no host check (evidence: workload-bisect-first-profile-nonreturn; workload-bisect-run5-gic-restore; docs/evidence/w)) only.
- Open: the FIQ entry (reason 1) and the EL1 cleaning helper (reasons 1 and 2) have not run on a board. A forced-stall variant (the deadline profile with one secondary stopped before its completion record, expected to return with reason 2) would prove the cleaning path and is not built.
- Once CPU0 leaves, nothing takes the secondaries' own deadline exit, so a run without a return cannot separate a CPU0 that took no interrupt from a return that hung.
- Two one-word follow-ups remain available if a record points at the tick or a lost wake: programming the tick from the current counter (replacing the CVAL load in `platform_tick_ack`) and enabling the CNTKCTL event stream.
