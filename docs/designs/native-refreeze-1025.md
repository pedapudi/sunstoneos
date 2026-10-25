# Design: running the returning diagnostics on the native image built from bringup

Status: **implemented and checked on host models; not run on a board.** The
board sequence is [the qualification runbook](../board-runbooks/refreeze-qualification.md).

The returning diagnostics patch a frozen native image and add an overlay.
Until now that image was `762237cd…`, frozen on 2026-10-18. It predates the
corrected EL1 entry: VPIDR_EL2, VMPIDR_EL2 and CNTHP_CTL_EL2, the three
active-priority copies, the Secure priority mask and SCTLR_EL1.nTWE, and the
per-core controller's wait for GICD_CTLR.RWP
([multicore correctness](multicore-correctness-1025.md)). The overlay copied
some of those steps; the controller's wait was absent from every returning
run.

## The new image

`a733-smp-supervised.elf`, built from a clean archive of `bringup` at
`d502f8ea`, has SHA-256 `a8228eaa…` and build ID `5a454f77…`
(`~/.cache/sunstoneos/evidence/refreeze-20261025/native/`, with its input
manifest and provenance). `tools/prepare-primary-publication-probe.py` pins
it. Its admission template (`refreeze-20261025/template/`) copies the
2026-10-18 first-task records with the image hash replaced; its launcher is
byte-identical to the pinned launcher.

## What the builders and host modules pin

The new image moves or changes these words; each is an exact pin, checked
against the image when a profile is built:

| Pin | Old | New |
| --- | --- | --- |
| First EL1 interrupt-interface write (`MSR ICC_CTLR_EL1, XZR`), the cached patch point in both overlay builders | `a733_cached_enter_el1+0x144` | `+0x194` |
| First-task patch setting SCTLR_EL1.nTWE (`+0xac`) | present | removed: the entry sets nTWE and nTWI |
| EL1 SCTLR expected by the overlay, the host decoders and the first-task model | `0x30d01805`, or `0x30d41805` with the patch | `NATIVE_EL1_SCTLR`, `0x30d51805`, in `boot/a733/secondary-local-probe.h` |
| Quiet profile's `BL ucPortGetCoreID` | displacement `0x3d61` | computed from the symbols |
| `worker` | `0x41001280` | `0x4100123c` |
| End of BSS | `0x41290d10` | `0x41290f00` (the owned writable interval still ends at `0x41291000`) |
| `a733_cached_el1_vectors` in the first-task saved context | `0x4100a000` | `0x4100a800` |
| Workload completion patch | `0x410012ec` | `worker+0x6c`, from the manifest |

The overlay keeps its own copies of the entry's EL3 steps; they now repeat
the native writes, which is harmless and costs 18 instructions per core.

## The instruction models follow the newer entry

Three behaviours of the native image are new to the models:

- the EL1 coherence helper reads the two sealed EL1 table pages and their
  stored sum, and cleans their lines, before it enables the MMU;
- the entry sets the Secure priority mask to 255 at EL3;
- the idle tasks wait with `DSB; WFI` at EL0 and are longer. An EL0 WFI now
  waits in the model until an interrupt is taken, which returns to the
  next instruction; the secondary and pre-scheduler models no longer count
  it as a park.

The base model now provides `sealed_el1_table_read` and
`production_table_clean` and the symbol sizes. The secondary, CPU0,
coherence, interrupt, pre-scheduler, first-task and workload models use them
instead of rules that assumed the old helper. In `stalled-core`, CPU0 can
wait in `identity_park` when the stalled core had already arrived; the
deadline interrupt then returns it with reason 1. The model records which
wait CPU0 reached, and the model and both host modules expect the matching
reason.

## The task-switch profile keeps its idle loops yielding

The task-switch profile stops the tick (`platform_tick_setup` returns at
once) and sends no SGI, and it has no deadline unit. An idle task that
resumed after its first yield would reach its WFI and wait for an interrupt
that never comes; the board would not return. The instruction case
`idle-resumes` makes core 3's first idle yield return to the idle task:
with the native loop, core 3 waits at `prvPassiveIdleTask+0x10` and the
survey never ends. The profile therefore replaces each idle WFI
(`prvIdleTask+0x1c`, `prvPassiveIdleTask+0x10`) with `SVC #0x69`, the idle
yield; a resumed idle task then yields until its worker runs, and the case
completes. The workload keeps the native WFI: it has the tick, the SGIs and
the deadline unit, and its idle tasks always yield because the background
tasks share their priority.

## The deadline record says where CPU0 waited

For a reason 1 return, the host decoder compares ELR_EL3 with the overlay's
`identity_park`. Inside it, CPU0 was waiting after a completion barrier
timed out; elsewhere, CPU0 may be wedged where it was interrupted. The
failure message names which, and the instruction evidence must agree with
the model's record of the wait. The workload host takes its completion
patch address from the pinned native ELF.

## Verification

From the source tree at `6cd9d008` (`refreeze-20261025/source-6cd9d008/`):
all fifteen profiles build, and the builds equal those of the working tree;
only the task-switch native segment differs from the first refreeze set, by
the two idle patches. The identity model, which boots the new image from
reset, and the 31 model cases, `idle-resumes` among them, pass. The four
CPU0 profile models, all fifteen host checks and the eight-core scheduler
model built from the tree pass too. All twelve bundles under
`refreeze-20261025/bundles-6cd9d008/` bind offline. No board
has run the new image; the qualification runbook is the physical test.
