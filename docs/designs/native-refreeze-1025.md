# Design: running the returning diagnostics on the native image built from bringup

Status: **host-tested**. The builders, host modules and instruction models are implemented and pass against the current native image; no board has run it. The board sequences are [the refreeze qualification runbook](../board-runbooks/refreeze-qualification.md) and [the EL3 qualification runbook](../board-runbooks/el3-qualification.md).

The returning diagnostics patch a frozen native image and add an overlay. The image must carry the corrected EL1 entry: VPIDR_EL2, VMPIDR_EL2 and CNTHP_CTL_EL2, the three active-priority copies, the Secure priority mask, SCTLR_EL1.nTWE, and the per-core controller's wait for GICD_CTLR.RWP ([multicore correctness](multicore-correctness-1025.md)). The image `762237cd…`, frozen on 2026-10-18, lacks them, and the old frozen bundles that still bind it run from their own source copies.

## The native image

`a733-smp-supervised.elf` with SHA-256 `6fd7af86…` and build ID `b6d3e7dc…`, built from a clean archive of the tree, is the image that `tools/prepare-primary-publication-probe.py` pins. It includes `a733_el3_security_setup` ([`include/a733_el3_security.h`](../../include/a733_el3_security.h)), which `a733_smp_boot` calls after it releases the secondaries and before CPU0 enters EL1. The admission template copies the 2026-10-18 first-task records with the image hash replaced, and its launcher is byte-identical to the pinned launcher. Provenance and input manifests: `docs/evidence/native-refreeze-1025.jsonl`.

## What the builders and host modules pin

Each is an exact pin, checked against the image when a profile is built:

| Pin | Value |
| --- | --- |
| First EL1 interrupt-interface write (`MSR ICC_CTLR_EL1, XZR`), the cached patch point in both overlay builders | `a733_cached_enter_el1+0x194` |
| First-task patch setting SCTLR_EL1.nTWE | None: the entry sets nTWE and nTWI |
| EL1 SCTLR expected by the overlay, the host decoders and the first-task model | `NATIVE_EL1_SCTLR`, `0x30d51805`, in `boot/a733/secondary-local-probe.h` |
| Quiet profile's `BL ucPortGetCoreID` | Displacement computed from the symbols |
| `worker` | `0x41001244` |
| End of BSS | `0x41290f00` (the owned writable interval still ends at `0x41291000`) |
| `a733_cached_el1_vectors` in the first-task saved context | `0x4100a800` |
| Workload completion patch | `worker+0x6c`, from the manifest |

The overlay keeps its own copies of the entry's EL3 steps; they repeat the native writes, which is harmless and costs 18 instructions per core.

## EL3 security setup in the returning overlays

The returning overlays start the secondaries themselves and never reach `a733_smp_boot`'s call. The pre-scheduler overlay, which the first-task, task-switch, workload and deadline profiles extend, calls the native `a733_el3_security_setup` at the native place: after all seven secondaries are released and before CPU0 enters EL1. The identity, local, cached, coherence, interrupt and CPU0 profiles make no write. The base model gives the setup's three reads their FEL-boot values (PRCM switch 0, both DMA controllers gated and in reset) and records its writes, which only CPU0 at EL3 may make. The pre-scheduler, first-task, workload and deadline models require the exact list of 31 writes once, after the seventh release and before CPU0's EL1 entry. The first-task refusals require none, and the supervised scheduler model requires the list from `a733_smp_boot`.

## Instruction models

The models follow three behaviours of the native entry:

- the EL1 coherence helper reads the two sealed EL1 table pages and their stored sum, and cleans their lines, before it enables the MMU;
- the entry sets the Secure priority mask to 255 at EL3;
- the idle tasks wait with `DSB; WFI` at EL0. An EL0 `WFI` waits in the model until an interrupt is taken, which returns to the next instruction; the secondary and pre-scheduler models do not count it as a park.

The base model provides `sealed_el1_table_read`, `production_table_clean` and the symbol sizes, and the secondary, CPU0, coherence, interrupt, pre-scheduler, first-task and workload models use them. In `stalled-core`, CPU0 can wait in `identity_park` when the stalled core had already arrived; the deadline interrupt then returns it with reason 1. The model records which wait CPU0 reached, and the model and both host modules expect the matching reason.

## Task-switch profile idle loops

The task-switch profile stops the tick (`platform_tick_setup` returns at once) and sends no SGI, and it has no deadline unit. An idle task that resumed after its first yield would reach its `WFI` and wait for an interrupt that never comes. The profile replaces each idle `WFI` (`prvIdleTask+0x1c`, `prvPassiveIdleTask+0x10`) with `SVC #0x69`, the idle yield, so a resumed idle task yields until its worker runs. The instruction case `idle-resumes` checks it. The workload keeps the native `WFI`: it has the tick, the SGIs and the deadline unit, and its idle tasks always yield because the background tasks share their priority.

## The deadline record says where CPU0 waited

For a reason 1 return, the host decoder compares ELR_EL3 with the overlay's `identity_park`. Inside it, CPU0 was waiting after a completion barrier timed out; elsewhere, CPU0 may be wedged where it was interrupted. The failure message names which, and the instruction evidence must agree with the model's record of the wait. The workload host takes its completion patch address from the pinned native ELF.

## Qualified figures

Host-model results only, from the archive at `22501205`: all fifteen profiles build, the 32 model runs, the four CPU0 models, the fifteen host checks and the eight-core scheduler model pass, and all twelve bundles bind offline. Record: `docs/evidence/native-refreeze-1025.jsonl`, `native-refreeze-1025-el3-security`.

## Limitations and open work

- Open: the board runs of [the refreeze qualification](../board-runbooks/refreeze-qualification.md) and [the EL3 qualification](../board-runbooks/el3-qualification.md) with this image.
- `a733_smp_boot`'s call to `a733_el3_security_setup` is unrun on a board, because no returning image enters EL1 through it.
