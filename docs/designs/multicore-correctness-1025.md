# Design: GIC status checks, distributor write waits and EL2 setup for the eight-core images

Status: **host-tested**. The changes are implemented and pass the instruction models and host checks; no board has run the returning profiles with them. The reference comparison is the CPU reference backsolve, [reference-backsolve-cpu.md](../reference-backsolve-cpu.md).

The design corrects six points in the eight-core startup code and in the returning diagnostics (the overlays that run one interval of startup on the board and return to the boot ROM with a report). The tick, the interrupt exit path, the FreeRTOS port, newlib and the counter clock belong to [multicore-performance-1025.md](multicore-performance-1025.md). Test results and the per-item verification runs are in `docs/evidence/multicore-correctness-1025.jsonl`.

## 1. Snapshot comparisons ignore read-only status bits

The returning restorations take a ten-word snapshot of each core's redistributor before initialization and again after restoration, and require the two to match. Four of the words mix writable configuration with status that hardware changes on its own, so a comparison masks the status bits:

- GICR_CTLR: UWP (bit 31) and RWP (bit 3), write-pending status (IHI 0069G 12.11.2).
- GICR_WAKER: Quiescent (bit 31) and ChildrenAsleep (bit 2), which follow ProcessorSleep.
- GICR_ISPENDR0: the pending state, which a disabled level-sensitive PPI can still assert.
- GICR_PWRR: RDGPO (bit 3) and RDGPD (bit 2). All eight frames report group 0, so one core's status bits reflect the power transitions of all eight.

`boot/a733/secondary-local-probe.h` defines one mask per word (`GICR_CTLR_STATUS`, `GICR_WAKER_STATUS`, `GICR_ISPENDR0_STATUS`, `GICR_PWRR_STATUS`). `secondary_local_same`, defined in `secondary-distributor-probe.c` because both overlay builders that run a restoration link that unit, applies them; CPU0's restoration (`primary-local-cache-probe.c`) and the secondaries' cleanup (`secondary-local-probe.c`) both call it. The host decoders (`tools/secondary-local-host.py`, `tools/all-core-pre-scheduler-host.py` and CPU0's in `tools/prepare-primary-cache-probe.py`) apply the same masks, and the survey builder holds the masking function that all three decoders call.

These checks stay unmasked: the RDGPD = RDGPO stability check before each power request, each wait in a handshake (RWP after the disabling and restoring writes, ChildrenAsleep after a ProcessorSleep change), the drain checks (nothing enabled, pending or active at baseline; no pending SGI or PPI after cleanup disables them; no unexpected pending PPI after the workload), and the raw words in every record.

Reference: Linux and TF-A poll RWP and ChildrenAsleep and never compare a GIC register with a saved value (`irq-gic-v3.c` `gic_do_wait_for_rwp`; TF-A `gicv3_helpers.c`).

Scope: the overlay code of every returning profile that runs a local restoration. No runtime image contains this code.

## 2. Distributor write-pending bits are waited for

A set GICD_CTLR.RWP (bit 31) is waited for, bounded by the deadline the site already holds, and a timeout reports its own code:

| Site | Behaviour; timeout result |
| --- | --- |
| `platform/a733/cpu_continue.c`, phase 0 of the per-core controller | The phase repeats until RWP reads clear; a timeout fails with `ERROR_DISTRIBUTOR_BUSY` (5). The controller state keeps the last GICD_CTLR value read in a field appended after the existing ones, so the layout still matches the frozen native image. |
| `boot/a733/secondary-distributor-probe.c`, distributor preparation | Waits, then reads the configuration; `DISTRIBUTOR_ERROR_RWP_TIMEOUT` (2) in the record's `error` word, and `DISTRIBUTOR_ERROR_CONFIGURATION` (1) for a wrong configuration. |
| `secondary-distributor-probe.c`, final restoration check | Equality outside RWP, after the existing wait. |
| `platform/a733-smp-cached/irq_table.c`, SPI routing | Waits before reading the distributor and after the route writes; `A733_IRQ_TABLE_RWP_TIMEOUT` (2), which the boot patches of the OS, I2S and NPU images report as stop detail `0x311`. A refusal keeps `0x310`. |

Reference: Linux `gic_do_wait_for_rwp` and TF-A `gicd_wait_for_pending_write` poll RWP until it clears. In the cached image a refusal parks the core, the other cores then fail their arrival barrier and the image stops without a report. In the current images the two waits in `irq_table.c` cannot find RWP set: the primary boot waits for RWP after its own distributor writes immediately before routing, no secondary runs yet, and route writes do not set RWP (IHI 0069G 12.9.4). They keep the routing correct if another distributor writer is added.

Scope: `cpu_continue.c` links into every image built from the cached sources and into the uncached diagnostic. `irq_table.c` links into the OS, I2S and NPU images. The returning profiles still call the controller of the frozen native image, so their per-core phase 0 keeps the refusal until the native image is refrozen ([native-refreeze-1025.md](native-refreeze-1025.md)).

## 3. Every core sets up the unused EL2 and zeroes its active priorities

`a733_cached_enter_el1` in `platform/a733-smp-cached/continue.S` performs the steps that the references perform for an operating system entered at EL1 with EL2 unused:

- beside CNTHCTL_EL2 and CNTVOFF_EL2: VPIDR_EL2 = MIDR_EL1, VMPIDR_EL2 = MPIDR_EL1, CNTHP_CTL_EL2 = 0;
- at EL3, after the group enables are cleared and before SCR_EL3 is set: SCR_EL3.NS cleared, then ICC_AP0R0_EL1 = 0, an ISB, and the Secure ICC_AP1R0_EL1 = 0;
- at EL1, after the priority mask and before Group 1 is enabled: the Nonsecure ICC_AP1R0_EL1 = 0.

All three EL2 registers are UNKNOWN after a reset. EL1 reads of MIDR_EL1 and MPIDR_EL1 return VPIDR_EL2 and VMPIDR_EL2, and SunstoneOS derives its core index from MPIDR. An enabled EL2 physical timer asserts PPI 26, which the drain checks refuse. A nonzero active priority would mask every interrupt at or below that priority. The CPU interface has five priority bits (Cortex-A55 TRM B4.7), so only the APxR0 registers exist (IHI 0069G 12.2.1), written in the order that section requires: ICC_AP0R0, the Secure ICC_AP1R0, then the Nonsecure ICC_AP1R0. With SCR_EL3.FIQ set, a Group 0 register access from EL1 traps to EL3, so the Group 0 and Secure Group 1 copies are zeroed at EL3.

The returning profiles run the frozen native entry, which lacks these steps. `native_el1_cache_probe` (`boot/a733/native-el1-cache-probe.S`), the EL3 code that calls that entry, mirrors them before the call and zeroes all three active-priority copies, selecting each ICC_AP1R0_EL1 copy with SCR_EL3.NS and restoring SCR_EL3.

Reference: TF-A `init_nonsecure_el2_unused` (`context_mgmt.c`); the vendor BL31 does the same; Linux `gic_cpu_sys_reg_init` zeroes ICC_AP0Rn, then ICC_AP1Rn, then enables Group 1. TF-A writes no active-priority register.

Cost: each core executes 13 more instructions once per entry; the mirror adds 18 once per core to the returning profiles from the secondary cache profile onwards and to the CPU0 profiles with `--el1`. The observer and storage-maintenance builds pin the hash of `continue.S` in `cmake/a733-smp-observe.cmake` and `cmake/a733-storage-maintenance.cmake`. The uncached diagnostic's continuation (`platform/a733/cpu_continue.S`) is unchanged because it does not deliver interrupts.

## 4. The return-deadline constants have one definition

`boot/a733/secondary-local-probe.h` defines `DEADLINE_RECORD` (`0x4201b200`), `DEADLINE_RECORD_BYTES`, `DEADLINE_RECORD_MAGIC` and `DEADLINE_TICKS` as plain literals, with the C declarations under `#ifndef __ASSEMBLER__`. `workload-deadline-probe.S` and `secondary-identity-probe.c` include it. The builder reads every `#define NAME VALUE` line with a regular expression, as `checks/memtest-image.py` does; the host modules take the values from the builder, and the model reads the header with the same expression.

## 5. The host module reapplies the builder's patch plan

`tools/prepare-secondary-identity-probe.py` defines `profile`, `schema`, `sources`, `report_area`, `extension_address`, `patch_plan(native, hooks, flags)` and `apply_plan`. `patch_plan` lists every native word that the profile replaces as (address, original word, replacement) in application order. `apply_plan` applies it to the frozen segments, requires each original word and each patch to land exactly once, and returns the patched segments and the patch records. The builder writes those results. `load` in `tools/secondary-identity-host.py` takes the schema, sources and plan from the builder, reapplies the plan to the frozen base overlay and native segments, and requires every built byte and the recorded patch inventory to match.

## 6. The resident contract has its own magic number

The resident contract magic in `platform/a733-smp-cached/boot.h` is `0x4153373352434e31`, which no other file uses; the uncached image's result magic is different. The resident build refuses the uncached value for a FEL contract; no host check covers this. Only builds that define `SUNSTONE_A733_RESIDENT` use it, and no image does.

## Workload model counter rate

The workload instruction models (`checks/all-core-task-workload-probe.py` and the return-deadline model built on it) advance the counter `COUNTER_STEP` = 250 ticks per scheduling quantum, about 50,000 CPU0 instructions per tick period. The image reprograms its tick as the previous compare value plus the period, so a tick handled late fires again at once; a faster modeled counter stalls CPU0's worker behind back-to-back ticks. Both workload models fail at once, naming the saturated model, when CPU0's enabled tick compare value falls more than one period (24,000 counter ticks) behind the modeled counter. A core on the board executes roughly a million instructions per tick period, so the board does not approach this limit. Programming the tick from the counter, as Linux does, would remove the catch-up behavior and belongs to [multicore-performance-1025.md](multicore-performance-1025.md).

The model lets the EL3-routed deadline interrupt take CPU0 in `identity_park` at EL1 in the `stalled-no-tick` case, as the hardware does. The core 3 reproduction cases (`-secondary-unfixed`) require only that core 3 itself did not reach OFF.

## Budgets

Budgets follow [performance-architecture.md](../performance-architecture.md): no hot path changes, and each added step runs once per core per launch. The runtime entry adds 13 instructions per core per entry and the probe mirror 18. Phase 0 of the controller reads GICD_CTLR once when RWP is clear. SPI routing reads GICD_CTLR twice more and the counter once more; the overlay's distributor preparation reads GICD_CTLR once more. Every returning overlay stays within its 12,288-byte text limit (record `multicore-correctness-1025-budgets-and-models`).

## Limitations and open work

- Frozen returning bundles bind sources that differ at the head and need a refreeze before their next board use ([native-refreeze-1025.md](native-refreeze-1025.md)). Each bundle runs from its own source copy, so none changes.
- The frozen native image `762237cd…` lacks items 2 and 3 at runtime; the returning profiles get item 3 from the probe mirror.
- Open: a board run of the returning profiles with these changes, through [the qualification runbook](../board-runbooks/refreeze-qualification.md).
